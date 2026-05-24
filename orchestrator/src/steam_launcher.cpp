#include "steam_launcher.h"
#include "injector.h"
#include "bot_dota_dir.h"
#include <cstdio>
#include <sstream>
#include <ctime>
#include <TlHelp32.h>

// Build "<steamId>_<YYYY-MM>" seed (monthly rotation). Пустой если steamId=0.
static std::string BuildHwidSeed( uint64_t steamId )
{
	if ( steamId == 0 )
		return "";
	time_t now = time( nullptr );
	tm tmv{};
	localtime_s( &tmv, &now );
	char buf[64];
	snprintf( buf, sizeof( buf ), "%llu_%04d-%02d",
		(unsigned long long)steamId, tmv.tm_year + 1900, tmv.tm_mon + 1 );
	return buf;
}

// ── Per-bot Steam dir (separate config/loginusers.vdf per account) ──
static const char* BOT_STEAM_DIR = "C:\\BotSteam";

std::pair<std::string, std::string> SteamLauncher::GetSteamExeFor( int instanceIdx, const FarmConfig& cfg )
{
	// Check for per-bot Steam copy first
	char botSteam[MAX_PATH];
	snprintf( botSteam, sizeof( botSteam ), "%s\\%d\\steam.exe", BOT_STEAM_DIR, instanceIdx );

	if ( GetFileAttributesA( botSteam ) != INVALID_FILE_ATTRIBUTES )
	{
		char botDir[MAX_PATH];
		snprintf( botDir, sizeof( botDir ), "%s\\%d", BOT_STEAM_DIR, instanceIdx );
		return { botSteam, botDir };
	}

	// Fallback to main Steam install
	std::string steamDir = cfg.steamExe;
	auto lastSlash = steamDir.find_last_of( "\\/" );
	if ( lastSlash != std::string::npos )
		steamDir = steamDir.substr( 0, lastSlash );

	return { cfg.steamExe, steamDir };
}

bool SteamLauncher::SetAutoLogin( int instanceIdx, const FarmConfig& cfg )
{
	if ( instanceIdx >= (int)cfg.accounts.size() )
		return false;

	const auto& acc = cfg.accounts[instanceIdx];

	// Per-bot Steam: check loginusers.vdf for auto-login account
	// For main Steam: set registry AutoLoginUser
	HKEY hKey = nullptr;
	if ( RegOpenKeyExA( HKEY_CURRENT_USER, "Software\\Valve\\Steam",
		0, KEY_SET_VALUE, &hKey ) != ERROR_SUCCESS )
		return false;

	// LOGIN-FIX 2026-05-21: ВСЕГДА удаляем stale AutoLoginUser в HKCU. Раньше
	// мы ставили AutoLoginUser=<login> когда password пустой и надеялись на
	// saved token. Проблема: если в registry уже лежит другой login (например
	// `bxgom83235` от предыдущей сессии), а в accounts.json login сменился
	// (`zrvqd87257`), но password не введён → RegSet перетирал — но Steam
	// fallback'ил на ПРЕДЫДУЩИЙ saved token, логиня СОВСЕМ ДРУГОЙ аккаунт.
	// Юзер думал что играет под zrvqd87257, а реально играл под bxgom83235.
	// Теперь: ВСЕГДА чистим AutoLoginUser. Если password есть — CLI -login
	// решит логин. Если password пустой — Steam покажет login window и юзер
	// явно увидит что аккаунт надо ввести (вместо тихого логина чужого).
	RegDeleteValueA( hKey, "AutoLoginUser" );

	DWORD one = 1;
	RegSetValueExA( hKey, "RememberPassword", 0, REG_DWORD, (const BYTE*)&one, sizeof( one ) );

	// If per-bot Steam dir exists, point SteamPath there
	char botDir[MAX_PATH];
	snprintf( botDir, sizeof( botDir ), "%s\\%d", BOT_STEAM_DIR, instanceIdx );
	if ( GetFileAttributesA( botDir ) != INVALID_FILE_ATTRIBUTES )
	{
		// Convert backslash to forward slash, lowercase (Steam convention)
		std::string steamPath = botDir;
		for ( auto& c : steamPath )
		{
			if ( c == '\\' ) c = '/';
			c = (char)tolower( (unsigned char)c );
		}
		RegSetValueExA( hKey, "SteamPath", 0, REG_SZ,
			(const BYTE*)steamPath.c_str(), (DWORD)steamPath.size() + 1 );

		std::string steamExe = steamPath + "/steam.exe";
		RegSetValueExA( hKey, "SteamExe", 0, REG_SZ,
			(const BYTE*)steamExe.c_str(), (DWORD)steamExe.size() + 1 );
	}

	RegCloseKey( hKey );
	return true;
}

// Build null-separated ANSI environment block with redirected paths
std::string SteamLauncher::BuildEnvironmentBlock( int instanceIdx, const std::string& profilesDir )
{
	char profile[MAX_PATH];
	snprintf( profile, sizeof( profile ), "%s\\bot%d", profilesDir.c_str(), instanceIdx );

	// Get current env
	auto envStrings = GetEnvironmentStringsA();
	if ( !envStrings )
		return {};

	// Collect existing vars, skipping ones we override
	std::string block;
	const char* p = envStrings;
	while ( *p )
	{
		std::string var( p );
		std::string upper = var.substr( 0, var.find( '=' ) );
		for ( auto& c : upper ) c = (char)toupper( (unsigned char)c );

		bool skip = ( upper == "USERPROFILE" || upper == "APPDATA" ||
					  upper == "LOCALAPPDATA" || upper == "TEMP" || upper == "TMP" );

		if ( !skip )
		{
			block.append( var );
			block.push_back( '\0' );
		}

		p += var.size() + 1;
	}
	FreeEnvironmentStringsA( envStrings );

	// Add our overrides
	auto addVar = [&]( const char* name, const std::string& value )
	{
		block.append( name );
		block.push_back( '=' );
		block.append( value );
		block.push_back( '\0' );
	};

	addVar( "USERPROFILE", profile );
	addVar( "APPDATA", std::string( profile ) + "\\AppData\\Roaming" );
	addVar( "LOCALAPPDATA", std::string( profile ) + "\\AppData\\Local" );
	addVar( "TEMP", std::string( profile ) + "\\Temp" );
	addVar( "TMP", std::string( profile ) + "\\Temp" );

	block.push_back( '\0' ); // double-null terminator
	return block;
}

// Overload с proxy для per-account setup.
// ВАЖНО: НЕ добавляем HTTP_PROXY/HTTPS_PROXY/ALL_PROXY env vars. Если их
// выставить, Steam сам коннектится к прокси → наш ProxyHook ловит этот
// connect как обычный → пытается SOCKS5 handshake поверх него → double-proxy
// → REP=junk → Steam отваливается. Прокси должен быть невидим для Steam,
// работать ТОЛЬКО через захук Winsock в ProxyHook.dll.
std::string SteamLauncher::BuildEnvironmentBlock( int instanceIdx,
	const std::string& profilesDir, const std::string& /*proxy*/ )
{
	return BuildEnvironmentBlock( instanceIdx, profilesDir );
}

std::string SteamLauncher::BuildLaunchArgs( int idx, const FarmConfig& cfg )
{
	const AccountConfig* acc = ( idx < (int)cfg.accounts.size() )
		? &cfg.accounts[idx] : nullptr;
	std::string ipc = acc ? acc->ipcName : ( "steam" + std::to_string( idx ) );

	std::ostringstream ss;

	// Auto-login: `steam.exe -login <user> <pass>` работает ТОЛЬКО если:
	//   (1) Steam не запущен (наш kill в StartFarmThread это обеспечивает)
	//   (2) Steam Guard выключен
	//   (3) registry AutoLoginUser пустой/совпадает (иначе Steam берёт registry token)
	// `-silent` опускает Steam в tray, не открывая Chromium UI window — иначе
	// окно перекрывает GUI и user видит login form даже при успешном -login.
	if ( acc && !acc->login.empty() && !acc->password.empty() )
	{
		ss << "-login " << acc->login << " " << acc->password << " -silent ";
	}

	ss << "-master_ipc_name_override " << ipc;

	// Steam spawns dota2.exe через applaunch — это правильный way для VAC:
	// process tree = steam.exe → dota2.exe, env vars (SteamAppId, SteamGameId,
	// SteamOverlayGameId) установлены Steam'ом, overlay DLL injected автоматически.
	// Разделение 5 инстансов Dota по proxy делается через ProxyHook.dll inject
	// в каждый dota2.exe + per-PID config (C:\temp\andromeda\proxy_<pid>.json).
	// sing-box per-account matching работает ТОЛЬКО для Steam (BotSteam\N\ path);
	// для Dota все 5 инстансов имеют одинаковый path (D:\SteamLibrary\...\), и
	// разделение делает ProxyHook user-mode hook Winsock.
	ss << " -applaunch " << cfg.dotaAppId;

	// Core Dota args (passed through Steam to dota2.exe)
	ss << " -novid -nojoy -noaafonts";
	ss << " -w 640 -h 480 -sw";
	ss << " +fps_max 15";
	ss << " +r_drawparticles 0";
	ss << " +cl_globallight_shadow_mode 0";
	ss << " +dota_cheap_water 1";
	ss << " +r_deferred_height_fog 0";
	ss << " -console";

	// Minifier extras — добавляются если cfg.minifier.enabled И applyLaunchOptions.
	// LaunchOptions safe-path: VR/browser/intro disable + thread cap + map preload.
	// Резкое снижение startup RAM (~150 MB) и idle CPU.
	// Render backend: -dx11 (default), -vulkan (легче драйверы), -empty (null).
	// Раньше тут стоял -dx9 — он игнорировался движком (rendersystemdx9.dll нет
	// в установке), engine fallback'ил на DX11. Теперь явно выбираем backend.
	if ( cfg.minifier.enabled && cfg.minifier.applyLaunchOptions )
	{
		ss << " -novr -no-browser -map dota -threads 2";
		const std::string& backend = cfg.minifier.renderBackend;
		if ( backend == "vulkan" )       ss << " -vulkan";
		else if ( backend == "empty" )   ss << " -empty";
		else                              ss << " -dx11";
	}

	return ss.str();
}

std::string SteamLauncher::BuildDotaLaunchArgs( int idx, const FarmConfig& cfg )
{
	std::string ipc = ( idx < (int)cfg.accounts.size() )
		? cfg.accounts[idx].ipcName
		: "steam" + std::to_string( idx );

	std::ostringstream ss;
	// -steam говорит Source2 engine работать в режиме Steam client (читает IPC,
	// не запускает локальный Steam если его нет). -master_ipc_name_override даёт
	// Dota понять, к какому Steam IPC подключаться — тому же name что и у Steam.
	ss << "-steam -master_ipc_name_override " << ipc;
	ss << " -novid -nojoy -noaafonts";
	ss << " -w 640 -h 480 -sw";   // bordered windowed
	ss << " +fps_max 15";
	ss << " +r_drawparticles 0";
	ss << " +cl_globallight_shadow_mode 0";
	ss << " +dota_cheap_water 1";
	ss << " +r_deferred_height_fog 0";
	ss << " -console";

	// Minifier extras (см. BuildLaunchArgs выше).
	if ( cfg.minifier.enabled && cfg.minifier.applyLaunchOptions )
	{
		ss << " -novr -no-browser -map dota -threads 2";
		const std::string& backend = cfg.minifier.renderBackend;
		if ( backend == "vulkan" )       ss << " -vulkan";
		else if ( backend == "empty" )   ss << " -empty";
		else                              ss << " -dx11";
	}

	return ss.str();
}

DWORD SteamLauncher::LaunchDota( int instanceIdx, const FarmConfig& cfg, DWORD steamPid )
{
	// 1. Detect Dota install path из steamExe
	std::string dotaInstallDir = bot_dota_dir::FindDotaInstallDir( cfg.steamExe );
	if ( dotaInstallDir.empty() )
		return 0; // Dota not found

	// 2. Ensure BotDota\<idx>\ — hardlink dota2.exe + junctions для assets
	std::string botDota2Exe = bot_dota_dir::EnsureBotDotaDir( instanceIdx, dotaInstallDir );
	if ( botDota2Exe.empty() )
		return 0; // setup failed

	// 3. Build cmd line
	std::string args = BuildDotaLaunchArgs( instanceIdx, cfg );
	std::string cmdLine = "\"" + botDota2Exe + "\" " + args;

	// CWD для Dota — директория где лежит dota2.exe (botDir/game/bin/win64).
	std::string cwd = botDota2Exe;
	auto lastSlash = cwd.find_last_of( "\\/" );
	if ( lastSlash != std::string::npos )
		cwd = cwd.substr( 0, lastSlash );

	// Per-bot env (AppData redirect, как у Steam)
	std::string envBlock = BuildEnvironmentBlock( instanceIdx, cfg.profilesDir );

	PROCESS_INFORMATION pi{};
	DWORD creationFlags = 0;

	// VAC spoof: parent process = steam.exe. Без этого VAC показывает
	// "Античит Valve не может подтвердить надёжность устройства" и блокирует
	// matchmaking. Kernel EPROCESS tree должен показывать dota2 -> steam, не
	// dota2 -> orchestrator (DotaFarm.exe).
	HANDLE hSteam = nullptr;
	LPPROC_THREAD_ATTRIBUTE_LIST attrList = nullptr;
	STARTUPINFOEXA siEx{};
	STARTUPINFOA siPlain{};
	siPlain.cb = sizeof( siPlain );
	siEx.StartupInfo.cb = sizeof( siEx );

	bool useParentSpoof = ( steamPid != 0 );
	if ( useParentSpoof )
	{
		hSteam = OpenProcess(
			PROCESS_CREATE_PROCESS | PROCESS_QUERY_INFORMATION | PROCESS_DUP_HANDLE,
			FALSE, steamPid );
		if ( !hSteam )
		{
			// Fall back to обычный CreateProcess — Dota запустится, но с VAC warning.
			useParentSpoof = false;
		}
	}

	if ( useParentSpoof )
	{
		SIZE_T attrSize = 0;
		InitializeProcThreadAttributeList( nullptr, 1, 0, &attrSize );
		attrList = (LPPROC_THREAD_ATTRIBUTE_LIST)HeapAlloc(
			GetProcessHeap(), 0, attrSize );
		if ( !attrList ||
			 !InitializeProcThreadAttributeList( attrList, 1, 0, &attrSize ) ||
			 !UpdateProcThreadAttribute(
				attrList, 0, PROC_THREAD_ATTRIBUTE_PARENT_PROCESS,
				&hSteam, sizeof( HANDLE ), nullptr, nullptr ) )
		{
			if ( attrList )
			{
				DeleteProcThreadAttributeList( attrList );
				HeapFree( GetProcessHeap(), 0, attrList );
				attrList = nullptr;
			}
			CloseHandle( hSteam );
			hSteam = nullptr;
			useParentSpoof = false;
		}
		else
		{
			siEx.lpAttributeList = attrList;
			creationFlags |= EXTENDED_STARTUPINFO_PRESENT;
		}
	}

	LPSTARTUPINFOA lpSi = useParentSpoof
		? reinterpret_cast<LPSTARTUPINFOA>( &siEx )
		: &siPlain;

	BOOL ok = CreateProcessA(
		nullptr,
		const_cast<char*>( cmdLine.c_str() ),
		nullptr, nullptr, FALSE,
		creationFlags,
		envBlock.empty() ? nullptr : const_cast<char*>( envBlock.c_str() ),
		cwd.c_str(),
		lpSi, &pi
	);

	// Cleanup VAC spoof resources (handle closed early — не нужен после CreateProcess)
	if ( attrList )
	{
		DeleteProcThreadAttributeList( attrList );
		HeapFree( GetProcessHeap(), 0, attrList );
	}
	if ( hSteam )
		CloseHandle( hSteam );

	if ( !ok )
		return 0;

	DWORD pid = pi.dwProcessId;
	CloseHandle( pi.hProcess );
	CloseHandle( pi.hThread );
	return pid;
}

DWORD SteamLauncher::LaunchInstance( int instanceIdx, const FarmConfig& cfg )
{
	// 1. Set registry auto-login for this account
	SetAutoLogin( instanceIdx, cfg );

	// 2. Get the right Steam exe (per-bot or main)
	auto [steamExe, steamDir] = GetSteamExeFor( instanceIdx, cfg );

	// 3. Build command line
	std::string args = BuildLaunchArgs( instanceIdx, cfg );
	std::string cmdLine = "\"" + steamExe + "\" " + args;

	// Диагностический лог — видно что реально передано в CreateProcess.
	// Маскируем password чтобы в логе не появлялся plaintext.
	{
		std::string safeCmdLine = cmdLine;
		const std::string marker = "-login ";
		size_t p = safeCmdLine.find( marker );
		if ( p != std::string::npos )
		{
			size_t userStart = p + marker.size();
			size_t userEnd = safeCmdLine.find( ' ', userStart );
			if ( userEnd != std::string::npos )
			{
				size_t passStart = userEnd + 1;
				size_t passEnd = safeCmdLine.find( ' ', passStart );
				if ( passEnd == std::string::npos ) passEnd = safeCmdLine.size();
				safeCmdLine.replace( passStart, passEnd - passStart, "***" );
			}
		}

		FILE* f = fopen( "C:\\temp\\andromeda\\steam_launch.log", "a" );
		if ( f )
		{
			SYSTEMTIME st; GetLocalTime( &st );
			fprintf( f, "[%04d-%02d-%02d %02d:%02d:%02d] #%d cmdLine: %s\n",
				st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
				instanceIdx, safeCmdLine.c_str() );
			fclose( f );
		}
	}

	// 4. Build env block with AppData redirect (+ proxy если указан)
	uint64_t steamId = ( instanceIdx < (int)cfg.accounts.size() )
		? cfg.accounts[instanceIdx].steamId : 0;
	// Per-account toggles: если выключены — обнуляем соответствующие поля
	// для вычислений ниже (proxy="" и hwid disabled). Настройки не трогаем.
	bool accProxyOn = ( instanceIdx < (int)cfg.accounts.size() )
		? cfg.accounts[instanceIdx].proxyEnabled : true;
	bool accHwidOn  = ( instanceIdx < (int)cfg.accounts.size() )
		? cfg.accounts[instanceIdx].hwidSpoofEnabled : true;
	std::string proxy = ( instanceIdx < (int)cfg.accounts.size() && accProxyOn )
		? cfg.accounts[instanceIdx].proxy : std::string();
	std::string envBlock = BuildEnvironmentBlock( instanceIdx, cfg.profilesDir, proxy );

	// ── Решаем надо ли делать CREATE_SUSPENDED + ProxyHook.dll inject ──
	// ProxyHook выполняет ДВЕ функции:
	//   1. Proxy redirect (Winsock SOCKS5) — только если proxy задан И НЕ useTun2Socks
	//   2. Per-process HWID spoof — если hwidSpoofEnabled=true
	// Inject нужен если хотя бы одна из них активна.
	// Per-account proxyEnabled уже применён через обнуление `proxy` выше.
	bool proxyViaHook = !proxy.empty() && !cfg.useTun2Socks;
	bool hwidViaHook  = cfg.IsSteamSpoofEnabled() && accHwidOn && steamId != 0;
	bool dllAvail = !cfg.proxyHookDllPath.empty() &&
		GetFileAttributesA( cfg.proxyHookDllPath.c_str() ) != INVALID_FILE_ATTRIBUTES;
	bool useProxyHook = ( proxyViaHook || hwidViaHook ) && dllAvail;

	// 5. Create process (NO CREATE_UNICODE_ENVIRONMENT — block is ANSI)
	STARTUPINFOA si{};
	si.cb = sizeof( si );
	PROCESS_INFORMATION pi{};

	DWORD flags = 0;
	if ( useProxyHook )
		flags |= CREATE_SUSPENDED;

	BOOL ok = CreateProcessA(
		nullptr,
		const_cast<char*>( cmdLine.c_str() ),
		nullptr, nullptr, FALSE,
		flags,
		envBlock.empty() ? nullptr : const_cast<char*>( envBlock.c_str() ),
		steamDir.c_str(),
		&si, &pi
	);

	if ( !ok )
		return 0;

	DWORD pid = pi.dwProcessId;

	if ( useProxyHook )
	{
		Injector inj;
		inj.Init();

		// Proxy передаём только если proxy-hook активен (не useTun2Socks).
		std::string proxyForDll = proxyViaHook ? proxy : std::string();
		std::string hwidSeed = hwidViaHook ? BuildHwidSeed( steamId ) : std::string();

		bool cfgOk  = Injector::WriteProxyConfig( pid, proxyForDll, hwidSeed );
		bool injOk  = inj.InjectLoadLibrary( pid, cfg.proxyHookDllPath );

		if ( !cfgOk || !injOk )
		{
			// Inject провалился — решение: убиваем процесс (лучше чем
			// ходить без прокси/HWID и словить detection).
			TerminateProcess( pi.hProcess, 1 );
			CloseHandle( pi.hProcess );
			CloseHandle( pi.hThread );
			return 0;
		}

		ResumeThread( pi.hThread );
	}

	CloseHandle( pi.hProcess );
	CloseHandle( pi.hThread );
	return pid;
}

// Helper: create NTFS junction (mklink /J) — works without special privileges
static bool CreateJunction( const char* linkDir, const char* targetDir )
{
	char cmd[512];
	snprintf( cmd, sizeof( cmd ), "cmd.exe /c mklink /J \"%s\" \"%s\"", linkDir, targetDir );

	STARTUPINFOA si{};
	si.cb = sizeof( si );
	si.dwFlags = STARTF_USESHOWWINDOW;
	si.wShowWindow = SW_HIDE;
	PROCESS_INFORMATION pi{};

	if ( !CreateProcessA( nullptr, cmd, nullptr, nullptr,
		FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi ) )
		return false;

	WaitForSingleObject( pi.hProcess, 10000 );
	DWORD exitCode = 1;
	GetExitCodeProcess( pi.hProcess, &exitCode );
	CloseHandle( pi.hProcess );
	CloseHandle( pi.hThread );
	return exitCode == 0;
}

// Helper: same-volume check для hardlink. Hardlink works только если source
// и target лежат на одном томе (NTFS ограничение — hardlink = второй
// directory entry на тот же MFT record).
static bool SameVolume( const char* pathA, const char* pathB )
{
	char volA[MAX_PATH] = {};
	char volB[MAX_PATH] = {};
	if ( !GetVolumePathNameA( pathA, volA, MAX_PATH ) ) return false;
	if ( !GetVolumePathNameA( pathB, volB, MAX_PATH ) ) return false;
	return _stricmp( volA, volB ) == 0;
}

// Create hardlink or fall back to copy if on different volume.
// Returns true if link/copy was made. If dst already exists — no-op returns true.
static bool HardLinkOrCopy( const char* dst, const char* src )
{
	if ( GetFileAttributesA( dst ) != INVALID_FILE_ATTRIBUTES )
		return true;

	if ( SameVolume( dst, src ) )
	{
		// Hardlink — не требует admin privileges, NTFS работает прозрачно.
		// Process path резолв у Windows через directory entry → dst, а не src:
		// kernel EPROCESS->SeAuditProcessCreationInfo хранит путь из CreateProcess.
		if ( CreateHardLinkA( dst, src, nullptr ) )
			return true;
		// Если не удалось — падаем в копирование ниже как safety.
	}

	// Cross-volume или hardlink fail — копируем.
	// ~7 MB steam.exe × 5 bots = 35 MB disk — acceptable.
	return CopyFileA( src, dst, TRUE ) != FALSE;
}

// Recursive hardlink: real directories (mkdir), files as hardlinks. КРИТИЧНО:
// junction'и резолвятся NT kernel'ом при CreateFile → process path = target.
// Поэтому BotSteam\N\bin\... — если bin\ был junction'ом, steamwebhelper.exe
// процесс имеет path=real install, sing-box не матчит. Recursive hardlinks
// решают проблему — каждая директория real, path preserved end-to-end.
static void RecursiveHardLink( const std::string& srcBase, const std::string& dstBase )
{
	CreateDirectoryA( dstBase.c_str(), nullptr );

	std::string search = srcBase + "\\*";
	WIN32_FIND_DATAA fd;
	HANDLE hFind = FindFirstFileA( search.c_str(), &fd );
	if ( hFind == INVALID_HANDLE_VALUE ) return;

	do
	{
		if ( fd.cFileName[0] == '.' ) continue;
		std::string childSrc = srcBase + "\\" + fd.cFileName;
		std::string childDst = dstBase + "\\" + fd.cFileName;

		if ( fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY )
		{
			// Пропускаем reparse points (если Steam install содержит symlinks сам)
			if ( fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT )
				continue;
			RecursiveHardLink( childSrc, childDst );
		}
		else
		{
			// Очищаем stale reparse point если был
			DWORD dstAttrs = GetFileAttributesA( childDst.c_str() );
			if ( dstAttrs != INVALID_FILE_ATTRIBUTES &&
				( dstAttrs & FILE_ATTRIBUTE_REPARSE_POINT ) )
			{
				DeleteFileA( childDst.c_str() );
			}
			HardLinkOrCopy( childDst.c_str(), childSrc.c_str() );
		}
	}
	while ( FindNextFileA( hFind, &fd ) );
	FindClose( hFind );
}

bool SteamLauncher::EnsureBotSteamDir( int idx, const std::string& realSteamDir )
{
	char botDir[MAX_PATH];
	snprintf( botDir, sizeof( botDir ), "%s\\%d", BOT_STEAM_DIR, idx );

	// Already exists? Check steam.exe is valid (can actually be opened, not broken symlink/missing)
	char botExe[MAX_PATH];
	snprintf( botExe, sizeof( botExe ), "%s\\steam.exe", botDir );
	{
		HANDLE hTest = CreateFileA( botExe, GENERIC_READ, FILE_SHARE_READ,
			nullptr, OPEN_EXISTING, 0, nullptr );
		if ( hTest != INVALID_HANDLE_VALUE )
		{
			// Check by file identity — hardlink on same volume == real steam.exe.
			// If steam.exe is a SYMLINK from old BotSteam layout, QueryFullProcessImageName
			// resolves it to the target, ломая sing-box matching. Надо переделать.
			BY_HANDLE_FILE_INFORMATION botInfo{};
			GetFileInformationByHandle( hTest, &botInfo );
			CloseHandle( hTest );

			// Detect stale symlink: FILE_ATTRIBUTE_REPARSE_POINT
			DWORD attrs = GetFileAttributesA( botExe );
			bool isReparse = ( attrs != INVALID_FILE_ATTRIBUTES ) &&
				( attrs & FILE_ATTRIBUTE_REPARSE_POINT );
			if ( !isReparse )
				return true; // already a real file/hardlink — ok
			// Else — old symlink, rebuild.
		}
		// Broken or missing — recreate
		DeleteFileA( botExe );
	}

	// Create base dirs
	CreateDirectoryA( BOT_STEAM_DIR, nullptr );
	CreateDirectoryA( botDir, nullptr );

	// Dirs to create as REAL (empty) — Steam writes per-instance data here
	const char* perInstanceDirs[] = { "config", "dumps", "logs", "depotcache",
		"appcache", "userdata", "package" };
	for ( auto d : perInstanceDirs )
	{
		char path[MAX_PATH];
		snprintf( path, sizeof( path ), "%s\\%s", botDir, d );
		CreateDirectoryA( path, nullptr );
	}

	// steamapps/ — large game installs, safe to junction (game binaries не
	// запускаются напрямую через этот путь; мы LaunchDota'им из BotDota отдельно).
	const char* junctionDirs[] = { "steamapps" };
	for ( auto d : junctionDirs )
	{
		std::string src = realSteamDir + "\\" + d;
		std::string dst = std::string( botDir ) + "\\" + d;
		if ( GetFileAttributesA( src.c_str() ) != INVALID_FILE_ATTRIBUTES &&
			 GetFileAttributesA( dst.c_str() ) == INVALID_FILE_ATTRIBUTES )
		{
			CreateJunction( dst.c_str(), src.c_str() );
		}
	}

	// Top-level files → hardlink (steam.exe + *.dll + *.vdf + прочее).
	WIN32_FIND_DATAA fd;
	std::string search = realSteamDir + "\\*";
	HANDLE hFind = FindFirstFileA( search.c_str(), &fd );
	if ( hFind != INVALID_HANDLE_VALUE )
	{
		do
		{
			if ( fd.cFileName[0] == '.' ) continue;
			if ( fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY ) continue;

			std::string srcPath = realSteamDir + "\\" + fd.cFileName;
			std::string dstPath = std::string( botDir ) + "\\" + fd.cFileName;

			DWORD dstAttrs = GetFileAttributesA( dstPath.c_str() );
			if ( dstAttrs != INVALID_FILE_ATTRIBUTES &&
				( dstAttrs & FILE_ATTRIBUTE_REPARSE_POINT ) )
			{
				DeleteFileA( dstPath.c_str() );
			}
			HardLinkOrCopy( dstPath.c_str(), srcPath.c_str() );
		}
		while ( FindNextFileA( hFind, &fd ) );
		FindClose( hFind );
	}

	// Top-level directories → RECURSIVE HARDLINK (не junction!).
	// Причина: если bin\ это junction, то CreateFile через C:\BotSteam\N\bin\...
	// резолвит reparse в kernel'е → FILE_OBJECT.FileName = target path → process
	// path у steamwebhelper.exe, gameoverlayui.exe и прочих child процессов
	// будет "C:\Program Files (x86)\Steam\...", sing-box НЕ матчит. Recursive
	// hardlink даёт 0 disk overhead (same MFT records) и реальные пути.
	hFind = FindFirstFileA( search.c_str(), &fd );
	if ( hFind != INVALID_HANDLE_VALUE )
	{
		do
		{
			if ( fd.cFileName[0] == '.' ) continue;
			if ( !( fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY ) ) continue;

			std::string name = fd.cFileName;
			std::string dstPath = std::string( botDir ) + "\\" + name;

			// Skip perInstanceDirs (already created empty)
			bool skip = false;
			for ( auto d : perInstanceDirs )
				if ( _stricmp( name.c_str(), d ) == 0 ) { skip = true; break; }
			// Skip junctionDirs (already created as junction)
			for ( auto d : junctionDirs )
				if ( _stricmp( name.c_str(), d ) == 0 ) { skip = true; break; }
			if ( skip ) continue;

			// If dst exists as reparse from old layout — remove and rebuild
			DWORD dstAttrs = GetFileAttributesA( dstPath.c_str() );
			if ( dstAttrs != INVALID_FILE_ATTRIBUTES &&
				( dstAttrs & FILE_ATTRIBUTE_REPARSE_POINT ) )
			{
				// Can't simply DeleteFileA on a junction — use RemoveDirectoryA
				RemoveDirectoryA( dstPath.c_str() );
			}

			std::string srcPath = realSteamDir + "\\" + name;
			RecursiveHardLink( srcPath, dstPath );
		}
		while ( FindNextFileA( hFind, &fd ) );
		FindClose( hFind );
	}

	// Copy essential config files from real Steam (NOT loginusers.vdf — that's per-bot)
	// libraryfolders.vdf is critical — tells Steam where games are installed
	{
		const char* configFiles[] = { "libraryfolders.vdf", "config.vdf" };
		for ( auto cfgFile : configFiles )
		{
			char src[MAX_PATH], dst[MAX_PATH];
			snprintf( src, sizeof( src ), "%s\\config\\%s", realSteamDir.c_str(), cfgFile );
			snprintf( dst, sizeof( dst ), "%s\\config\\%s", botDir, cfgFile );
			if ( GetFileAttributesA( dst ) == INVALID_FILE_ATTRIBUTES &&
				GetFileAttributesA( src ) != INVALID_FILE_ATTRIBUTES )
			{
				CopyFileA( src, dst, TRUE );
			}
		}
	}

	// Steam.cfg — block auto-updates (critical for BotSteam copies).
	// Должен быть REAL file, не hardlink — удаляем возможный hardlink и пишем.
	{
		char cfgPath[MAX_PATH];
		snprintf( cfgPath, sizeof( cfgPath ), "%s\\Steam.cfg", botDir );
		DeleteFileA( cfgPath );
		FILE* f = fopen( cfgPath, "w" );
		if ( f )
		{
			fputs( "BootStrapperInhibitAll=enable\n"
				   "BootStrapperForceSelfUpdate=disable\n", f );
			fclose( f );
		}
	}

	return GetFileAttributesA( botExe ) != INVALID_FILE_ATTRIBUTES;
}

std::string SteamLauncher::DetectSteamExe()
{
	// 1. Try HKCU SteamExe (most common, but BotSteam may overwrite it)
	{
		HKEY hKey = nullptr;
		if ( RegOpenKeyExA( HKEY_CURRENT_USER, "Software\\Valve\\Steam",
			0, KEY_READ, &hKey ) == ERROR_SUCCESS )
		{
			char buf[MAX_PATH] = {};
			DWORD sz = MAX_PATH;
			if ( RegQueryValueExA( hKey, "SteamExe", nullptr, nullptr,
				(BYTE*)buf, &sz ) == ERROR_SUCCESS )
			{
				RegCloseKey( hKey );
				// Normalize forward slashes
				for ( char* p = buf; *p; p++ )
					if ( *p == '/' ) *p = '\\';

				// Skip if it's a BotSteam path
				std::string path = buf;
				if ( path.find( "BotSteam" ) == std::string::npos &&
					GetFileAttributesA( path.c_str() ) != INVALID_FILE_ATTRIBUTES )
					return path;
			}
			else
			{
				RegCloseKey( hKey );
			}
		}
	}

	// 2. Try HKLM InstallPath
	{
		const char* keys[] = {
			"Software\\Valve\\Steam",
			"Software\\WOW6432Node\\Valve\\Steam"
		};
		for ( auto regKey : keys )
		{
			HKEY hKey = nullptr;
			if ( RegOpenKeyExA( HKEY_LOCAL_MACHINE, regKey,
				0, KEY_READ, &hKey ) == ERROR_SUCCESS )
			{
				char buf[MAX_PATH] = {};
				DWORD sz = MAX_PATH;
				if ( RegQueryValueExA( hKey, "InstallPath", nullptr, nullptr,
					(BYTE*)buf, &sz ) == ERROR_SUCCESS )
				{
					RegCloseKey( hKey );
					std::string path = std::string( buf ) + "\\steam.exe";
					if ( path.find( "BotSteam" ) == std::string::npos &&
						GetFileAttributesA( path.c_str() ) != INVALID_FILE_ATTRIBUTES )
						return path;
				}
				else
				{
					RegCloseKey( hKey );
				}
			}
		}
	}

	// 3. Common paths
	const char* candidates[] = {
		"C:\\Program Files (x86)\\Steam\\steam.exe",
		"C:\\Program Files\\Steam\\steam.exe",
		"D:\\Steam\\steam.exe",
		"D:\\Program Files (x86)\\Steam\\steam.exe",
		"E:\\Steam\\steam.exe",
	};
	for ( auto path : candidates )
	{
		if ( GetFileAttributesA( path ) != INVALID_FILE_ATTRIBUTES )
			return path;
	}

	// 4. Fallback
	return "C:\\Program Files (x86)\\Steam\\steam.exe";
}

void SteamLauncher::KillAllSteam()
{
	// Kill all steam.exe
	HANDLE snap = CreateToolhelp32Snapshot( TH32CS_SNAPPROCESS, 0 );
	if ( snap == INVALID_HANDLE_VALUE ) return;

	PROCESSENTRY32 pe{};
	pe.dwSize = sizeof( pe );

	if ( Process32First( snap, &pe ) )
	{
		do
		{
			if ( _stricmp( pe.szExeFile, "steam.exe" ) == 0 ||
				 _stricmp( pe.szExeFile, "steamwebhelper.exe" ) == 0 )
			{
				HANDLE h = OpenProcess( PROCESS_TERMINATE, FALSE, pe.th32ProcessID );
				if ( h )
				{
					TerminateProcess( h, 0 );
					CloseHandle( h );
				}
			}
		}
		while ( Process32Next( snap, &pe ) );
	}
	CloseHandle( snap );
}

void SteamLauncher::KillSteamForInstance( int idx, DWORD steamPid )
{
	// Способ 1: точечное убийство по pid (если он передан). Это корневой
	// steam.exe данной инстанции — убийство каскадом снимает связанные
	// steamwebhelper'ы (Steam перезапускает их сам, но при kill родителя
	// они тоже завершаются). Wait до 5s чтобы lock'и в C:\BotSteam\<idx>\
	// успели освободиться до следующего LaunchInstance.
	if ( steamPid )
	{
		HANDLE h = OpenProcess(
			PROCESS_TERMINATE | SYNCHRONIZE, FALSE, steamPid );
		if ( h )
		{
			TerminateProcess( h, 0 );
			WaitForSingleObject( h, 5000 );
			CloseHandle( h );
		}
	}

	// Способ 2 (fallback и cleanup): найти всех steam*.exe чей image path
	// лежит внутри C:\BotSteam\<idx>\ и убить — на случай если зомби
	// steamwebhelper'ы остались после kill корневого процесса, или steamPid
	// был 0 (не успели засечь). Tasks #13: задача гарантирует чистый slot
	// перед следующим LaunchInstance.
	char botPrefix[MAX_PATH];
	snprintf( botPrefix, sizeof( botPrefix ), "C:\\BotSteam\\%d\\", idx );
	size_t prefixLen = strlen( botPrefix );

	HANDLE snap = CreateToolhelp32Snapshot( TH32CS_SNAPPROCESS, 0 );
	if ( snap == INVALID_HANDLE_VALUE ) return;

	PROCESSENTRY32 pe{};
	pe.dwSize = sizeof( pe );

	if ( Process32First( snap, &pe ) )
	{
		do
		{
			if ( _stricmp( pe.szExeFile, "steam.exe" ) != 0 &&
			     _stricmp( pe.szExeFile, "steamwebhelper.exe" ) != 0 )
				continue;

			HANDLE hProc = OpenProcess(
				PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE | SYNCHRONIZE,
				FALSE, pe.th32ProcessID );
			if ( !hProc ) continue;

			char imgPath[MAX_PATH] = {};
			DWORD n = (DWORD)sizeof( imgPath );
			if ( QueryFullProcessImageNameA( hProc, 0, imgPath, &n ) &&
			     _strnicmp( imgPath, botPrefix, prefixLen ) == 0 )
			{
				TerminateProcess( hProc, 0 );
				WaitForSingleObject( hProc, 3000 );
			}
			CloseHandle( hProc );
		}
		while ( Process32Next( snap, &pe ) );
	}
	CloseHandle( snap );
}
