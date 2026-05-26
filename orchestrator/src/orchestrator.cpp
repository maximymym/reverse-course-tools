#include "orchestrator.h"
#include "telemetry.h"
#include "auth.h"
#include "gsi_install.h"
#include "hwid_spoof.h"
#include "strategy_writer.h"
#include "mem_reclaim.h"
#include "payload_loader.h"
#include "manual_mapper.h"
#include <algorithm>
#include <json.hpp>

// Defined in gui.h (inline globals) — declare here to avoid circular include
namespace gui {
	extern std::string      g_licenseKey;
	extern auth::AuthResult g_authResult;
}
#include <fstream>
#include <sstream>
#include <filesystem>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <ctime>

using json = nlohmann::json;

// ── File logger (next to EXE) ──
static FILE* g_logFile = nullptr;

static void OpenLogFile( const std::string& exeDir )
{
	if ( g_logFile ) return;
	std::string path = exeDir + "\\DotaFarm.log";
	g_logFile = fopen( path.c_str(), "a" );
	if ( g_logFile )
	{
		fprintf( g_logFile, "\n========== DotaFarm started ==========\n" );
		fflush( g_logFile );
	}
}

static std::string ShortHero( const std::string& full )
{
	const std::string prefix = "npc_dota_hero_";
	if ( full.compare( 0, prefix.size(), prefix ) == 0 )
		return full.substr( prefix.size() );
	return full;
}

// ── Lazy cache for decrypted embedded DLLs ──
// The encrypted bytes live in our own RT_RCDATA. payload_loader decrypts on
// first request; subsequent injects reuse the same plaintext buffer (no
// re-decrypt, no disk hit). Two slots: ANDROMEDA_DLL + PROXYHOOK_DLL.
static std::vector<uint8_t>& EmbeddedDllBuf( const char* resName )
{
	static std::vector<uint8_t> sAndromeda;
	static std::vector<uint8_t> sProxyHook;
	std::vector<uint8_t>& slot = ( strcmp( resName, "PROXYHOOK_DLL" ) == 0 )
		? sProxyHook : sAndromeda;
	if ( slot.empty() )
	{
		if ( !payload_loader::LoadEmbeddedDll( resName, slot ) )
			slot.clear();
	}
	return slot;
}

Orchestrator::Orchestrator()
{
	for ( int i = 0; i < MAX_BOTS; i++ )
		m_bots[i].idx = i;
}

// ── Path resolution ──

std::string Orchestrator::ResolvePath( const std::string& path )
{
	if ( path.empty() )
		return path;

	// Already absolute?
	if ( path.size() >= 2 && path[1] == ':' )
		return path;
	if ( path.size() >= 2 && path[0] == '\\' && path[1] == '\\' )
		return path;

	// Resolve relative to EXE directory
	return m_exeDir + "\\" + path;
}

// Adapter — мост manual_mapper LogFn в основной DotaFarm.log. mapper делает
// vprintf-like calls с trailing '\n'; мы их трим и кидаем в LogPublic.
namespace {
	Orchestrator* g_mapperLogTarget = nullptr;
	void MapperLogAdapter( const char* fmt, ... )
	{
		if ( !g_mapperLogTarget ) return;
		char buf[1024];
		va_list ap; va_start( ap, fmt );
		vsnprintf( buf, sizeof( buf ), fmt, ap );
		va_end( ap );
		size_t len = strlen( buf );
		while ( len > 0 && ( buf[len-1] == '\n' || buf[len-1] == '\r' ) )
			buf[--len] = 0;
		if ( len > 0 ) g_mapperLogTarget->LogPublic( buf );
	}
}

bool Orchestrator::Init( const std::string& configDir, const std::string& exeDir )
{
	m_exeDir = exeDir;
	m_config.configDir = configDir;

	// Wire manual_mapper logger в DotaFarm.log — иначе [mm] трасса уходит
	// только в OutputDebugStringA и юзеру невидима.
	g_mapperLogTarget = this;
	manual_mapper::SetLogger( &MapperLogAdapter );

	// 2026-05-15: CleanupStaleRoutes() перенесён ниже под if(useTun2Socks).
	// Раньше route DELETE 0.0.0.0/128.0.0.0 стрелял на каждом Init() безусловно —
	// если у юзера use_tun2socks=false это просто шум, но если в системе ЕСТЬ
	// другие route'ы с такими же префиксами (например, от чужого VPN/TUN),
	// мы их сносили и ломали сеть. Теперь чистим только если sing-box будет
	// реально запускаться.

	std::string accountsPath = configDir + "\\accounts.json";
	std::string farmPath = configDir + "\\farm.json";

	if ( !config::LoadAccounts( accountsPath, m_config ) )
	{
		Log( "FAIL: accounts.json not found: %s", accountsPath.c_str() );
		return false;
	}

	config::LoadFarmSettings( farmPath, m_config );

	if ( m_config.minifier.memReclaimMigratedFromLegacy )
	{
		Log( "[mem-reclaim] migrated from legacy periodic_ws_trim=%s ws_trim_interval_s=%d "
			"-> memReclaimEnabled=%s memReclaimIntervalS=%d (EmptyAllWorkingSets forced OFF). "
			"Удали старые ключи из farm.json и добавь 'mem_reclaim' секцию.",
			m_config.minifier.memReclaimEnabled ? "true" : "false",
			m_config.minifier.memReclaimIntervalS,
			m_config.minifier.memReclaimEnabled ? "true" : "false",
			m_config.minifier.memReclaimIntervalS );
	}

	// Bundle MEMRED: enable SeProfileSingleProcess + SeIncreaseQuota privileges
	// для system-wide NtSetSystemInformation вызовов. Manifest требует admin,
	// поэтому failure здесь = OS-уровень ошибка, не отсутствие elevation.
	if ( m_config.minifier.memReclaimEnabled )
	{
		if ( MemReclaim::EnablePrivileges() )
			Log( "[mem-reclaim] privileges enabled: SeProfileSingleProcess + SeIncreaseQuota" );
		else
			Log( "[mem-reclaim] WARN: failed to enable privileges — system-wide reclaim не будет работать" );
	}

	m_nBotCount = (int)m_config.accounts.size();
	if ( m_nBotCount > MAX_BOTS )
		m_nBotCount = MAX_BOTS;

	// ── Resolve all paths to absolute ──
	m_config.dllPath = ResolvePath( m_config.dllPath );
	m_config.handleExe = ResolvePath( m_config.handleExe );
	m_config.proxyHookDllPath = ResolvePath( m_config.proxyHookDllPath );

	// Verify critical files
	if ( !m_config.dllPath.empty() &&
		GetFileAttributesA( m_config.dllPath.c_str() ) == INVALID_FILE_ATTRIBUTES )
	{
		Log( "WARN: DLL not found: %s", m_config.dllPath.c_str() );
	}

	if ( GetFileAttributesA( m_config.steamExe.c_str() ) == INVALID_FILE_ATTRIBUTES )
	{
		// Auto-detect Steam path
		std::string detected = SteamLauncher::DetectSteamExe();
		if ( GetFileAttributesA( detected.c_str() ) != INVALID_FILE_ATTRIBUTES )
		{
			Log( "Steam auto-detected: %s", detected.c_str() );
			m_config.steamExe = detected;
		}
		else
		{
			Log( "FAIL: Steam not found: %s (auto-detect also failed)", m_config.steamExe.c_str() );
			return false;
		}
	}

	if ( GetFileAttributesA( m_config.handleExe.c_str() ) == INVALID_FILE_ATTRIBUTES )
	{
		// Try next to EXE
		std::string alt = m_exeDir + "\\handle64.exe";
		if ( GetFileAttributesA( alt.c_str() ) != INVALID_FILE_ATTRIBUTES )
			m_config.handleExe = alt;
		else
		{
			// Bundle H portable: handle64.exe больше НЕ в bundle (Sysinternals
			// non-redistributable). DotaLauncher::KillDotaMutex использует native
			// NtQuerySystemInformation как primary path — handle64 был fallback.
			Log( "INFO: handle64.exe not bundled (using native NtQuerySystemInformation for mutex kill)" );
			m_config.handleExe.clear();
		}
	}

	// Set hero per bot
	for ( int i = 0; i < m_nBotCount; i++ )
		m_bots[i].hero = m_config.GetHero( i );

	m_injector.Init();
	OpenLogFile( exeDir );

	// ── Minifier setup + crash recovery ──
	// Если предыдущая сессия orchestrator упала без StopFarm — на диске остались
	// minifier_backup\<idx>\.applied markers + минифицированные autoexec.cfg/video.txt
	// в Steam dirs. Здесь revert их, чтобы юзер не получил «дота навсегда поломалась».
	//
	// Portable: wrapperExe + wrapperScript хранятся как relative paths
	// (default "scripts\\dota2_minify_wrapper.exe"), но RunPython вызывает
	// CreateProcess который зависит от CWD. main.cpp не делает SetCurrentDirectory,
	// поэтому пользователь запускающий из любого CWD получит "wrapper not found".
	// Резолвим к абсолюту здесь.
	m_config.minifier.wrapperExe    = ResolvePath( m_config.minifier.wrapperExe );
	m_config.minifier.wrapperScript = ResolvePath( m_config.minifier.wrapperScript );
	m_minifier.SetConfig( m_config.minifier );
	m_minifier.SetLogger(
		[]( void* ctx, const char* msg ) { ( (Orchestrator*)ctx )->Log( "%s", msg ); },
		this );
	int stale = m_minifier.DetectStaleBackups();
	if ( stale > 0 )
	{
		Log( "[minifier] detected %d stale backup(s) from previous crash — reverting", stale );
		m_minifier.RevertStale();
	}

	// VPK_DISABLED 2026-05-17 — kill-switch навсегда.
	// Apply пути полностью выпилены (см. dota_minifier.cpp::ApplyVpkPatches).
	// Defensive cleanup ВСЕГДА runs (без else if): сносит leftover
	// dota_minify/pak66_dir.vpk у юзеров которые включали VPK ДО kill-switch'а —
	// stale pak ломает Source 2 mount → MessageBox "FATAL ERROR: Application
	// unable to load gameinfo.gi". RevertVpkPatches идемпотентен: нет pak'ов = no-op.
	if ( m_minifier.DetectStaleVpkPatches() )
	{
		Log( "[minifier] vpk: stale marker from previous crash — reverting pak files" );
		if ( !m_minifier.RevertVpkPatches() )
			Log( "[minifier] vpk: stale revert FAILED — manual cleanup may be needed (steam://validate/570)" );
	}
	else
	{
		Log( "[minifier] vpk: defensive cleanup (always-on, kill-switch active) — removing any leftover dota_minify pak's" );
		m_minifier.RevertVpkPatches();  // best-effort; нет pak'ов = no-op
	}

	if ( m_config.minifier.vpkAttemptedFromConfig )
	{
		Log( "[minifier] WARN: apply_vpk_patches=true в farm.json игнорируется — "
			"VPK patches отключены навсегда (kill-switch). См. UPDATE_LOG про gameinfo.gi." );
	}

	// C3: restore persisted match_state из предыдущей сессии. Если ts свежий
	// (< 10 мин), значит мы упали mid-match и можем попытаться reconnect после
	// первого launch'а — bot.match.lobbyId + watchdog.lastKnownLobbyId дадут
	// IssueReconnect нужный target. Старее 10 мин — мусор, удаляем.
	{
		const uint64_t nowMs = crash_watchdog::NowMs();
		const uint64_t maxAgeMs = 10ULL * 60ULL * 1000ULL;
		for ( int i = 0; i < MAX_BOTS; i++ )
		{
			char path[MAX_PATH];
			snprintf( path, sizeof( path ),
				"C:\\temp\\andromeda\\match_state_%d.json", i );
			if ( GetFileAttributesA( path ) == INVALID_FILE_ATTRIBUTES )
				continue;

			std::ifstream f( path );
			if ( !f )
				continue;
			try
			{
				json j = json::parse( f );
				uint64_t lobbyId = j.value( "lobby_id", (uint64_t)0 );
				uint64_t ts      = j.value( "ts",       (uint64_t)0 );
				if ( lobbyId && ts && nowMs > ts && ( nowMs - ts ) < maxAgeMs )
				{
					if ( i < m_nBotCount )
					{
						m_bots[i].match.lobbyId = lobbyId;
						m_watchdog.RestoreLobbyId( i, lobbyId );
						Log( "#%d: restored lobby_id=%llu from match_state (age=%llus)",
							i, (unsigned long long)lobbyId,
							(unsigned long long)( ( nowMs - ts ) / 1000 ) );
					}
				}
				else
				{
					DeleteFileA( path );
					Log( "#%d: discarded stale match_state (age>10m or invalid)", i );
				}
			}
			catch ( ... )
			{
				DeleteFileA( path );
				Log( "#%d: bad match_state JSON — deleted", i );
			}
		}
	}

	// Поднимаем GSI как можно раньше — он работает даже без активного фарма,
	// и если дота уже запущена/в матче, мы сразу начнём получать GameState.
	EnsureGsi();

	// tun2socks (sing-box) — основной per-account SOCKS5 path.
	// Стартуем заранее, чтобы Steam launch сразу работал через правильный proxy.
	// ВАЖНО: m_singbox.Start содержит Sleep(2000) + 4× route commands → 2-3 сек
	// блокировки. Если звать синхронно — DotaFarm.exe показывает "Не отвечает"
	// во время старта. Уносим на detached thread; StartFarmThread ждёт готовности.
	if ( m_config.useTun2Socks )
	{
		m_singbox.SetLogger( [this]( const char* m ) { Log( "%s", m ); } );

		// WorkDir для sing-box конфига/логов: %LOCALAPPDATA%\DotaFarm
		char localAppData[MAX_PATH] = {};
		DWORD n = GetEnvironmentVariableA( "LOCALAPPDATA", localAppData, MAX_PATH );
		std::string workDir = ( n > 0 && n < MAX_PATH )
			? ( std::string( localAppData ) + "\\DotaFarm" )
			: ( m_exeDir + "\\singbox_work" );
		CreateDirectoryA( workDir.c_str(), nullptr );

		std::thread( [this, workDir]() {
			if ( !m_singbox.Start( m_config.accounts, m_exeDir, workDir ) )
				Log( "WARN: sing-box start failed — Steam будет идти без proxy!" );
		} ).detach();
	}

	// Kernel-level proxy redirect (WinDivert) — dead path, on по умолчанию false.
	if ( m_config.useKernelRedirect )
	{
		m_proxy.SetLogger( [this]( const char* m ) { Log( "%s", m ); } );
		if ( !m_proxy.Start() )
			Log( "WARN: kernel proxy redirect failed — running without WinDivert" );
	}
	else if ( !m_config.useTun2Socks )
	{
		Log( "[net] tun2socks disabled (use_tun2socks=false in farm.json) — user network untouched" );
	}

	Log( "Loaded %d accounts, %d heroes, region=0x%X",
		m_nBotCount, (int)m_config.heroes.size(), m_config.region );

	// Dump accounts for debug
	for ( int i = 0; i < m_nBotCount; i++ )
	{
		auto& acc = m_config.accounts[i];
		Log( "  account[%d]: login=%s steam_id=%llu ipc=%s hero=%s",
			i, acc.login.c_str(), (unsigned long long)acc.steamId,
			acc.ipcName.c_str(), m_config.GetHero( i ).c_str() );
	}
	Log( "DLL: %s", m_config.dllPath.c_str() );
	Log( "Steam: %s", m_config.steamExe.c_str() );
	Log( "Profiles: %s", m_config.profilesDir.c_str() );

	// ── Pairing 5v5 self-play (опциональный) ──
	if ( m_config.pairing.enabled )
	{
		EnsureTempDirs();
		InitPairing_();

		// Загрузить ротацию ролей.
		m_roleRotation.Load( "C:\\temp\\andromeda\\last_role.json" );
		Log( "[pairing] role rotation loaded: last=%s history=%d",
			m_roleRotation.LastStrategy().empty() ? "(cold)" : m_roleRotation.LastStrategy().c_str(),
			(int)m_roleRotation.History().size() );
	}

	return true;
}

// Поднимает pairing transports + wires callbacks. Extracted из Init() чтобы
// переиспользовать в ReinitPairing(). Caller гарантирует что
// m_config.pairing.enabled == true и старые peer'ы остановлены/освобождены.
void Orchestrator::InitPairing_()
{
	m_pairing.Init(
		m_config.pairing.matchSyncTimeoutS,
		m_config.pairing.maxConsecutiveCancels,
		m_nBotCount );

	const bool useRelay = ( m_config.pairing.transport == "relay" );

	// Broadcast callback теперь использует единый helper.
	m_pairing.onBroadcast = [this]( const nlohmann::json& msg )
	{
		SendPairingMessage_( msg );
	};

	// Sync-start handshake routes через тот же transport.
	m_syncStart.broadcast = [this]( const nlohmann::json& msg )
	{
		SendPairingMessage_( msg );
	};
	m_syncStart.startFarmFn = [this]()
	{
		Log( "[sync-start] handshake complete → StartFarmThread" );
		std::thread( &Orchestrator::StartFarmThread, this ).detach();
	};
	m_syncStart.onStateChange = [this]( SyncStartState s )
	{
		Log( "[sync-start] state → %d", (int)s );
	};

	// Match-FSM передаёт sync-start msg types в наш SyncStartCoordinator.
	m_pairing.onSyncStartMsg = [this]( const PeerMsg& m )
	{
		m_syncStart.OnPeerMessage( m.type, m.body );
	};

	// Единая точка приёма peer-сообщений.
	auto onPeerMsg = [this]( const PeerMsg& m )
	{
		OnPairingMessage( m );
	};

	// RelayPeer state callback (для GUI status badge).
	auto onRelayState = [this]( RelayPeer::State s )
	{
		Log( "[pairing] relay state: %d", (int)s );
	};

	if ( useRelay )
	{
		if ( m_config.pairing.relayHost.empty() )
		{
			Log( "[pairing] relay transport selected but relay_host empty — pairing inactive" );
		}
		else if ( m_config.pairing.userId.empty() || m_config.pairing.userAuthToken.empty() )
		{
			Log( "[pairing] relay transport requires user_id AND user_auth_token in farm.json — "
			     "pairing inactive (contact relay admin to get credentials)" );
		}
		else
		{
			m_relayPeer.reset( new RelayPeer() );
			if ( m_relayPeer->Start(
				m_config.pairing.relayHost, m_config.pairing.relayPort,
				m_config.pairing.userId, m_config.pairing.userAuthToken,
				m_config.pairing.pairId, m_config.pairing.role,
				m_config.pairing.pairSecret, onPeerMsg, onRelayState ) )
			{
				Log( "[pairing] relay client started: host=%s:%u user_id=%s pair_id=%s role=%s",
					m_config.pairing.relayHost.c_str(),
					(unsigned)m_config.pairing.relayPort,
					m_config.pairing.userId.c_str(),
					m_config.pairing.pairId.c_str(),
					m_config.pairing.role.c_str() );
			}
			else
			{
				Log( "[pairing] relay client start failed: %s",
					m_relayPeer->LastError().c_str() );
			}
		}
	}
	else if ( m_config.pairing.role == "master" )
	{
		if ( m_ipc.Start( m_config.pairing.ipcPort, m_config.pairing.masterIp,
			m_config.pairing.pairSecret, onPeerMsg ) )
		{
			Log( "[pairing] master listening on %s:%u",
				m_config.pairing.masterIp.c_str(), (unsigned)m_config.pairing.ipcPort );
		}
		else
		{
			Log( "[pairing] master start failed: %s", m_ipc.LastError().c_str() );
		}
	}
	else
	{
		m_slavePeer.reset( new SlavePeer() );
		if ( m_slavePeer->Start( m_config.pairing.masterIp, m_config.pairing.ipcPort,
			m_config.pairing.pairSecret, onPeerMsg ) )
		{
			Log( "[pairing] slave connecting to %s:%u",
				m_config.pairing.masterIp.c_str(), (unsigned)m_config.pairing.ipcPort );
		}
		else
		{
			Log( "[pairing] slave start failed: %s", m_slavePeer->LastError().c_str() );
		}
	}
}

void Orchestrator::SendPairingMessage_( const nlohmann::json& msg )
{
	if ( m_relayPeer )
		m_relayPeer->Send( msg );
	else if ( m_slavePeer )
		m_slavePeer->Send( msg );
	else if ( m_config.pairing.role == "master" )
		m_ipc.Broadcast( msg );
}

void Orchestrator::EarlyStartProxy( const std::string& exeDir )
{
	if ( m_exeDir.empty() )
		m_exeDir = exeDir;

	OpenLogFile( exeDir );

	// Per-account SOCKS5 через sing-box — основной путь.
	// Async — Start() блокирует 2-3 сек, не можем держать UI thread.
	if ( m_config.useTun2Socks && !m_singbox.IsRunning() )
	{
		m_singbox.SetLogger( [this]( const char* m ) { Log( "%s", m ); } );

		char localAppData[MAX_PATH] = {};
		DWORD n = GetEnvironmentVariableA( "LOCALAPPDATA", localAppData, MAX_PATH );
		std::string workDir = ( n > 0 && n < MAX_PATH )
			? ( std::string( localAppData ) + "\\DotaFarm" )
			: ( m_exeDir + "\\singbox_work" );
		CreateDirectoryA( workDir.c_str(), nullptr );

		std::thread( [this, workDir]() {
			if ( !m_singbox.Start( m_config.accounts, m_exeDir, workDir ) )
				Log( "EarlyStartProxy: sing-box start failed" );
			else
				Log( "EarlyStartProxy: sing-box running" );
		} ).detach();
	}
	else if ( !m_config.useTun2Socks )
	{
		// Явный лог чтобы юзер видел что это намеренно (default false с
		// 2026-05-12: чтобы не ломать существующий VPN/маршрутизацию).
		Log( "[net] tun2socks disabled (use_tun2socks=false in farm.json) — user network untouched" );
	}

	// Legacy WinDivert (dead path).
	if ( m_config.useKernelRedirect && !m_proxy.IsRunning() )
	{
		m_proxy.SetLogger( [this]( const char* m ) { Log( "%s", m ); } );
		if ( !m_proxy.Start() )
			Log( "EarlyStartProxy: m_proxy.Start failed" );
		else
			Log( "EarlyStartProxy: kernel redirect engine running" );
	}
}

// ── Helper: create all profile directories ──

void Orchestrator::CreateProfileDirs()
{
	CreateDirectoryA( m_config.profilesDir.c_str(), nullptr );
	for ( int i = 0; i < m_nBotCount; i++ )
	{
		char profile[MAX_PATH];
		snprintf( profile, sizeof( profile ), "%s\\bot%d", m_config.profilesDir.c_str(), i );
		CreateDirectoryA( profile, nullptr );
		CreateDirectoryA( ( std::string( profile ) + "\\AppData" ).c_str(), nullptr );
		CreateDirectoryA( ( std::string( profile ) + "\\AppData\\Roaming" ).c_str(), nullptr );
		CreateDirectoryA( ( std::string( profile ) + "\\AppData\\Local" ).c_str(), nullptr );
		CreateDirectoryA( ( std::string( profile ) + "\\Temp" ).c_str(), nullptr );
	}
}

void Orchestrator::EnsureTempDirs()
{
	CreateDirectoryA( "C:\\temp", nullptr );
	CreateDirectoryA( "C:\\temp\\andromeda", nullptr );
}

// ── Mutex kill with retries ──

bool Orchestrator::KillMutexWithRetry( DWORD pid, int retries )
{
	for ( int r = 0; r < retries; r++ )
	{
		if ( m_dotaLauncher.KillDotaMutex( pid, m_config.handleExe ) )
			return true;
		Sleep( 1000 );
	}
	return false;
}

// ── StartFarm: launches background thread ──

void Orchestrator::StartFarm()
{
	if ( m_state != State::IDLE || m_busy.load() )
	{
		Log( "Cannot start in state: %s", GetStateStr() );
		return;
	}

	m_busy = true;
	m_state = State::LAUNCHING;
	m_startTime = std::chrono::steady_clock::now();

	// Detach previous thread if any
	if ( m_workerThread.joinable() )
		m_workerThread.join();

	m_workerThread = std::thread( &Orchestrator::StartFarmThread, this );
	m_workerThread.detach();
}

// ── Actual work (background thread) ──

void Orchestrator::StartFarmThread()
{
	Log( "[autostart] StartFarmThread entered" );
	CreateProfileDirs();
	EnsureTempDirs();

	// sing-box стартуется async в Init/EarlyStartProxy — подождём готовности
	// перед запуском Steam, иначе первый connect утечёт real IP. 10 сек cap.
	if ( m_config.useTun2Socks && !m_singbox.IsRunning() )
	{
		Log( "Waiting for sing-box to be ready..." );
		for ( int i = 0; i < 100 && !m_singbox.IsRunning(); i++ )
			Sleep( 100 );
		if ( !m_singbox.IsRunning() )
			Log( "WARN: sing-box not ready after 10s — continuing without TUN proxy!" );
		else
			Log( "sing-box ready, proceeding with launch" );
	}

	// Cleanup stale reload_*.flag от прошлых DLL крашей — чтобы свежеинжекченные
	// DLL не словили hot-reload сразу после Init().
	{
		WIN32_FIND_DATAA fd{};
		HANDLE hFind = FindFirstFileA( "C:\\temp\\andromeda\\reload_*.flag", &fd );
		if ( hFind != INVALID_HANDLE_VALUE )
		{
			do
			{
				char path[MAX_PATH];
				snprintf( path, sizeof( path ), "C:\\temp\\andromeda\\%s", fd.cFileName );
				DeleteFileA( path );
			} while ( FindNextFileA( hFind, &fd ) );
			FindClose( hFind );
		}
	}

	// Подтянуть/обновить GSI cfg перед запуском доты — Valve читает cfg при старте клиента.
	if ( !EnsureGsi() )
		Log( "WARN: GSI not ready: %s", m_gsiInstallError.c_str() );
	else
		Log( "GSI cfg installed: %s", m_gsiCfgPath.c_str() );

	// 1. Kill existing
	Log( "Killing existing Steam/Dota..." );
	{
		auto dotas = DotaLauncher::FindDotaPids();
		for ( DWORD pid : dotas )
		{
			HANDLE h = OpenProcess( PROCESS_TERMINATE, FALSE, pid );
			if ( h ) { TerminateProcess( h, 0 ); CloseHandle( h ); }
		}
		Sleep( 2000 );
		SteamLauncher::KillAllSteam();
		Sleep( 3000 );
	}

	// 1b. FullPC HWID spoof (machine-wide, one-shot перед launch loop).
	// Запускается только при spoof_mode = full_pc | both. Делаем ПОСЛЕ killSteam
	// чтобы старые процессы не сошли с ума на смене HWID на лету. Один seed на
	// всю ферму (все 5 ботов с одного ПК должны иметь один HWID — это нормально
	// для machine-wide режима; для per-bot уникальности используется steam_only
	// или both).
	if ( m_config.IsFullPCSpoofEnabled() )
	{
		if ( m_config.spooferExe.empty() )
		{
			Log( "WARN: spoof_mode=%s но spoofer_exe не задан в farm.json — пропускаем driver spoof",
				SpoofModeToString( m_config.spoofMode ) );
		}
		else if ( GetFileAttributesA( m_config.spooferExe.c_str() ) == INVALID_FILE_ATTRIBUTES )
		{
			Log( "WARN: spoof_mode=%s но HwidSpoofer.exe не найден по пути %s — пропускаем driver spoof",
				SpoofModeToString( m_config.spoofMode ), m_config.spooferExe.c_str() );
			if ( m_config.spoofMode == SpoofMode::FullPC )
			{
				Log( "FATAL: чистый FullPC mode без рабочего HwidSpoofer.exe — фарм остановлен" );
				return;
			}
		}
		else
		{
			std::string seed = hwid_spoof::MakeMachineSeed();
			Log( "Pre-farm HWID spoof: mode=%s seed=%s exe=%s timeout=%d",
				SpoofModeToString( m_config.spoofMode ), seed.c_str(),
				m_config.spooferExe.c_str(), m_config.spooferTimeoutMs );
			bool ok = hwid_spoof::RunSpoof(
				m_config.spooferExe, seed, m_config.spooferTimeoutMs );
			if ( ok )
			{
				Log( "Pre-farm HWID spoof OK" );
			}
			else if ( m_config.spoofMode == SpoofMode::FullPC )
			{
				// Чистый FullPC — fail-stop. Юзер явно выбрал driver-режим, нечего
				// silently деградировать в нет-спуфа.
				Log( "FATAL: pre-farm HWID spoof FAILED в чистом FullPC mode — фарм остановлен" );
				return;
			}
			else
			{
				// Both — продолжаем; per-process хуки спасут per-account уникальность,
				// но machine-wide identity не спрятана.
				Log( "WARN: pre-farm HWID spoof FAILED в Both mode — продолжаем на per-process хуках, machine-wide identity exposed" );
			}
		}
	}

	// VPK_DISABLED 2026-05-17 — Bundle G2 VPK apply path выпилен.
	// Юзеры стабильно ловили "FATAL ERROR: Application unable to load gameinfo.gi"
	// от stale pak66_dir.vpk. Теперь только safe-path: autoexec.cfg + video.txt
	// + mem_reclaim. applyVpkPatches force false в config parser → этот блок
	// не сработает даже если осталось условие; оставлен как guard.
	// if ( m_config.minifier.enabled && m_config.minifier.applyVpkPatches )
	// {
	// 	Log( "[minifier] applying VPK patches (preset=%s)...",
	// 		m_config.minifier.vpkPreset.c_str() );
	// 	if ( !m_minifier.ApplyVpkPatches() )
	// 		Log( "[minifier] WARN: VPK apply failed — продолжаем без VPK" );
	// }

	// 2. Launch each instance SEQUENTIALLY (в processor sense)
	//    Wait for dota2.exe to appear → wait 10s for stability → kill mutex → next
	//
	// B.2: launchMode="sequential" — запускается только 1 аккаунт из списка
	// (ротация по кругу между StartFarm вызовами). Это antiban mitigation
	// (5 одновременных логинов с одной машины = сильный smurf signal).

	int launchStartIdx = 0;
	int launchEndIdx = m_nBotCount;
	bool isSequential = ( m_config.launchMode == "sequential" );

	if ( isSequential )
	{
		int found = -1;
		for ( int k = 0; k < m_nBotCount; k++ )
		{
			int idx = ( m_nextSequentialIdx + k ) % m_nBotCount;
			if ( idx < (int)m_config.accounts.size() && m_config.accounts[idx].enabled )
			{
				found = idx;
				break;
			}
		}
		if ( found < 0 )
		{
			Log( "sequential mode: no enabled account found" );
			m_busy = false;
			m_state = State::IDLE;
			return;
		}
		launchStartIdx = found;
		launchEndIdx = found + 1;
		int next = ( found + 1 ) % m_nBotCount;
		m_nextSequentialIdx = next;
		Log( "sequential mode: launching bot #%d only (next run: #%d, cooldown=%ds)",
			found, next, m_config.launchCooldownSec );
	}
	else
	{
		Log( "Launching %d instances (parallel)...", m_nBotCount );
	}
	auto existingPids = DotaLauncher::FindDotaPids();

	for ( int i = launchStartIdx; i < launchEndIdx; i++ )
	{
		// Skip disabled accounts
		if ( i < (int)m_config.accounts.size() && !m_config.accounts[i].enabled )
		{
			Log( "#%d: SKIPPED (disabled)", i );
			continue;
		}

		// For 2nd+: kill mutex on ALL running Dotas before launching next
		if ( i > 0 )
		{
			auto dotas = DotaLauncher::FindDotaPids();
			Log( "#%d: killing mutex on %d running dotas...", i, (int)dotas.size() );
			for ( DWORD pid : dotas )
			{
				bool ok = KillMutexWithRetry( pid, 5 );
				Log( "#%d:   PID %lu mutex %s", i, pid, ok ? "killed" : "FAILED (may not exist yet)" );
			}
			Log( "#%d: waiting 5s after mutex kill...", i );
			Sleep( 5000 );
		}

		// Anti-ban: machine-wide HWID spoof (FullPC | Both modes).
		// ВАЖНО: driver спуфит machine-wide, поэтому fall-thrоugh per-account
		// бессмысленен (5 разных seed'ов перепишут один и тот же глобальный
		// HWID). Реальный one-shot вызов перед фермой делается в RunPreFarmSpoof()
		// до этого цикла; здесь только legacy fallback на случай если кто-то
		// зашёл в LaunchInstance минуя StartAll.
		// SteamOnly (per-process IAT хуки) — отдельная ветка через ProxyHook.dll
		// при инжекте в Steam-процесс ниже, не здесь.

		// Log proxy — с ProxyHook.dll полное покрытие TCP+UDP+DNS.
		// env HTTP_PROXY/HTTPS_PROXY/ALL_PROXY остаются как fallback для downloader.
		if ( i < (int)m_config.accounts.size() && !m_config.accounts[i].proxy.empty() )
		{
			bool hookAvail = !m_config.proxyHookDllPath.empty() &&
				GetFileAttributesA( m_config.proxyHookDllPath.c_str() ) != INVALID_FILE_ATTRIBUTES;
			Log( "#%d: proxy=%s (%s)",
				i, m_config.accounts[i].proxy.c_str(),
				hookAvail ? "ProxyHook.dll will be injected"
				          : "WARN: ProxyHook.dll missing — env-var fallback only" );
		}

		// Pre-register proxy для kernel redirect ДО CreateProcess. Steam с
		// первого пакета будет под NAT'ом — иначе ранние bootstrap пакеты
		// успеют уйти direct.
		if ( m_proxy.IsRunning() && i < (int)m_config.accounts.size() &&
			!m_config.accounts[i].proxy.empty() )
		{
			// PID мы пока не знаем — добавим сразу после CreateProcess. Здесь
			// ничего не делаем; below регистрируем root после получения PID.
		}

		// Apply minifier ДО запуска Steam: Dota читает video.txt при старте,
		// autoexec.cfg при коннекте к серверу — оба должны лежать на диске
		// до spawn'а dota2.exe. Если minifier disabled — no-op.
		if ( m_config.minifier.enabled )
		{
			uint64_t sid = ( i < (int)m_config.accounts.size() )
				? m_config.accounts[i].steamId : 0;
			if ( !m_minifier.ApplyToBot( i, sid, m_config ) )
				Log( "#%d: WARN minifier ApplyToBot failed — launching без минификации", i );
		}

		// Launch Steam + Dota через -applaunch (Steam сам spawn'ит dota2.exe,
		// VAC видит legitimate process tree, env vars, overlay injected).
		// Per-account routing: Steam через sing-box (BotSteam\N\ path), Dota через
		// ProxyHook.dll которую мы инжектим в dota2.exe после spawn'а.
		DWORD steamPid = m_steamLauncher.LaunchInstance( i, m_config );
		m_bots[i].steamPid = steamPid;

		if ( steamPid && m_proxy.IsRunning() &&
			i < (int)m_config.accounts.size() &&
			!m_config.accounts[i].proxy.empty() )
		{
			m_proxy.AddRootPid( steamPid, m_config.accounts[i].proxy );
		}

		if ( steamPid )
			Log( "#%d: Steam+Dota launched (login=%s, steamPid=%lu)",
				i, m_config.accounts[i].login.c_str(), steamPid );
		else
		{
			Log( "#%d: LAUNCH FAILED", i );
			continue;
		}

		// Wait for dota2.exe to appear. Два сценария:
		//   A) Everything cached → dota2.exe появится за 30-60с
		//   B) Steam качает patch/workshop (например новый Dota update):
		//      при 10 Mbps proxy 700MB = ~10 минут. Только первый бот ждёт
		//      полный download — остальные пользуются shared steamapps (junction).
		//
		// Adaptive wait: base=120s, если видим активность в
		// C:\BotSteam\N\steamapps\downloading\ (файлы mtime < 30s), продлеваем
		// до max=900s. Иначе считаем stuck.
		auto isSteamDownloading = [&]() -> bool
		{
			char dlDir[MAX_PATH];
			snprintf( dlDir, sizeof( dlDir ),
				"C:\\BotSteam\\%d\\steamapps\\downloading", i );

			WIN32_FIND_DATAA fd;
			char pattern[MAX_PATH];
			snprintf( pattern, sizeof( pattern ), "%s\\*", dlDir );
			HANDLE hFind = FindFirstFileA( pattern, &fd );
			if ( hFind == INVALID_HANDLE_VALUE ) return false;

			FILETIME now;
			GetSystemTimeAsFileTime( &now );
			ULARGE_INTEGER nowU;
			nowU.LowPart = now.dwLowDateTime;
			nowU.HighPart = now.dwHighDateTime;

			bool active = false;
			do
			{
				if ( fd.cFileName[0] == '.' ) continue;
				ULARGE_INTEGER ft;
				ft.LowPart = fd.ftLastWriteTime.dwLowDateTime;
				ft.HighPart = fd.ftLastWriteTime.dwHighDateTime;
				int64_t ageMs = (int64_t)( ( nowU.QuadPart - ft.QuadPart ) / 10000ULL );
				if ( ageMs >= 0 && ageMs < 30000 )
				{
					active = true;
					break;
				}
			} while ( FindNextFileA( hFind, &fd ) );
			FindClose( hFind );
			return active;
		};

		auto waitForDotaPid = [&]( int baseSec, int maxSec ) -> DWORD
		{
			Log( "#%d: waiting for dota2.exe (base=%ds, max=%ds with download detect)...",
				i, baseSec, maxSec );
			int lastLoggedDl = -1;
			for ( int t = 0; t < maxSec; t++ )
			{
				Sleep( 1000 );
				auto all = DotaLauncher::FindDotaPids();
				for ( DWORD dpid : all )
				{
					bool known = false;
					for ( DWORD ep : existingPids )
						if ( ep == dpid ) { known = true; break; }
					for ( int j = 0; j < i; j++ )
						if ( m_bots[j].dotaPid == dpid ) { known = true; break; }
					if ( !known ) return dpid;
				}

				// Past base timeout — check if Steam is still downloading.
				if ( t >= baseSec )
				{
					bool downloading = isSteamDownloading();
					if ( !downloading )
					{
						Log( "#%d: no Steam download activity, abort wait at %ds", i, t );
						return 0;
					}
					// Log прогресс каждые 30с (не заспамливаем)
					if ( ( t / 30 ) != lastLoggedDl )
					{
						lastLoggedDl = t / 30;
						Log( "#%d: Steam downloading patch — waited %ds/%ds", i, t, maxSec );
					}
				}
			}
			Log( "#%d: hit max timeout %ds (still downloading?)", i, maxSec );
			return 0;
		};

		DWORD dotaPid = waitForDotaPid( 120, 900 );

		// Retry: если dota не появилась и НЕ из-за download — Steam застрял.
		// Kill + relaunch + ещё adaptive wait.
		if ( !dotaPid )
		{
			Log( "#%d: dota2.exe NOT found — killing Steam, retrying", i );
			if ( steamPid )
			{
				HANDLE hp = OpenProcess( PROCESS_TERMINATE, FALSE, steamPid );
				if ( hp )
				{
					TerminateProcess( hp, 1 );
					CloseHandle( hp );
				}
			}
			Sleep( 30000 );  // wait for locks to release

			steamPid = m_steamLauncher.LaunchInstance( i, m_config );
			m_bots[i].steamPid = steamPid;
			if ( !steamPid )
			{
				Log( "#%d: Steam relaunch FAILED, skip", i );
				continue;
			}
			Log( "#%d: Steam relaunched (PID %lu), retry waiting", i, steamPid );
			dotaPid = waitForDotaPid( 120, 900 );
		}

		if ( !dotaPid )
		{
			Log( "#%d: dota2.exe NOT found after retry — giving up", i );
			continue;
		}

		m_bots[i].dotaPid = dotaPid;
		existingPids.push_back( dotaPid );
		Log( "#%d: dota2.exe PID %lu found", i, dotaPid );

		// 2026-05-22: CPU affinity ОТЛОЖЕНА до момента когда client.dll
		// загружена (см. ниже после WaitForDotaReady). Если применить сразу
		// после spawn'а — Source 2 init на WARP с ограниченным числом ядер
		// не успевает за timeout (краш на recovery: client.dll timeout @ 60s
		// при cores=2). Affinity применяется только после готовности —
		// экономия CPU включается уже на работающей dota2.exe.

		// Per-account Dota ProxyHook: write proxy_<pid>.json + inject ProxyHook.dll.
		// Выполняет ДВЕ функции:
		//   1. Proxy redirect (Winsock SOCKS5) — только если НЕ useTun2Socks (TUN сам ловит).
		//   2. Per-process HWID spoof (registry/network/storage/SMBIOS) — если hwidSpoofEnabled.
		// Inject нужен если хотя бы одна из них активна.
		if ( i < (int)m_config.accounts.size() )
		{
			const auto& acc = m_config.accounts[i];
			// Для dota2.exe: proxy ВСЕГДА через ProxyHook user-mode SOCKS5.
			// sing-box TUN не ловит dota2.exe (она запущена из D:\SteamLibrary\...
			// и не матчит regex BotDota\<N>\ т.к. Steam applaunch игнорирует наш
			// hardlink в BotDota\N\). Без DLL-level proxy все dota2 идут через
			// direct outbound → один реальный IP → Valve smurf detect.
			// Steam.exe при этом через sing-box (BotSteam\<N>\ match) = per-account IP.
			// Per-account toggles (acc.proxyEnabled / acc.hwidSpoofEnabled) —
			// фильтры ПОВЕРХ глобальных/наличия прокси строки. Выключен pеr-account
			// флаг = считаем что прокси/spoof для этого аккаунта не запрошен.
			bool proxyViaHook = acc.proxyEnabled && !acc.proxy.empty();
			bool hwidViaHook  = m_config.IsSteamSpoofEnabled() && acc.hwidSpoofEnabled
				&& acc.steamId != 0;
			bool dllAvail = !m_config.proxyHookDllPath.empty() &&
				GetFileAttributesA( m_config.proxyHookDllPath.c_str() ) != INVALID_FILE_ATTRIBUTES;

			if ( dllAvail && ( proxyViaHook || hwidViaHook ) )
			{
				std::string proxyForDll = proxyViaHook ? acc.proxy : std::string();
				std::string hwidSeed;
				if ( hwidViaHook )
				{
					time_t now = time( nullptr );
					tm tmv{};
					localtime_s( &tmv, &now );
					char buf[64];
					snprintf( buf, sizeof( buf ), "%llu_%04d-%02d",
						(unsigned long long)acc.steamId,
						tmv.tm_year + 1900, tmv.tm_mon + 1 );
					hwidSeed = buf;
				}

				bool cfgOk = Injector::WriteProxyConfig(
					dotaPid, proxyForDll, hwidSeed );
				// MAPPER-FIX 2026-05-18: TLS callbacks + SecurityCookie init + section
				// perms добавлены в manual_mapper — manual map снова основной путь,
				// DLL живёт только в encrypted resource в EXE.
				auto& phBuf = EmbeddedDllBuf( "PROXYHOOK_DLL" );
				bool injOk = !phBuf.empty()
					&& m_injector.InjectManualMap( dotaPid, phBuf.data(), phBuf.size() );
				if ( !injOk && !phBuf.empty() )
				{
					Log( "#%d: ProxyHook MM failed → fallback (temp-file LoadLibrary)", i );
					injOk = m_injector.InjectViaTempFile( dotaPid,
						phBuf.data(), phBuf.size(), "ph" );
				}
				Log( "#%d: ProxyHook inject dota PID %lu: cfg=%s inj=%s (proxy='%s' hwid='%s')",
					i, dotaPid,
					cfgOk ? "OK" : "FAIL",
					injOk ? "OK" : "FAIL",
					proxyForDll.c_str(),
					hwidSeed.c_str() );

				// Запоминаем seed для recovery — при relaunch используем ТОТ ЖЕ
				// (mid-match HWID change = VAC flag, см. RECONNECT_DESIGN.md §7).
				if ( !hwidSeed.empty() )
					m_watchdog.SetHwidSeed( i, hwidSeed );
			}
			else if ( !acc.proxy.empty() && !dllAvail )
			{
				Log( "#%d: WARN — no ProxyHook.dll at '%s', Dota will leak real IP!",
					i, m_config.proxyHookDllPath.c_str() );
			}
		}

		// CRITICAL: wait 40s AFTER dota appears before launching next.
		// Dota needs time to fully init + create singleton mutex + apply any
		// pending patches to steamapps\downloading (shared junction), чтобы
		// следующий Steam не уткнулся в file lock.
		if ( i < m_nBotCount - 1 )
		{
			Log( "#%d: waiting 40s for Dota to init + create mutex + release patch locks...", i );
			Sleep( 40000 );
		}
	}

	// 3. Wait for client.dll
	m_state = State::WAITING_READY;
	Log( "Waiting for client.dll..." );

	for ( int i = 0; i < m_nBotCount; i++ )
	{
		if ( !m_bots[i].dotaPid ) continue;
		Log( "#%d: waiting for client.dll...", i );
		if ( m_dotaLauncher.WaitForDotaReady( m_bots[i].dotaPid, 120000 ) )
		{
			m_bots[i].dotaReady = true;
			Log( "#%d: ready", i );

			// Affinity применяется ТОЛЬКО после client.dll ready — Source 2
			// startup CPU-bound, ограничение ядер при init на WARP даёт
			// client.dll timeout (см. recovery crash 2026-05-22).
			if ( m_config.coresPerInstance > 0 )
			{
				bool ok = ApplyDotaCpuAffinity( m_bots[i].dotaPid, i,
					m_config.coresPerInstance );
				Log( "#%d: cpu affinity %s (cores=%d, post-ready)", i,
					ok ? "applied" : "FAILED", m_config.coresPerInstance );
			}
		}
		else
			Log( "#%d: client.dll timeout", i );
	}

	// 4. Clean party state files from previous session
	DeleteFileA( "C:\\temp\\andromeda\\party_id.txt" );
	WIN32_FIND_DATAA fd;
	HANDLE hFind = FindFirstFileA( "C:\\temp\\andromeda\\party_joined_*.txt", &fd );
	if ( hFind != INVALID_HANDLE_VALUE )
	{
		do {
			char path[MAX_PATH];
			snprintf( path, sizeof( path ), "C:\\temp\\andromeda\\%s", fd.cFileName );
			DeleteFileA( path );
		} while ( FindNextFileA( hFind, &fd ) );
		FindClose( hFind );
	}
	Log( "Cleaned party state files" );

	// 5. Inject
	m_state = State::INJECTING;
	Log( "Injecting..." );

	// Find first enabled account — that's the leader
	int leaderIdx = -1;
	for ( int i = 0; i < m_nBotCount; i++ )
	{
		if ( i < (int)m_config.accounts.size() && m_config.accounts[i].enabled )
		{
			leaderIdx = i;
			break;
		}
	}

	for ( int i = 0; i < m_nBotCount; i++ )
	{
		if ( !m_bots[i].dotaPid || !m_bots[i].dotaReady )
		{
			Log( "#%d: skip (not ready)", i );
			continue;
		}

		// Build hero pool for this bot: preferred hero first, then all others as fallback
		uint64_t partyMembers[4];
		int partyCount = 0;
		BuildPartyMembers( i, partyMembers, partyCount );

		const char* role = ( i == leaderIdx ) ? "leader" : "member";

		std::vector<std::string> heroPool;
		// First: this bot's primary hero
		heroPool.push_back( m_config.GetHero( i ) );
		// Then: all other heroes from config as fallback
		for ( size_t h = 0; h < m_config.heroes.size(); h++ )
		{
			if ( m_config.heroes[h] != heroPool[0] )
				heroPool.push_back( m_config.heroes[h] );
		}

		Injector::WriteInstanceConfig( m_bots[i].dotaPid, i, role,
			heroPool, partyMembers, partyCount,
			m_config.region, m_config.gameMode );

		// Dump what we wrote
		Log( "#%d: config written — role=%s, heroes=%d, party_members=%d",
			i, role, (int)heroPool.size(), partyCount );
		for ( int p = 0; p < partyCount; p++ )
			Log( "#%d:   party[%d] = %llu", i, p, (unsigned long long)partyMembers[p] );

		// MAPPER-FIX 2026-05-21 (v3): Andromeda is VMProtect-wrapped and crashes
		// dota2.exe from inside its DllMain when launched via manual map (VMP
		// detects the missing PEB.Ldr entry and aborts the host process). The
		// breadcrumbs from the 16:58 run confirmed: prologue=1 rtlFT=1 cookie=1
		// tls=1 dllmain=0 → DllMain itself kills dota2.exe.
		// → For Andromeda use temp-file LoadLibrary FIRST (proven to work — see
		//   pre-2026-05-17 sessions in DotaFarm.log). Manual map kept as a
		//   secondary path in case temp-file write is blocked by AV.
		auto& andromedaBuf = EmbeddedDllBuf( "ANDROMEDA_DLL" );
		if ( andromedaBuf.empty() )
		{
			Log( "#%d: embedded ANDROMEDA_DLL resource missing/decrypt-failed", i );
			continue;
		}
		bool injOk = m_injector.InjectViaTempFile( m_bots[i].dotaPid,
			andromedaBuf.data(), andromedaBuf.size(), "andro" );
		if ( !injOk )
		{
			Log( "#%d: temp-file LoadLibrary failed → secondary attempt via MM", i );
			injOk = m_injector.InjectManualMap( m_bots[i].dotaPid,
				andromedaBuf.data(), andromedaBuf.size() );
		}
		if ( injOk )
		{
			m_bots[i].injected = true;
			Log( "#%d: injected (%s + %d fallbacks)", i, heroPool[0].c_str(), (int)heroPool.size() - 1 );
		}
		else
			Log( "#%d: INJECT FAILED (both paths)", i );

		Sleep( 2000 ); // brief pause between injections
	}

	int ok = 0;
	for ( int i = 0; i < m_nBotCount; i++ )
		if ( m_bots[i].injected ) ok++;

	// ── Барьер: ждём gc_ready на ВСЕХ injected ботах ──
	//
	// Andromeda DLL после inject'а резолвит native handler'ы (включая
	// AcceptPartyInviteNative) — это занимает 5-30s в зависимости от состояния
	// GC и offsets. До этого момента party invite от leader'а приходит, но
	// member'ы не могут его обработать ("AcceptPartyInviteNative: handler is NULL"),
	// и retry-loop крутится пока handler не resolved. У юзеров это выглядит как
	// "один бот пригласил, остальным не дошло".
	//
	// gc_ready приходит в status JSON когда DLL получила первое GC сообщение
	// (значит client.dll → GC link установлен И handler'ы resolved). Это
	// надёжный сигнал что бот готов принимать party invite.
	//
	// Timeout 90s — больше чем реальное время резолва. При timeout продолжаем
	// (better degraded run than blocked forever); такие боты могут не получить
	// invite, но остальные будут работать.
	{
		m_state = State::WAITING_GC_READY;
		Log( "Waiting for GC ready on all injected bots (timeout 90s)..." );

		const int  pollMs    = 500;
		const int  timeoutMs = 90000;
		const auto deadline  = std::chrono::steady_clock::now() +
			std::chrono::milliseconds( timeoutMs );
		const auto startWait = std::chrono::steady_clock::now();

		int prevReady = -1;
		while ( std::chrono::steady_clock::now() < deadline )
		{
			ReadStatusFiles();

			int total = 0, ready = 0;
			for ( int i = 0; i < m_nBotCount; i++ )
			{
				if ( !m_bots[i].injected ) continue;
				total++;
				if ( m_bots[i].conn.gcReady ) ready++;
			}

			if ( ready != prevReady )
			{
				Log( "  gc_ready: %d/%d bots", ready, total );
				prevReady = ready;
			}

			if ( total > 0 && ready == total )
				break;

			Sleep( pollMs );
		}

		// Финальный отчёт.
		int total = 0, ready = 0;
		for ( int i = 0; i < m_nBotCount; i++ )
		{
			if ( !m_bots[i].injected ) continue;
			total++;
			if ( m_bots[i].conn.gcReady ) ready++;
		}
		auto waitedMs = std::chrono::duration_cast<std::chrono::seconds>(
			std::chrono::steady_clock::now() - startWait ).count();
		if ( ready == total && total > 0 )
		{
			Log( "All %d bots GC-ready in %llds — party invite таймер DLL'ки стартует синхронно",
				total, (long long)waitedMs );
		}
		else
		{
			Log( "WARN: GC-ready timeout — %d/%d ready after %llds. "
				"Боты без gc_ready могут пропустить первый party invite.",
				ready, total, (long long)waitedMs );
			for ( int i = 0; i < m_nBotCount; i++ )
			{
				if ( m_bots[i].injected && !m_bots[i].conn.gcReady )
					Log( "  #%d: gc_ready=false (heartbeat_age=%llds)",
						i, m_bots[i].heartbeatAgeMs / 1000 );
			}
		}
	}

	// Crash watchdog: применяем конфиг, поднимаем dump-watch threads.
	m_watchdog.SetConfig( m_config.crashRecovery );
	if ( m_config.crashRecovery.enabled && m_config.crashRecovery.dumpWatchEnabled )
	{
		for ( int i = 0; i < m_nBotCount; i++ )
		{
			if ( m_bots[i].injected )
				m_watchdog.StartDumpWatch( i );
		}
		Log( "Crash watchdog: enabled (suspect=%ds confirm=%ds dumpWatch=%s)",
			m_config.crashRecovery.heartbeatSuspectS,
			m_config.crashRecovery.heartbeatConfirmS,
			m_config.crashRecovery.dumpWatchEnabled ? "on" : "off" );
	}

	// Strategy.json + strategy.txt начальная запись (Lua боты читают с первой
	// секунды). На single-machine setup два orchestrator'а пишут общий json —
	// каждый со своими pids; LockFileEx внутри StrategyWriter::Write защищает RMW.
	{
		m_currentStrategy = ResolveStrategyForNextMatch();
		std::vector<DWORD> pids;
		for ( int i = 0; i < m_nBotCount; i++ )
			if ( m_bots[i].dotaPid )
				pids.push_back( m_bots[i].dotaPid );
		StrategyWriter::Write( pids, m_currentStrategy );
		Log( "[strategy] initial strategy = %s (mode=%s, pairing=%s, %d pid(s))",
			m_currentStrategy.c_str(),
			m_config.teamStrategyMode.c_str(),
			m_config.pairing.enabled ? "enabled" : "disabled",
			(int)pids.size() );
	}

	m_state = State::RUNNING;
	m_busy = false;
	Log( "Farm running: %d/%d bots. Party invite ready (DLL timer стартует от gc_ready).", ok, m_nBotCount );
}

void Orchestrator::StopFarm()
{
	m_state = State::STOPPING;
	Log( "Stopping farm..." );

	// Watchdog: гасим dump-watch threads + сбрасываем FSM ботов.
	m_watchdog.StopAllDumpWatch();
	for ( int i = 0; i < m_nBotCount; i++ )
		m_watchdog.Reset( i );

	// Pairing tear-down (если был enabled) — гасим IPC, FSM в IDLE.
	if ( m_config.pairing.enabled )
	{
		m_pairing.Reset();
		m_ipc.Stop();
		if ( m_slavePeer ) m_slavePeer->Stop();
		if ( m_relayPeer ) m_relayPeer->Stop();
		m_pidToBotIdx.clear();
		m_postGameHandled = false;
		m_lastHandledMatchId = 0;
		m_pauseUntilMs = 0;
	}

	for ( int i = 0; i < m_nBotCount; i++ )
	{
		if ( m_bots[i].steamPid && m_proxy.IsRunning() )
			m_proxy.RemoveRootPid( m_bots[i].steamPid );

		if ( m_bots[i].dotaPid )
		{
			HANDLE h = OpenProcess( PROCESS_TERMINATE, FALSE, m_bots[i].dotaPid );
			if ( h ) { TerminateProcess( h, 0 ); CloseHandle( h ); }
		}
		m_bots[i].dotaPid = 0;
		m_bots[i].steamPid = 0;
		m_bots[i].injected = false;
		m_bots[i].dotaReady = false;
		m_bots[i].state = "STOPPED";
	}

	SteamLauncher::KillAllSteam();

	// Revert minifier ПОСЛЕ kill процессов (чтобы Dota не успела перезаписать
	// video.txt при graceful shutdown). RevertAll идемпотентен — если
	// minifier.enabled=false и не было apply, no-op.
	if ( !m_minifier.RevertAll() )
		Log( "[minifier] WARN: RevertAll partial failure — check log" );

	// Bundle G2 — VPK revert ПОСЛЕ per-bot revert (порядок не критичен: VPK
	// session-scoped, не пересекается с per-bot autoexec/video.txt). Делается
	// безусловно если был apply (marker present) — даже если cfg сейчас disabled.
	if ( m_minifier.DetectStaleVpkPatches() )
	{
		Log( "[minifier] vpk: reverting session pak patches" );
		if ( !m_minifier.RevertVpkPatches() )
			Log( "[minifier] vpk: WARN revert partial failure — manual: steam://validate/570" );
	}

	// Оставляем sing-box работающим — он нужен и после StopFarm для любых
	// будущих Steam launches ("Open Steam" setup, StartFarm снова). Умрёт
	// вместе с orchestrator через JobObject.

	m_busy = false;
	m_state = State::IDLE;
	Log( "Farm stopped" );
}

// ── Monitor: read status, detect crashes ──

void Orchestrator::MonitorTick()
{
	auto now = std::chrono::steady_clock::now();
	auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>( now - m_lastMonitorTick ).count();
	if ( elapsed < 2000 )
		return;
	m_lastMonitorTick = now;

	// GSI читаем всегда (даже до RUNNING) — это даст hot-inject видимость:
	// если дота уже в матче, мы увидим GSI-данные сразу, без зависимости от DLL.
	ReadGsiSnapshots();

	// Health — читаем всегда. ProxyHook.dll пишет независимо от game state —
	// может быть даже сам Steam залогинен, а dota ещё не запустилась.
	ReadHealthFiles();

	if ( m_state != State::RUNNING )
		return;

	ReadStatusFiles();

	// ── Pairing 5v5 self-play (если enabled) ──
	if ( m_config.pairing.enabled )
	{
		uint64_t nowMsP = crash_watchdog::NowMs();

		// SyncStartCoordinator всегда тикается (timeouts независимы от long-pause).
		m_syncStart.Tick( (int64_t)nowMsP );

		// Long-pause после max_consecutive_cancels — не сканим pending файлы,
		// не Tick'аем FSM. Просто ждём.
		if ( m_pauseUntilMs == 0 || nowMsP >= m_pauseUntilMs )
		{
			ReadMatchPendingFiles();
			auto decision = m_pairing.Tick( nowMsP );
			if ( decision.kind != PairingDecision::NONE )
				HandlePairingDecision( decision );

			// POST_GAME → record + write next strategy.txt + bump rotation.
			TickPostGameDetection();

			// Strategy.json refresh — если ротация шагнула, файл должен отражать
			// то что Lua боты будут читать в СЛЕДУЮЩЕМ матче. dotaPid могли
			// смениться после crash recovery — пересобираем pids каждый раз.
			std::string nextStr = ResolveStrategyForNextMatch();
			if ( m_postGameHandled && nextStr != m_currentStrategy )
			{
				m_currentStrategy = nextStr;
				std::vector<DWORD> pids;
				for ( int i = 0; i < m_nBotCount; i++ )
					if ( m_bots[i].dotaPid )
						pids.push_back( m_bots[i].dotaPid );
				StrategyWriter::Write( pids, m_currentStrategy );
				Log( "[strategy] rotated → %s (%d pid(s))",
					m_currentStrategy.c_str(), (int)pids.size() );
			}
		}
	}


	// Telemetry: upload logs every 60s
	{
		static auto s_lastUpload = std::chrono::steady_clock::time_point{};
		auto sinceLast = std::chrono::duration_cast<std::chrono::seconds>( now - s_lastUpload ).count();
		if ( sinceLast >= 60 )
		{
			s_lastUpload = now;
			// Fire-and-forget on a detached thread (WinHTTP blocks)
			std::thread( [exeDir = m_exeDir]()
			{
				std::string logs = telemetry::CollectLogs( exeDir );
				if ( !logs.empty() )
				{
					(void)telemetry::UploadLog(
						gui::g_authResult.sessionToken,
						gui::g_licenseKey,
						gui::g_authResult.hwidHash,
						logs );
				}

				// Upload any new crash dumps (once per dump — tracked via .sent sidecar)
				try
				{
					namespace fs = std::filesystem;
					for ( auto& e : fs::directory_iterator( exeDir ) )
					{
						auto fname = e.path().filename().string();
						if ( fname.rfind( "dump_", 0 ) != 0 ||
							 fname.size() < 4 ||
							 fname.substr( fname.size() - 4 ) != ".dmp" )
							continue;

						auto sentMarker = e.path().string() + ".sent";
						if ( fs::exists( sentMarker ) ) continue;

						bool ok = telemetry::UploadDump(
							gui::g_authResult.sessionToken,
							gui::g_licenseKey,
							gui::g_authResult.hwidHash,
							e.path().string() );

						if ( ok )
						{
							std::ofstream m( sentMarker );
							m << "uploaded";
						}
					}
				}
				catch ( ... ) {}
			} ).detach();
		}
	}

	// ── Crash detection через CrashWatchdog FSM ──
	// Дизайн: tools/dota2/RECONNECT_DESIGN.md
	uint64_t nowMs = crash_watchdog::NowMs();
	m_watchdog.SetConfig( m_config.crashRecovery );

	for ( int i = 0; i < m_nBotCount; i++ )
	{
		auto& bot = m_bots[i];

		WatchdogSnapshot snap;
		snap.dotaPid        = bot.dotaPid;
		snap.steamPid       = bot.steamPid;
		snap.injected       = bot.injected;
		snap.dotaReady      = bot.dotaReady;
		snap.heartbeatAgeMs = bot.heartbeatAgeMs;
		snap.gameStateId    = bot.gameStateId;
		snap.smState        = bot.state;
		snap.paused         = bot.paused;
		snap.gcReady        = bot.conn.gcReady;
		snap.lobbyId        = bot.match.lobbyId;

		m_watchdog.Tick( nowMs, i, snap );

		// Dispatch actions (orchestrator выполняет на background threads).
		if ( m_watchdog.ShouldRelaunch( i ) )
		{
			Log( "#%d: watchdog → RELAUNCH (state=%s, crashes=%d, reconnects/match=%d)",
				i, m_watchdog.GetStateStr( i ),
				m_watchdog.GetCrashCount( i ),
				m_watchdog.GetReconnectsThisMatch( i ) );
			bot.dotaPid = 0;
			bot.injected = false;
			bot.dotaReady = false;
			bot.state = "RECOVERING";
			std::thread( &Orchestrator::RecoveryThread, this, i ).detach();
		}
		if ( m_watchdog.ShouldReconnect( i ) )
		{
			Log( "#%d: watchdog → RECONNECT (lobby=%llu)",
				i, (unsigned long long)bot.match.lobbyId );
			IssueReconnect( i );
		}
		if ( m_watchdog.ShouldKillForever( i ) )
		{
			Log( "#%d: watchdog → DEAD (manual restart needed)", i );
			if ( bot.dotaPid )
			{
				HANDLE h = OpenProcess( PROCESS_TERMINATE, FALSE, bot.dotaPid );
				if ( h ) { TerminateProcess( h, 1 ); CloseHandle( h ); }
				bot.dotaPid = 0;
			}
			bot.injected = false;
			bot.dotaReady = false;
			bot.state = "DEAD";
		}
	}

	// ── Memory reclamation (Bundle MEMRED, 2026-05-16) ──
	//
	// Заменяет старый per-process TrimWorkingSetTree(dota+steam) ws-trim
	// (визуальный stutter в Dota'е из-за принудительного re-page-ин'а
	// активных текстур/шейдеров). Теперь идём через memreduct-style
	// system-wide NtSetSystemInformation(SystemMemoryListInformation, ...):
	//   • PurgeStandbyList            — главный workhorse, чистит file cache
	//   • PurgeLowPriorityStandbyList  — лёгкая версия, priority-0 only
	//   • CombineMemoryLists           — дедупликация PFN между N dota'ми (Win10+)
	//   • FlushModifiedList            — disk pressure, opt-in
	//   • EmptyAllWorkingSets          — старое поведение, OFF by default
	// Auto-trigger: если memReclaimAutoThreshold > 0 и %memory load ≥ threshold —
	// reclaim запускается даже если interval ещё не прошёл.
	// См. src/mem_reclaim.h / .cpp для деталей syscall'ов.
	if ( m_config.minifier.memReclaimEnabled )
	{
		static auto s_lastReclaim = std::chrono::steady_clock::time_point{};
		int intervalS = m_config.minifier.memReclaimIntervalS;
		intervalS = std::clamp( intervalS, 10, 3600 );

		auto stats = MemReclaim::QuerySystemMemory();
		bool pressure = false;
		if ( m_config.minifier.memReclaimAutoThreshold > 0 &&
			stats.valid &&
			(int)stats.memoryLoadPct >= m_config.minifier.memReclaimAutoThreshold )
			pressure = true;

		auto sinceReclaim = std::chrono::duration_cast<std::chrono::seconds>(
			now - s_lastReclaim ).count();

		if ( pressure || sinceReclaim >= intervalS )
		{
			s_lastReclaim = now;
			int ops = 0;
			// Порядок важен: EmptyWS первым (выгружает активные WS в standby),
			// потом FlushModified (modified → standby), потом Purge* (standby → free),
			// потом Combine (дедуплицирует то что осталось).
			if ( m_config.minifier.memReclaimEmptyAllWorkingSets )
				{ MemReclaim::EmptyAllWorkingSets();      ops++; }
			if ( m_config.minifier.memReclaimFlushModified )
				{ MemReclaim::FlushModified();            ops++; }
			if ( m_config.minifier.memReclaimPurgeLowPrioStandby )
				{ MemReclaim::PurgeLowPriorityStandby();  ops++; }
			if ( m_config.minifier.memReclaimPurgeStandby )
				{ MemReclaim::PurgeStandby();             ops++; }
			if ( m_config.minifier.memReclaimCombinePages )
				{ MemReclaim::CombineMemoryLists();       ops++; }

			// Лог приблизительно раз в 10 минут (logEvery тиков), но при
			// pressure auto-trigger логаем всегда — это важное событие.
			static int s_reclaimTick = 0;
			int logEvery = 600 / intervalS; if ( logEvery < 1 ) logEvery = 1;
			if ( pressure || ( ++s_reclaimTick % logEvery ) == 1 )
			{
				auto after = MemReclaim::QuerySystemMemory();
				Log( "[mem-reclaim] ops=%d load=%u%%->%u%% avail=%llu->%llu MB (auto=%s)",
					ops, stats.memoryLoadPct, after.memoryLoadPct,
					(unsigned long long)stats.availPhysMB,
					(unsigned long long)after.availPhysMB,
					pressure ? "yes" : "no" );
			}
		}
	}
}

// ── Recovery thread (RELAUNCHING → INJECTING) ──
//
// Запускается detached из MonitorTick когда watchdog сказал "пора relaunch".
// Не блокирует main monitor loop.
//
// Sequence (полное восстановление инстанса):
//   1. Kill dota2.exe (если жива) ИМЕННО этого инстанса — TerminateProcess + wait.
//   2. KillSteamForInstance — убить ровно тот steam.exe чей image path лежит в
//      C:\BotSteam\<idx>\ (НЕ main Steam юзера). Использует bot.steamPid точечно
//      + path-prefix scan для зомби steamwebhelper.
//   3. Sleep 5s — даём Windows освободить file locks / mutex'ы.
//   4. m_steamLauncher.LaunchInstance(idx, m_config) — те же args/sandbox/login
//      что и при первоначальном запуске StartFarmThread. Steam с -applaunch
//      может сразу спаунить dota.
//   5. Poll до 60s пока новый steam.exe из C:\BotSteam\<idx>\ alive.
//   6. LaunchDotaOnly — спавн dota2.exe (если LaunchInstance ещё не успела).
//   7. m_watchdog.OnRelaunchStarted → INJECTING.
//   8. WaitForDotaReady (client.dll) → reinject ProxyHook + Andromeda.
//
// Если на любом шаге fail (Steam не поднялся, dota не появилась) — просто
// return; watchdog timeout (120s в RELAUNCHING) переведёт обратно в
// CONFIRMED_DEAD откуда пойдёт ещё одна попытка (если crash budget позволит).

void Orchestrator::RecoveryThread( int idx )
{
	if ( idx < 0 || idx >= m_nBotCount ) return;

	// CR-FIX 2026-05-26: global serialization. До этого LaunchDotaOnly мог
	// вернуть один и тот же PID двум параллельным recovery thread'ам (см.
	// orchestrator.h m_recoveryMx). Также избегаем concurrent shader recompile
	// на WARP (5 parallel = всё лагает 1FPS, recovery ещё хуже).
	std::lock_guard<std::mutex> recoveryLock( m_recoveryMx );

	auto& bot = m_bots[idx];

	DWORD oldSteamPid = bot.steamPid;
	DWORD oldDotaPid  = bot.dotaPid;

	// Решаем нужен ли full restart Steam'а. Если steam.exe жив И до этого был
	// gcReady — это чисто dota-side crash, экономим 70-90s downtime пропуская
	// kill+relaunch Steam (см. crash_watchdog.h CrashRecoveryConfig::steamRelaunchEnabled).
	//
	// CR-FIX 2026-05-26: ALSO force Steam restart на 2-м+ crash подряд того же
	// бота. Симптом из field-debug 2026-05-24: dota crashed в terminal stage
	// (WARP GPU 1FPS) → recovery пропустил Steam restart (steam_alive+gc_ready)
	// → новая dota fast-fail (Steam session state отравлен после crash) →
	// watchdog → DEAD. Steam restart на retry #2 чистит session/IPC state.
	const bool steamAlive = oldSteamPid &&
		DotaLauncher::IsProcessAlive( oldSteamPid );
	const bool gcWasReady = bot.conn.gcReady;
	const int  crashCount = m_watchdog.GetCrashCount( idx );
	const bool needSteamRestart =
		m_config.crashRecovery.steamRelaunchEnabled &&
		( !steamAlive || !gcWasReady || crashCount >= 2 );

	Log( "#%d: recovery: %s — kill dota=%lu steam=%lu (steam_alive=%d gc_ready=%d crashes=%d)",
		idx, needSteamRestart ? "full restart" : "dota-only restart",
		oldDotaPid, oldSteamPid, (int)steamAlive, (int)gcWasReady, crashCount );

	// B3: cleanup stale per-pid state files в C:\temp\andromeda\ — защита от
	// PID reuse и от того что match_pending-scanner подхватит старую status.
	if ( oldDotaPid )
	{
		char p[MAX_PATH];
		snprintf( p, sizeof(p), "C:\\temp\\andromeda\\status_%lu.json",   oldDotaPid ); DeleteFileA( p );
		snprintf( p, sizeof(p), "C:\\temp\\andromeda\\instance_%lu.json", oldDotaPid ); DeleteFileA( p );
		snprintf( p, sizeof(p), "C:\\temp\\andromeda\\command_%lu.json",  oldDotaPid ); DeleteFileA( p );
		snprintf( p, sizeof(p), "C:\\temp\\andromeda\\proxy_%lu.json",    oldDotaPid ); DeleteFileA( p );
	}

	// 1. Kill dota если ещё бегает.
	if ( oldDotaPid && DotaLauncher::IsProcessAlive( oldDotaPid ) )
	{
		HANDLE h = OpenProcess(
			PROCESS_TERMINATE | SYNCHRONIZE, FALSE, oldDotaPid );
		if ( h )
		{
			TerminateProcess( h, 1 );
			WaitForSingleObject( h, 3000 );
			CloseHandle( h );
		}
	}
	bot.dotaPid = 0;
	bot.injected = false;
	bot.dotaReady = false;

	DWORD newSteamPid = oldSteamPid;

	if ( needSteamRestart )
	{
		// 2. Kill steam ИМЕННО этого инстанса — точечно по pid + scan path-prefix
		// C:\BotSteam\<idx>\. Main Steam юзера (Program Files) не трогаем.
		SteamLauncher::KillSteamForInstance( idx, oldSteamPid );
		bot.steamPid = 0;
		bot.conn.gcReady = false;

		// 3. Cool-down: NTFS file locks / mutex flush.
		std::this_thread::sleep_for( std::chrono::seconds( 5 ) );

		// 4. Запустить Steam заново — те же args/sandbox/login что StartFarmThread.
		DWORD spawnedSteamPid = m_steamLauncher.LaunchInstance( idx, m_config );
		if ( !spawnedSteamPid )
		{
			Log( "#%d: recovery: LaunchInstance returned 0 — abort", idx );
			return; // watchdog timeout эскалирует обратно в CONFIRMED_DEAD
		}

		// 5. Poll до 60s — ждём чтобы steam.exe реально жил (LaunchInstance может
		// вернуть pid сразу, но Steam Web Helper и IPC поднимаются ~10-30s).
		newSteamPid = spawnedSteamPid;
		{
			DWORD start = GetTickCount();
			bool ready = false;
			while ( (int)( GetTickCount() - start ) < 60000 )
			{
				if ( !DotaLauncher::IsProcessAlive( newSteamPid ) )
				{
					// Спавн умер — Steam часто перезапускает себя через single-
					// instance check. Поищем live steam.exe в C:\BotSteam\<idx>\.
					char botPrefix[MAX_PATH];
					snprintf( botPrefix, sizeof( botPrefix ),
						"C:\\BotSteam\\%d\\", idx );
					size_t prefixLen = strlen( botPrefix );

					HANDLE snap = CreateToolhelp32Snapshot( TH32CS_SNAPPROCESS, 0 );
					if ( snap != INVALID_HANDLE_VALUE )
					{
						PROCESSENTRY32 pe{};
						pe.dwSize = sizeof( pe );
						DWORD found = 0;
						if ( Process32First( snap, &pe ) )
						{
							do
							{
								if ( _stricmp( pe.szExeFile, "steam.exe" ) != 0 )
									continue;
								HANDLE hP = OpenProcess(
									PROCESS_QUERY_LIMITED_INFORMATION,
									FALSE, pe.th32ProcessID );
								if ( !hP ) continue;
								char img[MAX_PATH] = {};
								DWORD n = sizeof( img );
								if ( QueryFullProcessImageNameA( hP, 0, img, &n )
									&& _strnicmp( img, botPrefix, prefixLen ) == 0 )
								{
									found = pe.th32ProcessID;
								}
								CloseHandle( hP );
								if ( found ) break;
							}
							while ( Process32Next( snap, &pe ) );
						}
						CloseHandle( snap );
						if ( found ) { newSteamPid = found; }
						else
						{
							std::this_thread::sleep_for( std::chrono::milliseconds( 500 ) );
							continue;
						}
					}
				}
				// 5s от первой жизни — считаем готовым.
				if ( (int)( GetTickCount() - start ) >= 5000 )
				{
					ready = true;
					break;
				}
				std::this_thread::sleep_for( std::chrono::milliseconds( 500 ) );
			}
			if ( !ready )
			{
				Log( "#%d: recovery: steam not ready after 60s — abort", idx );
				return;
			}
		}

		bot.steamPid = newSteamPid;
		Log( "#%d: recovery: steam.exe PID %lu ready", idx, newSteamPid );
	}
	else
	{
		Log( "#%d: recovery: Steam alive+GC ready, skipping Steam restart (pid=%lu)",
			idx, newSteamPid );
	}

	// 6. Перед relaunch — убить ТОЛЬКО zombie dota2.exe ЭТОГО конкретного
	// бота (по bot.dotaPid от watchdog'а), чтобы освободить file handles на
	// gameinfo.gi / VPK / shaders. Иначе новая dota2 ловит "FATAL ERROR:
	// Application unable to load gameinfo.gi" потому что zombie процесс
	// ещё держит lock. КРИТИЧНО: НЕ трогать другие dota2.exe (4 живых бота
	// в ферме параллельно — kill всех = катастрофа).
	if ( bot.dotaPid != 0 )
	{
		HANDLE h = OpenProcess( PROCESS_TERMINATE | SYNCHRONIZE,
			FALSE, bot.dotaPid );
		if ( h )
		{
			DWORD ec = 0;
			if ( GetExitCodeProcess( h, &ec ) && ec == STILL_ACTIVE )
			{
				TerminateProcess( h, 0 );
				WaitForSingleObject( h, 2000 );
				Log( "#%d: recovery: killed zombie dota2.exe PID %lu to release gameinfo.gi handle",
					idx, bot.dotaPid );
			}
			CloseHandle( h );
		}
	}

	// 7. LaunchDotaOnly — даже если LaunchInstance уже -applaunch'нула dota,
	// LaunchDotaOnly корректно поллит "новый PID не из excluded" и вернёт его.
	std::vector<DWORD> existing = DotaLauncher::FindDotaPids();

	DWORD newDotaPid = m_dotaLauncher.LaunchDotaOnly(
		idx, m_config, bot.steamPid, existing, 120000 );

	if ( !newDotaPid )
	{
		Log( "#%d: recovery: LaunchDotaOnly timeout (120s)", idx );
		// Watchdog timeout перенесёт нас обратно в CONFIRMED_DEAD.
		return;
	}

	bot.dotaPid = newDotaPid;
	Log( "#%d: recovery: dota2.exe PID %lu spawned", idx, newDotaPid );
	m_watchdog.OnRelaunchStarted( idx, newDotaPid );

	// 2026-05-22: НЕ применяем affinity до WaitForDotaReady. Source 2 init
	// CPU-bound (schema parse, shader compile на WARP), ограничение ядер
	// при init → client.dll timeout (recovery crash 2026-05-22 @ cores=2).
	// Wait client.dll — timeout увеличен с 60s до 180s т.к. на WARP+spoofer
	// + bg-load Steam родительский процесс может затянуть start-up.
	if ( !m_dotaLauncher.WaitForDotaReady( newDotaPid, 180000 ) )
	{
		Log( "#%d: recovery: client.dll timeout (180s)", idx );
		return;
	}
	bot.dotaReady = true;

	// client.dll готов — теперь безопасно сузить affinity.
	if ( m_config.coresPerInstance > 0 )
	{
		bool ok = ApplyDotaCpuAffinity( newDotaPid, idx, m_config.coresPerInstance );
		Log( "#%d: recovery: cpu affinity %s (cores=%d, post-ready)", idx,
			ok ? "applied" : "FAILED", m_config.coresPerInstance );
	}

	// Re-inject ProxyHook (используем ТОТ ЖЕ hwidSeed что и при оригинале — иначе
	// HWID change mid-match → instant VAC flag). См. RECONNECT_DESIGN.md §7.
	if ( idx < (int)m_config.accounts.size() )
	{
		const auto& acc = m_config.accounts[idx];
		bool dllAvail = !m_config.proxyHookDllPath.empty() &&
			GetFileAttributesA( m_config.proxyHookDllPath.c_str() ) != INVALID_FILE_ATTRIBUTES;
		bool proxyViaHook = acc.proxyEnabled && !acc.proxy.empty();
		bool hwidViaHook  = m_config.IsSteamSpoofEnabled() && acc.hwidSpoofEnabled
			&& acc.steamId != 0;

		if ( dllAvail && ( proxyViaHook || hwidViaHook ) )
		{
			std::string proxyForDll = proxyViaHook ? acc.proxy : std::string();
			std::string hwidSeed = m_watchdog.GetHwidSeed( idx );
			if ( hwidSeed.empty() && hwidViaHook )
			{
				// Никогда не было первого инжекта (странно, но fallback).
				time_t now = time( nullptr );
				tm tmv{};
				localtime_s( &tmv, &now );
				char buf[64];
				snprintf( buf, sizeof( buf ), "%llu_%04d-%02d",
					(unsigned long long)acc.steamId,
					tmv.tm_year + 1900, tmv.tm_mon + 1 );
				hwidSeed = buf;
				m_watchdog.SetHwidSeed( idx, hwidSeed );
			}

			if ( m_config.crashRecovery.respoofHwidOnRelaunch && hwidViaHook )
			{
				// Юзер явно попросил respoof — генерим новый seed (рискованно!).
				time_t now = time( nullptr );
				char buf[64];
				snprintf( buf, sizeof( buf ), "%llu_%lld",
					(unsigned long long)acc.steamId, (long long)now );
				hwidSeed = buf;
				m_watchdog.SetHwidSeed( idx, hwidSeed );
				Log( "#%d: recovery: HWID re-spoofed (NEW seed) — VAC risk!", idx );
			}

			Injector::WriteProxyConfig( newDotaPid, proxyForDll, hwidSeed );
			// B2: retry inject 3x с backoff. Двойной inject безопасен — LoadLibraryA
			// при повторе increment'ит refcount, DllMain второй раз не вызывается.
			bool phOk = false;
			// MAPPER-FIX 2026-05-18: manual map обратно.
			auto& phBufR = EmbeddedDllBuf( "PROXYHOOK_DLL" );
			for ( int attempt = 0; attempt < 3 && !phOk; ++attempt )
			{
				if ( attempt > 0 )
				{
					int backoffMs = 1000 * ( 1 << attempt ); // 2s, 4s
					Log( "#%d: recovery: ProxyHook inject retry %d/3 in %dms",
						idx, attempt + 1, backoffMs );
					std::this_thread::sleep_for( std::chrono::milliseconds( backoffMs ) );
					if ( !DotaLauncher::IsProcessAlive( newDotaPid ) )
					{
						Log( "#%d: recovery: dota died during ProxyHook inject retry — abort", idx );
						return;
					}
				}
				phOk = !phBufR.empty()
					&& m_injector.InjectManualMap( newDotaPid, phBufR.data(), phBufR.size() );
				if ( !phOk && !phBufR.empty() )
				{
					Log( "#%d: recovery: ProxyHook MM failed → fallback (temp-file LoadLibrary)", idx );
					phOk = m_injector.InjectViaTempFile( newDotaPid,
						phBufR.data(), phBufR.size(), "ph" );
				}
			}
			if ( phOk )
				Log( "#%d: recovery: ProxyHook reinjected (hwid='%s')", idx, hwidSeed.c_str() );
			else
				Log( "#%d: recovery: ProxyHook INJECT FAILED after 3 tries (hwid='%s')",
					idx, hwidSeed.c_str() );
		}
	}

	// MAPPER-FIX 2026-05-18: manual map снова используется.
	auto& andromedaBufR = EmbeddedDllBuf( "ANDROMEDA_DLL" );
	if ( andromedaBufR.empty() )
	{
		Log( "#%d: recovery: embedded ANDROMEDA_DLL missing/decrypt-failed — abort", idx );
		return;
	}

	// Build instance config (тот же idx/role/hero/party — recovery должен match-ить
	// pre-crash state).
	int leaderIdx = -1;
	for ( int j = 0; j < m_nBotCount; j++ )
	{
		if ( j < (int)m_config.accounts.size() && m_config.accounts[j].enabled )
		{ leaderIdx = j; break; }
	}
	const char* role = ( idx == leaderIdx ) ? "leader" : "member";

	uint64_t partyMembers[4];
	int partyCount = 0;
	BuildPartyMembers( idx, partyMembers, partyCount );

	std::vector<std::string> heroPool;
	heroPool.push_back( m_config.GetHero( idx ) );
	for ( size_t h = 0; h < m_config.heroes.size(); h++ )
		if ( m_config.heroes[h] != heroPool[0] )
			heroPool.push_back( m_config.heroes[h] );

	// B4: если знаем lobby_id (был активный match до краша) — передаём DLL'е
	// чтобы она стартовала в reconnect-режиме, а не в matchmaking-init.
	// Fallback chain: bot.match.lobbyId (свежий status) → watchdog last-known
	// (C1, обновляется в Tick) → 0 (DLL пойдёт обычным auto_queue).
	uint64_t reconnectLobbyId = bot.match.lobbyId;
	if ( reconnectLobbyId == 0 )
		reconnectLobbyId = m_watchdog.GetLastKnownLobbyId( idx );

	Injector::WriteInstanceConfig( newDotaPid, idx, role,
		heroPool, partyMembers, partyCount,
		m_config.region, m_config.gameMode,
		reconnectLobbyId );

	// B2: retry Andromeda inject 3x с backoff. WaitForSingleObject(10s) в Injector
	// может тайм-аутнуть пока процесс busy initial-module-load — повторим.
	bool andromedaOk = false;
	for ( int attempt = 0; attempt < 3 && !andromedaOk; ++attempt )
	{
		if ( attempt > 0 )
		{
			int backoffMs = 1000 * ( 1 << attempt ); // 2s, 4s
			Log( "#%d: recovery: Andromeda inject retry %d/3 in %dms",
				idx, attempt + 1, backoffMs );
			std::this_thread::sleep_for( std::chrono::milliseconds( backoffMs ) );
			if ( !DotaLauncher::IsProcessAlive( newDotaPid ) )
			{
				Log( "#%d: recovery: dota died during Andromeda inject retry — abort", idx );
				return;
			}
		}
		// MAPPER-FIX 2026-05-21 (v3): Andromeda — temp-file LoadLibrary first
		// (MM crashes dota2 from inside Andromeda's VMP-wrapped DllMain).
		andromedaOk = m_injector.InjectViaTempFile( newDotaPid,
			andromedaBufR.data(), andromedaBufR.size(), "andro" );
		if ( !andromedaOk )
		{
			Log( "#%d: recovery: temp-file LoadLibrary failed → secondary MM attempt", idx );
			andromedaOk = m_injector.InjectManualMap( newDotaPid,
				andromedaBufR.data(), andromedaBufR.size() );
		}
	}
	if ( andromedaOk )
	{
		bot.injected = true;
		Log( "#%d: recovery: Andromeda reinjected (role=%s)", idx, role );
		m_watchdog.OnInjected( idx );
	}
	else
	{
		Log( "#%d: recovery: Andromeda INJECT FAILED after 3 tries", idx );
	}
}

// ── Issue reconnect command для DLL (быстрый action) ──

void Orchestrator::IssueReconnect( int idx )
{
	if ( idx < 0 || idx >= m_nBotCount ) return;
	auto& bot = m_bots[idx];
	if ( !bot.dotaPid ) return;

	// C1: fallback chain. bot.match.lobbyId может быть 0 если status_<pid>.json
	// был cleanup'нут или ещё не написан — берём из watchdog snapshot.
	uint64_t lobbyId = bot.match.lobbyId;
	if ( !lobbyId )
		lobbyId = m_watchdog.GetLastKnownLobbyId( idx );
	if ( !lobbyId )
	{
		Log( "#%d: reconnect skipped — no known lobby", idx );
		m_watchdog.OnReconnectIssued( idx, crash_watchdog::NowMs() );
		return;
	}

	bool ok = crash_watchdog::WriteReconnectCommand( bot.dotaPid, lobbyId );
	Log( "#%d: command_<pid>.json reconnect lobby=%llu: %s",
		idx, (unsigned long long)lobbyId, ok ? "WRITTEN" : "FAILED" );
	m_watchdog.OnReconnectIssued( idx, crash_watchdog::NowMs() );
}

// Маленький helper для безопасного чтения JSON
template<typename T>
static T jget( const json& j, const char* key, const T& def )
{
	auto it = j.find( key );
	if ( it == j.end() ) return def;
	try { return it->get<T>(); } catch ( ... ) { return def; }
}

void Orchestrator::ReadStatusFiles()
{
	for ( int i = 0; i < m_nBotCount; i++ )
	{
		auto& bot = m_bots[i];
		if ( !bot.dotaPid )
			continue;

		char path[MAX_PATH];
		snprintf( path, sizeof( path ), "C:\\temp\\andromeda\\status_%lu.json", bot.dotaPid );

		std::ifstream file( path );
		if ( !file.is_open() )
			continue;

		try
		{
			json j;
			file >> j;

			int schema = jget<int>( j, "schema", 1 );
			bot.schema = schema;

			// Schema 1: плоский старый формат
			bot.state       = jget<std::string>( j, "state", bot.state );
			bot.hero        = jget<std::string>( j, "hero", bot.hero );
			bot.hp          = jget<int>( j, "hp", bot.hp );
			bot.maxHp       = jget<int>( j, "maxHp", bot.maxHp );
			bot.gameTime    = jget<float>( j, "gameTime", bot.gameTime );
			bot.gamesPlayed = jget<int>( j, "gamesPlayed", bot.gamesPlayed );
			bot.role        = jget<std::string>( j, "role", bot.role );

			// Schema 2: расширенный
			if ( schema >= 2 )
			{
				bot.ownSteamId  = jget<uint64_t>( j, "own_steam_id", 0 );
				bot.gameState   = jget<std::string>( j, "game_state", bot.gameState );
				bot.gameStateId = jget<int>( j, "game_state_id", -1 );
				bot.heartbeatMs = jget<int64_t>( j, "heartbeat_ms", 0 );
				bot.paused      = jget<bool>( j, "paused", false );

				if ( bot.heartbeatMs > 0 )
				{
					int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
						std::chrono::system_clock::now().time_since_epoch() ).count();
					bot.heartbeatAgeMs = now - bot.heartbeatMs;
				}

				if ( j.contains( "sm" ) && j["sm"].is_object() )
				{
					auto& sm = j["sm"];
					bot.state = jget<std::string>( sm, "state", bot.state );
					bot.smStateSeconds = jget<float>( sm, "state_seconds", 0.f );
				}

				if ( j.contains( "connection" ) && j["connection"].is_object() )
				{
					auto& c = j["connection"];
					bot.conn.gcReady       = jget<bool>( c, "gc_ready", false );
					bot.conn.gcMsgsTotal   = jget<uint32_t>( c, "gc_msgs_total", 0 );
					bot.conn.clientVersion = jget<uint32_t>( c, "client_version", 0 );
					bot.conn.pingDataBytes = jget<uint32_t>( c, "ping_data_bytes", 0 );
				}

				if ( j.contains( "party" ) && j["party"].is_object() )
				{
					auto& p = j["party"];
					bot.party.id              = jget<uint64_t>( p, "id", 0 );
					bot.party.size            = jget<int>( p, "size", 0 );
					bot.party.leaderSteamId   = jget<uint64_t>( p, "leader_steam_id", 0 );
					bot.party.isLeader        = jget<bool>( p, "is_leader", false );
					bot.party.pendingInviteId = jget<uint64_t>( p, "pending_invite_id", 0 );
					bot.party.memberSteamIds.clear();
					if ( p.contains( "member_steam_ids" ) && p["member_steam_ids"].is_array() )
					{
						for ( auto& v : p["member_steam_ids"] )
						{
							try { bot.party.memberSteamIds.push_back( v.get<uint64_t>() ); }
							catch ( ... ) {}
						}
					}
				}

				if ( j.contains( "queue" ) && j["queue"].is_object() )
				{
					auto& q = j["queue"];
					bot.queue.inQueue    = jget<bool>( q, "in_queue", false );
					bot.queue.resultCode = jget<int>( q, "result_code", -1 );
				}

				if ( j.contains( "match" ) && j["match"].is_object() )
				{
					auto& m = j["match"];
					bot.match.found   = jget<bool>( m, "found", false );
					bot.match.lobbyId = jget<uint64_t>( m, "lobby_id", 0 );
				}

				if ( j.contains( "game" ) && j["game"].is_object() )
				{
					auto& g = j["game"];
					bot.alive = jget<bool>( g, "alive", false );
				}
			}

			// Log first status from each bot (DLL is alive)
			bool wasEmpty = bot.lastStatusUpdate == std::chrono::steady_clock::time_point{};
			bot.lastStatusUpdate = std::chrono::steady_clock::now();
			if ( wasEmpty )
				Log( "#%d: DLL alive — schema=%d state=%s role=%s sid=%llu",
					i, schema, bot.state.c_str(), bot.role.c_str(),
					(unsigned long long)bot.ownSteamId );
		}
		catch ( ... ) {}
	}
}

void Orchestrator::RestartInstance( int idx )
{
	auto dotas = DotaLauncher::FindDotaPids();
	for ( DWORD pid : dotas )
		m_dotaLauncher.KillDotaMutex( pid, m_config.handleExe );

	Sleep( 1000 );

	DWORD steamPid = m_steamLauncher.LaunchInstance( idx, m_config );
	m_bots[idx].steamPid = steamPid;
	Log( "#%d: restarting (Steam PID %lu)", idx, steamPid );
}

// ── Setup: account login ──

DWORD Orchestrator::LaunchSteamForLogin( int idx )
{
	if ( idx >= m_nBotCount ) return 0;

	// Set registry to this account
	SteamLauncher::SetAutoLogin( idx, m_config );

	// Get Steam exe
	auto [steamExe, steamDir] = SteamLauncher::GetSteamExeFor( idx, m_config );

	// Launch bare Steam (no Dota, no IPC override — normal login window)
	std::string cmdLine = "\"" + steamExe + "\"";

	STARTUPINFOA si{};
	si.cb = sizeof( si );
	PROCESS_INFORMATION pi{};

	BOOL ok = CreateProcessA( nullptr, const_cast<char*>( cmdLine.c_str() ),
		nullptr, nullptr, FALSE, 0, nullptr, steamDir.c_str(), &si, &pi );

	if ( !ok ) return 0;

	DWORD pid = pi.dwProcessId;
	CloseHandle( pi.hProcess );
	CloseHandle( pi.hThread );

	Log( "Setup: launched Steam for account #%d (%s)", idx, m_config.accounts[idx].login.c_str() );
	return pid;
}

void Orchestrator::KillSteamProcess( DWORD pid )
{
	if ( !pid ) return;
	HANDLE h = OpenProcess( PROCESS_TERMINATE, FALSE, pid );
	if ( h ) { TerminateProcess( h, 0 ); CloseHandle( h ); }

	// Also kill steamwebhelper etc
	SteamLauncher::KillAllSteam();
}

bool Orchestrator::IsAccountSetUp( int idx )
{
	// Check if Steam has saved credentials for this account
	// by looking at loginusers.vdf in the Steam config directory
	std::string steamDir = m_config.steamExe;
	auto sl = steamDir.find_last_of( "\\/" );
	if ( sl != std::string::npos ) steamDir = steamDir.substr( 0, sl );

	// Also check per-bot Steam dir
	char botDir[MAX_PATH];
	snprintf( botDir, sizeof( botDir ), "C:\\BotSteam\\%d\\config\\loginusers.vdf", idx );
	if ( GetFileAttributesA( botDir ) != INVALID_FILE_ATTRIBUTES )
		return true;

	// Check main Steam loginusers.vdf for this account's name
	std::string vdfPath = steamDir + "\\config\\loginusers.vdf";
	if ( idx < (int)m_config.accounts.size() )
	{
		std::ifstream f( vdfPath );
		if ( f.is_open() )
		{
			std::string line;
			while ( std::getline( f, line ) )
			{
				if ( line.find( m_config.accounts[idx].login ) != std::string::npos )
					return true;
			}
		}
	}

	return false;
}

bool Orchestrator::AreAllAccountsSetUp()
{
	for ( int i = 0; i < m_nBotCount; i++ )
		if ( !IsAccountSetUp( i ) )
			return false;
	return true;
}

void Orchestrator::BuildPartyMembers( int excludeIdx, uint64_t* out, int& count )
{
	count = 0;
	for ( int i = 0; i < m_nBotCount && count < 4; i++ )
	{
		// Skip disabled accounts — don't invite them
		if ( i < (int)m_config.accounts.size() && !m_config.accounts[i].enabled )
			continue;
		if ( i == excludeIdx )
			continue;
		out[count++] = m_config.accounts[i].steamId;
	}
}

int Orchestrator::GetTotalGames() const
{
	return m_nBotCount > 0 ? m_bots[0].gamesPlayed : 0;
}

const char* Orchestrator::GetStateStr() const
{
	switch ( m_state )
	{
	case State::IDLE:             return "IDLE";
	case State::LAUNCHING:        return "LAUNCHING";
	case State::WAITING_READY:    return "WAITING_READY";
	case State::INJECTING:        return "INJECTING";
	case State::WAITING_GC_READY: return "WAITING_GC";
	case State::RUNNING:          return "RUNNING";
	case State::STOPPING:         return "STOPPING";
	case State::ERROR_STATE:      return "ERROR";
	default:                      return "UNKNOWN";
	}
}

// ── GSI ─────────────────────────────────────────────────

bool Orchestrator::EnsureGsi()
{
	// 1) Запустить HTTP listener (идемпотентно)
	auto& server = GetGsiServer();
	if ( !server.IsRunning() )
	{
		if ( !server.Start( 3477 ) )
		{
			m_gsiInstallError = "GsiServer.Start failed: " + server.GetLastError();
			return false;
		}
	}

	// 2) Найти Dota и положить cfg
	std::string dotaPath = gsi_install::FindDotaInstall( m_config.steamExe );
	if ( dotaPath.empty() )
	{
		m_gsiInstallError = "Dota install not found (steamExe=" + m_config.steamExe + ")";
		return false;
	}

	// Токен — фиксированный per-machine. Меняем редко: Dota подхватит при перезапуске.
	// В JSON GSI всегда шлёт auth.token — orchestrator может его сверять (пока опционально).
	const std::string token = "andromeda_v1";

	std::string cfgPath, err;
	if ( !gsi_install::InstallGsiConfig( dotaPath, server.GetPort(), token, &cfgPath, &err ) )
	{
		m_gsiInstallError = "InstallGsiConfig failed: " + err;
		return false;
	}

	m_gsiCfgPath = cfgPath;
	m_gsiInstallError.clear();
	m_gsiInstalled = true;
	return true;
}

Orchestrator::GsiStatus Orchestrator::GetGsiStatus() const
{
	GsiStatus s;
	auto& server = GetGsiServer();
	s.running = server.IsRunning();
	s.port = server.GetPort();
	s.totalRequests = server.GetTotalRequests();
	s.seenSteamIds = server.GetSeenSteamIdCount();
	s.cfgPath = m_gsiCfgPath;
	s.lastError = m_gsiInstallError;
	return s;
}

static bool ReadOneHealth( DWORD pid, BotHealth& out )
{
	if ( pid == 0 ) return false;

	char path[MAX_PATH];
	snprintf( path, sizeof( path ), "C:\\temp\\andromeda\\proxyhook_health_%lu.json", pid );

	std::ifstream f( path );
	if ( !f.is_open() ) return false;

	try
	{
		json j;
		f >> j;

		out.seen = true;
		if ( j.contains( "written_ms" ) ) out.writtenMs = j["written_ms"].get<int64_t>();

		FILETIME ft; GetSystemTimeAsFileTime( &ft );
		ULARGE_INTEGER u; u.LowPart = ft.dwLowDateTime; u.HighPart = ft.dwHighDateTime;
		int64_t nowMs = (int64_t)( u.QuadPart / 10000ULL - 11644473600000ULL );
		out.ageMs = ( out.writtenMs > 0 ) ? ( nowMs - out.writtenMs ) : -1;

		if ( j.contains( "proxy" ) && j["proxy"].is_object() )
		{
			auto& jp = j["proxy"];
			if ( jp.contains( "enabled" ) )         out.proxyHookActive = jp["enabled"].get<bool>();
			if ( jp.contains( "raw_url" ) )         out.proxyRawUrl = jp["raw_url"].get<std::string>();
			if ( jp.contains( "last_probe_ok" ) )   out.probeOk = jp["last_probe_ok"].get<bool>();
			if ( jp.contains( "last_probe_exit_ip" ) ) out.probeExitIp = jp["last_probe_exit_ip"].get<std::string>();
			if ( jp.contains( "last_probe_latency" ) ) out.probeLatencyMs = jp["last_probe_latency"].get<int64_t>();
			if ( jp.contains( "last_probe_error" ) ) out.probeError = jp["last_probe_error"].get<std::string>();
			if ( jp.contains( "last_probe_mode" ) )  out.probeMode = jp["last_probe_mode"].get<std::string>();
			if ( jp.contains( "last_probe_ms" ) )
			{
				int64_t pm = jp["last_probe_ms"].get<int64_t>();
				out.probeAgeMs = ( pm > 0 ) ? ( nowMs - pm ) : -1;
			}
			if ( jp.contains( "socks5_ok" ) )       out.socks5Ok = jp["socks5_ok"].get<uint64_t>();
			if ( jp.contains( "socks5_fail" ) )     out.socks5Fail = jp["socks5_fail"].get<uint64_t>();
		}

		if ( j.contains( "hwid" ) && j["hwid"].is_object() )
		{
			auto& jh = j["hwid"];
			if ( jh.contains( "enabled" ) )     out.hwidEnabled = jh["enabled"].get<bool>();
			if ( jh.contains( "seed" ) )        out.hwidSeed = jh["seed"].get<std::string>();
			if ( jh.contains( "machine_guid_match" ) )  out.machineGuidMatch = jh["machine_guid_match"].get<bool>();
			if ( jh.contains( "mac_match" ) )           out.macMatch = jh["mac_match"].get<bool>();
			if ( jh.contains( "volume_serial_match" ) ) out.volumeSerialMatch = jh["volume_serial_match"].get<bool>();
			if ( jh.contains( "system_serial_match" ) ) out.systemSerialMatch = jh["system_serial_match"].get<bool>();
			if ( jh.contains( "system_serial_patched" ) ) out.systemSerialPatched = jh["system_serial_patched"].get<bool>();
			if ( jh.contains( "critical_match" ) )      out.criticalMatch = jh["critical_match"].get<bool>();
			if ( jh.contains( "all_match" ) )           out.allMatch = jh["all_match"].get<bool>();
			if ( jh.contains( "expected" ) && jh["expected"].is_object() )
			{
				auto& je = jh["expected"];
				if ( je.contains( "machine_guid" ) ) out.expectedMachineGuid = je["machine_guid"].get<std::string>();
				if ( je.contains( "mac" ) )          out.expectedMac = je["mac"].get<std::string>();
				if ( je.contains( "system_serial" ) ) out.expectedSystemSerial = je["system_serial"].get<std::string>();
				if ( je.contains( "volume_serial" ) ) out.expectedVolumeSerial = je["volume_serial"].get<uint32_t>();
			}
			if ( jh.contains( "observed" ) && jh["observed"].is_object() )
			{
				auto& jo = jh["observed"];
				if ( jo.contains( "machine_guid" ) ) out.observedMachineGuid = jo["machine_guid"].get<std::string>();
				if ( jo.contains( "mac" ) )          out.observedMac = jo["mac"].get<std::string>();
				if ( jo.contains( "system_serial" ) ) out.observedSystemSerial = jo["system_serial"].get<std::string>();
				if ( jo.contains( "volume_serial" ) ) out.observedVolumeSerial = jo["volume_serial"].get<uint32_t>();
			}
		}
		return true;
	}
	catch ( ... )
	{
		return false;
	}
}

void Orchestrator::ReadHealthFiles()
{
	for ( int i = 0; i < m_nBotCount; i++ )
	{
		auto& bot = m_bots[i];
		BotHealth hDota, hSteam;

		// Dota приоритет — она главный бот-процесс. Если не видна — используем Steam.
		bool gotDota  = ReadOneHealth( bot.dotaPid, hDota );
		bool gotSteam = ReadOneHealth( bot.steamPid, hSteam );

		if ( gotDota )
			bot.health = hDota;
		else if ( gotSteam )
			bot.health = hSteam;
		// else: оставляем предыдущий snapshot
	}
}

void Orchestrator::ReadGsiSnapshots()
{
	auto& server = GetGsiServer();
	if ( !server.IsRunning() )
		return;

	for ( int i = 0; i < m_nBotCount; i++ )
	{
		auto& bot = m_bots[i];

		// Нет аккаунта — нечего матчить
		if ( i >= (int)m_config.accounts.size() ) continue;
		uint64_t sid = m_config.accounts[i].steamId;
		if ( sid == 0 ) continue;

		GsiSnapshot snap;
		if ( server.GetSnapshot( sid, snap ) )
		{
			bot.gsi = snap;
			bot.gsiSeen = true;
			bot.gsiAgeMs = server.GetAgeMs( sid );

			// Если GSI шлёт hero/maxHp — заполним резервно (DLL перезапишет если живёт)
			if ( bot.maxHp == 0 && snap.maxHp > 0 )
			{
				bot.hp = snap.hp;
				bot.maxHp = snap.maxHp;
			}
			if ( bot.hero.empty() && !snap.heroName.empty() )
				bot.hero = snap.heroName;
			if ( snap.gameTime > bot.gameTime )
				bot.gameTime = snap.gameTime;
		}
	}
}

// ── Pairing helpers ──

void Orchestrator::ReadMatchPendingFiles()
{
	if ( !m_config.pairing.enabled ) return;

	// Build pid → idx map (refresh каждый Tick — pids меняются после relaunch).
	m_pidToBotIdx.clear();
	for ( int i = 0; i < m_nBotCount; i++ )
	{
		if ( m_bots[i].dotaPid )
			m_pidToBotIdx[m_bots[i].dotaPid] = i;
	}

	WIN32_FIND_DATAA fd;
	HANDLE hFind = FindFirstFileA( "C:\\temp\\andromeda\\match_pending_*.json", &fd );
	if ( hFind == INVALID_HANDLE_VALUE ) return;

	FILETIME ftNow;
	GetSystemTimeAsFileTime( &ftNow );
	ULARGE_INTEGER nowU;
	nowU.LowPart = ftNow.dwLowDateTime;
	nowU.HighPart = ftNow.dwHighDateTime;

	do
	{
		if ( fd.cFileName[0] == '.' ) continue;

		// Извлечь pid из имени.
		// match_pending_<pid>.json
		const char* p = strchr( fd.cFileName, '_' );
		if ( !p ) continue;
		p = strchr( p + 1, '_' );
		if ( !p ) continue;
		p++;
		DWORD pid = (DWORD)strtoul( p, nullptr, 10 );
		if ( !pid ) continue;

		std::string fullPath = std::string( "C:\\temp\\andromeda\\" ) + fd.cFileName;

		// Stale (>60s) — удалить.
		ULARGE_INTEGER ft;
		ft.LowPart  = fd.ftLastWriteTime.dwLowDateTime;
		ft.HighPart = fd.ftLastWriteTime.dwHighDateTime;
		int64_t ageMs = (int64_t)( ( nowU.QuadPart - ft.QuadPart ) / 10000ULL );
		if ( ageMs > 60000 )
		{
			DeleteFileA( fullPath.c_str() );
			continue;
		}

		auto it = m_pidToBotIdx.find( pid );
		if ( it == m_pidToBotIdx.end() ) continue;
		int idx = it->second;

		// Парсим JSON.
		std::ifstream f( fullPath );
		if ( !f.is_open() ) continue;
		try
		{
			json j; f >> j;
			uint64_t lobby = 0;
			if ( j.contains( "lobby_id" ) )
			{
				if ( j["lobby_id"].is_number() )
					lobby = j["lobby_id"].get<uint64_t>();
				else if ( j["lobby_id"].is_string() )
					lobby = strtoull( j["lobby_id"].get<std::string>().c_str(), nullptr, 10 );
			}
			if ( lobby )
				m_pairing.OnMatchPendingFile( idx, lobby );
		}
		catch ( ... ) {}
	} while ( FindNextFileA( hFind, &fd ) );
	FindClose( hFind );
}

void Orchestrator::HandlePairingDecision( const PairingDecision& d )
{
	if ( d.kind == PairingDecision::NONE ) return;

	if ( d.kind == PairingDecision::ACCEPT )
	{
		Log( "[pairing] DECIDED ACCEPT lobby=%llu reason=%s",
			(unsigned long long)d.lobbyId, d.reason.c_str() );
		for ( int i = 0; i < m_nBotCount; i++ )
		{
			DWORD pid = m_bots[i].dotaPid;
			if ( !pid ) continue;
			std::ostringstream ss;
			ss << "{\"cmd\":\"ready_up\",\"lobby_id\":" << d.lobbyId
			   << ",\"ts\":" << crash_watchdog::NowMs() << "}\n";
			crash_watchdog::WriteCommandFile( pid, ss.str() );
		}
		// Sync match_id для post-game detection.
		m_lastHandledMatchId = 0;  // сбрасываем — POST_GAME сам подхватит
		m_postGameHandled = false;
	}
	else if ( d.kind == PairingDecision::CANCEL )
	{
		Log( "[pairing] DECIDED CANCEL reason=%s streak=%d",
			d.reason.c_str(), m_pairing.CancelStreak() + 1 );
		for ( int i = 0; i < m_nBotCount; i++ )
		{
			DWORD pid = m_bots[i].dotaPid;
			if ( !pid ) continue;
			std::ostringstream ss;
			ss << "{\"cmd\":\"cancel_queue\",\"ts\":"
			   << crash_watchdog::NowMs() << "}\n";
			crash_watchdog::WriteCommandFile( pid, ss.str() );
		}

		// Удалить match_pending файлы — следующая попытка должна начать с нуля.
		WIN32_FIND_DATAA fd;
		HANDLE h = FindFirstFileA( "C:\\temp\\andromeda\\match_pending_*.json", &fd );
		if ( h != INVALID_HANDLE_VALUE )
		{
			do
			{
				if ( fd.cFileName[0] == '.' ) continue;
				std::string p = std::string( "C:\\temp\\andromeda\\" ) + fd.cFileName;
				DeleteFileA( p.c_str() );
			} while ( FindNextFileA( h, &fd ) );
			FindClose( h );
		}
	}

	m_pairing.OnDecisionApplied( d );

	// Long pause если streak ≥ max.
	if ( d.kind == PairingDecision::CANCEL
		&& m_pairing.CancelStreak() >= m_config.pairing.maxConsecutiveCancels )
	{
		m_pauseUntilMs = crash_watchdog::NowMs() + 10ULL * 60ULL * 1000ULL; // 10 мин
		Log( "[pairing] cancel streak %d >= max — pause queueing for 10 min",
			m_pairing.CancelStreak() );
		m_pairing.ResetCancelStreak();
	}
}

// Единая точка приёма peer-сообщений со всех транспортов (ipc/slave/relay).
// match_result reconciliation: master = source of truth для последнего матча.
// Все остальные типы (match_found / decision / hb-derived) идут в FSM.
void Orchestrator::OnPairingMessage( const PeerMsg& m )
{
	if ( m.type != "match_result" )
	{
		m_pairing.OnPeerMessage( m );
		return;
	}

	// match_result body (master broadcast'ит):
	//   master_strategy : что master играл ("WIN"|"LOSE"|"DEBOOST")
	//   slave_strategy  : что slave играл (противоположное обычно)
	//   master_we_won   : выиграл ли master
	// Backward-compat: старая схема имела {match_id, we_won} без префикса role —
	// если master_we_won отсутствует, fall back на we_won (как master verdict).
	uint64_t matchId = m.body.value( "match_id", (uint64_t)0 );
	if ( matchId == 0 )
	{
		Log( "[pairing] received match_result with empty match_id — ignored" );
		return;
	}

	bool masterWeWon = m.body.contains( "master_we_won" )
		? m.body.value( "master_we_won", false )
		: m.body.value( "we_won", false );

	std::string masterStrategy = m.body.value( "master_strategy", std::string{} );
	std::string slaveStrategy  = m.body.value( "slave_strategy",  std::string{} );

	// Master ничего не делает с собственным echo'ем (на случай если relay
	// зеркалит обратно): он уже записал свой verdict локально в TickPostGameDetection.
	if ( m_config.pairing.role == "master" )
	{
		Log( "[pairing] received match_result echo from peer (match=%llu) — ignored on master",
			(unsigned long long)matchId );
		return;
	}

	// На slave: применяем мастерский verdict.
	// Наш we_won = !master_we_won (противоборствующие команды в self-play).
	bool ourWeWon = !masterWeWon;
	std::string ourStrategy = !slaveStrategy.empty() ? slaveStrategy
		: ( !m_currentStrategy.empty() ? m_currentStrategy : std::string( "LOSE" ) );

	m_roleRotation.UpsertMatch( matchId, ourStrategy, ourWeWon );
	m_roleRotation.SaveAtomic( "C:\\temp\\andromeda\\last_role.json" );
	m_lastHandledMatchId = matchId;
	m_postGameHandled    = true;

	Log( "[pairing] reconciled match_result match=%llu master_strategy=%s master_we_won=%d → "
	     "our_strategy=%s our_we_won=%d",
		(unsigned long long)matchId,
		masterStrategy.empty() ? "?" : masterStrategy.c_str(), masterWeWon ? 1 : 0,
		ourStrategy.c_str(), ourWeWon ? 1 : 0 );
}

void Orchestrator::TickPostGameDetection()
{
	if ( !m_config.pairing.enabled ) return;

	// Кто-нибудь из ботов в POST_GAME?
	uint64_t matchId = 0;
	bool inPostGame = false;
	int  radiantScore = 0;
	int  direScore    = 0;
	int  ourTeam      = 0;        // 2 = radiant, 3 = dire
	bool sawTeam      = false;

	for ( int i = 0; i < m_nBotCount; i++ )
	{
		auto& bot = m_bots[i];
		if ( !bot.gsiSeen ) continue;
		if ( bot.gsi.gameState == "DOTA_GAMERULES_STATE_POST_GAME" )
		{
			inPostGame = true;
			if ( !matchId ) matchId = bot.gsi.matchId;
			radiantScore = bot.gsi.radiantScore;
			direScore    = bot.gsi.direScore;
			if ( !sawTeam && bot.gsi.team != 0 )
			{
				ourTeam = bot.gsi.team;
				sawTeam = true;
			}
		}
	}

	if ( !inPostGame ) return;
	if ( matchId == 0 || matchId == m_lastHandledMatchId ) return;
	if ( m_postGameHandled ) return;

	bool weWon = false;
	if ( sawTeam )
	{
		if ( ourTeam == 2 )      weWon = ( radiantScore > direScore );
		else if ( ourTeam == 3 ) weWon = ( direScore > radiantScore );
	}

	std::string strategy = m_currentStrategy.empty() ? std::string( "WIN" ) : m_currentStrategy;
	m_roleRotation.RecordMatch( matchId, strategy, weWon );
	m_roleRotation.SaveAtomic( "C:\\temp\\andromeda\\last_role.json" );
	Log( "[pairing] POST_GAME match=%llu strategy=%s we_won=%d (radiant=%d dire=%d ourTeam=%d)",
		(unsigned long long)matchId, strategy.c_str(), weWon ? 1 : 0,
		radiantScore, direScore, ourTeam );

	// Master broadcast'ит расширенный verdict — slave перезатрёт свой
	// локальный последний record мастерским через UpsertMatch (см. OnPairingMessage).
	// Slave НЕ broadcast'ит — master уже знает своё.
	if ( m_config.pairing.role == "master" )
	{
		// slave_strategy = противоположный (в self-play стороны играют WIN vs LOSE).
		std::string slaveStrategy =
			( strategy == "WIN" )  ? std::string( "LOSE" ) :
			( strategy == "LOSE" ) ? std::string( "WIN" )  :
			strategy;  // DEBOOST → DEBOOST (на slave тоже, не наша забота)

		json mr;
		mr["type"]     = "match_result";
		mr["body"]     = json::object();
		mr["body"]["match_id"]        = matchId;
		mr["body"]["master_strategy"] = strategy;
		mr["body"]["slave_strategy"]  = slaveStrategy;
		mr["body"]["master_we_won"]   = weWon;
		// Legacy alias для старых slave'ов которые ждали "we_won":
		mr["body"]["we_won"]          = weWon;

		// Транспорт-агностично: используем onBroadcast того же FSM (он же знает
		// какой канал сейчас активен — relay/ipc).
		if ( m_pairing.onBroadcast )
			m_pairing.onBroadcast( mr );
		else if ( m_relayPeer )
			m_relayPeer->Send( mr );
		else
			m_ipc.Broadcast( mr );
	}

	m_lastHandledMatchId = matchId;
	m_postGameHandled    = true;
}

std::string Orchestrator::ResolveStrategyForNextMatch()
{
	if ( m_config.teamStrategyMode != "auto" )
		return m_config.teamStrategyMode;
	if ( !m_config.pairing.enabled )
		return "DEBOOST";
	if ( m_roleRotation.History().empty() )
		return "WIN";  // cold start
	return ( m_roleRotation.LastStrategy() == "WIN" ) ? std::string( "LOSE" ) : std::string( "WIN" );
}

Orchestrator::PairingStatus Orchestrator::GetPairingStatus() const
{
	PairingStatus s;
	s.enabled = m_config.pairing.enabled;
	if ( !s.enabled )
	{
		s.currentStrategy = m_currentStrategy;
		return s;
	}
	s.isMaster   = ( m_config.pairing.role == "master" );
	// Relay mode (приоритет — обе роли используют RelayPeer).
	if ( m_relayPeer )
	{
		s.usingRelay = true;
		s.connected = m_relayPeer->IsConnected();
		s.clientCount = s.connected ? 1 : 0;  // в relay-режиме всегда 1 канал
		int64_t hb  = m_relayPeer->LastPeerActivityMs();
		s.lastPeerHbAgeMs = hb ? ( crash_watchdog::NowMs() - hb ) : -1;
		s.lastError = m_relayPeer->LastError();
		s.relayErrorCode = m_relayPeer->LastRelayErrorCode();
	}
	else if ( s.isMaster )
	{
		s.connected   = m_ipc.IsConnected();
		s.clientCount = m_ipc.ClientCount();
		int64_t hb    = m_ipc.LastPeerHbMs();
		s.lastPeerHbAgeMs = hb ? ( crash_watchdog::NowMs() - hb ) : -1;
		s.lastError   = m_ipc.LastError();
	}
	else if ( m_slavePeer )
	{
		s.connected = m_slavePeer->IsConnected();
		int64_t hb  = m_slavePeer->LastPeerHbMs();
		s.lastPeerHbAgeMs = hb ? ( crash_watchdog::NowMs() - hb ) : -1;
		s.lastError = m_slavePeer->LastError();
	}

	auto dbg = m_pairing.GetDebug();
	s.phase              = dbg.phase;
	s.localFilled        = dbg.localFilled;
	s.localTotal         = dbg.localTotal;
	s.localMajorityLobby = dbg.localMajorityLobby;
	s.peerCount          = dbg.peerCount;
	s.cancelStreak       = dbg.cancelStreak;

	s.currentStrategy = m_currentStrategy;
	// nextStrategy — read-only снимок, не меняет состояние.
	if ( m_config.teamStrategyMode != "auto" )
		s.nextStrategy = m_config.teamStrategyMode;
	else if ( m_roleRotation.History().empty() )
		s.nextStrategy = "WIN";
	else
		s.nextStrategy = ( m_roleRotation.LastStrategy() == "WIN" ) ? std::string( "LOSE" ) : std::string( "WIN" );

	s.history = m_roleRotation.History();

	// Relay telemetry (msgSent / msgRecv / rttMs).
	if ( m_relayPeer )
	{
		auto snap = m_relayPeer->GetSnapshot();
		s.msgSent = snap.msgSent;
		s.msgRecv = snap.msgRecv;
		s.rttMs   = snap.lastRttMs;
	}

	// SyncStartCoordinator snapshot.
	s.syncStart = m_syncStart.GetSnapshot();

	return s;
}

void Orchestrator::SetTeamStrategyOverride( const std::string& mode )
{
	if ( mode != "auto" && mode != "WIN" && mode != "LOSE" && mode != "DEBOOST" )
		return;
	m_config.teamStrategyMode = mode;
	Log( "[pairing] team_strategy_mode override → %s", mode.c_str() );
}

void Orchestrator::ApplyMinifierAll()
{
	// Если уже идёт apply/revert — не запускаем вторую копию.
	bool expected = false;
	if ( !m_minifierBusy.compare_exchange_strong( expected, true ) )
	{
		Log( "[minifier] busy — apply request ignored" );
		return;
	}

	// Кнопка "APPLY MINIFY" — применяет safe-path настройки: autoexec.cfg +
	// video.txt + launch options. VPK_DISABLED 2026-05-17 — VPK patches убраны
	// полностью (вызывали gameinfo.gi FATAL у юзеров). Альтернатива — настройки
	// + memreduct (mem_reclaim секция в farm.json + GUI панель сверху).
	std::thread( [this]() {
		MinifierConfig override = m_config.minifier;
		override.enabled            = true;
		override.applyLaunchOptions = true;
		override.applyAutoexec      = true;
		override.applyVideoTxt      = true;
		override.applyVpkPatches    = false;   // VPK_DISABLED — kill-switch
		m_minifier.SetConfig( override );

		int applied = 0;
		int considered = 0;
		for ( int i = 0; i < m_nBotCount; i++ )
		{
			if ( i >= (int)m_config.accounts.size() ) continue;
			const auto& acc = m_config.accounts[i];
			if ( !acc.enabled ) continue;
			considered++;
			uint64_t sid = acc.steamId;
			if ( m_minifier.ApplyToBot( i, sid, m_config ) )
			{
				applied++;
				Log( "[minifier] applied to bot %d", i );
			}
			else
			{
				Log( "[minifier] FAILED to apply to bot %d", i );
			}
		}

		// VPK_DISABLED: ApplyVpkPatches больше не зовём.
		// if ( m_minifier.ApplyVpkPatches() ) ...

		Log( "[minifier] APPLY ALL: %d/%d bots done (VPK skipped — kill-switch)", applied, considered );
		m_minifierBusy.store( false );
	} ).detach();
}

void Orchestrator::RevertMinifierAll()
{
	bool expected = false;
	if ( !m_minifierBusy.compare_exchange_strong( expected, true ) )
	{
		Log( "[minifier] busy — revert request ignored" );
		return;
	}

	// Revert тоже может зависнуть на VPK subprocess 10-30s — bg thread.
	std::thread( [this]() {
		if ( !m_minifier.RevertAll() )
			Log( "[minifier] WARN: RevertAll partial failure" );
		if ( m_minifier.DetectStaleVpkPatches() )
		{
			Log( "[minifier] vpk patches: reverting (10-30s subprocess)..." );
			if ( !m_minifier.RevertVpkPatches() )
				Log( "[minifier] WARN: vpk revert failed — manual: steam://validate/570" );
			else
				Log( "[minifier] vpk patches reverted" );
		}
		Log( "[minifier] REVERT ALL done" );
		m_minifierBusy.store( false );
	} ).detach();
}

void Orchestrator::LogPublic( const char* msg )
{
	Log( "%s", msg );
}

void Orchestrator::Log( const char* fmt, ... )
{
	char buf[512];
	va_list args;
	va_start( args, fmt );
	vsnprintf( buf, sizeof( buf ), fmt, args );
	va_end( args );

	time_t now = time( nullptr );
	struct tm* lt = localtime( &now );
	char ts[32];
	snprintf( ts, sizeof( ts ), "%04d-%02d-%02d %02d:%02d:%02d",
		lt->tm_year + 1900, lt->tm_mon + 1, lt->tm_mday,
		lt->tm_hour, lt->tm_min, lt->tm_sec );

	// Write to file
	if ( g_logFile )
	{
		fprintf( g_logFile, "[%s] %s\n", ts, buf );
		fflush( g_logFile );
	}

	// Write to GUI log (short timestamp)
	LogEntry entry;
	entry.timestamp = std::string( ts + 11 ); // "HH:MM:SS"
	entry.message = buf;

	std::lock_guard<std::mutex> lock( m_mutex );
	m_log.push_back( entry );
	if ( m_log.size() > 200 )
		m_log.erase( m_log.begin() );
}

// ─────────────────────────────────────────────────────────────────────────
// Pairing lifecycle controls (Wave 2)
// ─────────────────────────────────────────────────────────────────────────

bool Orchestrator::ReinitPairing()
{
	if ( m_state == State::RUNNING )
	{
		Log( "[pairing] ReinitPairing refused: farm RUNNING" );
		return false;
	}

	auto sync = m_syncStart.GetSnapshot();
	if ( sync.state != SyncStartState::IDLE )
	{
		Log( "[pairing] ReinitPairing refused: sync-start in progress (state=%d)",
			(int)sync.state );
		return false;
	}

	// Stop old peers (любые из 3 транспортов).
	if ( m_relayPeer )
	{
		m_relayPeer->Stop();
		m_relayPeer.reset();
	}
	m_ipc.Stop();
	if ( m_slavePeer )
	{
		m_slavePeer->Stop();
		m_slavePeer.reset();
	}

	m_pairing.Reset();
	m_syncStart.Reset( "reinit" );

	if ( !m_config.pairing.enabled )
	{
		Log( "[pairing] reinitialized (disabled — no transport started)" );
		return true;
	}

	EnsureTempDirs();
	InitPairing_();

	Log( "[pairing] reinitialized (transport=%s role=%s)",
		m_config.pairing.transport.c_str(), m_config.pairing.role.c_str() );
	return true;
}

bool Orchestrator::GuardedStartFarm()
{
	// Legacy path: uxV2 не активен → прямой StartFarm.
	if ( !m_config.pairing.uxV2 )
	{
		StartFarm();
		return true;
	}

	// uxV2=true но pairing выключен → single-stand, fallback на StartFarm.
	if ( !m_config.pairing.enabled )
	{
		StartFarm();
		return true;
	}

	// Pairing required: peer должен быть connected + hb свежий.
	PairingStatus ps = GetPairingStatus();
	if ( !ps.connected )
	{
		Log( "[pairing] GuardedStartFarm refused: not connected" );
		return false;
	}
	if ( ps.lastPeerHbAgeMs < 0 || ps.lastPeerHbAgeMs > 5000 )
	{
		Log( "[pairing] GuardedStartFarm refused: peer hb stale (%lld ms)",
			(long long)ps.lastPeerHbAgeMs );
		return false;
	}

	auto sync = m_syncStart.GetSnapshot();
	if ( sync.state != SyncStartState::IDLE )
	{
		Log( "[pairing] GuardedStartFarm refused: sync-start busy (state=%d)",
			(int)sync.state );
		return false;
	}

	// configHash — placeholder. Не security-critical: используется только в
	// start_request body для optional sanity check на peer side.
	std::string configHash = "todo-hash";
	if ( !m_syncStart.Initiate( m_config.pairing.role, configHash ) )
	{
		Log( "[pairing] GuardedStartFarm refused: SyncStartCoordinator::Initiate failed" );
		return false;
	}

	Log( "[pairing] GuardedStartFarm initiated handshake (role=%s)",
		m_config.pairing.role.c_str() );
	return true;
}

void Orchestrator::RequestForceReconnect()
{
	if ( m_relayPeer )
	{
		m_relayPeer->RequestReconnect();
		Log( "[pairing] force reconnect requested" );
		return;
	}

	// Direct mode: нет separate "force reconnect" API на m_ipc/m_slavePeer.
	// Делаем полный Reinit.
	Log( "[pairing] force reconnect: direct mode — falling back to ReinitPairing" );
	ReinitPairing();
}

void Orchestrator::RequestDisconnect()
{
	if ( m_relayPeer )
	{
		m_relayPeer->Stop();
		m_relayPeer.reset();
	}
	m_ipc.Stop();
	if ( m_slavePeer )
	{
		m_slavePeer->Stop();
		m_slavePeer.reset();
	}
	m_pairing.Reset();
	m_syncStart.Reset( "user_disconnect" );
	Log( "[pairing] disconnected (user)" );
}

bool Orchestrator::ApplyPairCodeAndReinit( const pair_code::Decoded& decoded )
{
	// In-memory apply.
	config::ApplyPairCode( decoded, m_config.pairing );
	m_config.pairing.uxV2    = true;
	m_config.pairing.enabled = true;

	std::string path = m_config.configDir + "\\farm.json";
	if ( !config::SavePairingConfigAtomic( path, m_config.pairing ) )
	{
		Log( "[pairing] ApplyPairCodeAndReinit: SavePairingConfigAtomic FAILED (%s)",
			path.c_str() );
		return false;
	}

	if ( !ReinitPairing() )
	{
		Log( "[pairing] ApplyPairCodeAndReinit: ReinitPairing FAILED "
			"(config saved but transports not active)" );
		return false;
	}

	Log( "[pairing] ApplyPairCodeAndReinit OK (relay=%s user=%s pair=%s role=%s)",
		m_config.pairing.relayHost.c_str(),
		m_config.pairing.userId.c_str(),
		m_config.pairing.pairId.c_str(),
		m_config.pairing.role.c_str() );
	return true;
}

std::string Orchestrator::GenerateCurrentPairCode() const
{
	const auto& p = m_config.pairing;
	if ( p.relayHost.empty() || p.userId.empty() || p.userAuthToken.empty() ||
	     p.pairId.empty() || p.pairSecret.empty() )
	{
		return "";
	}
	// Master генерит код для slave (peer противоположной роли).
	std::string oppositeRole = ( p.role == "master" ) ? "S" : "M";
	return pair_code::Encode(
		p.relayHost, p.relayPort, p.userId, p.userAuthToken,
		p.pairId, p.pairSecret, oppositeRole, 0 );
}

void Orchestrator::SyncStartUserAccept()
{
	m_syncStart.UserAccept();
}

void Orchestrator::SyncStartUserDecline( const std::string& reason )
{
	m_syncStart.UserDecline( reason );
}

void Orchestrator::SyncStartUserCancel()
{
	m_syncStart.UserCancel();
}
