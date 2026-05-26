#include "gui.h"
#include "config.h"
#include "injector.h"
#include "theme.h"
#include "hero_portraits.h"
#include "window_tiler.h"
#include "mem_reclaim.h"
#include "dota_launcher.h"
#include "crypto/sealed_file.h"
#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_impl_win32.h>
#include <imgui_impl_dx11.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <thread>
#include <commdlg.h>  // GetOpenFileNameA

#pragma comment(lib, "comdlg32.lib")

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dwmapi.lib")

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler( HWND, UINT, WPARAM, LPARAM );

namespace gui
{

// ── Global pause (shared with DLL via C:\temp\andromeda\pause.flag) ──

static constexpr const char* kPauseFlagPath = "C:\\temp\\andromeda\\pause.flag";

static bool IsPauseFlagSet()
{
	return GetFileAttributesA( kPauseFlagPath ) != INVALID_FILE_ATTRIBUTES;
}

static void SetPauseFlag( bool paused )
{
	if ( paused )
	{
		CreateDirectoryA( "C:\\temp", nullptr );
		CreateDirectoryA( "C:\\temp\\andromeda", nullptr );
		HANDLE h = CreateFileA( kPauseFlagPath, GENERIC_WRITE, 0, nullptr,
			CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr );
		if ( h != INVALID_HANDLE_VALUE )
			CloseHandle( h );
	}
	else
	{
		DeleteFileA( kPauseFlagPath );
	}
}

// Собрать dotaPid'ы всех живых ботов (для tiler).
static std::vector<DWORD> CollectDotaPids( Orchestrator& orch )
{
	std::vector<DWORD> pids;
	for ( int i = 0; i < orch.GetBotCount(); i++ )
	{
		const auto& b = orch.GetBot( i );
		if ( b.dotaPid ) pids.push_back( b.dotaPid );
	}
	return pids;
}

// Применить tile с настройками из cfg.tile* и залогировать результат.
static int TileNow( Orchestrator& orch )
{
	const auto& cfg = orch.GetConfig();
	auto pids = CollectDotaPids( orch );
	window_tiler::TileOptions opt;
	opt.layout         = window_tiler::LayoutFromString( cfg.tileLayout );
	opt.forceWindowed  = cfg.tileForceWindowed;
	opt.padding        = cfg.tilePadding;
	opt.monitorIndex   = cfg.tileMonitorIndex;
	int n = window_tiler::ApplyTile( pids, opt );
	char msg[160];
	snprintf( msg, sizeof( msg ), "Window tile: layout=%s applied to %d/%zu window(s)",
		window_tiler::LayoutName( opt.layout ), n, pids.size() );
	orch.LogPublic( msg );
	return n;
}

// Trigger Lua hot-reload на всех live ботах — per-PID flag, DLL читает только свой.
// Возвращает количество ботов, которым отправлен флаг.
static int TriggerReloadAll( Orchestrator& orch )
{
	CreateDirectoryA( "C:\\temp", nullptr );
	CreateDirectoryA( "C:\\temp\\andromeda", nullptr );

	int sent = 0;
	for ( int i = 0; i < orch.GetBotCount(); i++ )
	{
		const auto& b = orch.GetBot( i );
		if ( !b.dotaPid ) continue;

		char path[MAX_PATH];
		snprintf( path, sizeof( path ),
			"C:\\temp\\andromeda\\reload_%lu.flag", b.dotaPid );
		HANDLE h = CreateFileA( path, GENERIC_WRITE, 0, nullptr,
			CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr );
		if ( h != INVALID_HANDLE_VALUE )
		{
			CloseHandle( h );
			sent++;
		}
	}
	return sent;
}

// ── Key persistence (AES-256-GCM sealed, HWID-derived key) ──
//
// license.dat layout: "DFRM" magic + IV(12) + tag(16) + ciphertext.
// Legacy plain-text license.dat is auto-migrated on first read.

static std::string TrimLicenseKey( std::string key )
{
	size_t b = key.find_first_not_of( " \t\r\n" );
	if ( b == std::string::npos ) return {};
	key.erase( 0, b );
	size_t e = key.find_last_not_of( " \t\r\n" );
	if ( e != std::string::npos ) key.erase( e + 1 );
	return key;
}

std::string LoadSavedKey( const std::string& exeDir )
{
	std::string path = exeDir + "\\license.dat";
	std::string raw;
	bool hadMagic = false;
	bool ok = sealed_file::UnsealFileToString( path, raw, hadMagic );

	if ( ok )
		return TrimLicenseKey( std::move( raw ) );

	// Migration path: file existed but had no magic — legacy plain license key.
	// Re-seal under current HWID so next start is encrypted.
	if ( !hadMagic && !raw.empty() )
	{
		std::string key = TrimLicenseKey( std::move( raw ) );
		if ( !key.empty() )
		{
			sealed_file::SealFile( path, key );
			return key;
		}
		return {};
	}

	// hadMagic && !ok ⇒ HWID changed or file corrupted ⇒ fail loud (empty).
	return {};
}

void SaveKey( const std::string& exeDir, const std::string& key )
{
	sealed_file::SealFile( exeDir + "\\license.dat", key );
}

// LEGACY_PLAIN: removed plain-text fstream form of SaveKey/LoadSavedKey.
// Kept for reference in git history. Migration handled inline above.

// ── D3D / Window ──

static LRESULT CALLBACK WndProc( HWND hwnd, UINT msg, WPARAM wp, LPARAM lp )
{
	if ( ImGui_ImplWin32_WndProcHandler( hwnd, msg, wp, lp ) )
		return true;
	if ( msg == WM_SIZE && g_pd3dDevice && wp != SIZE_MINIMIZED )
	{
		if ( g_pRTV ) { g_pRTV->Release(); g_pRTV = nullptr; }
		g_pSwapChain->ResizeBuffers( 0, (UINT)LOWORD( lp ), (UINT)HIWORD( lp ), DXGI_FORMAT_UNKNOWN, 0 );
		ID3D11Texture2D* bb = nullptr;
		g_pSwapChain->GetBuffer( 0, IID_PPV_ARGS( &bb ) );
		g_pd3dDevice->CreateRenderTargetView( bb, nullptr, &g_pRTV );
		bb->Release();
		return 0;
	}
	if ( msg == WM_DESTROY ) { PostQuitMessage( 0 ); return 0; }
	return DefWindowProcW( hwnd, msg, wp, lp );
}

bool Init( int width, int height )
{
	WNDCLASSEXW wc{};
	wc.cbSize = sizeof( wc );
	wc.style = CS_CLASSDC;
	wc.lpfnWndProc = WndProc;
	wc.hInstance = GetModuleHandleW( nullptr );
	wc.lpszClassName = L"DotaFarmOrchestrator";
	RegisterClassExW( &wc );

	g_hwnd = CreateWindowExW( 0, wc.lpszClassName, L"DOTAFARM — ORCHESTRATOR CONSOLE",
		WS_OVERLAPPEDWINDOW, 100, 100, width, height, nullptr, nullptr, wc.hInstance, nullptr );

	DXGI_SWAP_CHAIN_DESC sd{};
	sd.BufferCount = 2;
	sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	sd.BufferDesc.RefreshRate = { 60, 1 };
	sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
	sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	sd.OutputWindow = g_hwnd;
	sd.SampleDesc = { 1, 0 };
	sd.Windowed = TRUE;
	sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

	D3D_FEATURE_LEVEL fl, levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
	if ( FAILED( D3D11CreateDeviceAndSwapChain( nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
		levels, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain, &g_pd3dDevice, &fl, &g_pd3dCtx ) ) )
		return false;

	ID3D11Texture2D* bb = nullptr;
	g_pSwapChain->GetBuffer( 0, IID_PPV_ARGS( &bb ) );
	g_pd3dDevice->CreateRenderTargetView( bb, nullptr, &g_pRTV );
	bb->Release();

	ShowWindow( g_hwnd, SW_SHOWDEFAULT );
	UpdateWindow( g_hwnd );

	ImGui::CreateContext();
	auto& io = ImGui::GetIO();
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

	// Tarnished-gold industrial theme — see theme.h
	if ( !theme::LoadDotaFarmFonts( io ) )
	{
		// Should not happen on Windows (Segoe/Consolas/Georgia are stock).
		io.Fonts->AddFontDefault();
	}
	theme::ApplyDotaFarmTheme();
	g_fontBold = theme::gFontSerifLg;  // back-compat handle

	ImGui_ImplWin32_Init( g_hwnd );
	ImGui_ImplDX11_Init( g_pd3dDevice, g_pd3dCtx );
	return true;
}

// ── Helpers ──

static const char* ShortHero( const char* f )
{
	const char* p = "npc_dota_hero_";
	return ( strncmp( f, p, 14 ) == 0 ) ? f + 14 : f;
}

static ImVec4 StateColor( const char* st )
{
	if ( strstr( st, "GAME" ) )    return { 0.2f, 0.8f, 0.2f, 1.f };
	if ( strstr( st, "QUEUING" ) ) return { 1.0f, 0.8f, 0.2f, 1.f };
	if ( strstr( st, "FORMING" ) ) return { 0.4f, 0.7f, 1.0f, 1.f };
	if ( strstr( st, "HERO" ) )    return { 0.8f, 0.4f, 1.0f, 1.f };
	if ( strstr( st, "MATCH" ) )   return { 1.0f, 0.5f, 0.0f, 1.f };
	if ( strstr( st, "CRASH" ) )   return { 1.0f, 0.2f, 0.2f, 1.f };
	if ( strstr( st, "STOP" ) )    return { 0.5f, 0.5f, 0.5f, 1.f };
	return { 0.7f, 0.7f, 0.7f, 1.f };
}

// ═══════════════════════════════════════════════════════════
// SCREEN 1: Login
// ═══════════════════════════════════════════════════════════

static char s_keyBuf[64] = {};
static std::string s_authError;

static void RenderLogin()
{
	auto ds = ImGui::GetIO().DisplaySize;

	// Atmospheric backdrop on the FULL screen — dotted grid + soft scanlines.
	{
		ImDrawList* bdl = ImGui::GetBackgroundDrawList();
		theme::DrawDottedGrid( bdl, ImVec2( 0, 0 ), ds,
			IM_COL32( 0xd4, 0xa1, 0x4a, 0x14 ), 28.f );
		theme::DrawScanlines( bdl, ImVec2( 0, 0 ), ds,
			IM_COL32( 0xd4, 0xa1, 0x4a, 0x05 ), 3 );
	}

	float w = 460, h = 280;
	ImGui::SetNextWindowPos( { ( ds.x - w ) / 2, ( ds.y - h ) / 2 } );
	ImGui::SetNextWindowSize( { w, h } );
	ImGui::PushStyleColor( ImGuiCol_WindowBg, theme::V( theme::kColBg1 ) );
	ImGui::PushStyleColor( ImGuiCol_Border,    theme::V( theme::kColLineHot ) );
	ImGui::Begin( "##login", nullptr,
		ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
		ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse );

	// Engineering corner brackets around the login panel.
	{
		ImVec2 mn = ImGui::GetWindowPos();
		ImVec2 mx = ImVec2( mn.x + w, mn.y + h );
		theme::DrawCornerBrackets( ImGui::GetWindowDrawList(), mn, mx, 12.f, 1.f, theme::kColGold );
	}

	// Wordmark — DOTA·FARM serif
	if ( theme::gFontSerifLg ) ImGui::PushFont( theme::gFontSerifLg );
	const char* mark1 = "DOTA";
	const char* mark2 = "FARM";
	float w1 = ImGui::CalcTextSize( mark1 ).x;
	float w2 = ImGui::CalcTextSize( mark2 ).x;
	float total = w1 + w2;
	ImGui::SetCursorPos( ImVec2( ( w - total ) / 2, 24 ) );
	ImGui::TextColored( theme::V( theme::kColInkBright ), "%s", mark1 );
	ImGui::SameLine( 0, 0 );
	ImGui::TextColored( theme::V( theme::kColGold ),      "%s", mark2 );
	if ( theme::gFontSerifLg ) ImGui::PopFont();

	if ( theme::gFontMonoSm ) ImGui::PushFont( theme::gFontMonoSm );
	const char* sub = "MULTI-ACCOUNT  ORCHESTRATOR  CONSOLE";
	float subW = ImGui::CalcTextSize( sub ).x;
	ImGui::SetCursorPosX( ( w - subW ) / 2 );
	ImGui::TextColored( theme::V( theme::kColInkMute ), "%s", sub );
	if ( theme::gFontMonoSm ) ImGui::PopFont();

	ImGui::Dummy( ImVec2( 0, 14 ) );

	// Centered key block
	float fieldW = w - 60;
	ImGui::SetCursorPosX( 30 );
	ImGui::TextColored( theme::V( theme::kColInkMute ), "[--  LICENSE  KEY  ----------------------------]" );
	ImGui::SetCursorPosX( 30 );
	ImGui::SetNextItemWidth( fieldW );
	bool enter = ImGui::InputText( "##key", s_keyBuf, sizeof( s_keyBuf ), ImGuiInputTextFlags_EnterReturnsTrue );

	ImGui::Dummy( ImVec2( 0, 6 ) );

	if ( !s_authError.empty() )
	{
		ImGui::SetCursorPosX( 30 );
		ImGui::TextColored( theme::V( theme::kColCrash ), "× %s", s_authError.c_str() );
		ImGui::Dummy( ImVec2( 0, 4 ) );
	}

	ImGui::SetCursorPosX( 30 );
	bool clicked = theme::ChamferedButton( "ACTIVATE", ImVec2( fieldW, 36 ),
		theme::kColGold, theme::kColGold, theme::kColBg0, true );
	if ( ( clicked || enter ) && strlen( s_keyBuf ) > 0 )
	{
		g_licenseKey = s_keyBuf;
		g_authResult = auth::Authenticate( g_licenseKey );
		if ( g_authResult.allowed )
		{
			g_authenticated = true;
			SaveKey( g_exeDir, g_licenseKey );
			s_authError.clear();
		}
		else
		{
			s_authError = g_authResult.message.empty() ? "Authentication failed" : g_authResult.message;
		}
	}

	ImGui::End();
	ImGui::PopStyleColor( 2 );
}

// ═══════════════════════════════════════════════════════════
// SCREEN 2: Account Setup — add logins + Steam login wizard
// ═══════════════════════════════════════════════════════════

static int   s_setupStep = 0;
// s_setupSteamPid и s_setupRequestLaunch читаются из polling ветки и пишутся
// в launch ветке в пределах одного render frame. Torn read на 32-bit DWORD
// на x86-64 формально невозможен (aligned), но компилятор может держать
// значение в регистре между чтениями — atomic гарантирует барьеры и видимость
// записей, сделанных другой путью в том же кадре.
static std::atomic<DWORD> s_setupSteamPid{ 0 };
static bool  s_setupDone = false;
static bool  s_inSteamLogin = false;  // Steam login wizard active
static DWORD s_setupLaunchTime = 0;   // tick count when Steam launched for current step
static DWORD s_setupLastPoll = 0;
static char  s_setupStatus[256] = {}; // user-visible status line
static std::atomic<bool> s_setupRequestLaunch{ false };  // signal to (re)launch Steam for current step

// Pair Code modal triggers (T10 renders the actual dialogs). Set by T9 panel
// buttons, consumed and cleared by T10 modal render block.
static bool s_showGeneratePairCodeDialog = false;
static bool s_showPastePairCodeDialog    = false;

// Track moment when pairing transport first reported connected=true so the
// panel can show uptime. Reset to 0 on disconnect transition.
static int64_t s_pairingConnectedSinceMs = 0;

// Login list (just usernames — steam_id resolved after Steam login)
static constexpr int MAX_LOGINS = 10;
static char  s_logins[MAX_LOGINS][64] = {};
static char  s_passwords[MAX_LOGINS][128] = {};  // Steam password (optional)
static char  s_proxies[MAX_LOGINS][128] = {};  // per-account IPv4 proxy (optional)
static bool  s_proxyOn[MAX_LOGINS] = {};        // per-account proxy enable toggle
static bool  s_hwidOn[MAX_LOGINS]  = {};        // per-account HWID spoof toggle
static int   s_nLogins = 0;
static char  s_newLogin[64] = {};
static char  s_newPassword[128] = {};
static char  s_newProxy[128] = {};
static bool  s_editorInited = false;
static char  s_importStatus[256] = {};  // "Imported N accounts" / "Error: ..."

// Detect если аккаунт logins[step] залогинен (MostRecent=1, AllowAutoLogin=1)
// в C:\BotSteam\<step>\config\loginusers.vdf. Возвращает true после успешного login.
//
// Важно: Steam пишет loginusers.vdf атомарно через tmp+rename, но момент
// rename'а может поймать наш read в пустом/частичном состоянии → false
// positive "logged in" (нет Timestamp, но уже появился AccountName). Или
// file.fail() пока Steam держит эксклюзивный хендл. Retry 3× по 100мс —
// спокойно переживаем окно записи.
//
// Кроме MostRecent=1 + AllowAutoLogin=1 требуем Timestamp (non-zero) — он
// появляется только после того как Steam добросовестно сохранил JWT token.
// Без этой проверки мы ловим промежуточное состояние, KillAllSteam убивает
// Steam ДО записи JWT → следующий запуск снова просит пароль.
static bool IsSetupStepLoggedIn( int step )
{
	if ( step < 0 || step >= MAX_LOGINS ) return false;
	const char* login = s_logins[step];
	if ( login[0] == 0 ) return false;

	char vdfPath[MAX_PATH];
	snprintf( vdfPath, sizeof( vdfPath ),
		"C:\\BotSteam\\%d\\config\\loginusers.vdf", step );

	std::string content;
	for ( int attempt = 0; attempt < 3; attempt++ )
	{
		std::ifstream f( vdfPath, std::ios::binary );
		if ( !f.is_open() )
		{
			std::this_thread::sleep_for( std::chrono::milliseconds( 100 ) );
			continue;
		}
		content.assign( ( std::istreambuf_iterator<char>( f ) ),
			std::istreambuf_iterator<char>() );
		if ( !content.empty() ) break;
		std::this_thread::sleep_for( std::chrono::milliseconds( 100 ) );
	}
	if ( content.empty() ) return false;

	// Ищем блок с "AccountName" == login. Далее в блоке проверяем MostRecent=1
	// и AllowAutoLogin=1 — значит Steam сохранил JWT token и автологин готов.
	std::string needle = std::string( "\"AccountName\"\t\t\"" ) + login + "\"";
	size_t p = content.find( needle );
	if ( p == std::string::npos ) return false;

	// Находим конец ближайшего } после нашего AccountName
	size_t blockEnd = content.find( '}', p );
	if ( blockEnd == std::string::npos ) blockEnd = content.size();
	std::string block = content.substr( p, blockEnd - p );

	bool mostRecent = block.find( "\"MostRecent\"\t\t\"1\"" ) != std::string::npos;
	bool autoLogin  = block.find( "\"AllowAutoLogin\"\t\t\"1\"" ) != std::string::npos;
	// Timestamp появляется после полной записи — если его нет, Steam ещё
	// дописывает блок, считаем что login не завершён.
	bool timestamp  = block.find( "\"Timestamp\"" ) != std::string::npos;
	return mostRecent && autoLogin && timestamp;
}

// Load existing accounts into login list
static void InitLoginList( const Orchestrator& orch )
{
	auto& cfg = orch.GetConfig();
	s_nLogins = 0;
	// Default toggles — true для новых аккаунтов (до загрузки из cfg).
	for ( int i = 0; i < MAX_LOGINS; i++ )
	{
		s_proxyOn[i] = true;
		s_hwidOn[i]  = true;
	}
	for ( int i = 0; i < (int)cfg.accounts.size() && i < MAX_LOGINS; i++ )
	{
		strncpy( s_logins[i], cfg.accounts[i].login.c_str(), 63 );
		strncpy( s_passwords[i], cfg.accounts[i].password.c_str(), 127 );
		strncpy( s_proxies[i], cfg.accounts[i].proxy.c_str(), 127 );
		s_proxyOn[i] = cfg.accounts[i].proxyEnabled;
		s_hwidOn[i]  = cfg.accounts[i].hwidSpoofEnabled;
		s_nLogins++;
	}
	s_editorInited = true;
}

// ── Парсер строки формата login:pass  ИЛИ  login:pass:ip:port:proxy_user:proxy_pass ──
// При 6 полях формирует "socks5://user:pass@ip:port" из полей 3-6.
// Возвращает true если распарсил, иначе false.
static bool ParseAccountLine( const std::string& line, std::string& loginOut,
	std::string& passOut, std::string& proxyOut )
{
	std::vector<std::string> parts;
	size_t start = 0;
	for ( size_t i = 0; i <= line.size(); i++ )
	{
		if ( i == line.size() || line[i] == ':' )
		{
			std::string s = line.substr( start, i - start );
			// trim whitespace
			while ( !s.empty() && ( s.back() == ' ' || s.back() == '\t' ||
			        s.back() == '\r' || s.back() == '\n' ) )
				s.pop_back();
			while ( !s.empty() && ( s.front() == ' ' || s.front() == '\t' ) )
				s.erase( s.begin() );
			parts.push_back( s );
			start = i + 1;
		}
	}

	if ( parts.size() == 2 && !parts[0].empty() && !parts[1].empty() )
	{
		loginOut = parts[0];
		passOut  = parts[1];
		proxyOut.clear();
		return true;
	}
	if ( parts.size() == 6 && !parts[0].empty() && !parts[1].empty() )
	{
		loginOut = parts[0];
		passOut  = parts[1];
		// ip:port:proxy_user:proxy_pass → socks5://user:pass@ip:port
		if ( !parts[2].empty() && !parts[3].empty() )
			proxyOut = "socks5://" + parts[4] + ":" + parts[5] +
				"@" + parts[2] + ":" + parts[3];
		else
			proxyOut.clear();
		return true;
	}
	return false;
}

// ── Import от файла через GetOpenFileName. Replace all accounts. ──
static void ImportAccountsFromFile()
{
	OPENFILENAMEA ofn{};
	char fname[MAX_PATH] = {};
	ofn.lStructSize = sizeof( ofn );
	ofn.hwndOwner   = GetActiveWindow();
	ofn.lpstrFilter = "Text Files (*.txt)\0*.txt\0All Files (*.*)\0*.*\0";
	ofn.lpstrFile   = fname;
	ofn.nMaxFile    = sizeof( fname );
	ofn.lpstrTitle  = "Import accounts (login:pass or login:pass:ip:port:puser:ppass)";
	ofn.Flags       = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY;

	if ( !GetOpenFileNameA( &ofn ) )
		return; // user cancelled

	std::ifstream f( fname );
	if ( !f.is_open() )
	{
		snprintf( s_importStatus, sizeof( s_importStatus ),
			"Error: cannot open %s", fname );
		return;
	}

	int loaded = 0, skipped = 0;
	std::string line;
	// Clear existing
	for ( int i = 0; i < MAX_LOGINS; i++ )
	{
		memset( s_logins[i], 0, 64 );
		memset( s_passwords[i], 0, 128 );
		memset( s_proxies[i], 0, 128 );
		s_proxyOn[i] = true;   // default ON для всех импортируемых
		s_hwidOn[i]  = true;
	}
	s_nLogins = 0;

	while ( std::getline( f, line ) && loaded < 5 )
	{
		if ( line.empty() || line[0] == '#' ) continue;
		std::string lg, ps, px;
		if ( !ParseAccountLine( line, lg, ps, px ) )
		{
			skipped++;
			continue;
		}
		strncpy( s_logins[loaded],    lg.c_str(), 63 );
		strncpy( s_passwords[loaded], ps.c_str(), 127 );
		strncpy( s_proxies[loaded],   px.c_str(), 127 );
		loaded++;
	}
	s_nLogins = loaded;

	snprintf( s_importStatus, sizeof( s_importStatus ),
		"Imported %d account(s)%s from %s",
		loaded,
		skipped ? ( std::string( " (" ) + std::to_string( skipped ) + " line(s) skipped)" ).c_str() : "",
		fname );
}

// After all Steam logins done: read actual accounts from each BotSteam/{idx}/config/loginusers.vdf
// Don't rely on what the user typed — use what Steam actually saved.
static bool FinalizeAccounts( Orchestrator& orch )
{
	auto& cfg = orch.GetConfigMut();
	cfg.accounts.clear();

	for ( int i = 0; i < s_nLogins; i++ )
	{
		AccountConfig acc;

		// Read the ACTUAL logged-in account from this bot's loginusers.vdf
		char botSteamDir[MAX_PATH];
		snprintf( botSteamDir, sizeof( botSteamDir ), "C:\\BotSteam\\%d", i );
		auto vdfAccounts = config::ParseLoginUsersVdf( botSteamDir );

		// Take the MostRecent account, or first if none marked
		for ( auto& [name, vdf] : vdfAccounts )
		{
			// Always take the last one we see (MostRecent is usually last)
			acc.login = name;
			acc.steamId = vdf.steamId;
			acc.persona = vdf.persona;
		}

		// If BotSteam VDF is empty, use whatever the user entered as fallback
		if ( acc.login.empty() )
			acc.login = s_logins[i];

		char ipc[16];
		snprintf( ipc, sizeof( ipc ), "steam%d", i );
		acc.ipcName = ipc;
		acc.userchooser = false;
		acc.password = s_passwords[i];
		acc.proxy = config::NormalizeProxyString( s_proxies[i] );
		acc.proxyEnabled = s_proxyOn[i];
		acc.hwidSpoofEnabled = s_hwidOn[i];
		// Обновляем буфер чтобы юзер сразу видел в GUI "socks5://..." форму
		strncpy( s_proxies[i], acc.proxy.c_str(), 127 );
		s_proxies[i][127] = 0;

		cfg.accounts.push_back( acc );
	}

	// Save
	std::string configDir = g_exeDir + "\\config";
	CreateDirectoryA( configDir.c_str(), nullptr );
	std::string path = configDir + "\\accounts.json";
	bool ok = config::SaveAccounts( path, cfg );

	// Re-init orchestrator
	if ( ok )
		orch.Init( configDir, g_exeDir );

	return ok;
}

static void RenderSetup( Orchestrator& orch )
{
	if ( !s_editorInited )
		InitLoginList( orch );

	auto ds = ImGui::GetIO().DisplaySize;
	{
		ImDrawList* bdl = ImGui::GetBackgroundDrawList();
		theme::DrawDottedGrid( bdl, ImVec2( 0, 0 ), ds,
			IM_COL32( 0xd4, 0xa1, 0x4a, 0x10 ), 28.f );
	}
	ImGui::SetNextWindowPos( { 0, 0 } );
	ImGui::SetNextWindowSize( ds );
	ImGui::PushStyleColor( ImGuiCol_WindowBg, ImVec4( 0.039f, 0.035f, 0.027f, 1.f ) );
	ImGui::Begin( "##setup", nullptr,
		ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
		ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse );

	if ( theme::gFontSerifLg ) ImGui::PushFont( theme::gFontSerifLg );
	ImGui::TextColored( theme::V( theme::kColInkBright ), "ACCOUNT" );
	ImGui::SameLine( 0, 12 );
	ImGui::TextColored( theme::V( theme::kColGold ),      "SETUP" );
	if ( theme::gFontSerifLg ) ImGui::PopFont();

	if ( theme::gFontMonoSm ) ImGui::PushFont( theme::gFontMonoSm );
	ImGui::TextColored( theme::V( theme::kColInkMute ),
		"BOT-STEAM PROVISIONING  |  PER-ACCOUNT JWT TOKEN HARVEST" );
	if ( theme::gFontMonoSm ) ImGui::PopFont();

	ImGui::Spacing();
	{
		ImDrawList* dl = ImGui::GetWindowDrawList();
		ImVec2 p = ImGui::GetCursorScreenPos();
		float w = ImGui::GetContentRegionAvail().x;
		dl->AddLine( p, ImVec2( p.x + w, p.y ), theme::kColLineHot, 1.f );
	}
	ImGui::Spacing();

	// ═══ Steam Login Wizard ═══
	if ( s_inSteamLogin )
	{
		ImGui::Text( "Log into each Steam account and tick 'Remember password'." );
		ImGui::Text( "Close Steam after each login." );
		ImGui::Spacing();

		for ( int i = 0; i < s_nLogins; i++ )
		{
			bool done = ( i < s_setupStep );
			bool current = ( i == s_setupStep && s_setupSteamPid != 0 );
			ImVec4 col = done ? ImVec4( 0.2f, 0.8f, 0.2f, 1.f )
				: current ? ImVec4( 1.0f, 0.8f, 0.2f, 1.f )
				: ImVec4( 0.5f, 0.5f, 0.5f, 1.f );
			const char* st = done ? "[OK]" : current ? "[...]" : "[ ]";
			ImGui::TextColored( col, "  %s  #%d  %s", st, i, s_logins[i] );
		}

		ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();

		if ( s_setupStep < s_nLogins )
		{
			if ( s_setupSteamPid == 0 )
			{
				ImGui::Text( "Step %d/%d: Log into '%s'", s_setupStep + 1, s_nLogins, s_logins[s_setupStep] );
				if ( s_passwords[s_setupStep][0] != 0 )
					ImGui::TextColored( { 0.4f, 0.9f, 0.4f, 1.f },
						"Password set — will auto-login via CLI and advance to next." );
				else
					ImGui::TextColored( { 0.9f, 0.7f, 0.3f, 1.f },
						"No password — login manually, tick 'Remember Password', then close Steam." );
				ImGui::Spacing();

				bool clicked = ImGui::Button( "Open Steam", { 200, 30 } );
				if ( s_setupRequestLaunch )
				{
					clicked = true;
					s_setupRequestLaunch = false;
				}
				if ( clicked )
				{
					// Kill existing Steam
					SteamLauncher::KillAllSteam();

					// Detect real Steam path
					std::string steamExe = SteamLauncher::DetectSteamExe();
					std::string steamDir = steamExe;
					auto sl = steamDir.find_last_of( "\\/" );
					if ( sl != std::string::npos ) steamDir = steamDir.substr( 0, sl );

					// Create per-bot Steam dir (C:\BotSteam\{idx}) with junctions
					SteamLauncher::EnsureBotSteamDir( s_setupStep, steamDir );

					// Create per-bot profile dir (C:\BotProfiles\bot{idx})
					char profileDir[MAX_PATH];
					snprintf( profileDir, sizeof( profileDir ), "C:\\BotProfiles\\bot%d", s_setupStep );
					CreateDirectoryA( "C:\\BotProfiles", nullptr );
					CreateDirectoryA( profileDir, nullptr );
					CreateDirectoryA( ( std::string( profileDir ) + "\\AppData" ).c_str(), nullptr );
					CreateDirectoryA( ( std::string( profileDir ) + "\\AppData\\Roaming" ).c_str(), nullptr );
					CreateDirectoryA( ( std::string( profileDir ) + "\\AppData\\Local" ).c_str(), nullptr );
					CreateDirectoryA( ( std::string( profileDir ) + "\\Temp" ).c_str(), nullptr );

					// Set registry to point to this bot's Steam dir
					char botDir[MAX_PATH];
					snprintf( botDir, sizeof( botDir ), "C:\\BotSteam\\%d", s_setupStep );
					char botExe[MAX_PATH];
					snprintf( botExe, sizeof( botExe ), "%s\\steam.exe", botDir );

					HKEY hKey = nullptr;
					if ( RegOpenKeyExA( HKEY_CURRENT_USER, "Software\\Valve\\Steam",
						0, KEY_SET_VALUE, &hKey ) == ERROR_SUCCESS )
					{
						const char* login = s_logins[s_setupStep];
						RegSetValueExA( hKey, "AutoLoginUser", 0, REG_SZ,
							(const BYTE*)login, (DWORD)strlen( login ) + 1 );
						DWORD one = 1;
						RegSetValueExA( hKey, "RememberPassword", 0, REG_DWORD,
							(const BYTE*)&one, sizeof( one ) );

						std::string sp = botDir;
						for ( auto& c : sp ) { if ( c == '\\' ) c = '/'; c = (char)tolower( (unsigned char)c ); }
						RegSetValueExA( hKey, "SteamPath", 0, REG_SZ,
							(const BYTE*)sp.c_str(), (DWORD)sp.size() + 1 );
						std::string se = sp + "/steam.exe";
						RegSetValueExA( hKey, "SteamExe", 0, REG_SZ,
							(const BYTE*)se.c_str(), (DWORD)se.size() + 1 );
						RegCloseKey( hKey );
					}

					// Нормализуем формат прокси перед использованием — юзер в setup
					// может не нажимать Save, а ввести напрямую "ip:port:user:pass".
					// Если per-account toggle выключен — считаем что прокси пуст,
					// даже если поле заполнено. Та же логика при запуске фермы.
					std::string setupProxyRaw = config::NormalizeProxyString( s_proxies[s_setupStep] );
					// Обновляем GUI-буфер, чтобы юзер видел канонический URL.
					strncpy( s_proxies[s_setupStep], setupProxyRaw.c_str(), 127 );
					s_proxies[s_setupStep][127] = 0;
					std::string setupProxy = s_proxyOn[s_setupStep] ? setupProxyRaw : std::string();

					std::string envBlock = setupProxy.empty()
						? SteamLauncher::BuildEnvironmentBlock( s_setupStep, "C:\\BotProfiles" )
						: SteamLauncher::BuildEnvironmentBlock( s_setupStep, "C:\\BotProfiles", setupProxy );

					// ProxyHook.dll путь ОБЯЗАТЕЛЬНО абсолютный — LoadLibraryA в target
					// процессе с CWD=C:\BotSteam\N не найдёт "ProxyHook.dll" без пути.
					// Ищем в двух местах: рядом с DotaFarm.exe (g_exeDir) и в resolved
					// orch.Config. Первый совпавший wins.
					std::string dllPath;
					{
						std::string candidates[] = {
							g_exeDir + "\\ProxyHook.dll",
							orch.GetConfig().proxyHookDllPath,
						};
						for ( const auto& c : candidates )
						{
							if ( c.empty() ) continue;
							// Резолвим к абсолютному (на случай relative пути из config'а)
							char abs[MAX_PATH] = {};
							if ( GetFullPathNameA( c.c_str(), MAX_PATH, abs, nullptr ) > 0 &&
								 GetFileAttributesA( abs ) != INVALID_FILE_ATTRIBUTES )
							{
								dllPath = abs;
								break;
							}
						}
					}
					// useTun2Socks=true (default) — sing-box уже ловит весь трафик
					// BotSteam\<N>\ через process_path_regex. ProxyHook для proxy redirect
					// становится бесполезен, но per-process HWID spoof всё ещё нужен,
					// если hwidSpoofEnabled — тогда инжектим только ради HWID hooks.
					// Per-account HWID toggle работает ТОЛЬКО если global on — это
					// фильтр поверх мастер-свитча, не override.
					bool hwidOn = orch.GetConfig().IsSteamSpoofEnabled() && s_hwidOn[s_setupStep];
					bool proxyViaHook = !setupProxy.empty() && !orch.GetConfig().useTun2Socks;
					bool useProxyHook = !dllPath.empty() && ( proxyViaHook || hwidOn );

					// Launch Steam from per-bot dir with redirected env.
					// Auto-login через CLI args. Работает без Steam Guard.
					// БЕЗ `-silent` — иначе Steam в tray и user думает что login failed
					// хотя на самом деле JWT token получен. Window visible = user видит
					// что Steam логинится и переходит в main UI.
					std::string cmd = "\"" + std::string( botExe ) + "\"";
					if ( s_passwords[s_setupStep][0] != 0 )
					{
						cmd += " -login ";
						cmd += s_logins[s_setupStep];
						cmd += " ";
						cmd += s_passwords[s_setupStep];
					}

					// Очищаем registry AutoLoginUser — иначе Steam выбирает stale token
					// из registry и показывает login UI вместо CLI `-login`.
					if ( s_passwords[s_setupStep][0] != 0 )
					{
						HKEY h = nullptr;
						if ( RegOpenKeyExA( HKEY_CURRENT_USER, "Software\\Valve\\Steam",
								0, KEY_SET_VALUE, &h ) == ERROR_SUCCESS )
						{
							RegDeleteValueA( h, "AutoLoginUser" );
							RegCloseKey( h );
						}
					}

					// Diagnostic log — masked password
					{
						std::string safeCmd = cmd;
						size_t pp = safeCmd.find( "-login " );
						if ( pp != std::string::npos )
						{
							size_t ue = safeCmd.find( ' ', pp + 7 );
							if ( ue != std::string::npos )
							{
								size_t pe = safeCmd.find( ' ', ue + 1 );
								if ( pe == std::string::npos ) pe = safeCmd.size();
								safeCmd.replace( ue + 1, pe - ue - 1, "***" );
							}
						}
						FILE* lf = fopen( "C:\\temp\\andromeda\\steam_launch.log", "a" );
						if ( lf )
						{
							SYSTEMTIME st; GetLocalTime( &st );
							fprintf( lf, "[%04d-%02d-%02d %02d:%02d:%02d] setup #%d cmd: %s\n",
								st.wYear, st.wMonth, st.wDay,
								st.wHour, st.wMinute, st.wSecond,
								s_setupStep, safeCmd.c_str() );
							fclose( lf );
						}
					}

					STARTUPINFOA si{}; si.cb = sizeof( si );
					PROCESS_INFORMATION pi{};

					DWORD cpflags = useProxyHook ? CREATE_SUSPENDED : 0;

					BOOL cpResult = CreateProcessA( nullptr, const_cast<char*>( cmd.c_str() ),
						nullptr, nullptr, FALSE, cpflags,
						envBlock.empty() ? nullptr : const_cast<char*>( envBlock.c_str() ),
						botDir, &si, &pi );

					if ( cpResult )
					{
						// Регистрируем Steam PID в kernel redirect ДО ResumeThread —
						// чтобы первый же AFD-direct connect Steam'а уже попал под NAT.
						// EarlyStartProxy в main.cpp уже стартанул engine; если нет — no-op.
						if ( orch.GetProxyService().IsRunning() && !setupProxy.empty() )
							orch.GetProxyService().AddRootPid( pi.dwProcessId, setupProxy );

						if ( useProxyHook )
						{
							Injector inj; inj.Init();
							std::string proxyForDll = proxyViaHook ? setupProxy : std::string();
							// Setup wizard: steamId часто ещё неизвестен. Используем
							// login + bot index как seed (stable per account).
							std::string hwidSeed;
							if ( hwidOn )
							{
								const auto& accs = orch.GetConfig().accounts;
								uint64_t sid = ( s_setupStep < (int)accs.size() ) ? accs[s_setupStep].steamId : 0;
								time_t now = time( nullptr );
								tm tmv{}; localtime_s( &tmv, &now );
								char buf[128];
								if ( sid != 0 )
									snprintf( buf, sizeof( buf ), "%llu_%04d-%02d",
										(unsigned long long)sid, tmv.tm_year + 1900, tmv.tm_mon + 1 );
								else
									snprintf( buf, sizeof( buf ), "setup_%d_%s_%04d-%02d",
										s_setupStep, s_logins[s_setupStep],
										tmv.tm_year + 1900, tmv.tm_mon + 1 );
								hwidSeed = buf;
							}
							bool cfgOk = Injector::WriteProxyConfig( pi.dwProcessId, proxyForDll, hwidSeed );
							bool injOk = inj.InjectLoadLibrary( pi.dwProcessId, dllPath );

							// Не убиваем процесс при сбое — хуже будет Steam без прокси,
							// чем suspended vanish'нутое окно. Лог через MessageBox.
							if ( !cfgOk || !injOk )
							{
								char msg[512];
								snprintf( msg, sizeof( msg ),
									"ProxyHook attach failed (cfg=%d inj=%d).\nDLL path: %s\n\n"
									"Resuming Steam WITHOUT proxy hook — login will go via real IP!",
									(int)cfgOk, (int)injOk, dllPath.c_str() );
								MessageBoxA( nullptr, msg, "DotaFarm — Open Steam", MB_OK | MB_ICONWARNING );
							}
							ResumeThread( pi.hThread );
						}

						s_setupSteamPid = pi.dwProcessId;
						s_setupLaunchTime = GetTickCount();
						s_setupLastPoll = 0;
						snprintf( s_setupStatus, sizeof( s_setupStatus ),
							"Launched Steam for '%s', waiting for login...",
							s_logins[s_setupStep] );

						CloseHandle( pi.hProcess );
						CloseHandle( pi.hThread );
					}
					else
					{
						DWORD err = GetLastError();
						char msg[512];
						snprintf( msg, sizeof( msg ),
							"CreateProcess failed (err=%lu).\nCommand: %s\nWorkDir: %s\n\n"
							"Most likely causes:\n"
							"- C:\\BotSteam\\%d\\steam.exe is broken symlink (need Developer Mode ON)\n"
							"- Real Steam path changed — delete C:\\BotSteam\\%d and retry",
							err, cmd.c_str(), botDir, s_setupStep, s_setupStep );
						MessageBoxA( nullptr, msg, "DotaFarm — Open Steam FAILED", MB_OK | MB_ICONERROR );
					}
				}
			}
			else
			{
				// Auto-detect login success раз в 1.5 сек (polling loginusers.vdf).
				DWORD nowTick = GetTickCount();
				if ( nowTick - s_setupLastPoll > 1500 )
				{
					s_setupLastPoll = nowTick;

					bool loggedIn = IsSetupStepLoggedIn( s_setupStep );
					DWORD elapsedSec = s_setupLaunchTime
						? ( nowTick - s_setupLaunchTime ) / 1000 : 0;

					if ( loggedIn )
					{
						snprintf( s_setupStatus, sizeof( s_setupStatus ),
							"✓ '%s' logged in (took %lus). Advancing...",
							s_logins[s_setupStep], elapsedSec );
						SteamLauncher::KillAllSteam();
						s_setupSteamPid = 0;
						s_setupStep++;
						s_setupLaunchTime = 0;
						// Auto-launch next step если ещё есть password
						if ( s_setupStep < s_nLogins &&
							 s_passwords[s_setupStep][0] != 0 )
						{
							s_setupRequestLaunch = true;
						}
					}
					else if ( s_setupLaunchTime && elapsedSec > 120 )
					{
						snprintf( s_setupStatus, sizeof( s_setupStatus ),
							"⚠ timeout 120s — '%s' not logged in. Login manually or click Skip.",
							s_logins[s_setupStep] );
					}
					else
					{
						snprintf( s_setupStatus, sizeof( s_setupStatus ),
							"Logging in '%s'... %lus elapsed",
							s_logins[s_setupStep], elapsedSec );
					}
				}

				ImGui::Text( "%s", s_setupStatus );
				ImGui::Spacing();
				if ( ImGui::Button( "Skip / Next", { 200, 30 } ) )
				{
					SteamLauncher::KillAllSteam();
					s_setupSteamPid = 0;
					s_setupStep++;
					s_setupLaunchTime = 0;
					if ( s_setupStep < s_nLogins &&
						 s_passwords[s_setupStep][0] != 0 )
					{
						s_setupRequestLaunch = true;
					}
				}
			}
		}
		else
		{
			// All done — parse steam_id from VDF and save
			ImGui::TextColored( { 0.2f, 0.8f, 0.2f, 1.f }, "All accounts logged in!" );
			ImGui::Spacing();
			if ( ImGui::Button( "Finish Setup", { 200, 30 } ) )
			{
				FinalizeAccounts( orch );
				s_inSteamLogin = false;
				s_setupDone = true;
			}
		}

		ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();
		if ( ImGui::Button( "Skip (accounts already logged in)", { -1, 24 } ) )
		{
			if ( s_setupSteamPid )
			{
				orch.KillSteamProcess( s_setupSteamPid );
				s_setupSteamPid = 0;
				Sleep( 1000 );
			}
			FinalizeAccounts( orch );
			s_inSteamLogin = false;
			s_setupDone = true;
		}

		ImGui::End();
		ImGui::PopStyleColor();
		return;
	}

	// ═══ Login List Editor ═══
	ImGui::Text( "Add Steam logins for bot accounts (1-5)." );
	ImGui::TextColored( { 0.5f, 0.5f, 0.5f, 1.f },
		"Password optional — only needed for first auto-login. Steam ID detected after login." );
	ImGui::Spacing();

	// Import from file button
	if ( ImGui::Button( "Import from file...", { 180, 0 } ) )
		ImportAccountsFromFile();
	ImGui::SameLine();
	ImGui::TextColored( { 0.5f, 0.5f, 0.5f, 1.f },
		"format: login:pass  OR  login:pass:ip:port:proxy_user:proxy_pass" );
	if ( s_importStatus[0] )
		ImGui::TextColored( { 0.4f, 0.9f, 0.4f, 1.f }, "%s", s_importStatus );
	ImGui::Spacing();

	// Current logins (3 строки на аккаунт: login+remove, password, proxy)
	int removeIdx = -1;
	for ( int i = 0; i < s_nLogins; i++ )
	{
		ImGui::PushID( i );
		ImGui::Text( "  #%d", i );
		ImGui::SameLine( 50 );
		ImGui::Text( "%s", s_logins[i] );
		ImGui::SameLine( 250 );
		if ( ImGui::SmallButton( "Remove" ) )
			removeIdx = i;

		// Password (hidden with asterisks)
		ImGui::Text( " pass:" );
		ImGui::SameLine( 70 );
		ImGui::SetNextItemWidth( 340 );
		ImGui::InputTextWithHint( "##pass", "Steam password (optional, plaintext)",
			s_passwords[i], sizeof( s_passwords[i] ), ImGuiInputTextFlags_Password );

		// Proxy inline input
		ImGui::Text( "proxy:" );
		ImGui::SameLine( 70 );
		ImGui::SetNextItemWidth( 340 );
		ImGui::InputTextWithHint( "##proxy", "socks5://user:pass@host:port (optional)",
			s_proxies[i], sizeof( s_proxies[i] ) );

		// Per-account toggles для proxy/HWID spoof. Применяются при запуске этого
		// аккаунта — позволяют держать строку прокси в конфиге но временно
		// отключить (например при отладке). Global SpoofMode в farm.json
		// остаётся master switch'ем — per-account имеет смысл только при
		// SteamOnly или Both (per-process хуки selectable per-bot). При FullPC
		// driver-режим machine-wide → checkbox игнорируется (greyed out).
		const bool perAccountMatters = orch.GetConfig().IsSteamSpoofEnabled();
		ImGui::Text( "      " );
		ImGui::SameLine( 70 );
		ImGui::Checkbox( "Proxy", &s_proxyOn[i] );
		ImGui::SameLine( 180 );
		if ( !perAccountMatters )
			ImGui::BeginDisabled();
		ImGui::Checkbox( "HWID spoof", &s_hwidOn[i] );
		if ( !perAccountMatters )
			ImGui::EndDisabled();
		ImGui::SameLine();
		if ( perAccountMatters )
		{
			ImGui::TextColored( { 0.5f, 0.5f, 0.5f, 1.f },
				"(per-account; applied on Steam launch)" );
		}
		else
		{
			ImGui::TextColored( { 0.6f, 0.6f, 0.4f, 1.f },
				"(spoof_mode=%s — per-account checkbox не применяется)",
				SpoofModeToString( orch.GetConfig().spoofMode ) );
		}

		ImGui::PopID();
		ImGui::Spacing();
	}

	if ( removeIdx >= 0 )
	{
		for ( int i = removeIdx; i < s_nLogins - 1; i++ )
		{
			memcpy( s_logins[i], s_logins[i + 1], 64 );
			memcpy( s_passwords[i], s_passwords[i + 1], 128 );
			memcpy( s_proxies[i], s_proxies[i + 1], 128 );
			s_proxyOn[i] = s_proxyOn[i + 1];
			s_hwidOn[i]  = s_hwidOn[i + 1];
		}
		memset( s_logins[s_nLogins - 1], 0, 64 );
		memset( s_passwords[s_nLogins - 1], 0, 128 );
		memset( s_proxies[s_nLogins - 1], 0, 128 );
		s_proxyOn[s_nLogins - 1] = true;
		s_hwidOn[s_nLogins - 1]  = true;
		s_nLogins--;
	}

	ImGui::Spacing();

	// Add new login manually
	if ( s_nLogins < 5 )
	{
		ImGui::SetNextItemWidth( 160 );
		ImGui::InputTextWithHint( "##newlogin", "steam login", s_newLogin, 64 );
		ImGui::SameLine();
		ImGui::SetNextItemWidth( 160 );
		ImGui::InputTextWithHint( "##newpass", "password (optional)",
			s_newPassword, 128, ImGuiInputTextFlags_Password );
		ImGui::SameLine();
		ImGui::SetNextItemWidth( 200 );
		bool enter = ImGui::InputTextWithHint( "##newproxy", "socks5:// (optional)",
			s_newProxy, 128, ImGuiInputTextFlags_EnterReturnsTrue );
		ImGui::SameLine();
		if ( ( ImGui::Button( "Add", { 60, 0 } ) || enter ) && strlen( s_newLogin ) > 0 )
		{
			strncpy( s_logins[s_nLogins],    s_newLogin,    63 );
			strncpy( s_passwords[s_nLogins], s_newPassword, 127 );
			strncpy( s_proxies[s_nLogins],   s_newProxy,    127 );
			s_proxyOn[s_nLogins] = true;  // default ON для новых аккаунтов
			s_hwidOn[s_nLogins]  = true;
			s_nLogins++;
			s_newLogin[0] = 0;
			s_newPassword[0] = 0;
			s_newProxy[0] = 0;
		}
	}
	else
	{
		ImGui::TextColored( { 0.5f, 0.5f, 0.5f, 1.f }, "Max 5 accounts (Dota party limit)" );
	}

	ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();

	if ( s_nLogins > 0 )
	{
		if ( ImGui::Button( "Start Steam Login", { 200, 32 } ) )
		{
			s_setupStep = 0;
			s_setupSteamPid = 0;
			s_inSteamLogin = true;
			s_setupLaunchTime = 0;
			s_setupLastPoll = 0;
			s_setupStatus[0] = 0;
			// Если у первого шага password есть — сразу авто-запуск.
			if ( s_passwords[0][0] != 0 )
				s_setupRequestLaunch = true;
		}

		// Skip login if accounts already logged in from before
		ImGui::SameLine();
		if ( ImGui::Button( "Skip (already logged in)", { 220, 32 } ) )
		{
			FinalizeAccounts( orch );
			s_setupDone = true;
		}
	}
	else
	{
		ImGui::TextColored( { 0.8f, 0.4f, 0.2f, 1.f }, "Add at least 1 account to continue" );
	}

	ImGui::End();
	ImGui::PopStyleColor();
}

// ═══════════════════════════════════════════════════════════
// PC STATS PANEL — Variant A "Panorama Scope" (Bundle MEMRED)
// ═══════════════════════════════════════════════════════════
//
// Editorial banner-style: одна доминирующая memory bar занимает горизонт,
// под ней caption с numeric strip (MEM/STBY/MOD/COMM/BOTS), справа dock
// primary [ RECLAIM NOW ] + secondary [ TRIM BOTS ] + [ + SETTINGS ] toggle.
// Settings свернуты по умолчанию (5 checkbox'ов + auto threshold + interval).
//
// Refresh policy:
//   QuerySystemMemory  — каждый frame (cheap, ~10µs)
//   EnumProcessesRam   — 2s cache (5-15ms, тяжелее)
//   bots WS sum        — пересчёт когда обновлён proc snapshot

static void RenderPcStatsPanel( Orchestrator& orch )
{
	using namespace theme;

	// ── State (cached across frames) ──
	static MemReclaim::MemoryStats        s_stats{};
	static std::vector<MemReclaim::ProcInfo> s_procs;
	static uint64_t                       s_botsWsBytes = 0;
	static int                            s_botInstCount = 0;
	static auto                           s_procsRefresh =
		std::chrono::steady_clock::time_point{};
	static bool                           s_showSettings = false;

	const auto now = std::chrono::steady_clock::now();

	// Live system memory snapshot — каждый frame.
	s_stats = MemReclaim::QuerySystemMemory();

	// Process snapshot + BOTS sum — раз в 2 секунды.
	if ( std::chrono::duration_cast<std::chrono::seconds>(
			now - s_procsRefresh ).count() >= 2 )
	{
		s_procs = MemReclaim::EnumProcessesRam();
		std::vector<DWORD> botPids;
		botPids.reserve( orch.GetBotCount() * 2 );
		s_botInstCount = 0;
		for ( int i = 0; i < orch.GetBotCount(); i++ )
		{
			const auto& b = orch.GetBot( i );
			if ( b.dotaPid )  botPids.push_back( b.dotaPid );
			if ( b.steamPid ) botPids.push_back( b.steamPid );
			if ( b.dotaPid || b.steamPid ) s_botInstCount++;
		}
		s_botsWsBytes = MemReclaim::SumWsBytesIn( s_procs, botPids );
		s_procsRefresh = now;
	}

	auto& cfgMut = orch.GetConfigMut();
	auto& mr     = cfgMut.minifier;

	// Memory load → color (single source of truth).
	auto loadColor = []( unsigned pct ) -> ImU32 {
		if ( pct >= MemReclaim::kPressureCrit ) return kColCrash;
		if ( pct >= MemReclaim::kPressureWarn ) return kColWarn;
		return kColSignal;
	};
	const ImU32 barCol = loadColor( s_stats.memoryLoadPct );

	// ── Geometry (ВСЁ через absolute Y от outerMn.y — нет Dummy/BeginGroup drift'а) ──
	ImDrawList* dl = ImGui::GetWindowDrawList();
	ImVec2 outerMn = ImGui::GetCursorScreenPos();
	float  availW  = ImGui::GetContentRegionAvail().x;

	const float padX   = 14.f;
	const float padY   = 10.f;
	const float row1H  = 16.f;   // section bar + AUTO
	const float row2H  = 36.f;   // bar (22) + tick scale (12) + 2 gap
	const float row3H  = 20.f;   // numeric strip + buttons
	const float panelH = padY + row1H + row2H + row3H + padY;

	const float innerX = outerMn.x + padX;
	const float innerW = availW - padX * 2;
	const float y1     = outerMn.y + padY;
	const float y2     = y1 + row1H;
	const float y3     = y2 + row2H;

	ImVec2 outerMx( outerMn.x + availW, outerMn.y + panelH );
	DrawChamferedRect( dl, outerMn, outerMx, 10.f, kColBg1 );
	DrawChamferedRect( dl, outerMn, outerMx, 10.f, kColLine, 1.f );
	DrawCornerBrackets( dl, outerMn, outerMx, 14.f, 1.f, kColGoldDeep );

	ImFont* fSm  = gFontMonoSm ? gFontMonoSm : ImGui::GetFont();
	float   fSmZ = gFontMonoSm ? gFontMonoSm->FontSize : ImGui::GetFontSize();

	// ───────────────────────────────────────────────────────────
	// Row 1 — section header + connector line + AUTO indicator
	// ───────────────────────────────────────────────────────────
	{
		const char* prefix = "[-- ";
		const char* label  = "SYSTEM RECLAIM";
		ImVec2 ps = fSm->CalcTextSizeA( fSmZ, FLT_MAX, 0, prefix );
		ImVec2 ls = fSm->CalcTextSizeA( fSmZ, FLT_MAX, 0, label );
		dl->AddText( fSm, fSmZ, ImVec2( innerX, y1 ), kColGoldDeep, prefix );
		dl->AddText( fSm, fSmZ, ImVec2( innerX + ps.x, y1 ), kColGold, label );

		bool autoActive    = mr.memReclaimEnabled && mr.memReclaimAutoThreshold > 0;
		bool autoTriggered = autoActive && s_stats.valid &&
			(int)s_stats.memoryLoadPct >= mr.memReclaimAutoThreshold;
		char autoBuf[32];
		const char* autoTxt;
		if ( autoActive )
		{
			snprintf( autoBuf, sizeof( autoBuf ), "AUTO @ %d%%",
				mr.memReclaimAutoThreshold );
			autoTxt = autoBuf;
		}
		else
		{
			autoTxt = "AUTO OFF";
		}
		ImVec2 autoSz = fSm->CalcTextSizeA( fSmZ, FLT_MAX, 0, autoTxt );
		float  autoX  = innerX + innerW - autoSz.x;
		ImU32  autoCol = autoTriggered ? kColCrash :
			( autoActive ? kColInkBright : kColInkMute );
		dl->AddText( fSm, fSmZ, ImVec2( autoX, y1 ), autoCol, autoTxt );

		float ledX = autoX - 14.f;
		DrawLEDSquare( dl, ImVec2( ledX, y1 + fSmZ * 0.5f ), 8.f,
			autoTriggered ? kColCrash : ( autoActive ? kColSignal : kColIdle ),
			kColLineHot, (float)( ImGui::GetTime() * 0.6 ), autoTriggered );

		float lineX0 = innerX + ps.x + ls.x + 8.f;
		float lineX1 = ledX - 12.f;
		if ( lineX1 > lineX0 )
			dl->AddLine( ImVec2( lineX0, y1 + fSmZ * 0.5f + 1 ),
				ImVec2( lineX1, y1 + fSmZ * 0.5f + 1 ), kColLineHot, 1.f );
	}

	// ───────────────────────────────────────────────────────────
	// Row 2 — memory bar (with overlay load%) + RECLAIM NOW
	// ───────────────────────────────────────────────────────────
	// load% сидит ВНУТРИ бара как overlay (правый край) — освобождает
	// горизонтальное место под бар в узких окнах и убирает риск
	// перекрытия с кнопкой.
	{
		const float btnH   = 30.f;
		const float btnGap = 10.f;
		const float barH   = 22.f;
		// Адаптивная ширина кнопки: 120 в узком окне, 140 в широком.
		float btnW = ( innerW < 720.f ) ? 110.f : 140.f;
		float barW = innerW - btnW - btnGap;
		if ( barW < 140.f ) barW = 140.f;   // hard floor, иначе bar слипается

		ImVec2 barMn( innerX, y2 );
		ImVec2 barMx( innerX + barW, y2 + barH );
		float  fill = s_stats.totalPhysMB > 0
			? (float)( s_stats.totalPhysMB - s_stats.availPhysMB ) /
			  (float)s_stats.totalPhysMB
			: 0.f;
		DrawHpBar( dl, barMn, barMx, fill, barCol, kColBg0 );

		// load% — overlay INSIDE bar, right-aligned. На kColBg0 background
		// (тёмный) с barCol текстом — контрастно даже на пустом fill.
		ImFont* fm  = gFontMono ? gFontMono : ImGui::GetFont();
		float   fmZ = gFontMono ? gFontMono->FontSize : ImGui::GetFontSize();
		char pctBuf[16];
		snprintf( pctBuf, sizeof( pctBuf ), "%u%%", s_stats.memoryLoadPct );
		ImVec2 pctSz = fm->CalcTextSizeA( fmZ, FLT_MAX, 0, pctBuf );
		// Если % находится в fill'нутой части — рисуем чёрным; иначе цветом.
		float pctRight = barMx.x - 8.f;
		float pctLeft  = pctRight - pctSz.x;
		bool  insideFill = pctLeft < ( barMn.x + barW * fill );
		ImU32 pctCol = insideFill ? IM_COL32( 0, 0, 0, 220 ) : barCol;
		dl->AddText( fm, fmZ,
			ImVec2( pctLeft, barMn.y + ( barH - pctSz.y ) * 0.5f ),
			pctCol, pctBuf );

		// Tick scale + threshold marker (скрываем подписи при узком баре).
		float tickY = barMx.y + 2.f;
		bool  showTickLabels = barW >= 200.f;
		for ( int i = 0; i <= 10; i++ )
		{
			float u  = (float)i / 10.f;
			float tx = barMn.x + barW * u;
			dl->AddLine( ImVec2( tx, tickY ), ImVec2( tx, tickY + 3.f ),
				kColInkMute, 1.f );
			if ( showTickLabels && ( i == 0 || i == 5 || i == 10 ) )
			{
				char tb[8];
				snprintf( tb, sizeof( tb ), "%d", i * 10 );
				ImVec2 tsz = fSm->CalcTextSizeA( fSmZ, FLT_MAX, 0, tb );
				dl->AddText( fSm, fSmZ,
					ImVec2( tx - tsz.x * 0.5f, tickY + 4.f ),
					kColInkMute, tb );
			}
		}
		if ( mr.memReclaimAutoThreshold > 0 && mr.memReclaimAutoThreshold < 100 )
		{
			float u  = mr.memReclaimAutoThreshold / 100.f;
			float tx = barMn.x + barW * u;
			ImVec2 tri[3] = {
				ImVec2( tx,       tickY + 1.f ),
				ImVec2( tx - 4.f, tickY + 7.f ),
				ImVec2( tx + 4.f, tickY + 7.f ),
			};
			dl->AddConvexPolyFilled( tri, 3, kColGold );
		}

		// RECLAIM NOW button. Y-center относительно бара. Лейбл сжимаем в узком.
		float btnY = barMn.y + ( barH - btnH ) * 0.5f;
		ImGui::SetCursorScreenPos( ImVec2( innerX + innerW - btnW, btnY ) );
		const char* btnLabel = ( innerW < 720.f ) ? "RECLAIM" : "RECLAIM NOW";
		if ( ChamferedButton( btnLabel, ImVec2( btnW, btnH ),
			kColBg1, kColGold, kColInkBright, true ) )
		{
			int ops = 0;
			auto before = MemReclaim::QuerySystemMemory();
			if ( mr.memReclaimEmptyAllWorkingSets )   { MemReclaim::EmptyAllWorkingSets();     ops++; }
			if ( mr.memReclaimFlushModified )         { MemReclaim::FlushModified();           ops++; }
			if ( mr.memReclaimPurgeLowPrioStandby )   { MemReclaim::PurgeLowPriorityStandby(); ops++; }
			if ( mr.memReclaimPurgeStandby )          { MemReclaim::PurgeStandby();            ops++; }
			if ( mr.memReclaimCombinePages )          { MemReclaim::CombineMemoryLists();      ops++; }
			auto after = MemReclaim::QuerySystemMemory();
			char msg[160];
			snprintf( msg, sizeof( msg ),
				"[mem-reclaim/manual] ops=%d load=%u%%->%u%% avail=%llu->%llu MB",
				ops, before.memoryLoadPct, after.memoryLoadPct,
				(unsigned long long)before.availPhysMB,
				(unsigned long long)after.availPhysMB );
			orch.LogPublic( msg );
		}
		if ( ImGui::IsItemHovered() )
		{
			int active = (int)mr.memReclaimEmptyAllWorkingSets +
				(int)mr.memReclaimFlushModified +
				(int)mr.memReclaimPurgeLowPrioStandby +
				(int)mr.memReclaimPurgeStandby +
				(int)mr.memReclaimCombinePages;
			ImGui::SetTooltip(
				"Запустить включённые reclaim операции прямо сейчас.\n"
				"Активных операций: %d.\n"
				"Дельту avail MB видно в логе.", active );
		}
	}

	// ───────────────────────────────────────────────────────────
	// Row 3 — numeric strip + TRIM BOTS + +SETTINGS
	// ───────────────────────────────────────────────────────────
	{
		auto fmtGB = []( uint64_t mb, char* out, size_t n ) {
			if ( mb < 1024 )
				snprintf( out, n, "%llu MB", (unsigned long long)mb );
			else
				snprintf( out, n, "%.1f GB", mb / 1024.0 );
		};

		char memBuf[64], stbyBuf[32], modBuf[32], commBuf[64], botsBuf[64];
		char memUsed[16], memTot[16], commUsed[16], commTot[16];
		fmtGB( s_stats.totalPhysMB - s_stats.availPhysMB, memUsed, sizeof( memUsed ) );
		fmtGB( s_stats.totalPhysMB,                       memTot,  sizeof( memTot  ) );
		fmtGB( s_stats.commitUsedMB,                      commUsed, sizeof( commUsed ) );
		fmtGB( s_stats.commitLimitMB,                     commTot,  sizeof( commTot  ) );
		snprintf( memBuf,  sizeof( memBuf  ), "%s / %s", memUsed, memTot );
		fmtGB( s_stats.standbyMB,  stbyBuf, sizeof( stbyBuf ) );
		fmtGB( s_stats.modifiedMB, modBuf,  sizeof( modBuf  ) );
		snprintf( commBuf, sizeof( commBuf ), "%s / %s", commUsed, commTot );
		uint64_t botsMb = s_botsWsBytes / ( 1024ULL * 1024ULL );
		char botsVal[16];
		fmtGB( botsMb, botsVal, sizeof( botsVal ) );
		snprintf( botsBuf, sizeof( botsBuf ), "%s/%d", botsVal, s_botInstCount );

		const float btnH   = 22.f;
		const float btnGap = 6.f;
		// Адаптивные ширины: узкое окно → короткие label'ы и узкие кнопки.
		bool   tight  = ( innerW < 720.f );
		float  trimW  = tight ? 80.f  : 100.f;
		float  setW   = tight ? 90.f  : 110.f;
		float  buttonsLeft = innerX + innerW - trimW - btnGap - setW;
		float  stripLimit  = buttonsLeft - 12.f;   // не доходить до кнопок

		// helper: рисует "LABEL VALUE", возвращает следующий x. Skip'ает если
		// не помещается (защита от перекрытия с кнопками).
		auto drawKv = [&]( float& x, const char* lab, const char* val, ImU32 valCol ) {
			ImVec2 ls = fSm->CalcTextSizeA( fSmZ, FLT_MAX, 0, lab );
			ImVec2 vs = fSm->CalcTextSizeA( fSmZ, FLT_MAX, 0, val );
			float endX = x + ls.x + 6 + vs.x + 18;
			if ( endX > stripLimit ) return;
			dl->AddText( fSm, fSmZ, ImVec2( x, y3 + 4.f ), kColInkMute, lab );
			dl->AddText( fSm, fSmZ, ImVec2( x + ls.x + 6, y3 + 4.f ), valCol, val );
			x = endX;
		};

		float x = innerX;
		drawKv( x, "MEM",  memBuf,  kColInkBright );
		drawKv( x, "STBY", stbyBuf, kColGold );      // gold = reclaimable
		drawKv( x, "MOD",  modBuf,  kColInk );
		drawKv( x, "COMM", commBuf, kColInk );
		drawKv( x, "BOTS", botsBuf, kColInkBright );

		// TRIM BOTS button.
		ImGui::SetCursorScreenPos( ImVec2( buttonsLeft, y3 ) );
		bool haveBotPids = ( s_botInstCount > 0 );
		const char* trimLabel = tight ? "TRIM" : "TRIM BOTS";
		if ( ChamferedButton( trimLabel, ImVec2( trimW, btnH ),
			kColBg1, kColCrashDim, haveBotPids ? kColInk : kColInkMute, false ) )
		{
			if ( haveBotPids )
			{
				int trimmed = 0;
				for ( int i = 0; i < orch.GetBotCount(); i++ )
				{
					const auto& b = orch.GetBot( i );
					if ( b.dotaPid )  trimmed += DotaLauncher::TrimWorkingSetTree( b.dotaPid );
					if ( b.steamPid ) trimmed += DotaLauncher::TrimWorkingSetTree( b.steamPid );
				}
				char msg[96];
				snprintf( msg, sizeof( msg ),
					"[mem-reclaim/trim-bots] EmptyWorkingSet trimmed %d processes "
					"(может вызвать stutter в dota!)", trimmed );
				orch.LogPublic( msg );
			}
		}
		if ( ImGui::IsItemHovered() )
		{
			ImGui::SetTooltip(
				"Fallback: per-process EmptyWorkingSet только на dota+steam деревья.\n"
				"ВНИМАНИЕ: вызывает визуальный фриз/stutter в Dota'е — активные\n"
				"текстуры/шейдеры принудительно выгружаются и re-page-ятся с диска.\n"
				"Используй только если RECLAIM NOW не помог.\n"
				"%s",
				haveBotPids ? "" : "(нет активных bot процессов)" );
		}

		// + SETTINGS toggle — explicit position, NO SameLine (после SetCursorScreenPos фрагильно).
		ImGui::SetCursorScreenPos( ImVec2( buttonsLeft + trimW + btnGap, y3 ) );
		const char* setLabel;
		if ( tight )
			setLabel = s_showSettings ? "- TUNE" : "+ TUNE";
		else
			setLabel = s_showSettings ? "- SETTINGS" : "+ SETTINGS";
		if ( ChamferedButton( setLabel, ImVec2( setW, btnH ),
			kColBg1, kColLineHot, kColInk, false ) )
		{
			s_showSettings = !s_showSettings;
		}
	}

	// Cursor → ниже outer chassis, готов к следующему контенту parent'а.
	ImGui::SetCursorScreenPos( ImVec2( outerMn.x, outerMn.y + panelH + 8.f ) );

	// ───────────────────────────────────────────────────────────
	// Collapsible settings sub-panel — 3-колоночный grid через
	// абсолютные X (НЕ SameLine(N) — там window-relative offset
	// который в parent с padding'ом съезжает).
	// ───────────────────────────────────────────────────────────
	if ( s_showSettings )
	{
		// Адаптивный grid: ≥620px inner = 3 колонки (3 ряда), иначе 2 колонки
		// (с дополнительными рядами под numerics). Высота панели подбирается
		// под layout — не overflow'нет за свои границы.
		bool   wideGrid = ( innerW >= 620.f );
		int    cols     = wideGrid ? 3 : 2;
		int    rows     = wideGrid ? 3 : 4;
		float  rowGap   = 24.f;
		float  setH     = padY + fSmZ + 8.f + rows * rowGap + padY;

		ImVec2 setMn = ImGui::GetCursorScreenPos();
		ImVec2 setMx( setMn.x + availW, setMn.y + setH );
		DrawChamferedRect( dl, setMn, setMx, 8.f, kColBg2 );
		DrawChamferedRect( dl, setMn, setMx, 8.f, kColLine, 1.f );

		// Header
		{
			const char* h = "[-- RECLAIM MODES";
			dl->AddText( fSm, fSmZ, ImVec2( setMn.x + padX, setMn.y + padY ),
				kColGoldDeep, h );
			ImVec2 hs = fSm->CalcTextSizeA( fSmZ, FLT_MAX, 0, h );
			float lineY = setMn.y + padY + fSmZ * 0.5f + 1;
			dl->AddLine(
				ImVec2( setMn.x + padX + hs.x + 8.f, lineY ),
				ImVec2( setMx.x - padX,              lineY ),
				kColLineHot, 1.f );
		}

		if ( gFontMonoSm ) ImGui::PushFont( gFontMonoSm );

		float gridX0 = setMn.x + padX;
		float gridY0 = setMn.y + padY + fSmZ + 8.f;
		float colW   = ( availW - padX * 2 ) / cols;

		auto setCol = [&]( int col, int row ) {
			ImGui::SetCursorScreenPos( ImVec2(
				gridX0 + col * colW, gridY0 + row * rowGap ) );
		};

		// helper: input/slider c label РЯДОМ (а не справа от widget'а)
		// чтобы не вылезать за границу колонки. Returns true on change.
		auto numericControl = [&]( const char* labelText, const char* uniqueId,
			bool isSlider, int& value, int vmin, int vmax ) -> bool
		{
			ImVec2 pos = ImGui::GetCursorScreenPos();
			ImVec2 lsz = fSm->CalcTextSizeA( fSmZ, FLT_MAX, 0, labelText );
			// Лейбл слева, у baseline'а виджета.
			dl->AddText( fSm, fSmZ,
				ImVec2( pos.x, pos.y + 3.f ), kColInkMute, labelText );
			ImGui::SetCursorScreenPos( ImVec2( pos.x + lsz.x + 8.f, pos.y ) );
			float widgetW = colW - lsz.x - 16.f;
			if ( widgetW < 60.f ) widgetW = 60.f;
			ImGui::SetNextItemWidth( widgetW );
			bool changed;
			if ( isSlider )
				changed = ImGui::SliderInt( uniqueId, &value, vmin, vmax );
			else
				changed = ImGui::InputInt( uniqueId, &value, 0, 0 );
			return changed;
		};

		// ── Column 0: безопасные операции ──
		setCol( 0, 0 );
		ImGui::Checkbox( "Purge standby", &mr.memReclaimPurgeStandby );
		setCol( 0, 1 );
		ImGui::Checkbox( "Purge low-prio", &mr.memReclaimPurgeLowPrioStandby );
		setCol( 0, 2 );
		ImGui::Checkbox( "Combine pages", &mr.memReclaimCombinePages );
		if ( ImGui::IsItemHovered() )
			ImGui::SetTooltip( "Win10+ only. Дедуплицирует идентичные PFN —\n"
				"для 5xdota2.exe с одинаковым client.dll даёт GB экономии." );

		// ── Column 1: ⚠ опасные + master switch ──
		setCol( 1, 0 );
		ImGui::PushStyleColor( ImGuiCol_Text, V( kColWarn ) );
		ImGui::Checkbox( "Flush modified", &mr.memReclaimFlushModified );
		ImGui::PopStyleColor();
		if ( ImGui::IsItemHovered() )
			ImGui::SetTooltip( "WARN: disk pressure (50-500ms write burst).\n"
				"Включай только при ОЧЕНЬ высокой modified count (>500 MB)." );

		setCol( 1, 1 );
		ImGui::PushStyleColor( ImGuiCol_Text, V( kColCrash ) );
		ImGui::Checkbox( "Empty WS (stutter!)", &mr.memReclaimEmptyAllWorkingSets );
		ImGui::PopStyleColor();
		if ( ImGui::IsItemHovered() )
			ImGui::SetTooltip( "WARN: system-wide EmptyWorkingSet.\n"
				"Вызывает визуальный stutter в Dota'е — активные страницы\n"
				"принудительно выгружаются. Включай ТОЛЬКО если ничего другое\n"
				"не освобождает достаточно RAM." );

		setCol( 1, 2 );
		ImGui::Checkbox( "Master enabled", &mr.memReclaimEnabled );
		if ( ImGui::IsItemHovered() )
			ImGui::SetTooltip( "Master switch: OFF = periodic reclaim отключен,\n"
				"но кнопки RECLAIM / TRIM продолжают работать." );

		// ── Numerics: column 2 в wide режиме / column 0-1 row 3 в tight ──
		if ( wideGrid )
		{
			setCol( 2, 0 );
			numericControl( "AUTO%", "##autopct", true,
				mr.memReclaimAutoThreshold, 0, 99 );
			if ( ImGui::IsItemHovered() )
				ImGui::SetTooltip( "Auto-trigger threshold. 0 = выкл.\n"
					"Если memory load >= threshold — reclaim запускается\n"
					"независимо от interval." );

			setCol( 2, 1 );
			numericControl( "EVERY", "##evsec", false,
				mr.memReclaimIntervalS, 10, 3600 );
			if ( ImGui::IsItemHovered() )
				ImGui::SetTooltip( "Периодический тик reclaim, сек (clamp 10-3600)." );
		}
		else
		{
			// Tight: numerics на 4-й строке через всю ширину (2 cols)
			setCol( 0, 3 );
			numericControl( "AUTO%", "##autopct", true,
				mr.memReclaimAutoThreshold, 0, 99 );
			setCol( 1, 3 );
			numericControl( "EVERY", "##evsec", false,
				mr.memReclaimIntervalS, 10, 3600 );
		}
		if ( mr.memReclaimIntervalS < 10 )   mr.memReclaimIntervalS = 10;
		if ( mr.memReclaimIntervalS > 3600 ) mr.memReclaimIntervalS = 3600;

		if ( gFontMonoSm ) ImGui::PopFont();

		ImGui::SetCursorScreenPos( ImVec2( setMn.x, setMn.y + setH + 8.f ) );
	}
}

// ═══════════════════════════════════════════════════════════
// T10: Pair Code + Sync Start modal dialogs
// ═══════════════════════════════════════════════════════════
//
// Triggered by s_show*PairCodeDialog flags (set by T9 Pairing Panel buttons)
// and by SyncStartCoordinator state (auto for responder-side accept/decline).
// Inline render via OpenPopup/BeginPopupModal — single shot per flag-edge.

static void RenderPairCodeGenerateModal_( Orchestrator& orch )
{
	static bool wasOpened = false;
	static std::string s_code;

	if ( !s_showGeneratePairCodeDialog )
	{
		if ( wasOpened )
		{
			wasOpened = false;
			s_code.clear();
		}
		return;
	}

	if ( !wasOpened )
	{
		ImGui::OpenPopup( "Generate Pair Code" );
		wasOpened = true;
		s_code = orch.GenerateCurrentPairCode();
	}

	ImGui::SetNextWindowSize( ImVec2( 600, 280 ), ImGuiCond_Always );
	if ( ImGui::BeginPopupModal( "Generate Pair Code", nullptr,
			ImGuiWindowFlags_NoResize ) )
	{
		ImGui::TextWrapped( "Передайте этот код вашему партнёру. "
			"После Paste у партнёра — оба orchestrator подключатся к одному pair." );
		ImGui::Spacing();

		if ( s_code.empty() )
		{
			ImGui::TextColored( theme::V( theme::kColCrash ),
				"ERROR: pairing config неполный.\n"
				"В farm.json должны быть выставлены relay_host, user_id,\n"
				"user_auth_token, pair_id, pair_secret.\n"
				"Если у вас новая установка — попросите код от Master orchestrator'а." );
		}
		else
		{
			ImGui::PushItemWidth( -1 );
			char buf[512];
			strncpy_s( buf, sizeof( buf ), s_code.c_str(), _TRUNCATE );
			ImGui::InputTextMultiline( "##paircode", buf, sizeof( buf ),
				ImVec2( -1, 60 ), ImGuiInputTextFlags_ReadOnly );
			ImGui::PopItemWidth();

			ImGui::Spacing();
			if ( theme::ChamferedButton( "COPY", ImVec2( 120, 28 ),
					theme::kColGold, theme::kColGold, theme::kColBg0, true ) )
			{
				if ( theme::CopyToClipboard( s_code ) )
					orch.LogPublic( "[pairing] pair code copied to clipboard" );
			}
			ImGui::SameLine();
			if ( theme::ChamferedButton( "REGENERATE", ImVec2( 140, 28 ),
					theme::kColWarn, theme::kColWarn, theme::kColBg0, true ) )
			{
				// Phase 2 будет ротейтить pair_secret; пока — re-encode existing.
				s_code = orch.GenerateCurrentPairCode();
			}
		}

		ImGui::Spacing();
		ImGui::TextColored( theme::V( theme::kColWarn ),
			"WARN: anyone with this code can join your relay pair.\n"
			"Share только через private channel." );

		ImGui::Spacing();
		if ( theme::ChamferedButton( "CLOSE", ImVec2( 100, 28 ),
				theme::kColIdle, theme::kColIdle, theme::kColBg0, true ) )
		{
			s_showGeneratePairCodeDialog = false;
			wasOpened = false;
			s_code.clear();
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
	}
}

static void RenderPairCodePasteModal_( Orchestrator& orch )
{
	static bool wasOpened = false;
	static char s_inputBuf[512] = {0};
	static std::string s_lastError;
	static pair_code::DecodeResult s_decoded;
	static bool s_decodedOk = false;
	static std::string s_lastDecodedInput;

	if ( !s_showPastePairCodeDialog )
	{
		if ( wasOpened )
		{
			wasOpened = false;
			s_inputBuf[0] = 0;
			s_lastError.clear();
			s_decodedOk = false;
			s_lastDecodedInput.clear();
		}
		return;
	}

	if ( !wasOpened )
	{
		ImGui::OpenPopup( "Paste Pair Code" );
		wasOpened = true;
		s_inputBuf[0] = 0;
		s_lastError.clear();
		s_decodedOk = false;
		s_lastDecodedInput.clear();
	}

	ImGui::SetNextWindowSize( ImVec2( 600, 380 ), ImGuiCond_Always );
	if ( ImGui::BeginPopupModal( "Paste Pair Code", nullptr,
			ImGuiWindowFlags_NoResize ) )
	{
		ImGui::TextWrapped( "Paste код полученный от партнёра." );
		ImGui::Spacing();

		ImGui::PushItemWidth( -1 );
		ImGui::InputTextMultiline( "##paste", s_inputBuf, sizeof( s_inputBuf ),
			ImVec2( -1, 60 ) );
		ImGui::PopItemWidth();

		// Re-decode только если input изменился (избегаем re-parse каждый frame).
		if ( s_inputBuf[0] != 0 && s_inputBuf != s_lastDecodedInput )
		{
			s_lastDecodedInput = s_inputBuf;
			s_decoded = pair_code::Decode( s_inputBuf );
			s_decodedOk = s_decoded.ok;
			s_lastError = s_decodedOk ? "" : pair_code::Describe( s_decoded.error );
		}

		ImGui::Spacing();
		if ( s_inputBuf[0] != 0 )
		{
			if ( s_decodedOk )
			{
				ImGui::TextColored( theme::V( theme::kColSignal ),
					"OK: code valid:" );
				ImGui::Text( "  Relay: %s:%u",
					s_decoded.decoded.relayHost.c_str(),
					(unsigned)s_decoded.decoded.relayPort );
				ImGui::Text( "  User:  %s   Pair: %s",
					s_decoded.decoded.userId.c_str(),
					s_decoded.decoded.pairId.c_str() );
				const char* assigned =
					( s_decoded.decoded.roleHint == "M" ) ? "master" : "slave";
				ImGui::Text( "  Role:  %s", assigned );
				if ( s_decoded.decoded.ttlSeconds > 0 )
					ImGui::Text( "  TTL:   %d s", s_decoded.decoded.ttlSeconds );
			}
			else
			{
				ImGui::TextColored( theme::V( theme::kColCrash ),
					"ERROR: %s", s_lastError.c_str() );
			}
		}

		ImGui::Spacing();

		const bool canApply = s_decodedOk;
		const ImU32 applyFill   = canApply ? theme::kColGold    : theme::kColBg2;
		const ImU32 applyBorder = canApply ? theme::kColGold    : theme::kColLineHot;
		const ImU32 applyText   = canApply ? theme::kColBg0     : theme::kColInkMute;
		if ( theme::ChamferedButton( "APPLY & CONNECT", ImVec2( 180, 32 ),
				applyFill, applyBorder, applyText, canApply ) )
		{
			if ( canApply )
			{
				if ( orch.ApplyPairCodeAndReinit( s_decoded.decoded ) )
				{
					orch.LogPublic( "[pairing] pair code applied + reinit OK" );
					s_showPastePairCodeDialog = false;
					wasOpened = false;
					s_inputBuf[0] = 0;
					s_lastError.clear();
					s_decodedOk = false;
					s_lastDecodedInput.clear();
					ImGui::CloseCurrentPopup();
				}
				else
				{
					s_lastError = "Apply failed — see orchestrator log";
					orch.LogPublic( "[pairing] pair code apply FAILED" );
				}
			}
		}
		ImGui::SameLine();
		if ( theme::ChamferedButton( "CANCEL", ImVec2( 120, 32 ),
				theme::kColIdle, theme::kColIdle, theme::kColBg0, true ) )
		{
			s_showPastePairCodeDialog = false;
			wasOpened = false;
			s_inputBuf[0] = 0;
			s_lastError.clear();
			s_decodedOk = false;
			s_lastDecodedInput.clear();
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
	}
}

static void RenderSyncStartModal_( Orchestrator& orch )
{
	static bool wasOpened = false;

	auto ps = orch.GetPairingStatus();
	const bool show = ( ps.syncStart.state == SyncStartState::PEER_REQUESTED );

	if ( !show )
	{
		if ( wasOpened )
		{
			wasOpened = false;  // coordinator advanced state → drop popup
		}
		return;
	}

	if ( !wasOpened )
	{
		ImGui::OpenPopup( "Peer Wants To Start Farming" );
		wasOpened = true;
	}

	ImGui::SetNextWindowSize( ImVec2( 520, 220 ), ImGuiCond_Always );
	if ( ImGui::BeginPopupModal( "Peer Wants To Start Farming", nullptr,
			ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove ) )
	{
		ImGui::TextWrapped( "Партнёр (%s) готов запустить ферму.",
			ps.syncStart.peerRole.empty() ? "peer" : ps.syncStart.peerRole.c_str() );
		ImGui::Spacing();

		const int64_t now      = (int64_t)GetTickCount64();
		const int64_t deadline = ps.syncStart.ackDeadlineMs;
		const int secsLeft = ( deadline > 0 )
			? (int)( ( deadline - now ) / 1000 )
			: -1;
		if ( secsLeft >= 0 )
			ImGui::Text( "Auto-decline через %d секунд...", secsLeft );

		ImGui::Spacing();
		ImGui::Spacing();

		if ( theme::ChamferedButton( "ACCEPT & START", ImVec2( 200, 36 ),
				theme::kColSignal, theme::kColSignal, theme::kColBg0, true ) )
		{
			orch.SyncStartUserAccept();
		}
		ImGui::SameLine();
		if ( theme::ChamferedButton( "DECLINE", ImVec2( 130, 36 ),
				theme::kColCrash, theme::kColCrash, theme::kColBg0, true ) )
		{
			orch.SyncStartUserDecline( "user_declined" );
		}
		ImGui::EndPopup();
	}
}

// ═══════════════════════════════════════════════════════════
// SCREEN 3: Dashboard
// ═══════════════════════════════════════════════════════════

static void RenderDashboard( Orchestrator& orch )
{
	ImGui::SetNextWindowPos( { 0, 0 } );
	RECT rc;
	GetClientRect( g_hwnd, &rc );
	ImGui::SetNextWindowSize( { (float)rc.right, (float)rc.bottom } );

	// Atmospheric backdrop — dotted engineering grid + faint scanlines.
	{
		ImDrawList* bdl = ImGui::GetBackgroundDrawList();
		ImVec2 mn( 0, 0 ), mx( (float)rc.right, (float)rc.bottom );
		theme::DrawDottedGrid( bdl, mn, mx,
			IM_COL32( 0xd4, 0xa1, 0x4a, 0x10 ), 28.f );
		theme::DrawScanlines( bdl, mn, mx,
			IM_COL32( 0xd4, 0xa1, 0x4a, 0x04 ), 3 );
	}

	ImGui::PushStyleColor( ImGuiCol_WindowBg, ImVec4( 0.039f, 0.035f, 0.027f, 1.f ) );
	ImGui::Begin( "##main", nullptr,
		ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
		ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse );

	// ── Title bar — DOTA·FARM wordmark + version pill + status pills ──
	{
		float tbY = ImGui::GetCursorPosY();
		if ( theme::gFontSerifLg ) ImGui::PushFont( theme::gFontSerifLg );
		ImGui::TextColored( theme::V( theme::kColInkBright ), "DOTA" );
		ImGui::SameLine( 0, 0 );
		ImGui::TextColored( theme::V( theme::kColGold ),      "FARM" );
		if ( theme::gFontSerifLg ) ImGui::PopFont();

		ImGui::SameLine();
		ImVec2 vpMn = ImGui::GetCursorScreenPos();
		vpMn.y += 6.f;
		ImGui::Dummy( ImVec2( 132, 22 ) );
		if ( theme::gFontMonoSm ) ImGui::PushFont( theme::gFontMonoSm );
		ImGui::GetWindowDrawList()->AddRect(
			vpMn, ImVec2( vpMn.x + 132, vpMn.y + 22 ),
			theme::kColLineHot, 0.f, 0, 1.f );
		ImGui::GetWindowDrawList()->AddText(
			ImVec2( vpMn.x + 7, vpMn.y + 5 ),
			theme::kColGold, "v2026.04.29.b4138" );
		if ( theme::gFontMonoSm ) ImGui::PopFont();

		// Right-side: state pill — color reflects orchestrator state
		ImVec4 stateCol;
		ImU32  stateDot;
		switch ( orch.GetState() )
		{
		case Orchestrator::State::RUNNING:    stateCol = theme::V( theme::kColSignal ); stateDot = theme::kColSignal; break;
		case Orchestrator::State::IDLE:       stateCol = theme::V( theme::kColInkDim ); stateDot = theme::kColIdle;   break;
		case Orchestrator::State::ERROR_STATE:stateCol = theme::V( theme::kColCrash );  stateDot = theme::kColCrash;  break;
		default:                              stateCol = theme::V( theme::kColWarn );   stateDot = theme::kColWarn;   break;
		}
		char pill[64];
		snprintf( pill, sizeof( pill ), "  %s", orch.GetStateStr() );
		float pillW = ImGui::CalcTextSize( pill ).x + 28.f;
		ImGui::SameLine( ImGui::GetWindowContentRegionMax().x - pillW );
		ImVec2 pMn = ImGui::GetCursorScreenPos();
		pMn.y += 4.f;
		ImGui::Dummy( ImVec2( pillW, 22 ) );
		ImDrawList* dl = ImGui::GetWindowDrawList();
		dl->AddRectFilled( pMn, ImVec2( pMn.x + pillW, pMn.y + 22 ), theme::kColBg1 );
		dl->AddRect      ( pMn, ImVec2( pMn.x + pillW, pMn.y + 22 ), theme::kColLineHot, 0.f, 0, 1.f );
		theme::DrawLED( dl, ImVec2( pMn.x + 12, pMn.y + 11 ), 4.f, stateDot,
			(float)( ImGui::GetTime() * 0.6 ), false );
		if ( theme::gFontMonoSm ) ImGui::PushFont( theme::gFontMonoSm );
		dl->AddText( ImVec2( pMn.x + 24, pMn.y + 5 ), ImGui::GetColorU32( stateCol ),
			orch.GetStateStr() );
		if ( theme::gFontMonoSm ) ImGui::PopFont();

		// Hairline below title.
		ImVec2 lMn = ImGui::GetCursorScreenPos();
		lMn.y += 6.f;
		float ww = ImGui::GetWindowContentRegionMax().x - ImGui::GetWindowContentRegionMin().x;
		dl->AddLine( lMn, ImVec2( lMn.x + ww, lMn.y ), theme::kColLine, 1.f );
		ImGui::Dummy( ImVec2( ww, 12 ) );
		(void)tbY;
	}

	// ── PC STATS panel (Bundle MEMRED) — telemetry banner ──
	// Permanent diagnostic readout, sits выше command bar чтобы load% и
	// reclaim controls были видны на каждом frame'е без скролла.
	RenderPcStatsPanel( orch );

	auto state = orch.GetState();
	bool isIdle = ( state == Orchestrator::State::IDLE );
	bool isRunning = ( state == Orchestrator::State::RUNNING );
	bool isBusy = orch.IsBusy();

	// ── Command bar — chamfered tarnished-gold buttons in a row ──
	if ( isIdle && !isBusy )
	{
		// Pairing-aware START FARM (uxV2). When uxV2+enabled — gate the button
		// on peer connectivity (recent hb < 5s) and route через GuardedStartFarm
		// для sync-start handshake. Legacy path (uxV2 off) — direct StartFarm.
		const auto& cfg = orch.GetConfig();
		const bool pairingGate = cfg.pairing.uxV2 && cfg.pairing.enabled;
		auto psBtn = orch.GetPairingStatus();
		const bool pairingReady = !pairingGate ||
			( psBtn.connected && psBtn.lastPeerHbAgeMs >= 0 && psBtn.lastPeerHbAgeMs < 5000 );
		const bool startFarmEnabled = pairingReady;
		const char* startBtnLabel = pairingGate ? "START FARM (sync)" : "START FARM";
		const ImVec2 startBtnSize = pairingGate ? ImVec2( 170, 32 ) : ImVec2( 150, 32 );
		const ImU32 sbFill   = startFarmEnabled ? theme::kColGold    : theme::kColBg2;
		const ImU32 sbBorder = startFarmEnabled ? theme::kColGold    : theme::kColLineHot;
		const ImU32 sbText   = startFarmEnabled ? theme::kColBg0     : theme::kColInkMute;
		if ( theme::ChamferedButton( startBtnLabel, startBtnSize,
				sbFill, sbBorder, sbText, startFarmEnabled ) )
		{
			if ( startFarmEnabled )
			{
				if ( pairingGate )
					orch.GuardedStartFarm();
				else
					orch.StartFarm();
			}
		}
		if ( !startFarmEnabled && pairingGate && ImGui::IsItemHovered() )
		{
			ImGui::SetTooltip( "Peer не подключён или связь устарела (>5s).\n"
				"Скажите второму оператору запустить orchestrator,\n"
				"или нажмите RECONNECT в Pairing panel." );
		}
	}
	else if ( isRunning && !isBusy )
	{
		if ( theme::ChamferedButton( "STOP FARM", ImVec2( 150, 32 ),
				theme::kColCrashDim, theme::kColCrashDim, IM_COL32( 0xff, 0xff, 0xff, 0xFF ), true ) )
			orch.StopFarm();
	}
	else
	{
		theme::ChamferedButton( orch.GetStateStr(), ImVec2( 150, 32 ),
			theme::kColBg2, theme::kColLineHot, theme::kColInkMute, false );
	}

	// Global Pause/Resume - freezes DLL state machine via shared flag file
	ImGui::SameLine();
	bool paused = IsPauseFlagSet();
	if ( paused )
	{
		if ( theme::ChamferedButton( "RESUME ALL", ImVec2( 130, 32 ),
				theme::kColWarn, theme::kColWarn, theme::kColBg0, true ) )
			SetPauseFlag( false );
	}
	else
	{
		if ( theme::ChamferedButton( "PAUSE ALL", ImVec2( 130, 32 ),
				theme::kColBg1, theme::kColLineHot, theme::kColInk, false ) )
			SetPauseFlag( true );
	}

	// Reload Lua — перезагружает скрипты на всех live ботах одновременно.
	ImGui::SameLine();
	if ( theme::ChamferedButton( "RELOAD LUA", ImVec2( 130, 32 ),
			theme::kColBg1, theme::kColQueueDim, theme::kColQueue, false ) )
	{
		int n = TriggerReloadAll( orch );
		char msg[128];
		snprintf( msg, sizeof( msg ), "Reload Lua: flag sent to %d bot(s)", n );
		orch.LogPublic( msg );
	}
	if ( ImGui::IsItemHovered() )
		ImGui::SetTooltip( "Перезагрузить Lua скрипты во всех активных dota2.exe (эквивалент F7 в окне Dota)" );

	// Tile Now — раскладка окон Dota по сетке. Combo + checkboxes под кнопкой.
	ImGui::SameLine();
	{
		auto& cfgMut = orch.GetConfigMut();
		bool haveBots = false;
		for ( int i = 0; i < orch.GetBotCount(); i++ )
			if ( orch.GetBot( i ).dotaPid ) { haveBots = true; break; }

		if ( theme::ChamferedButton( "TILE NOW", ImVec2( 110, 32 ),
				theme::kColBg1, theme::kColLineHot, theme::kColInk, haveBots ) )
		{
			if ( haveBots )
				TileNow( orch );
		}
		if ( ImGui::IsItemHovered() )
		{
			ImGui::SetTooltip(
				"Разложить все живые dota2.exe окна по выбранному layout.\n"
				"Layout / Force-windowed / Auto-on-launch — настройки ниже.\n"
				"%s",
				haveBots ? "" : "(нет активных dota2.exe)" );
		}
		(void)cfgMut;
	}

	// Apply / Revert minifier на лету. Force-override всех JSON-флагов
	// (autoexec/video.txt/launch options/VPK). Запускается в bg thread —
	// VPK apply 10-30s subprocess не должен замораживать GUI.
	ImGui::SameLine();
	{
		const bool applied = orch.IsMinifierAppliedToAnyBot();
		const bool busy = orch.IsMinifierBusy();
		const bool initialized = orch.IsInitialized();
		const char* label;
		if ( busy )
			label = applied ? "REVERTING..." : "APPLYING...";
		else
			label = applied ? "REVERT MINIFY" : "APPLY MINIFY";
		const bool clickable = initialized && !busy;
		ImU32 hot = applied ? theme::kColCrash : theme::kColLineHot;
		if ( theme::ChamferedButton( label, ImVec2( 140, 32 ),
				theme::kColBg1, hot, theme::kColInk, clickable ) )
		{
			if ( applied )
				orch.RevertMinifierAll();
			else
				orch.ApplyMinifierAll();
		}
		if ( ImGui::IsItemHovered() )
		{
			if ( !initialized )
			{
				ImGui::SetTooltip(
					"Setup ещё не завершён.\n"
					"Сначала добавь аккаунты в Setup wizard." );
			}
			else if ( busy )
			{
				ImGui::SetTooltip(
					"Minifier subprocess работает (VPK apply/revert до 30 сек).\n"
					"Прогресс — в логе." );
			}
			else if ( applied )
			{
				ImGui::SetTooltip(
					"Откатить ВСЁ что было применено:\n"
					"  • autoexec.cfg / video.txt — restore backup\n"
					"  • launch options — restore original\n"
					"  • VPK pak'и — revert pak01_dir.vpk\n"
					"Идемпотентно. VPK revert ~10-30s." );
			}
			else
			{
				ImGui::SetTooltip(
					"Применить полный minify ко всем enabled-аккаунтам:\n"
					"  • autoexec.cfg — низкие текстуры, отключить эффекты\n"
					"  • video.txt — FPS/resolution из farm.json\n"
					"  • launch options — -high -nojoy и т.п.\n"
					"  • VPK pak'и — minimal_visuals preset (по умолчанию)\n"
					"\n"
					"VPK apply ~10-30s (subprocess). Конфиг подхватится при\n"
					"следующем спавне dota2.exe. Все JSON-флаги force-override." );
			}
		}
	}

	ImGui::SameLine();
	auto& cfg = orch.GetConfig();
	int enabledCount = 0;
	for ( const auto& a : cfg.accounts ) if ( a.enabled ) enabledCount++;

	// Telemetry kv strip — uppercase mono labels with bright values, mockup .control-status feel
	if ( theme::gFontMonoSm ) ImGui::PushFont( theme::gFontMonoSm );
	ImGui::AlignTextToFramePadding();
	ImGui::TextColored( theme::V( theme::kColInkMute ), "BOTS" );
	ImGui::SameLine( 0, 4 );
	ImGui::TextColored( theme::V( theme::kColInkBright ), "%d/%d", enabledCount, orch.GetBotCount() );
	ImGui::SameLine( 0, 16 );
	ImGui::TextColored( theme::V( theme::kColInkMute ), "REGION" );
	ImGui::SameLine( 0, 4 );
	ImGui::TextColored( theme::V( theme::kColGold ), "0x%X", cfg.region );

	if ( isRunning )
	{
		ImGui::SameLine( 0, 16 );
		auto el = std::chrono::steady_clock::now() - orch.GetStartTime();
		auto m = std::chrono::duration_cast<std::chrono::minutes>( el ).count();
		ImGui::TextColored( theme::V( theme::kColInkMute ), "GAMES" );
		ImGui::SameLine( 0, 4 );
		ImGui::TextColored( theme::V( theme::kColInkBright ), "%d", orch.GetTotalGames() );
		ImGui::SameLine( 0, 16 );
		ImGui::TextColored( theme::V( theme::kColInkMute ), "UPTIME" );
		ImGui::SameLine( 0, 4 );
		ImGui::TextColored( theme::V( theme::kColInkBright ), "%lldh%02lldm", m / 60, m % 60 );
	}
	if ( theme::gFontMonoSm ) ImGui::PopFont();

	// Reset Accounts — only when idle.
	// Responsive: в широких окнах прижимается справа (SameLine с absolute X);
	// в узких — переносится на новую строку, чтобы не наезжать на TILE NOW.
	// Без этой проверки SameLine( winW - 140 ) может дать X МЕНЬШЕ чем правый
	// край предыдущей кнопки и кнопки перекрываются.
	if ( isIdle && !isBusy )
	{
		const float btnW       = 130.f;
		const float rightPad   = 10.f;
		float lastItemRight    = ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x;
		float targetX          = ImGui::GetWindowWidth() - btnW - rightPad;
		if ( targetX > lastItemRight + 8.f )
			ImGui::SameLine( targetX );
		// иначе — natural new-line flow, кнопка пойдёт под строкой действий
		ImGui::PushStyleColor( ImGuiCol_Button, ImVec4( 0.5f, 0.15f, 0.15f, 1.f ) );
		ImGui::PushStyleColor( ImGuiCol_ButtonHovered, ImVec4( 0.7f, 0.2f, 0.2f, 1.f ) );
		if ( ImGui::Button( "Reset Accounts", { btnW, 28 } ) )
		{
			// Delete accounts.json
			std::string accPath = g_exeDir + "\\config\\accounts.json";
			DeleteFileA( accPath.c_str() );

			// Delete C:\BotSteam\* (per-bot Steam dirs)
			for ( int i = 0; i < 10; i++ )
			{
				char cmd[256];
				snprintf( cmd, sizeof( cmd ), "cmd.exe /c rmdir /s /q C:\\BotSteam\\%d", i );
				STARTUPINFOA si{}; si.cb = sizeof( si );
				si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
				PROCESS_INFORMATION pi{};
				if ( CreateProcessA( nullptr, cmd, nullptr, nullptr,
					FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi ) )
				{
					WaitForSingleObject( pi.hProcess, 5000 );
					CloseHandle( pi.hProcess );
					CloseHandle( pi.hThread );
				}
			}

			// Reset GUI state → back to Setup
			s_setupDone = false;
			s_inSteamLogin = false;
			s_editorInited = false;
			s_nLogins = 0;
			s_setupStep = 0;
			s_setupSteamPid = 0;
			memset( s_logins, 0, sizeof( s_logins ) );
		}
		ImGui::PopStyleColor( 2 );
	}

	ImGui::Spacing();

	// ── Window tile controls (layout combo + force-windowed + auto-on-launch) ──
	{
		auto& cfgMut = orch.GetConfigMut();

		if ( theme::gFontMonoSm ) ImGui::PushFont( theme::gFontMonoSm );
		ImGui::TextColored( theme::V( theme::kColInkMute ), "TILE" );
		if ( theme::gFontMonoSm ) ImGui::PopFont();
		ImGui::SameLine( 0, 8 );

		const char* kLayoutItems[] = { "Grid 3+2", "Grid 2+2+1", "Strip 1x5", "Cascade" };
		static const char* kLayoutKeys[] = { "grid_3_2", "grid_2_2_1", "strip_5", "cascade" };
		int curLayout = 0;
		for ( int i = 0; i < 4; i++ )
			if ( cfgMut.tileLayout == kLayoutKeys[i] ) { curLayout = i; break; }

		ImGui::SetNextItemWidth( 140 );
		if ( ImGui::Combo( "##tilelayout", &curLayout, kLayoutItems, 4 ) )
			cfgMut.tileLayout = kLayoutKeys[curLayout];
		if ( ImGui::IsItemHovered() )
			ImGui::SetTooltip( "Layout для TILE NOW и Auto-tile-on-launch" );

		ImGui::SameLine( 0, 12 );
		ImGui::Checkbox( "Force windowed", &cfgMut.tileForceWindowed );
		if ( ImGui::IsItemHovered() )
			ImGui::SetTooltip(
				"Перед позиционированием снимать WS_POPUP / fullscreen стили.\n"
				"Нужно если Dota запустилась в exclusive fullscreen.\n"
				"При -windowed launch arg обычно НЕ нужно." );

		ImGui::SameLine( 0, 12 );
		ImGui::Checkbox( "Auto-tile on launch", &cfgMut.tileAutoOnLaunch );
		if ( ImGui::IsItemHovered() )
			ImGui::SetTooltip(
				"Автоматически вызывать TILE один раз когда все enabled боты\n"
				"имеют dota_pid != 0 и client.dll загружен (dotaReady)." );

		// ── CPU cores per Dota instance ──
		// Глобальный лимит: сколько ядер давать каждому окну Доты после спавна.
		// 0 = без лимита. Сохраняется в farm.json как "cores_per_instance" и
		// применяется через SetProcessAffinityMask в dota_launcher.cpp.
		{
			if ( theme::gFontMonoSm ) ImGui::PushFont( theme::gFontMonoSm );
			ImGui::TextColored( theme::V( theme::kColInkMute ), "CPU" );
			if ( theme::gFontMonoSm ) ImGui::PopFont();
			ImGui::SameLine( 0, 8 );

			int totalCores = (int)std::thread::hardware_concurrency();
			if ( totalCores <= 0 ) totalCores = 8;
			int curCores = cfgMut.coresPerInstance;
			ImGui::SetNextItemWidth( 200 );
			if ( ImGui::SliderInt( "##cores_per_instance", &curCores, 0, totalCores,
				curCores == 0 ? "Cores: unlimited" : "Cores: %d" ) )
			{
				if ( curCores < 0 ) curCores = 0;
				if ( curCores > totalCores ) curCores = totalCores;
				cfgMut.coresPerInstance = curCores;
				std::string farmPath = g_exeDir + "\\config\\farm.json";
				config::SaveFarmIntSetting( farmPath, "cores_per_instance", curCores );
			}
			if ( ImGui::IsItemHovered() )
				ImGui::SetTooltip(
					"Сколько ядер давать каждому dota2.exe ПОСЛЕ загрузки матча.\n"
					"0 = без лимита (рекомендуется на WARP/software GPU).\n"
					"Применяется ПОСЛЕ client.dll ready (на init Source 2 даём\n"
					"все ядра — иначе timeout на WARP).\n"
					"Меньше 3 ядер на slow-GPU стенде = laggy gameplay." );
		}
	}

	// ── Auto-tile-on-launch monitoring ──
	// Один раз раскладываем окна когда все enabled боты загружены.
	// Перезапускается когда orch уходит в IDLE (на StopFarm).
	{
		const auto& cfg = orch.GetConfig();
		static bool s_autoTileFired = false;

		if ( orch.GetState() == Orchestrator::State::IDLE )
			s_autoTileFired = false;

		if ( cfg.tileAutoOnLaunch && !s_autoTileFired && orch.IsInitialized() )
		{
			int enabled = 0, ready = 0;
			for ( int i = 0; i < orch.GetBotCount(); i++ )
			{
				if ( !cfg.accounts[i].enabled ) continue;
				enabled++;
				const auto& b = orch.GetBot( i );
				if ( b.dotaPid && b.dotaReady ) ready++;
			}
			if ( enabled > 0 && ready == enabled )
			{
				TileNow( orch );
				s_autoTileFired = true;
			}
		}
	}

	ImGui::Spacing();

	// ── GSI status line ──
	{
		auto gsi = orch.GetGsiStatus();
		if ( gsi.running )
		{
			ImGui::TextColored( { 0.55f, 0.55f, 0.55f, 1.f },
				"GSI: port %u | requests %llu | seen %d steam_ids",
				gsi.port, (unsigned long long)gsi.totalRequests, (int)gsi.seenSteamIds );
		}
		else
		{
			ImGui::TextColored( { 1.f, 0.4f, 0.3f, 1.f }, "GSI: NOT RUNNING %s",
				gsi.lastError.c_str() );
		}
	}

	// ── Network engine toggle ──
	// Global on/off для tun2socks (sing-box WinTUN). Default false — TUN
	// перехватывает ВЕСЬ трафик системы и становится default route, что
	// ломает существующий VPN. Юзер должен явно opt-in.
	// Изменение требует перезапуска orchestrator (EarlyStartProxy один раз
	// в WinMain), поэтому показываем warning в tooltip.
	{
		auto& cfgMut = const_cast<FarmConfig&>( orch.GetConfig() );
		if ( ImGui::Checkbox( "Use TUN (sing-box wintun)", &cfgMut.useTun2Socks ) )
		{
			std::string farmPath = g_exeDir + "\\config\\farm.json";
			config::SaveFarmBoolSetting( farmPath, "use_tun2socks", cfgMut.useTun2Socks );
		}
		if ( ImGui::IsItemHovered() )
			ImGui::SetTooltip(
				"Создаёт wintun-интерфейс который перехватывает ВЕСЬ трафик системы.\n"
				"Это сломает существующий VPN/маршрутизацию.\n"
				"Включай только если знаешь зачем.\n"
				"Требует перезапуска DotaFarm.exe для применения." );
	}

	// ── Network status line ──
	{
		const auto& cfg = orch.GetConfig();

		if ( cfg.useTun2Socks )
		{
			auto sb = orch.GetSingbox().GetStats();
			std::string nic = orch.GetSingbox().GetDefaultInterface();
			if ( sb.running )
			{
				ImGui::TextColored( { 0.55f, 0.55f, 0.55f, 1.f },
					"Tun: sing-box pid=%lu | outbounds=%zu | bind_iface=%s | %s",
					sb.pid, sb.outbounds,
					nic.empty() ? "AUTO(risky)" : nic.c_str(),
					sb.lastError.empty() ? "ok" : sb.lastError.c_str() );
			}
			else
			{
				ImGui::TextColored( { 1.f, 0.4f, 0.3f, 1.f },
					"Tun: sing-box NOT RUNNING — %s",
					sb.lastError.empty() ? "not started" : sb.lastError.c_str() );
			}
		}
		else if ( cfg.useKernelRedirect )
		{
			auto ps = orch.GetProxyService().GetStats();
			if ( ps.running )
			{
				ImGui::TextColored( { 0.55f, 0.55f, 0.55f, 1.f },
					"Net: relay tcp=%u udp=%u | watched %zu pids | "
					"tcp out=%llu in=%llu | udp out=%llu in=%llu | "
					"sock acc=%llu ok=%llu fail=%llu",
					ps.relayTcpPort, ps.relayUdpPort, ps.watchedPids,
					(unsigned long long)ps.tcpRedirOut, (unsigned long long)ps.tcpRedirIn,
					(unsigned long long)ps.udpRedirOut, (unsigned long long)ps.udpRedirIn,
					(unsigned long long)ps.tcpAccepted,
					(unsigned long long)ps.tcpHandshakeOk,
					(unsigned long long)ps.tcpHandshakeFail );
			}
			else
			{
				ImGui::TextColored( { 1.f, 0.4f, 0.3f, 1.f },
					"Net: kernel redirect ENABLED but engine not running "
					"(WinDivert load failed? need admin)" );
			}
		}
		else
		{
			ImGui::TextColored( { 1.f, 0.4f, 0.3f, 1.f },
				"Net: NO PROXY (useTun2Socks=false, useKernelRedirect=false) — Steam uses real IP!" );
		}

		// HWID — селектор SpoofMode + status. Combo меняет режим на лету и
		// сохраняет в farm.json (без перезагрузки фермы; новый mode применится
		// при следующем StartFarm). Активные ветки спуфа подсвечены.
		{
			static const char* kModeLabels[] = {
				"Off (no spoof)",
				"Steam-only (per-process IAT)",
				"Full PC (kernel driver)",
				"Both (driver + per-process)",
			};
			static const char* kModeDescr[] = {
				"Легитимный HWID. Все Steam инстансы видят реальные значения.",
				"User-mode IAT хуки в каждом Steam процессе. Per-bot уникальный HWID. Без admin/driver.",
				"HwidSpoofer.exe + spoofer.sys через KDU. Один HWID на всю машину. Требует admin.",
				"Driver спуфит базу, поверх per-process хуки разводят 5 ботов. Defense in depth.",
			};

			int idx = (int)cfg.spoofMode;
			ImGui::SetNextItemWidth( 280 );
			if ( ImGui::Combo( "Spoof Mode##sel", &idx, kModeLabels, IM_ARRAYSIZE( kModeLabels ) ) )
			{
				auto& cfgMut = orch.GetConfigMut();
				cfgMut.spoofMode = (SpoofMode)idx;
				config::SaveFarmStringSetting(
					cfgMut.configDir + "\\farm.json",
					"spoof_mode",
					SpoofModeToString( cfgMut.spoofMode ) );
				orch.LogPublic( "Spoof Mode changed via UI (will apply on next StartFarm)" );
			}
			ImGui::SameLine();
			ImGui::TextColored( { 0.55f, 0.55f, 0.55f, 1.f }, "%s", kModeDescr[idx] );

			ImVec4 col = ( cfg.spoofMode == SpoofMode::Off )
				? ImVec4{ 0.9f, 0.7f, 0.3f, 1.f }
				: ImVec4{ 0.55f, 0.75f, 0.55f, 1.f };
			ImGui::TextColored( col,
				"HWID active: full_pc=%s | steam_only=%s",
				cfg.IsFullPCSpoofEnabled() ? "YES (driver)" : "no",
				cfg.IsSteamSpoofEnabled() ? "YES (per-process)" : "no" );

			if ( cfg.IsFullPCSpoofEnabled() )
			{
				bool exeExists = !cfg.spooferExe.empty() &&
					GetFileAttributesA( cfg.spooferExe.c_str() ) != INVALID_FILE_ATTRIBUTES;
				if ( !exeExists )
				{
					ImGui::TextColored( { 1.f, 0.4f, 0.3f, 1.f },
						"WARN: spoofer_exe не найден (%s) — driver-spoof будет пропущен%s",
						cfg.spooferExe.empty() ? "<empty>" : cfg.spooferExe.c_str(),
						cfg.spoofMode == SpoofMode::FullPC ? " ← FATAL в чистом FullPC" : "" );
				}
			}
		}

		// Per-bot health blocks — CollapsingHeader для expand
		for ( int i = 0; i < orch.GetBotCount(); i++ )
		{
			const auto& b = orch.GetBot( i );
			if ( !b.steamPid && !b.dotaPid ) continue;

			const auto& h = b.health;

			// ── Compact header ──
			ImVec4 color;
			char header[256];

			if ( !h.seen )
			{
				color = ImVec4( 0.55f, 0.55f, 0.55f, 1.f );
				snprintf( header, sizeof( header ),
					"#%d %s | DLL health: NOT SEEN (wait 15-30s for first sample)###hlth%d",
					i, b.dotaPid ? "(dota)" : "(steam)", i );
			}
			else
			{
				bool proxyOk = h.probeOk;
				bool hwidOk  = !h.hwidEnabled || h.criticalMatch;
				bool ageOk   = h.ageMs >= 0 && h.ageMs < 60000;

				if ( proxyOk && hwidOk && ageOk )
					color = ImVec4( 0.4f, 0.9f, 0.4f, 1.f );  // green
				else if ( !ageOk )
					color = ImVec4( 0.9f, 0.7f, 0.3f, 1.f );  // yellow
				else
					color = ImVec4( 1.f, 0.4f, 0.3f, 1.f );   // red

				// IP block
				char ipBlock[128];
				if ( h.probeOk )
					snprintf( ipBlock, sizeof( ipBlock ),
						"Exit IP: %s (%s, %lldms)",
						h.probeExitIp.c_str(),
						h.probeMode.empty() ? "?" : h.probeMode.c_str(),
						(long long)h.probeLatencyMs );
				else
					snprintf( ipBlock, sizeof( ipBlock ),
						"Exit IP: ? (%s)",
						h.probeError.empty() ? "no probe yet" : h.probeError.c_str() );

				// HWID block
				char hwidBlock[128];
				if ( !h.hwidEnabled )
					snprintf( hwidBlock, sizeof( hwidBlock ), "HWID: off" );
				else
				{
					int ok = (int)h.machineGuidMatch + (int)h.macMatch + (int)h.volumeSerialMatch;
					int total = 3;
					if ( h.systemSerialPatched ) { total++; ok += h.systemSerialMatch ? 1 : 0; }
					snprintf( hwidBlock, sizeof( hwidBlock ),
						"HWID: %d/%d spoofed%s",
						ok, total,
						( ok < total ) ? " ⚠" : "" );
				}

				snprintf( header, sizeof( header ),
					"#%d %-8s | %s | %s###hlth%d",
					i, b.dotaPid ? "(dota)" : "(steam)",
					ipBlock, hwidBlock, i );
			}

			ImGui::PushStyleColor( ImGuiCol_Text, color );
			bool expanded = ImGui::CollapsingHeader( header );
			ImGui::PopStyleColor();

			if ( expanded && h.seen )
			{
				ImGui::Indent();
				ImGui::PushStyleColor( ImGuiCol_Text, ImVec4( 0.7f, 0.7f, 0.7f, 1.f ) );

				// Proxy details
				ImGui::Text( "PID: dota=%lu steam=%lu | last write: %llds ago",
					b.dotaPid, b.steamPid, (long long)( h.ageMs / 1000 ) );
				ImGui::Text( "Proxy hook: %s | SOCKS5: %llu ok / %llu fail",
					h.proxyHookActive ? "ACTIVE" : "off (tun2socks catches at TUN)",
					(unsigned long long)h.socks5Ok,
					(unsigned long long)h.socks5Fail );
				if ( !h.proxyRawUrl.empty() )
					ImGui::Text( "Configured proxy: %s", h.proxyRawUrl.c_str() );
				if ( h.probeOk )
					ImGui::Text( "Last probe: %llds ago, via %s",
						(long long)( h.probeAgeMs / 1000 ),
						h.probeMode.c_str() );

				if ( h.hwidEnabled )
				{
					ImGui::Separator();
					ImGui::Text( "HWID seed: %s", h.hwidSeed.c_str() );
					if ( ImGui::BeginTable( "hwid_table", 4,
						ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg ) )
					{
						ImGui::TableSetupColumn( "Field" );
						ImGui::TableSetupColumn( "Expected" );
						ImGui::TableSetupColumn( "Observed" );
						ImGui::TableSetupColumn( "Match" );
						ImGui::TableHeadersRow();

						auto row = [&]( const char* name, const std::string& exp,
							const std::string& obs, bool match )
						{
							ImGui::TableNextRow();
							ImGui::TableNextColumn(); ImGui::Text( "%s", name );
							ImGui::TableNextColumn(); ImGui::Text( "%s", exp.c_str() );
							ImGui::TableNextColumn(); ImGui::Text( "%s",
								obs.empty() ? "(empty)" : obs.c_str() );
							ImGui::TableNextColumn();
							ImGui::TextColored(
								match ? ImVec4( 0.4f, 0.9f, 0.4f, 1.f )
								      : ImVec4( 1.f, 0.4f, 0.3f, 1.f ),
								"%s", match ? "OK" : "FAIL" );
						};

						row( "MachineGuid", h.expectedMachineGuid, h.observedMachineGuid, h.machineGuidMatch );
						row( "MAC",         h.expectedMac,         h.observedMac,         h.macMatch );

						char volExp[32], volObs[32];
						snprintf( volExp, sizeof( volExp ), "%08X", h.expectedVolumeSerial );
						snprintf( volObs, sizeof( volObs ), "%08X", h.observedVolumeSerial );
						row( "VolumeSerial", volExp, volObs, h.volumeSerialMatch );

						if ( h.systemSerialPatched )
							row( "SystemSerial", h.expectedSystemSerial, h.observedSystemSerial, h.systemSerialMatch );
						else
						{
							ImGui::TableNextRow();
							ImGui::TableNextColumn(); ImGui::Text( "SystemSerial" );
							ImGui::TableNextColumn(); ImGui::Text( "%s", h.expectedSystemSerial.c_str() );
							ImGui::TableNextColumn();
							ImGui::TextColored( ImVec4( 0.6f, 0.6f, 0.6f, 1.f ),
								"n/a (OEM not set)" );
							ImGui::TableNextColumn();
							ImGui::TextColored( ImVec4( 0.6f, 0.6f, 0.6f, 1.f ), "-" );
						}
						ImGui::EndTable();
					}
				}

				ImGui::PopStyleColor();
				ImGui::Unindent();
			}
		}
	}

	// ── Party panel ──
	// Группируем ботов по party.id (схожий → одна пати).
	// Если хотя бы у одного есть party.id — показываем общий блок.
	{
		struct PartyView
		{
			uint64_t id = 0;
			uint64_t leader = 0;
			int      knownSize = 0; // макс по ботам
			std::vector<int> botIdxsIn;     // индексы ботов которые в этой пати
			std::vector<uint64_t> members;  // все member_steam_ids (union)
		};

		std::vector<PartyView> parties;
		for ( int i = 0; i < orch.GetBotCount(); i++ )
		{
			const auto& b = orch.GetBot( i );
			if ( b.party.id == 0 ) continue;
			PartyView* pv = nullptr;
			for ( auto& p : parties )
				if ( p.id == b.party.id ) { pv = &p; break; }
			if ( !pv )
			{
				parties.push_back( {} );
				pv = &parties.back();
				pv->id = b.party.id;
				pv->leader = b.party.leaderSteamId;
			}
			pv->botIdxsIn.push_back( i );
			if ( b.party.size > pv->knownSize ) pv->knownSize = b.party.size;
			for ( uint64_t sid : b.party.memberSteamIds )
			{
				bool dup = false;
				for ( uint64_t e : pv->members ) if ( e == sid ) { dup = true; break; }
				if ( !dup ) pv->members.push_back( sid );
			}
		}

		// Также собираем боты с pending invite (приглашение пришло, ещё не в пати)
		std::vector<int> pendingBots;
		for ( int i = 0; i < orch.GetBotCount(); i++ )
		{
			const auto& b = orch.GetBot( i );
			if ( b.party.id == 0 && b.party.pendingInviteId != 0 )
				pendingBots.push_back( i );
		}

		if ( !parties.empty() || !pendingBots.empty() )
		{
			ImGui::Spacing();
			theme::SectionBar( "PARTY  STATE" );

			for ( const auto& pv : parties )
			{
				ImGui::Text( "Party 0x%llx  size=%d  leader=%llu",
					(unsigned long long)pv.id, pv.knownSize,
					(unsigned long long)pv.leader );
				ImGui::SameLine();
				ImGui::TextColored( { 0.6f, 0.6f, 0.6f, 1.f }, " | bots:" );
				ImGui::SameLine();
				for ( int botIdx : pv.botIdxsIn )
				{
					const auto& b = orch.GetBot( botIdx );
					ImVec4 col = b.party.isLeader ? ImVec4( 1.0f, 0.85f, 0.3f, 1.f )
						: ImVec4( 0.4f, 0.85f, 0.4f, 1.f );
					ImGui::TextColored( col, "#%d%s", botIdx, b.party.isLeader ? "*" : "" );
					ImGui::SameLine();
				}

				// "Чужие" в пати — не наши боты
				int strangers = 0;
				for ( uint64_t sid : pv.members )
				{
					bool ours = false;
					for ( int botIdx : pv.botIdxsIn )
						if ( orch.GetBot( botIdx ).ownSteamId == sid ) { ours = true; break; }
					if ( !ours ) strangers++;
				}
				if ( strangers > 0 )
				{
					ImGui::TextColored( { 1.f, 0.6f, 0.3f, 1.f }, "  +%d non-bot", strangers );
				}
				else
				{
					ImGui::NewLine();
				}
			}

			if ( !pendingBots.empty() )
			{
				ImGui::TextColored( { 0.7f, 0.7f, 0.4f, 1.f }, "Pending invites:" );
				ImGui::SameLine();
				for ( int idx : pendingBots )
				{
					const auto& b = orch.GetBot( idx );
					ImGui::Text( "#%d (party=0x%llx)", idx,
						(unsigned long long)b.party.pendingInviteId );
					ImGui::SameLine();
				}
				ImGui::NewLine();
			}
			ImGui::Spacing();
		}
	}

	// ── Account matrix — full card grid (replaces the old 14-col table) ──
	// Lays out N cards in a responsive grid:
	//   width >= 1500   → 5 in one row
	//   1100..1500      → 3 + 2 (centred)
	//   < 1100          → 2 per row (last alone if odd)
	// Each card surfaces 7 visual states driven by BotInstance.state /
	// gsi / health: idle, launching, in-lobby/queue, playing-alive,
	// playing-dead, post-game, crashed. Per-account toggles
	// (enabled / proxyEnabled / hwidSpoofEnabled) survive as 3 small
	// clickable LED chips in the card footer; proxy URL editor moved
	// into a tooltip-popup on hover of the proxy LED chip to keep the
	// card dense.
	{
		auto& cfgMut = orch.GetConfigMut();
		bool enabledChanged = false;
		const int  nBots = orch.GetBotCount();

		// Section header — ASCII separator with live counters.
		{
			int live = 0, queue = 0, crash = 0, idle = 0;
			for ( int i = 0; i < nBots; i++ )
			{
				const auto& b = orch.GetBot( i );
				const char* s = b.state.c_str();
				if      ( strstr( s, "CRASH" ) || strstr( s, "STOP" ) ) crash++;
				else if ( strstr( s, "QUEU" ) || strstr( s, "FIND" )  ) queue++;
				else if ( strstr( s, "GAME" ) || strstr( s, "PLAY" )  ) live++;
				else                                                    idle++;
			}
			char hdr[160];
			snprintf( hdr, sizeof( hdr ),
				"ACCOUNTS  ·  %d LIVE  ·  %d QUEUE  ·  %d IDLE  ·  %d CRASH",
				live, queue, idle, crash );
			theme::SectionBar( hdr );
		}

		// Decide layout columns based on available width.
		// 4 layout tiers: max grid → medium grid → min grid → row strip.
		float availW   = ImGui::GetContentRegionAvail().x;
		int   columns;
		bool  rowMode  = false;
		if      ( availW >= 1500.f ) columns = 5;
		else if ( availW >= 1100.f ) columns = 3;
		else if ( availW >=  700.f ) columns = 2;
		else                       { columns = 1; rowMode = true; }

		const float spacing  = 14.f;
		float cardW = ( availW - spacing * ( columns - 1 ) ) / (float)columns;
		if ( !rowMode && cardW < 240.f ) cardW = 240.f;
		const float cardH = rowMode ? 70.f : 230.f;
		const float now   = (float)ImGui::GetTime();

		ImDrawList* dl = ImGui::GetWindowDrawList();

		for ( int i = 0; i < nBots; i++ )
		{
			const auto& b = orch.GetBot( i );

			int col = i % columns;
			if ( col != 0 ) ImGui::SameLine( 0.f, spacing );

			ImGui::PushID( i + 70000 );

			// Reserve the card box and grab its origin in screen coords.
			ImVec2 origin = ImGui::GetCursorScreenPos();
			ImVec2 cMn = origin;
			ImVec2 cMx = ImVec2( origin.x + cardW, origin.y + cardH );

			// Classify state once — drives stripe colour, LED, accents.
			const char* sst = b.state.c_str();
			bool isCrash = ( strstr( sst, "CRASH" ) != nullptr ) ||
				( strstr( sst, "STOP" ) != nullptr && b.dotaPid == 0 && i < nBots /* explicit stopped */ && false );
			bool isQueue = ( strstr( sst, "QUEU" ) != nullptr ) || ( strstr( sst, "FIND" ) != nullptr );
			bool isLoad  = ( strstr( sst, "LAUNCH" ) != nullptr ) || ( strstr( sst, "BOOT" ) != nullptr ) ||
				( strstr( sst, "INIT" ) != nullptr ) || ( strstr( sst, "WAIT" ) != nullptr ) ||
				( strstr( sst, "LOAD" ) != nullptr );
			bool isPlay  = ( strstr( sst, "GAME" ) != nullptr ) || ( strstr( sst, "PLAY" ) != nullptr ) ||
				( strstr( sst, "HERO" ) != nullptr ) || ( strstr( sst, "PRE_" ) != nullptr );
			bool isPost  = ( strstr( sst, "POST" ) != nullptr ) || ( strstr( sst, "END"  ) != nullptr );
			bool isAlive = isPlay && b.alive;
			bool isDead  = isPlay && !b.alive && b.maxHp > 0;
			bool isIdleSt= !isCrash && !isQueue && !isLoad && !isPlay && !isPost;

			// Choose accent colour for stripe / LED.
			ImU32 accent;
			bool  ledBlink = false;
			float ledPulse = now * 0.6f;
			if      ( isCrash ) { accent = theme::kColCrash;  ledBlink = true; ledPulse = now * 1.4f; }
			else if ( isLoad  ) { accent = theme::kColWarn;   ledPulse = now * 1.0f; }
			else if ( isQueue ) { accent = theme::kColQueue;  ledPulse = now * 0.8f; }
			else if ( isAlive ) { accent = theme::kColSignal; ledPulse = now * 0.5f; }
			else if ( isDead  ) { accent = theme::kColSignal; ledPulse = now * 0.3f; }
			else if ( isPost  ) { accent = theme::kColQueueDim; }
			else                { accent = theme::kColIdle;   }

			ImU32 borderCol = isCrash ? theme::kColCrash :
				( b.paused ? theme::kColWarn : theme::kColLineHot );

			// Reserve vertical/horizontal slot in the layout flow.
			ImGui::Dummy( ImVec2( cardW, cardH ) );
			bool hovered = ImGui::IsItemHovered();
			(void)hovered;

			// ── Row layout (narrow window) ─────────────────────────────────
			// One-line strip per bot: index plate · portrait · hero+state ·
			// LEDs · primary action button. Skips the full card render.
			if ( rowMode )
			{
				const float rpad = 8.f;
				// Backdrop.
				dl->AddRectFilled( cMn, cMx, theme::kColBg1 );
				dl->AddRect      ( cMn, cMx, borderCol, 0.f, 0, 1.f );
				// Left accent stripe — 4px wide, full height.
				dl->AddRectFilled( cMn, ImVec2( cMn.x + 4.f, cMx.y ), accent );

				// Index plate "#N" (32x40 was spec; we go 28x40 to match font).
				char ridx[4]; snprintf( ridx, sizeof( ridx ), "%d", i );
				ImVec2 ipMn( cMn.x + rpad + 4.f, cMn.y + ( cardH - 40.f ) * 0.5f );
				ImVec2 ipMx( ipMn.x + 28.f,     ipMn.y + 40.f );
				theme::DrawIndexPlate( dl, ipMn, ipMx, ridx );

				// Portrait — 60x60 squared crop.
				ImVec2 rpMn( ipMx.x + 8.f,        cMn.y + ( cardH - 60.f ) * 0.5f );
				ImVec2 rpMx( rpMn.x + 60.f,       rpMn.y + 60.f );
				dl->AddRectFilled( rpMn, rpMx, theme::kColBg2 );
				const char* heroShortR = ShortHero( b.hero.c_str() );
				bool heroPickingR =
					strstr( b.state.c_str(), "HERO_SELECT" ) ||
					strstr( b.state.c_str(), "PICK" );
				ImTextureID rtex = nullptr;
				if ( heroShortR && *heroShortR && !heroPickingR )
					rtex = hero_portraits::Get( b.hero.c_str() );
				if ( rtex )
				{
					dl->AddImage( rtex, rpMn, rpMx,
						ImVec2( 0.22f, 0.0f ), ImVec2( 0.78f, 1.0f ) );
					dl->AddRect( rpMn, rpMx, theme::kColLineHot, 0.f, 0, 1.f );
				}
				else
				{
					dl->AddRect( rpMn, rpMx, accent, 0.f, 0, 1.f );
					char rabbr[4] = "--";
					if ( heroPickingR )      { rabbr[0] = '?'; rabbr[1] = 0; }
					else if ( heroShortR && *heroShortR )
					{
						rabbr[0] = (char)toupper( (unsigned char)heroShortR[0] );
						const char* u = strchr( heroShortR, '_' );
						if ( u && *( u + 1 ) )
							rabbr[1] = (char)toupper( (unsigned char)*( u + 1 ) );
						else if ( heroShortR[1] )
							rabbr[1] = (char)toupper( (unsigned char)heroShortR[1] );
						rabbr[2] = 0;
					}
					else if ( isCrash ) { rabbr[0] = '!'; rabbr[1] = 0; }
					if ( theme::gFontSerifMd )
					{
						ImFont* fs = theme::gFontSerifMd;
						ImVec2 ts = fs->CalcTextSizeA( fs->FontSize, FLT_MAX, 0, rabbr );
						dl->AddText( fs, fs->FontSize,
							ImVec2( ( rpMn.x + rpMx.x - ts.x ) * 0.5f,
							        ( rpMn.y + rpMx.y - ts.y ) * 0.5f ),
							isCrash ? theme::kColCrash : theme::kColGold, rabbr );
					}
				}

				// LED on portrait corner — pulses for SUSPECT/DEAD watchdog,
				// otherwise the standard LED for state-machine accent.
				bool wdSuspectR =
					!isCrash && b.dotaPid && b.heartbeatAgeMs >= 5000 &&
					b.heartbeatAgeMs < 30000;
				bool wdDeadR =
					isCrash ||
					( b.dotaPid && b.heartbeatAgeMs >= 30000 );
				if ( wdDeadR )
					theme::PulseDot( dl,
						ImVec2( rpMx.x - 5.f, rpMn.y + 5.f ),
						3.f, theme::kColCrash, /*urgent=*/true );
				else if ( wdSuspectR )
					theme::PulseDot( dl,
						ImVec2( rpMx.x - 5.f, rpMn.y + 5.f ),
						3.f, theme::kColWarn, /*urgent=*/false );
				else
					theme::DrawLED( dl,
						ImVec2( rpMx.x - 5.f, rpMn.y + 5.f ),
						3.f, accent, ledPulse, ledBlink );

				// Centre block: hero name + state line + thin HP bar.
				float cX  = rpMx.x + 12.f;
				float cYn = cMn.y + 10.f;  // hero name row
				float cYs = cMn.y + 30.f;  // state row
				float cYh = cMn.y + 48.f;  // hp bar row

				// Hero name compact (serif 14).
				const char* hd;
				char hdBuf[64];
				if ( isCrash )                hd = "Process exited";
				else if ( heroPickingR )      hd = "PICKING…";
				else if ( heroShortR && *heroShortR )
				{
					size_t k = 0; bool cap = true;
					for ( const char* p = heroShortR; *p && k < sizeof( hdBuf ) - 1; ++p )
					{
						char c = *p;
						if ( c == '_' ) { hdBuf[k++] = ' '; cap = true; }
						else { hdBuf[k++] = cap ? (char)toupper( (unsigned char)c ) : c; cap = false; }
					}
					hdBuf[k] = 0; hd = hdBuf;
				}
				else if ( isQueue ) hd = "Searching match";
				else if ( isLoad )  hd = "Loading…";
				else                hd = "Standby";

				ImFont* fSerifMd = theme::gFontSerifMd;
				if ( fSerifMd )
					dl->AddText( fSerifMd, fSerifMd->FontSize,
						ImVec2( cX, cYn ),
						isCrash ? theme::kColCrash :
						isAlive ? theme::kColGold  : theme::kColInk, hd );

				// State line — "PLAYING · 04:23 · 604/604" mono.
				char stateLine[96];
				char tbuf[16] = "—";
				if ( b.gameTime != 0.f )
				{
					int s = (int)b.gameTime;
					snprintf( tbuf, sizeof( tbuf ), "%s%d:%02d",
						s < 0 ? "-" : "", s < 0 ? -s / 60 : s / 60,
						s < 0 ? -s % 60 : s % 60 );
				}
				else if ( isQueue )
				{
					int s = (int)b.smStateSeconds;
					snprintf( tbuf, sizeof( tbuf ), "%02d:%02d", s / 60, s % 60 );
				}
				char hpbuf[24] = "—";
				if ( b.maxHp > 0 ) snprintf( hpbuf, sizeof( hpbuf ), "%d/%d", b.hp, b.maxHp );
				snprintf( stateLine, sizeof( stateLine ), "%s · %s · %s",
					b.paused ? "PAUSED" : ( isCrash ? "CRASH" : sst ), tbuf, hpbuf );

				ImFont* fMonoSm = theme::gFontMonoSm;
				if ( fMonoSm )
					dl->AddText( fMonoSm, fMonoSm->FontSize,
						ImVec2( cX, cYs ),
						isCrash ? theme::kColCrash :
						isAlive ? theme::kColSignal :
						isQueue ? theme::kColQueue :
						isLoad  ? theme::kColWarn  : theme::kColInkDim,
						stateLine );

				// Thin HP bar 120×6 under the hero name.
				ImVec2 hpMn( cX, cYh );
				ImVec2 hpMx( cX + 120.f, cYh + 6.f );
				if ( b.maxHp > 0 && !isCrash )
				{
					float fill = (float)b.hp / (float)b.maxHp;
					if ( fill < 0.f ) fill = 0.f;
					if ( fill > 1.f ) fill = 1.f;
					ImU32 hpCol = isDead ? theme::kColCrashDim :
						( fill > 0.5f ? theme::kColSignal :
						  fill > 0.25f ? theme::kColWarn  : theme::kColCrash );
					theme::DrawHpBar( dl, hpMn, hpMx, fill, hpCol, theme::kColBg2 );
				}
				else if ( isQueue )
					theme::DrawStreamingProgress( dl, hpMn, hpMx, theme::kColQueue, now, 0.7f );
				else if ( isLoad )
					theme::DrawStreamingProgress( dl, hpMn, hpMx, theme::kColWarn, now, 1.2f );
				else
				{
					dl->AddRectFilled( hpMn, hpMx, theme::kColBg2 );
					dl->AddRect      ( hpMn, hpMx, theme::kColLine, 0.f, 0, 1.f );
				}

				// Right side — 3 LEDs + 1 primary action button.
				const float btnW = 80.f, btnH = 24.f;
				float rightX = cMx.x - rpad - btnW;

				// Action button.
				ImGui::SetCursorScreenPos( ImVec2( rightX, cMn.y + ( cardH - btnH ) * 0.5f ) );
				if ( isCrash )
				{
					if ( theme::ChamferedButton( "RECOVER", ImVec2( btnW, btnH ),
							theme::kColCrashDim, theme::kColCrash, theme::kColInkBright, true ) )
						if ( isIdle ) orch.StartFarm();
				}
				else if ( isPlay || isQueue || isLoad )
				{
					if ( theme::ChamferedButton( "STOP", ImVec2( btnW, btnH ),
							theme::kColBg2, theme::kColCrashDim, theme::kColCrash, false ) )
						orch.StopFarm();
				}
				else
				{
					if ( theme::ChamferedButton( "START", ImVec2( btnW, btnH ),
							theme::kColGoldDeep, theme::kColGold, theme::kColInkBright, true ) )
						if ( isIdle ) orch.StartFarm();
				}

				// 3 LED chips left of the button (EN / PRX / HWD).
				if ( i < (int)cfgMut.accounts.size() )
				{
					struct LC { const char* tag; bool on; ImU32 col; const char* tip;
						bool* mut; bool gateIdle; };
					LC chips[3] = {
						{ "EN",  cfgMut.accounts[i].enabled,        theme::kColSignal,
						  "Account enabled", &cfgMut.accounts[i].enabled, true },
						{ "PRX", cfgMut.accounts[i].proxyEnabled,   theme::kColQueue,
						  "Per-account SOCKS5 proxy",
						  &cfgMut.accounts[i].proxyEnabled, false },
						{ "HWD", cfgMut.accounts[i].hwidSpoofEnabled, theme::kColGold,
						  "Per-process HWID spoof",
						  &cfgMut.accounts[i].hwidSpoofEnabled, false },
					};
					float chipR = 5.f;
					float chipDX = 28.f;
					float chipsW = chipDX * 3.f;
					float chipX0 = rightX - chipsW - 8.f;
					float chipY0 = cMn.y + cardH * 0.5f;
					for ( int c = 0; c < 3; c++ )
					{
						float cxp = chipX0 + chipDX * c + chipR;
						ImU32 cc = chips[c].on ? chips[c].col : theme::kColInkMute;
						ImVec2 hb( cxp - chipR - 2.f, chipY0 - chipR - 2.f );
						ImVec2 hbmx( cxp + chipR + 14.f, chipY0 + chipR + 4.f );
						ImGui::SetCursorScreenPos( hb );
						char hbid[16]; snprintf( hbid, sizeof( hbid ), "##rc%d_%s", i, chips[c].tag );
						ImGui::InvisibleButton( hbid, ImVec2( hbmx.x - hb.x, hbmx.y - hb.y ) );
						bool clk = ImGui::IsItemClicked();
						bool hov = ImGui::IsItemHovered();
						if ( hov ) ImGui::SetTooltip( "%s", chips[c].tip );
						if ( clk && chips[c].mut && ( !chips[c].gateIdle || isIdle ) )
						{
							*chips[c].mut = !*chips[c].mut;
							enabledChanged = true;
						}
						dl->AddCircleFilled( ImVec2( cxp, chipY0 ), chipR, cc );
						dl->AddCircle      ( ImVec2( cxp, chipY0 ), chipR, theme::kColLineHot, 0, 1.f );
						if ( fMonoSm )
							dl->AddText( fMonoSm, fMonoSm->FontSize,
								ImVec2( cxp + chipR + 3.f, chipY0 - 6.f ),
								chips[c].on ? theme::kColInkBright : theme::kColInkMute,
								chips[c].tag );
					}
				}

				// Restore cursor and advance to next strip.
				ImGui::SetCursorScreenPos( ImVec2( cMn.x, cMx.y ) );
				ImGui::PopID();
				continue;  // skip the full vertical card body
			}

			// 1) Card backdrop (chamfered) + corner brackets + scanlines.
			theme::DrawChamferedRect( dl, cMn, cMx, 8.f, theme::kColBg1 );
			theme::DrawScanlines    ( dl, cMn, cMx,
				IM_COL32( 0xd4, 0xa1, 0x4a, 0x07 ), 3 );
			theme::DrawChamferedRect( dl, cMn, cMx, 8.f, borderCol, 1.f );
			theme::DrawCornerBrackets( dl, cMn, cMx, 12.f, 1.f, theme::kColLineHot );

			// 2) Top stripe — accent colour. Streaming for queue/loading.
			ImVec2 stMn( cMn.x + 1, cMn.y + 1 );
			ImVec2 stMx( cMx.x - 1, cMn.y + 5 );
			if ( isQueue || isLoad )
				theme::DrawStreamingProgress( dl, stMn, stMx, accent, now, isQueue ? 0.8f : 1.6f );
			else
				dl->AddRectFilled( stMn, stMx, accent );

			// 3) Header — index plate, login (masked), LED right.
			float pad = 12.f;
			float yy  = cMn.y + 12.f;

			// Index plate (#0..#4)
			char idx[4]; snprintf( idx, sizeof( idx ), "%d", i );
			ImVec2 ipMn( cMn.x + pad, yy );
			ImVec2 ipMx( ipMn.x + 28.f, ipMn.y + 28.f );
			theme::DrawIndexPlate( dl, ipMn, ipMx, idx );

			// Watchdog state pill — replaces the bare LED top-right.
			// Classify by DLL heartbeat age:
			//   no PID            → Unknown ("—")
			//   no heartbeat seen → Unknown until first sample
			//   < 5s              → HEALTHY
			//   5..30s            → SUSPECT  (gentle yellow pulse)
			//   ≥ 30s             → DEAD     (sharp red blink)
			// Crashed state is implicit DEAD regardless of heartbeat.
			theme::WatchdogState wdState;
			char wdExtra[16] = {};
			if ( isCrash )
				wdState = theme::WatchdogState::Dead;
			else if ( !b.dotaPid )
				wdState = theme::WatchdogState::Unknown;
			else if ( b.heartbeatAgeMs < 0 )
				wdState = theme::WatchdogState::Unknown;
			else if ( b.heartbeatAgeMs < 5000 )
				wdState = theme::WatchdogState::Healthy;
			else if ( b.heartbeatAgeMs < 30000 )
			{
				wdState = theme::WatchdogState::Suspect;
				snprintf( wdExtra, sizeof( wdExtra ), "%llds",
					(long long)( b.heartbeatAgeMs / 1000 ) );
			}
			else
			{
				wdState = theme::WatchdogState::Dead;
				snprintf( wdExtra, sizeof( wdExtra ), "%llds",
					(long long)( b.heartbeatAgeMs / 1000 ) );
			}

			// Right-aligned: measure first then place. We reserve ~92px for the
			// pill (covers HEALTHY+age extra) and let it sit at the inner edge.
			ImVec2 wdPos( cMx.x - pad - 92.f, yy + 2.f );
			theme::WatchdogPill( dl, wdPos, wdState,
				wdExtra[0] ? wdExtra : nullptr );

			// Login (masked) + alias (steam<N>) underneath.
			std::string loginStr = "—";
			std::string ipcStr;
			if ( i < (int)cfgMut.accounts.size() )
			{
				const auto& a = cfgMut.accounts[i];
				const std::string& src = a.login.empty() ? a.persona : a.login;
				if ( src.size() >= 8 )
					loginStr = src.substr( 0, 4 ) + "***" + src.substr( src.size() - 3 );
				else if ( !src.empty() )
					loginStr = src;
				ipcStr = a.ipcName;
			}
			ImFont* fSerif = theme::gFontSerifMd;
			ImFont* fMono  = theme::gFontMono;
			ImFont* fMonoS = theme::gFontMonoSm;

			float loginX = ipMx.x + 10.f;
			if ( fMono )
				dl->AddText( fMono, fMono->FontSize, ImVec2( loginX, yy + 2.f ),
					theme::kColInkBright, loginStr.c_str() );
			else
				dl->AddText( ImVec2( loginX, yy + 2.f ), theme::kColInkBright, loginStr.c_str() );
			if ( fMonoS && !ipcStr.empty() )
				dl->AddText( fMonoS, fMonoS->FontSize, ImVec2( loginX, yy + 16.f ),
					theme::kColInkMute, ipcStr.c_str() );

			// 4) State line — uppercase mono, accent-coloured.
			float ylineY = yy + 36.f;
			if ( fMonoS )
				dl->AddText( fMonoS, fMonoS->FontSize, ImVec2( cMn.x + pad, ylineY ),
					theme::kColInkMute, "SM·STATE" );
			const char* nowStr;
			char nowBuf[64];
			if ( b.paused )                      nowStr = "PAUSED";
			else if ( isCrash )                  nowStr = "CRASHED · NO HEARTBEAT";
			else if ( isPost )                   nowStr = "POST-GAME";
			else
			{
				snprintf( nowBuf, sizeof( nowBuf ), "%s", sst );
				nowStr = nowBuf;
			}
			ImU32 nowCol = isCrash ? theme::kColCrash :
				isAlive ? theme::kColSignal :
				isDead  ? theme::kColCrashDim :
				isQueue ? theme::kColQueue :
				isLoad  ? theme::kColWarn :
				          theme::kColInkDim;
			if ( fMono )
				dl->AddText( fMono, fMono->FontSize,
					ImVec2( cMn.x + pad + 70.f, ylineY - 1.f ), nowCol, nowStr );

			// 5) Hero plate — left portrait box (real PNG when available;
			//    2-letter abbreviation fallback) + LED status overlay.
			float heroY = ylineY + 22.f;
			ImVec2 portMn( cMn.x + pad, heroY );
			ImVec2 portMx( portMn.x + 46.f, heroY + 46.f );
			dl->AddRectFilled( portMn, portMx, theme::kColBg2 );

			// Hero short name — drives portrait lookup, abbreviation, hp logic.
			const char* heroShort = ShortHero( b.hero.c_str() );
			bool        heroPicking = false;
			{
				const char* sm = b.state.c_str();
				if ( strstr( sm, "HERO_SELECT" ) || strstr( sm, "PICK" ) )
					heroPicking = true;
			}

			// Try real portrait first. Square crop from a 16:9 source: keep full
			// height, take centred horizontal slice (uv0.x=0.22, uv1.x=0.78 ≈
			// matching aspect of the 46×46 portrait box).
			ImTextureID ptex = nullptr;
			if ( heroShort && *heroShort && !heroPicking )
				ptex = hero_portraits::Get( b.hero.c_str() );

			if ( ptex )
			{
				// Centre-crop horizontally: source ratio ~16:9, target square →
				// trim 28% from each side. This shows the hero face without the
				// wide letterboxed name area.
				dl->AddImage( ptex, portMn, portMx,
					ImVec2( 0.22f, 0.0f ), ImVec2( 0.78f, 1.0f ) );

				// Subtle gold border + accent stripe on top edge for status hue.
				dl->AddRect( portMn, portMx, theme::kColLineHot, 0.f, 0, 1.f );
				dl->AddRectFilled(
					ImVec2( portMn.x, portMn.y ),
					ImVec2( portMx.x, portMn.y + 2.f ),
					accent );
			}
			else
			{
				// Fallback — 2-letter abbreviation in chamfered box.
				dl->AddRect( portMn, portMx, accent, 0.f, 0, 1.f );
				char abbr[4] = "--";
				if ( heroPicking )      { abbr[0] = '?'; abbr[1] = 0; }
				else if ( heroShort && *heroShort )
				{
					const char* p = heroShort;
					abbr[0] = (char)toupper( (unsigned char)p[0] );
					const char* under = strchr( heroShort, '_' );
					if ( under && *( under + 1 ) )
						abbr[1] = (char)toupper( (unsigned char)*( under + 1 ) );
					else if ( p[1] )
						abbr[1] = (char)toupper( (unsigned char)p[1] );
					abbr[2] = 0;
				}
				else if ( isCrash ) { abbr[0] = '!'; abbr[1] = 0; }
				else if ( isLoad )  { abbr[0] = '~'; abbr[1] = 0; }
				else                { strcpy( abbr, "--" ); }
				if ( fSerif )
				{
					ImVec2 ts = fSerif->CalcTextSizeA( fSerif->FontSize, FLT_MAX, 0, abbr );
					dl->AddText( fSerif, fSerif->FontSize,
						ImVec2( ( portMn.x + portMx.x - ts.x ) * 0.5f,
						        ( portMn.y + portMx.y - ts.y ) * 0.5f ),
						isCrash ? theme::kColCrash : theme::kColGold, abbr );
				}
			}

			// Tiny LED status overlay (top-right corner of portrait box).
			theme::DrawLED( dl,
				ImVec2( portMx.x - 5.f, portMn.y + 5.f ),
				3.5f, accent, ledPulse, ledBlink );

			// Right column — hero name, party badge, HP bar / queue strip.
			float rcX  = portMx.x + 12.f;
			float rcY  = heroY;
			float rcW  = cMx.x - pad - rcX;

			// Hero name (gold serif).
			const char* heroDisplay;
			char heroDispBuf[64];
			if ( isCrash )                    heroDisplay = "Process exited";
			else if ( heroPicking )           heroDisplay = "PICKING…";
			else if ( heroShort && *heroShort )
			{
				// Capitalise + replace underscore.
				size_t k = 0;
				bool cap = true;
				for ( const char* p = heroShort; *p && k < sizeof( heroDispBuf ) - 1; ++p )
				{
					char c = *p;
					if ( c == '_' ) { heroDispBuf[k++] = ' '; cap = true; }
					else { heroDispBuf[k++] = cap ? (char)toupper( (unsigned char)c ) : c; cap = false; }
				}
				heroDispBuf[k] = 0;
				heroDisplay = heroDispBuf;
			}
			else if ( isLoad )                heroDisplay = "Loading…";
			else if ( isQueue )               heroDisplay = "Searching match";
			else if ( isIdleSt )              heroDisplay = "Standby";
			else                              heroDisplay = "—";

			if ( fSerif )
				dl->AddText( fSerif, fSerif->FontSize, ImVec2( rcX, rcY ),
					isCrash ? theme::kColCrash :
					isAlive ? theme::kColGold  : theme::kColInk, heroDisplay );

			// Party badge — LEADER (gold) / MEMBER (silver) / solo.
			const char* partyTxt;
			ImU32       partyCol;
			char        partyBuf[64];
			if ( b.party.id != 0 )
			{
				snprintf( partyBuf, sizeof( partyBuf ), "%s · party %llx",
					b.party.isLeader ? "LDR" : "MBR",
					(unsigned long long)( b.party.id & 0xFFFFFu ) );
				partyTxt = partyBuf;
				partyCol = b.party.isLeader ? theme::kColGold : theme::kColInkDim;
			}
			else
			{
				partyTxt = "solo";
				partyCol = theme::kColInkMute;
			}
			if ( fMonoS )
				dl->AddText( fMonoS, fMonoS->FontSize, ImVec2( rcX, rcY + 22.f ),
					partyCol, partyTxt );

			// HP bar / queue progress / dim red dead bar.
			ImVec2 hpMn( rcX, rcY + 38.f );
			ImVec2 hpMx( rcX + rcW, rcY + 48.f );
			if ( isCrash )
			{
				dl->AddRectFilled( hpMn, hpMx, theme::kColCrashDim );
				if ( fMonoS )
					dl->AddText( fMonoS, fMonoS->FontSize,
						ImVec2( hpMn.x + 6.f, hpMn.y - 1.f ),
						theme::kColCrash, "FAULT" );
			}
			else if ( isQueue )
			{
				theme::DrawStreamingProgress( dl, hpMn, hpMx, theme::kColQueue, now, 0.7f );
			}
			else if ( isLoad )
			{
				theme::DrawStreamingProgress( dl, hpMn, hpMx, theme::kColWarn, now, 1.2f );
			}
			else if ( b.maxHp > 0 )
			{
				float fill = (float)b.hp / (float)b.maxHp;
				if ( fill < 0.f ) fill = 0.f;
				if ( fill > 1.f ) fill = 1.f;
				ImU32 hpCol = isDead ? theme::kColCrashDim :
					( fill > 0.5f ? theme::kColSignal :
					  fill > 0.25f ? theme::kColWarn  : theme::kColCrash );
				theme::DrawHpBar( dl, hpMn, hpMx, fill, hpCol, theme::kColBg2 );
				char hpTxt[32];
				if ( isDead )
					snprintf( hpTxt, sizeof( hpTxt ), "DEAD" );
				else
					snprintf( hpTxt, sizeof( hpTxt ), "%d/%d", b.hp, b.maxHp );
				if ( fMonoS )
				{
					ImVec2 ts = fMonoS->CalcTextSizeA( fMonoS->FontSize, FLT_MAX, 0, hpTxt );
					dl->AddText( fMonoS, fMonoS->FontSize,
						ImVec2( hpMx.x - ts.x - 6.f, hpMn.y - 1.f ),
						theme::kColInkBright, hpTxt );
				}
			}
			else
			{
				dl->AddRectFilled( hpMn, hpMx, theme::kColBg2 );
				dl->AddRect      ( hpMn, hpMx, theme::kColLine, 0.f, 0, 1.f );
				if ( fMonoS )
					dl->AddText( fMonoS, fMonoS->FontSize,
						ImVec2( hpMn.x + 6.f, hpMn.y - 1.f ),
						theme::kColInkMute, isIdleSt ? "STANDBY" : "—" );
			}

			// 6) Mini telemetry table — 3 rows × 2 cols of K/V pairs.
			float tY = heroY + 58.f;
			struct KV { const char* k; char v[32]; ImU32 col; };
			KV rows[6];
			int rn = 0;

			// PID
			rows[rn].k = "PID";
			if ( b.dotaPid )      snprintf( rows[rn].v, 32, "%lu", b.dotaPid );
			else if ( b.steamPid) snprintf( rows[rn].v, 32, "S:%lu", b.steamPid );
			else                  snprintf( rows[rn].v, 32, "—" );
			rows[rn].col = b.dotaPid ? theme::kColInk : theme::kColInkMute;
			rn++;

			// Time / Q-Time / fault
			if ( isCrash )
			{
				rows[rn].k = "Fault";
				snprintf( rows[rn].v, 32, "0xC0000005" );
				rows[rn].col = theme::kColCrash;
			}
			else if ( isQueue )
			{
				rows[rn].k = "Q·Time";
				int s = (int)b.smStateSeconds;
				snprintf( rows[rn].v, 32, "%02d:%02d", s / 60, s % 60 );
				rows[rn].col = theme::kColQueue;
			}
			else if ( b.gameTime != 0.f )
			{
				rows[rn].k = "Time";
				int s = (int)b.gameTime;
				int mm = s / 60; int ss = s < 0 ? -s % 60 : s % 60;
				snprintf( rows[rn].v, 32, "%s%d:%02d", s < 0 ? "-" : "", mm < 0 ? -mm : mm, ss );
				rows[rn].col = theme::kColSignal;
			}
			else
			{
				rows[rn].k = "Time";
				snprintf( rows[rn].v, 32, "—" );
				rows[rn].col = theme::kColInkMute;
			}
			rn++;

			// GC msg
			rows[rn].k = "GC·msg";
			if ( b.conn.gcMsgsTotal > 0 )
				snprintf( rows[rn].v, 32, "%u", b.conn.gcMsgsTotal );
			else
				snprintf( rows[rn].v, 32, "—" );
			rows[rn].col = b.conn.gcReady ? theme::kColInk : theme::kColInkMute;
			rn++;

			// Ping (proxy probe latency)
			rows[rn].k = "Ping";
			if ( b.health.probeLatencyMs >= 0 )
				snprintf( rows[rn].v, 32, "%lldms", (long long)b.health.probeLatencyMs );
			else
				snprintf( rows[rn].v, 32, "—" );
			rows[rn].col = ( b.health.probeLatencyMs >= 0 && b.health.probeLatencyMs < 80 )
				? theme::kColSignal : theme::kColInkDim;
			rn++;

			// Lv or Lobby
			if ( b.match.lobbyId != 0 )
			{
				rows[rn].k = "Lobby";
				snprintf( rows[rn].v, 32, "%llu", (unsigned long long)( b.match.lobbyId & 0xFFFFFFFu ) );
				rows[rn].col = theme::kColInkBright;
			}
			else if ( b.gsi.heroLevel > 0 )
			{
				rows[rn].k = "Lv·KDA";
				snprintf( rows[rn].v, 32, "%d %d/%d/%d", b.gsi.heroLevel,
					b.gsi.kills, b.gsi.deaths, b.gsi.assists );
				rows[rn].col = theme::kColInkBright;
			}
			else
			{
				rows[rn].k = "Games";
				snprintf( rows[rn].v, 32, "%d", b.gamesPlayed );
				rows[rn].col = theme::kColInkDim;
			}
			rn++;

			// HB age
			rows[rn].k = "HB";
			if ( b.heartbeatAgeMs >= 0 )
			{
				snprintf( rows[rn].v, 32, "%lldms",
					(long long)b.heartbeatAgeMs );
				rows[rn].col = ( b.heartbeatAgeMs < 1500 ) ? theme::kColSignal :
					( b.heartbeatAgeMs < 5000 ) ? theme::kColWarn : theme::kColCrash;
			}
			else
			{
				snprintf( rows[rn].v, 32, "—" );
				rows[rn].col = theme::kColInkMute;
			}
			rn++;

			// Lay out 3 rows × 2 cols.
			float colW = ( cardW - pad * 2.f ) * 0.5f;
			for ( int r = 0; r < rn; r++ )
			{
				int rRow = r / 2, rCol = r % 2;
				float rx = cMn.x + pad + rCol * colW;
				float ry = tY + rRow * 14.f;
				if ( fMonoS )
				{
					dl->AddText( fMonoS, fMonoS->FontSize, ImVec2( rx, ry ),
						theme::kColInkMute, rows[r].k );
					ImVec2 ts = fMonoS->CalcTextSizeA( fMonoS->FontSize, FLT_MAX, 0, rows[r].v );
					dl->AddText( fMonoS, fMonoS->FontSize,
						ImVec2( rx + colW - ts.x - 8.f, ry ),
						rows[r].col, rows[r].v );
				}
			}

			// 7) Footer — per-card actions + per-account toggles.
			//    Three small clickable LED chips on the left
			//    (EN / PROXY / HWID), three chamfered action buttons
			//    on the right (Start/Stop/DLL — Start replaced by
			//    RECOVER on crash). The action buttons currently fan
			//    out to global StartFarm/StopFarm so single-card start
			//    keeps the old semantics; per-card RestartInstance is
			//    still private in Orchestrator.
			float fY    = cMx.y - 32.f;
			float chipY = fY + 5.f;
			float chipR = 5.f;
			float chipDX = 24.f;
			float chipX  = cMn.x + pad + chipR;

			auto chip = [&]( const char* lbl, bool on, ImU32 onCol, const char* tip,
				bool* mut, bool gateIdle )
			{
				ImU32 c = on ? onCol : theme::kColInkMute;
				ImVec2 hb( chipX - chipR - 2.f, chipY - chipR - 2.f );
				ImVec2 hbmx( chipX + chipR + 14.f, chipY + chipR + 4.f );
				ImGui::SetCursorScreenPos( hb );
				ImGui::InvisibleButton( lbl, ImVec2( hbmx.x - hb.x, hbmx.y - hb.y ) );
				bool clk = ImGui::IsItemClicked();
				bool hov = ImGui::IsItemHovered();
				if ( hov && tip ) ImGui::SetTooltip( "%s", tip );
				if ( clk && mut && ( !gateIdle || isIdle ) )
				{
					*mut = !*mut;
					enabledChanged = true;
				}
				dl->AddCircleFilled( ImVec2( chipX, chipY ), chipR, c );
				dl->AddCircle      ( ImVec2( chipX, chipY ), chipR, theme::kColLineHot, 0, 1.f );
				if ( fMonoS )
				{
					const char* tag = lbl;
					dl->AddText( fMonoS, fMonoS->FontSize,
						ImVec2( chipX + chipR + 3.f, chipY - 6.f ),
						on ? theme::kColInkBright : theme::kColInkMute, tag );
				}
				chipX += chipDX + ImGui::CalcTextSize( lbl ).x;
			};

			if ( i < (int)cfgMut.accounts.size() )
			{
				chip( "EN",  cfgMut.accounts[i].enabled,        theme::kColSignal,
					"Account enabled — included in StartFarm", &cfgMut.accounts[i].enabled, true );
				chip( "PRX", cfgMut.accounts[i].proxyEnabled,   theme::kColQueue,
					"Per-account SOCKS5 proxy (effective on next launch)",
					&cfgMut.accounts[i].proxyEnabled, false );
				chip( "HWD", cfgMut.accounts[i].hwidSpoofEnabled, theme::kColGold,
					"Per-process HWID spoof (next launch)",
					&cfgMut.accounts[i].hwidSpoofEnabled, false );
			}

			// Action buttons — right-aligned.
			float btnW = 56.f, btnH = 22.f;
			float bX = cMx.x - pad - btnW;
			ImGui::SetCursorScreenPos( ImVec2( bX, fY ) );
			if ( isCrash )
			{
				if ( theme::ChamferedButton( "RECOVER", ImVec2( btnW, btnH ),
						theme::kColCrashDim, theme::kColCrash, theme::kColInkBright, true ) )
				{
					if ( isIdle ) orch.StartFarm();
				}
			}
			else if ( isPlay || isQueue || isLoad )
			{
				if ( theme::ChamferedButton( "STOP", ImVec2( btnW, btnH ),
						theme::kColBg2, theme::kColCrashDim, theme::kColCrash, false ) )
				{
					orch.StopFarm();
				}
			}
			else
			{
				if ( theme::ChamferedButton( "START", ImVec2( btnW, btnH ),
						theme::kColGoldDeep, theme::kColGold, theme::kColInkBright, true ) )
				{
					if ( isIdle ) orch.StartFarm();
				}
			}

			bX -= ( btnW + 6.f );
			ImGui::SetCursorScreenPos( ImVec2( bX, fY ) );
			if ( theme::ChamferedButton( "DLL", ImVec2( btnW, btnH ),
					theme::kColBg2, theme::kColLineHot, theme::kColInk, false ) )
			{
				int n = TriggerReloadAll( orch );
				char msg[96];
				snprintf( msg, sizeof( msg ), "Reload Lua: flag sent to %d bot(s)", n );
				orch.LogPublic( msg );
			}
			if ( ImGui::IsItemHovered() )
				ImGui::SetTooltip( "Reload Lua scripts on all live bots" );

			// Hover whole card → tooltip with full PID / hero / dump path.
			ImGui::SetCursorScreenPos( cMn );
			ImGui::InvisibleButton( "##cardhit", ImVec2( cardW, cardH - 32.f ) );
			if ( ImGui::IsItemHovered() )
			{
				const auto& a = ( i < (int)cfgMut.accounts.size() )
					? cfgMut.accounts[i] : AccountConfig{};
				ImGui::BeginTooltip();
				ImGui::Text( "#%d  %s",        i, a.login.c_str() );
				ImGui::Text( "ipc:    %s",     a.ipcName.c_str() );
				ImGui::Text( "steam:  %llu",   (unsigned long long)a.steamId );
				ImGui::Separator();
				ImGui::Text( "state:  %s%s",   sst, b.paused ? "  (PAUSED)" : "" );
				ImGui::Text( "hero:   %s",     b.hero.c_str() );
				ImGui::Text( "PID:    steam=%lu  dota=%lu", b.steamPid, b.dotaPid );
				ImGui::Text( "exit IP:%s",     b.health.probeExitIp.c_str() );
				if ( isCrash )
				{
					ImGui::Separator();
					ImGui::TextColored( theme::V( theme::kColCrash ),
						"dump: C:\\BotProfiles\\%s\\dumps\\", a.ipcName.c_str() );
				}
				ImGui::EndTooltip();
			}

			// Restore cursor to bottom of card so the layout flow advances
			// correctly to the next card / next row.
			ImGui::SetCursorScreenPos( ImVec2( cMn.x, cMx.y ) );

			ImGui::PopID();

			// New row after every `columns`th card.
			if ( ( i + 1 ) % columns == 0 && ( i + 1 ) < nBots )
				ImGui::Dummy( ImVec2( 1.f, spacing ) );
		}

		// Persist enabled / proxy / hwid toggle changes immediately.
		if ( enabledChanged )
		{
			std::string path = g_exeDir + "\\config\\accounts.json";
			config::SaveAccounts( path, cfgMut );
		}
	}

	// ── Pairing 5v5 self-play (если enabled OR uxV2-on) ──
	// Snapshot taken ровно один раз за кадр — все badges/lines читают из ps,
	// чтобы не было torn read'ов и Pairing FSM не дёргался сильнее необходимого.
	{
		auto ps = orch.GetPairingStatus();
		const auto& cfg = orch.GetConfig();
		const bool uxV2 = cfg.pairing.uxV2;

		// uxV2=true но pairing.enabled=false тоже рисуем panel — там показываем
		// "NOT CONFIGURED" badge и кнопки настройки. Это часть нового UX'а.
		const bool showPanel = ps.enabled || uxV2;
		if ( showPanel )
		{
			ImGui::Spacing();
			theme::SectionBar( "PAIRING  5v5  SELF-PLAY" );

			// ── Uptime tracking ─────────────────────────────────────────
			// Простой edge-detector false→true в connected: на rising edge
			// запоминаем timestamp. На falling edge сбрасываем.
			const auto nowTp = std::chrono::steady_clock::now();
			const int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
				nowTp.time_since_epoch() ).count();
			if ( ps.connected )
			{
				if ( s_pairingConnectedSinceMs == 0 )
					s_pairingConnectedSinceMs = nowMs;
			}
			else
			{
				s_pairingConnectedSinceMs = 0;
			}

			// ── Compute effective state badge ───────────────────────────
			// Priority: sync-start != IDLE → relay auth → relay other →
			// connection lifecycle. Pick the most user-actionable one.
			const char* stateLabel = "UNKNOWN";
			ImU32       stateColor = theme::kColIdle;
			const bool  isAuthErr  = ( ps.relayErrorCode == "auth_failed"
				|| ps.relayErrorCode == "unknown_user"
				|| ps.relayErrorCode == "user_disabled" );

			// SyncStart overrides everything else when active — it's the most
			// time-critical UI signal (user is about to start a match).
			switch ( ps.syncStart.state )
			{
			case SyncStartState::WAITING_PEER_ACK:
				stateLabel = "WAITING PEER ACK"; stateColor = theme::kColQueue;  break;
			case SyncStartState::PEER_REQUESTED:
				stateLabel = "PEER WANTS TO START"; stateColor = theme::kColGold; break;
			case SyncStartState::CONFIRMING:
			case SyncStartState::STARTING:
				stateLabel = "STARTING...";      stateColor = theme::kColSignal; break;
			case SyncStartState::DECLINED:
				stateLabel = "DECLINED";         stateColor = theme::kColWarn;   break;
			case SyncStartState::TIMEOUT:
				stateLabel = "TIMEOUT";          stateColor = theme::kColCrash;  break;
			case SyncStartState::IDLE:
			default:
				// Fall through to connection-derived state.
				if ( uxV2 && !ps.enabled )
				{
					stateLabel = "NOT CONFIGURED"; stateColor = theme::kColIdle;
				}
				else if ( isAuthErr )
				{
					stateLabel = "AUTH FAILED";    stateColor = theme::kColCrash;
				}
				else if ( !ps.relayErrorCode.empty() )
				{
					stateLabel = "RELAY ERROR";    stateColor = theme::kColWarn;
				}
				else if ( !ps.connected )
				{
					stateLabel = ps.lastError.empty() ? "CONNECTING" : "DISCONNECTED";
					stateColor = ps.lastError.empty() ? theme::kColWarn : theme::kColCrash;
				}
				else if ( ps.lastPeerHbAgeMs < 0 || ps.clientCount < 1 )
				{
					stateLabel = "WAITING PEER";   stateColor = theme::kColWarn;
				}
				else if ( ps.lastPeerHbAgeMs > 30000 )
				{
					stateLabel = "STALE LINK";     stateColor = theme::kColWarn;
				}
				else if ( ps.lastPeerHbAgeMs > 10000 )
				{
					stateLabel = "WAITING PEER";   stateColor = theme::kColWarn;
				}
				else
				{
					stateLabel = "PAIR READY";     stateColor = theme::kColSignal;
				}
				break;
			}

			// ── Top status row: role | state badge | hb | RTT ───────────
			ImGui::TextColored( theme::V( theme::kColInkDim ), "role:" );
			ImGui::SameLine();
			theme::Pill( ps.isMaster ? "MASTER" : "SLAVE", theme::kColGold );
			ImGui::SameLine();
			theme::Pill( stateLabel, stateColor );

			ImGui::SameLine();
			if ( ps.lastPeerHbAgeMs >= 0 )
			{
				ImGui::TextColored( theme::V( theme::kColInkDim ),
					"hb:%lldms", (long long)ps.lastPeerHbAgeMs );
			}
			else
			{
				ImGui::TextColored( theme::V( theme::kColInkDim ), "hb:-" );
			}
			ImGui::SameLine();
			if ( ps.rttMs >= 0 )
			{
				ImGui::TextColored( theme::V( theme::kColInkDim ),
					"RTT:%lldms", (long long)ps.rttMs );
			}
			else
			{
				ImGui::TextColored( theme::V( theme::kColInkDim ), "RTT:-" );
			}

			// ── uxV2-only block: full live telemetry ─────────────────────
			// Legacy UI (uxV2=false) показывает только base status + strategy
			// чтобы не сломать существующий пользовательский опыт.
			if ( uxV2 )
			{
				// peer line — userId/pairId + client count.
				const std::string& uid = cfg.pairing.userId;
				const std::string& pid = cfg.pairing.pairId;
				ImGui::TextColored( theme::V( theme::kColInkDim ),
					"peer: %s/%s   clients: %d/1",
					uid.empty() ? "-" : uid.c_str(),
					pid.empty() ? "-" : pid.c_str(),
					ps.clientCount );

				// messages + uptime
				char uptimeBuf[16];
				if ( s_pairingConnectedSinceMs != 0 )
				{
					int64_t dt = ( nowMs - s_pairingConnectedSinceMs ) / 1000;
					if ( dt < 0 ) dt = 0;
					int hh = (int)( dt / 3600 );
					int mm = (int)( ( dt / 60 ) % 60 );
					int ss = (int)( dt % 60 );
					snprintf( uptimeBuf, sizeof( uptimeBuf ),
						"%02d:%02d:%02d", hh, mm, ss );
				}
				else
				{
					snprintf( uptimeBuf, sizeof( uptimeBuf ), "--:--:--" );
				}
				ImGui::TextColored( theme::V( theme::kColInkDim ),
					"messages: %s%llu %s%llu   uptime: %s",
					u8"↑",
					(unsigned long long)ps.msgSent,
					u8"↓",
					(unsigned long long)ps.msgRecv,
					uptimeBuf );

				// Громкая auth-error строка ниже (если applicable).
				if ( ps.usingRelay && !ps.relayErrorCode.empty() )
				{
					ImU32 badgeCol = isAuthErr ? theme::kColCrash : theme::kColWarn;
					ImGui::TextColored( theme::V( badgeCol ),
						isAuthErr
							? "[RELAY] AUTH FAILED — check user_id/auth_token in farm.json (code=%s)"
							: "[RELAY] %s",
						ps.relayErrorCode.c_str() );
				}
				else if ( !ps.lastError.empty() )
				{
					ImGui::TextColored( theme::V( theme::kColWarn ),
						"(%s)", ps.lastError.c_str() );
				}

				// ── Action buttons row ──────────────────────────────────
				// Modal dialogs (Generate / Paste) рендерятся в T10 — здесь
				// только триггерим флаги. Reconnect / Disconnect — runtime
				// control orchestrator'а напрямую.
				ImGui::Spacing();
				const ImVec2 kBtn( 168.f, 24.f );
				const ImVec2 kBtnSm( 130.f, 24.f );
				if ( theme::ChamferedButton( "GENERATE PAIR CODE", kBtn,
					theme::kColBg2, theme::kColGoldDeep, theme::kColGold ) )
				{
					s_showGeneratePairCodeDialog = true;
				}
				ImGui::SameLine();
				if ( theme::ChamferedButton( "PASTE PAIR CODE", kBtn,
					theme::kColBg2, theme::kColGoldDeep, theme::kColGold ) )
				{
					s_showPastePairCodeDialog = true;
				}
				ImGui::SameLine();
				const bool canReconnect = ps.enabled;
				if ( theme::ChamferedButton( "RECONNECT", kBtnSm,
					theme::kColBg2, theme::kColLineHot,
					canReconnect ? theme::kColInk : theme::kColInkMute,
					false /* primary */ ) )
				{
					if ( canReconnect )
					{
						orch.RequestForceReconnect();
						orch.LogPublic( "Pairing: force reconnect requested" );
					}
				}
				ImGui::SameLine();
				const bool canDisconnect = ps.enabled && ps.connected;
				if ( theme::ChamferedButton( "DISCONNECT", kBtnSm,
					theme::kColBg2, theme::kColLineHot,
					canDisconnect ? theme::kColInk : theme::kColInkMute,
					false /* primary */ ) )
				{
					if ( canDisconnect )
					{
						orch.RequestDisconnect();
						orch.LogPublic( "Pairing: disconnect requested" );
					}
				}
			}
			else
			{
				// Legacy single-line warning if relay error без uxV2.
				if ( ps.usingRelay && !ps.relayErrorCode.empty() )
				{
					ImU32 badgeCol = isAuthErr ? theme::kColCrash : theme::kColWarn;
					ImGui::TextColored( theme::V( badgeCol ),
						isAuthErr
							? "[RELAY] AUTH FAILED — check user_id/auth_token in farm.json (code=%s)"
							: "[RELAY] %s",
						ps.relayErrorCode.c_str() );
				}
				else if ( !ps.lastError.empty() )
				{
					ImGui::TextColored( theme::V( theme::kColWarn ),
						"(%s)", ps.lastError.c_str() );
				}
			}

			// ── Phase / FSM (показываем всегда — даже в legacy UI) ──────
			ImGui::Spacing();
			const char* phaseStr = "?";
			switch ( ps.phase )
			{
			case PairingPhase::IDLE:           phaseStr = "IDLE"; break;
			case PairingPhase::LOCAL_FOUND:    phaseStr = "LOCAL_FOUND"; break;
			case PairingPhase::WAIT_PEER:      phaseStr = "WAIT_PEER"; break;
			case PairingPhase::DECIDED_ACCEPT: phaseStr = "DECIDED_ACCEPT"; break;
			case PairingPhase::DECIDED_CANCEL: phaseStr = "DECIDED_CANCEL"; break;
			}
			ImGui::Text( "phase: " );
			ImGui::SameLine();
			ImGui::TextColored( theme::V( theme::kColQueue ), "%s", phaseStr );
			ImGui::SameLine();
			ImGui::TextColored( theme::V( theme::kColInkDim ),
				"local=%d/%d peer=%d streak=%d",
				ps.localFilled, ps.localTotal, ps.peerCount, ps.cancelStreak );
			if ( ps.localMajorityLobby )
			{
				ImGui::SameLine();
				ImGui::TextColored( theme::V( theme::kColGold ),
					"lobby=0x%llx", (unsigned long long)ps.localMajorityLobby );
			}

			// sync-start state name (uxV2 only — иначе users не понимают что это).
			if ( uxV2 )
			{
				const char* ssStr = "IDLE";
				ImU32       ssCol = theme::kColInkDim;
				switch ( ps.syncStart.state )
				{
				case SyncStartState::IDLE:             ssStr = "IDLE";       ssCol = theme::kColInkDim; break;
				case SyncStartState::WAITING_PEER_ACK: ssStr = "WAIT_ACK";   ssCol = theme::kColQueue;  break;
				case SyncStartState::PEER_REQUESTED:   ssStr = "PEER_REQ";   ssCol = theme::kColGold;   break;
				case SyncStartState::CONFIRMING:       ssStr = "CONFIRMING"; ssCol = theme::kColSignal; break;
				case SyncStartState::STARTING:         ssStr = "STARTING";   ssCol = theme::kColSignal; break;
				case SyncStartState::DECLINED:         ssStr = "DECLINED";   ssCol = theme::kColWarn;   break;
				case SyncStartState::TIMEOUT:          ssStr = "TIMEOUT";    ssCol = theme::kColCrash;  break;
				}
				ImGui::Text( "sync-start: " );
				ImGui::SameLine();
				ImGui::TextColored( theme::V( ssCol ), "%s", ssStr );
				if ( !ps.syncStart.lastDeclineReason.empty()
					&& ( ps.syncStart.state == SyncStartState::DECLINED
						|| ps.syncStart.state == SyncStartState::TIMEOUT ) )
				{
					ImGui::SameLine();
					ImGui::TextColored( theme::V( theme::kColInkDim ),
						"(%s)", ps.syncStart.lastDeclineReason.c_str() );
				}
			}

			// ── Strategy + next + last 5 (legacy + uxV2 — оставлены as-is) ─
			ImGui::Text( "strategy:" );
			ImGui::SameLine();
			ImU32 sCol = ps.currentStrategy == "WIN"  ? theme::kColSignal :
			             ps.currentStrategy == "LOSE" ? theme::kColWarn   :
			                                            theme::kColInkDim;
			ImGui::TextColored( theme::V( sCol ), "[%s]",
				ps.currentStrategy.empty() ? "-" : ps.currentStrategy.c_str() );
			ImGui::SameLine();
			ImGui::TextColored( theme::V( theme::kColInkDim ), "next:" );
			ImGui::SameLine();
			ImU32 nCol = ps.nextStrategy == "WIN"  ? theme::kColSignal :
			             ps.nextStrategy == "LOSE" ? theme::kColWarn   :
			                                         theme::kColInkDim;
			ImGui::TextColored( theme::V( nCol ), "[%s]", ps.nextStrategy.c_str() );

			ImGui::Text( "last 5:" );
			ImGui::SameLine();
			if ( ps.history.empty() )
				ImGui::TextDisabled( "—" );
			else
			{
				for ( size_t i = 0; i < ps.history.size(); i++ )
				{
					const auto& h = ps.history[i];
					char glyph = '?';
					if ( h.strategy == "WIN" )       glyph = 'W';
					else if ( h.strategy == "LOSE" ) glyph = 'L';
					else if ( h.strategy == "DEBOOST" ) glyph = 'D';
					ImU32 g = h.weWon ? theme::kColSignal : theme::kColCrash;
					ImGui::TextColored( theme::V( g ), "%c", glyph );
					if ( i + 1 < ps.history.size() )
					{
						ImGui::SameLine( 0, 2 );
						ImGui::TextColored( theme::V( theme::kColInkMute ), "·" );
						ImGui::SameLine( 0, 2 );
					}
				}
			}

			// Force buttons — runtime override.
			std::string mode = orch.GetTeamStrategyMode();
			ImGui::Spacing();
			ImGui::Text( "mode:" );
			ImGui::SameLine();
			if ( ImGui::SmallButton( mode == "auto" ? "[auto*]" : " auto " ) )
				orch.SetTeamStrategyOverride( "auto" );
			ImGui::SameLine();
			if ( ImGui::SmallButton( mode == "WIN" ? "[WIN*]" : " WIN " ) )
				orch.SetTeamStrategyOverride( "WIN" );
			ImGui::SameLine();
			if ( ImGui::SmallButton( mode == "LOSE" ? "[LOSE*]" : " LOSE " ) )
				orch.SetTeamStrategyOverride( "LOSE" );
			ImGui::SameLine();
			if ( ImGui::SmallButton( mode == "DEBOOST" ? "[DEBOOST*]" : " DEBOOST " ) )
				orch.SetTeamStrategyOverride( "DEBOOST" );
		}
	}

	// ── Operational status (per-bot одна строка) ──
	{
		ImGui::Spacing();
		theme::SectionBar( "OPERATIONAL  STATUS" );

		for ( int i = 0; i < orch.GetBotCount(); i++ )
		{
			const auto& b = orch.GetBot( i );

			// "#0"
			ImGui::Text( "#%d", i );
			ImGui::SameLine( 30 );

			// DLL heartbeat age
			if ( b.heartbeatAgeMs < 0 )
				ImGui::TextDisabled( "DLL:--" );
			else
			{
				int sec = (int)( b.heartbeatAgeMs / 1000 );
				ImVec4 col = ( sec < 5 )  ? ImVec4( 0.2f, 0.8f, 0.2f, 1.f )
					: ( sec < 30 ) ? ImVec4( 0.9f, 0.7f, 0.2f, 1.f )
					:                ImVec4( 0.9f, 0.3f, 0.3f, 1.f );
				ImGui::TextColored( col, "DLL:%ds", sec );
			}
			ImGui::SameLine( 90 );

			// GC ready/msgs/cv
			if ( b.schema < 2 )
				ImGui::TextDisabled( "GC:?" );
			else if ( !b.conn.gcReady )
				ImGui::TextColored( { 1.f, 0.4f, 0.3f, 1.f }, "GC:x" );
			else
				ImGui::TextColored( { 0.5f, 0.85f, 0.5f, 1.f },
					"GC:%u msgs cv:%u", b.conn.gcMsgsTotal, b.conn.clientVersion );
			ImGui::SameLine( 230 );

			// Party
			if ( b.schema < 2 )
				ImGui::TextDisabled( "P:?" );
			else if ( b.party.id == 0 && b.party.pendingInviteId != 0 )
				ImGui::TextColored( { 1.f, 0.85f, 0.3f, 1.f }, "P:invite" );
			else if ( b.party.id == 0 )
				ImGui::TextDisabled( "P:-" );
			else
				ImGui::TextColored( { 0.4f, 0.85f, 0.4f, 1.f },
					"P:%d%s", b.party.size, b.party.isLeader ? " LDR" : "" );
			ImGui::SameLine( 320 );

			// Queue
			if ( b.queue.inQueue )
			{
				float t = b.smStateSeconds; // приближение для лидера в QUEUING
				ImGui::TextColored( { 1.f, 0.85f, 0.3f, 1.f }, "Q:%.0fs", t );
			}
			else
				ImGui::TextDisabled( "Q:-" );
			ImGui::SameLine( 380 );

			// Match
			if ( b.match.found )
				ImGui::TextColored( { 1.f, 0.5f, 0.f, 1.f },
					"M:0x%llx", (unsigned long long)b.match.lobbyId );
			else
				ImGui::TextDisabled( "M:-" );
			ImGui::SameLine( 480 );

			// Full game state (короткое имя)
			ImGui::TextColored( { 0.55f, 0.85f, 1.f, 1.f }, "%s",
				b.gameState.empty() ? "-" : b.gameState.c_str() );
		}
	}

	ImGui::Spacing();
	theme::SectionBar( "LOG  STREAM" );

	// Bordered, chamfered log panel — child window with phosphor-on-black backdrop
	ImGui::PushStyleColor( ImGuiCol_ChildBg, theme::V( theme::kColBg0 ) );
	ImGui::PushStyleColor( ImGuiCol_Border,   theme::V( theme::kColLineHot ) );
	ImGui::PushStyleVar( ImGuiStyleVar_WindowPadding, ImVec2( 12, 10 ) );
	ImGui::BeginChild( "log", { 0, ImGui::GetContentRegionAvail().y }, true,
		ImGuiWindowFlags_HorizontalScrollbar );

	// Faint scanline overlay inside the log box.
	{
		ImDrawList* dl = ImGui::GetWindowDrawList();
		ImVec2 mn = ImGui::GetWindowPos();
		ImVec2 mx = ImVec2( mn.x + ImGui::GetWindowSize().x, mn.y + ImGui::GetWindowSize().y );
		theme::DrawScanlines( dl, mn, mx, IM_COL32( 0xd4, 0xa1, 0x4a, 0x0A ), 22 );
	}

	auto& log = orch.GetLog();
	for ( size_t i = log.size(); i > 0; i-- )
	{
		const auto& e = log[i - 1];
		// Color the source tag by content for ad-hoc severity hints.
		ImU32 tagCol = theme::kColGoldDeep;
		const char* msg = e.message.c_str();
		if ( strstr( msg, "error" ) || strstr( msg, "ERROR" ) || strstr( msg, "FAIL" ) )
			tagCol = theme::kColCrash;
		else if ( strstr( msg, "warn" ) || strstr( msg, "WARN" ) || strstr( msg, "timeout" ) )
			tagCol = theme::kColWarn;
		else if ( strstr( msg, "GC" ) || strstr( msg, "Party" ) || strstr( msg, "Queue" ) )
			tagCol = theme::kColQueue;

		ImGui::TextColored( theme::V( theme::kColInkMute ), "%s", e.timestamp.c_str() );
		ImGui::SameLine( 0, 8 );
		// 6-char "src" tag from the first whitespace-delimited token.
		// Init with ASCII dashes (UTF-8 "····" = 12 bytes — overflows char[8]).
		char tag[8] = "------";
		const char* sp = strchr( msg, ' ' );
		int tagLen = sp ? (int)( sp - msg ) : (int)strlen( msg );
		if ( tagLen > 6 ) tagLen = 6;
		memcpy( tag, msg, tagLen );
		tag[tagLen] = 0;
		ImGui::TextColored( theme::V( tagCol ), "%-6s", tag );
		ImGui::SameLine( 0, 8 );
		ImGui::PushStyleColor( ImGuiCol_Text, theme::V( theme::kColInk ) );
		ImGui::TextWrapped( "%s", sp ? sp + 1 : "" );
		ImGui::PopStyleColor();
	}
	ImGui::EndChild();
	ImGui::PopStyleVar();
	ImGui::PopStyleColor( 2 );

	// T10 modal dialogs (Pair Code Generate / Paste / Sync Start). Rendered
	// inside ImGui frame scope of this Window, после log panel; флаги
	// выставляются T9 Pairing Panel buttons + SyncStartCoordinator state.
	RenderPairCodeGenerateModal_( orch );
	RenderPairCodePasteModal_( orch );
	RenderSyncStartModal_( orch );

	ImGui::End();
	ImGui::PopStyleColor();  // WindowBg
}

// ═══════════════════════════════════════════════════════════
// Main Loop: Login → Setup → Dashboard
// ═══════════════════════════════════════════════════════════

void Run( Orchestrator& orch )
{
	// Auto-login with saved key
	std::string saved = LoadSavedKey( g_exeDir );
	if ( !saved.empty() )
	{
		strncpy( s_keyBuf, saved.c_str(), sizeof( s_keyBuf ) - 1 );
		g_licenseKey = saved;
		auto r = auth::Authenticate( saved );
		if ( r.allowed )
		{
			g_authenticated = true;
			g_authResult = r;
		}
	}

	// If all accounts already set up, skip setup wizard
	if ( g_authenticated && orch.IsInitialized() && orch.AreAllAccountsSetUp() )
		s_setupDone = true;

	while ( !g_exitRequested )
	{
		MSG msg;
		while ( PeekMessageW( &msg, nullptr, 0, 0, PM_REMOVE ) )
		{
			TranslateMessage( &msg );
			DispatchMessageW( &msg );
			if ( msg.message == WM_QUIT ) g_exitRequested = true;
		}
		if ( g_exitRequested ) break;

		if ( g_authenticated && s_setupDone )
			orch.MonitorTick();

		ImGui_ImplDX11_NewFrame();
		ImGui_ImplWin32_NewFrame();
		ImGui::NewFrame();

		// Cross-fade between the three top-level screens. We render the
		// currently active screen wrapped in a global Alpha style so the
		// transition reads as a 200ms editorial fade-in rather than an
		// abrupt swap. The previous screen is gone by the time we get here
		// (single-screen render path) — fading the active one in is enough
		// to feel intentional without bleeding state across screens.
		int activeScreen =
			!g_authenticated   ? 0 :
			!s_setupDone       ? 1 :
			                     2;
		static int s_lastScreen = -1;
		if ( s_lastScreen != activeScreen )
		{
			// Reset the fade animation when we switch screens — start at 0.
			theme::AnimateSnap( ImGui::GetID( "##screen_fade" ), 0.f );
			s_lastScreen = activeScreen;
		}
		float screenAlpha = theme::FadeInBlock(
			ImGui::GetID( "##screen_fade" ), true, 220.f );
		ImGui::PushStyleVar( ImGuiStyleVar_Alpha, screenAlpha );

		if ( !g_authenticated )
			RenderLogin();
		else if ( !s_setupDone )
			RenderSetup( orch );
		else
			RenderDashboard( orch );

		ImGui::PopStyleVar();

		ImGui::Render();
		// Mockup deepest bg: --bg-0 #0a0907
		constexpr float clr[4]{ 0.039f, 0.035f, 0.027f, 1.0f };
		g_pd3dCtx->OMSetRenderTargets( 1, &g_pRTV, nullptr );
		g_pd3dCtx->ClearRenderTargetView( g_pRTV, clr );
		ImGui_ImplDX11_RenderDrawData( ImGui::GetDrawData() );
		g_pSwapChain->Present( 1, 0 );
	}
}

void Shutdown()
{
	ImGui_ImplDX11_Shutdown();
	ImGui_ImplWin32_Shutdown();
	ImGui::DestroyContext();
	if ( g_pRTV ) { g_pRTV->Release(); g_pRTV = nullptr; }
	if ( g_pSwapChain ) { g_pSwapChain->Release(); g_pSwapChain = nullptr; }
	if ( g_pd3dCtx ) { g_pd3dCtx->Release(); g_pd3dCtx = nullptr; }
	if ( g_pd3dDevice ) { g_pd3dDevice->Release(); g_pd3dDevice = nullptr; }
	if ( g_hwnd ) { DestroyWindow( g_hwnd ); g_hwnd = nullptr; }
	UnregisterClassW( L"DotaFarmOrchestrator", GetModuleHandleW( nullptr ) );
}

} // namespace gui
