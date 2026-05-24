#include "crash_watchdog.h"
#include "dota_launcher.h"

#include <Windows.h>
#include <chrono>
#include <fstream>
#include <sstream>
#include <cstdio>
#include <cstring>

// ── EDOTAGameState constants (см. dota schema) ──
// Игнорируем S2 freeze пока в этих состояниях — main thread Dota занят
// (loading shaders, hero pick UI).
static constexpr int DOTA_GAMERULES_STATE_INIT             = 0;
static constexpr int DOTA_GAMERULES_STATE_WAIT_FOR_PLAYERS = 1;
static constexpr int DOTA_GAMERULES_STATE_HERO_SELECTION   = 2;
static constexpr int DOTA_GAMERULES_STATE_STRATEGY_TIME    = 3;
static constexpr int DOTA_GAMERULES_STATE_PRE_GAME         = 4;
static constexpr int DOTA_GAMERULES_STATE_GAME_IN_PROGRESS = 5;
static constexpr int DOTA_GAMERULES_STATE_POST_GAME        = 6;

static bool IsLoadingState( int gs )
{
	return gs == DOTA_GAMERULES_STATE_INIT
		|| gs == DOTA_GAMERULES_STATE_WAIT_FOR_PLAYERS
		|| gs == DOTA_GAMERULES_STATE_HERO_SELECTION
		|| gs == DOTA_GAMERULES_STATE_STRATEGY_TIME;
}

// ── ctor / dtor ──

CrashWatchdog::CrashWatchdog() = default;

CrashWatchdog::~CrashWatchdog()
{
	StopAllDumpWatch();
}

// ── Public utilities ──

namespace crash_watchdog
{
uint64_t NowMs()
{
	return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::system_clock::now().time_since_epoch() ).count();
}

const char* StateName( WatchdogState s )
{
	switch ( s )
	{
	case WatchdogState::HEALTHY:        return "HEALTHY";
	case WatchdogState::SUSPECTED_DEAD: return "SUSPECTED_DEAD";
	case WatchdogState::CONFIRMED_DEAD: return "CONFIRMED_DEAD";
	case WatchdogState::RELAUNCHING:    return "RELAUNCHING";
	case WatchdogState::INJECTING:      return "INJECTING";
	case WatchdogState::RECONNECTING:   return "RECONNECTING";
	case WatchdogState::IN_GAME:        return "IN_GAME";
	case WatchdogState::DEAD:           return "DEAD";
	}
	return "?";
}

// C3: atomic-write helper. Tmp + flush + MoveFileExA. Используется и для
// command_<pid>.json (legacy) и для match_state_<idx>.json (persistent state).
bool AtomicWriteFile( const char* dstPath, const std::string& content )
{
	if ( !dstPath || !*dstPath ) return false;

	CreateDirectoryA( "C:\\temp", nullptr );
	CreateDirectoryA( "C:\\temp\\andromeda", nullptr );

	char tmp[MAX_PATH];
	snprintf( tmp, sizeof( tmp ), "%s.tmp", dstPath );

	{
		std::ofstream f( tmp, std::ios::binary | std::ios::trunc );
		if ( !f ) return false;
		f << content;
		f.flush();
		if ( !f ) return false;
	}

	if ( !MoveFileExA( tmp, dstPath, MOVEFILE_REPLACE_EXISTING ) )
	{
		DeleteFileA( tmp );
		return false;
	}
	return true;
}

bool WriteCommandFile( DWORD pid, const std::string& jsonContent )
{
	if ( !pid ) return false;

	char dst[MAX_PATH];
	snprintf( dst, sizeof( dst ), "C:\\temp\\andromeda\\command_%lu.json", pid );
	return AtomicWriteFile( dst, jsonContent );
}

bool WriteReconnectCommand( DWORD pid, uint64_t lobbyId )
{
	std::ostringstream ss;
	ss << "{\"cmd\":\"reconnect_to_match\",\"lobby_id\":" << lobbyId
	   << ",\"ts\":" << NowMs() << "}\n";
	return WriteCommandFile( pid, ss.str() );
}
} // namespace crash_watchdog

// ── Private helpers ──

bool CrashWatchdog::Pop( int idx, std::atomic<bool> WatchdogBot::* member )
{
	if ( idx < 0 || idx >= MAX_BOTS_INTERNAL ) return false;
	bool expected = true;
	return ( m_bots[idx].*member ).compare_exchange_strong( expected, false );
}

void CrashWatchdog::Enter( int idx, WatchdogState newState, uint64_t nowMs, const char* reason )
{
	auto& b = m_bots[idx];
	if ( b.state == newState ) return;

	WatchdogState old = b.state;
	b.state = newState;
	b.stateEnteredMs = nowMs;

	// Логи через OutputDebugString (orchestrator может перенаправить если хочет).
	char buf[256];
	snprintf( buf, sizeof( buf ),
		"[watchdog] bot#%d: %s -> %s (%s)\n",
		idx,
		crash_watchdog::StateName( old ),
		crash_watchdog::StateName( newState ),
		reason ? reason : "" );
	OutputDebugStringA( buf );
}

void CrashWatchdog::TrimCrashWindow( int idx, uint64_t nowMs )
{
	auto& b = m_bots[idx];
	uint64_t windowMs = (uint64_t)m_cfg.crashWindowMin * 60ULL * 1000ULL;
	while ( !b.crashTimestampsMs.empty()
		&& nowMs - b.crashTimestampsMs.front() > windowMs )
	{
		b.crashTimestampsMs.pop_front();
	}
}

// ── Tick ──

void CrashWatchdog::Tick( uint64_t nowMs, int idx, const WatchdogSnapshot& s )
{
	if ( !m_cfg.enabled ) return;
	if ( idx < 0 || idx >= MAX_BOTS_INTERNAL ) return;

	std::lock_guard<std::mutex> lock( m_mutex );
	auto& b = m_bots[idx];

	// Lobby сменился → сбросить per-match счётчики + персистнуть на диск (C3).
	if ( s.lobbyId != 0 && s.lobbyId != b.lastKnownLobbyId )
	{
		b.lastKnownLobbyId = s.lobbyId;
		b.reconnectsThisMatch = 0;

		// C3: запись match_state_<idx>.json для переживания restart'а orchestrator'а.
		char path[MAX_PATH];
		snprintf( path, sizeof( path ),
			"C:\\temp\\andromeda\\match_state_%d.json", idx );
		std::ostringstream ss;
		ss << "{\"bot\":" << idx
		   << ",\"lobby_id\":" << s.lobbyId
		   << ",\"ts\":" << nowMs << "}\n";
		crash_watchdog::AtomicWriteFile( path, ss.str() );
	}
	// C1 bonus: при POST_GAME сбросить lastKnownLobbyId + удалить persistent state.
	if ( s.gameStateId == DOTA_GAMERULES_STATE_POST_GAME && b.lastKnownLobbyId != 0 )
	{
		b.lastKnownLobbyId = 0;
		b.reconnectsThisMatch = 0;
		char path[MAX_PATH];
		snprintf( path, sizeof( path ),
			"C:\\temp\\andromeda\\match_state_%d.json", idx );
		DeleteFileA( path );
	}

	// ── 3 сигнала ──
	const bool s1_processGone = ( s.dotaPid != 0 ) && !DotaLauncher::IsProcessAlive( s.dotaPid );
	const bool s3_dumpAppeared = b.dumpPending.load();

	// C2: loadingStateGraceS используется как продлённый S2-порог в loading/paused
	// фазах. Раньше это был binary gate — dota могла висеть на loading screen
	// вечно. Теперь после loadingStateGraceS секунд heartbeat-фриза в loading мы
	// всё-таки эскалируем в CONFIRMED_DEAD.
	const bool loadingPhase = IsLoadingState( s.gameStateId ) || s.paused;
	const int64_t confirmThresholdMs = loadingPhase
		? (int64_t)m_cfg.loadingStateGraceS * 1000
		: (int64_t)m_cfg.heartbeatConfirmS * 1000;
	const int64_t suspectThresholdMs = loadingPhase
		? (int64_t)m_cfg.loadingStateGraceS * 500   // half of grace
		: (int64_t)m_cfg.heartbeatSuspectS  * 1000;

	const bool s2_suspect = s.heartbeatAgeMs >= 0
		&& s.heartbeatAgeMs > suspectThresholdMs;
	const bool s2_confirm = s.heartbeatAgeMs >= 0
		&& s.heartbeatAgeMs > confirmThresholdMs;

	// Decision rule (RECONNECT_DESIGN.md §1): CONFIRMED = S1 || (S3 && hb>5s) || (S2>40s && !grace)
	const bool confirmed_dead = s1_processGone
		|| ( s3_dumpAppeared && s.heartbeatAgeMs > 5000 )
		|| s2_confirm;

	const bool suspected_dead = s2_suspect || s3_dumpAppeared;

	const uint64_t inStateMs = nowMs - b.stateEnteredMs;

	switch ( b.state )
	{
	case WatchdogState::HEALTHY:
	case WatchdogState::IN_GAME:
	{
		// Запоминаем wasInMatch для решения о reconnect позже.
		if ( s.gameStateId == DOTA_GAMERULES_STATE_GAME_IN_PROGRESS )
			b.wasInMatch = true;
		else if ( s.gameStateId == DOTA_GAMERULES_STATE_POST_GAME )
			b.wasInMatch = false;

		if ( confirmed_dead )
		{
			Enter( idx, WatchdogState::CONFIRMED_DEAD, nowMs, "fast path from HEALTHY" );
		}
		else if ( suspected_dead )
		{
			Enter( idx, WatchdogState::SUSPECTED_DEAD, nowMs,
				s3_dumpAppeared ? "dump appeared" : "heartbeat freeze" );
		}
		break;
	}

	case WatchdogState::SUSPECTED_DEAD:
	{
		if ( confirmed_dead )
		{
			Enter( idx, WatchdogState::CONFIRMED_DEAD, nowMs, "S2 escalation" );
		}
		else if ( !suspected_dead )
		{
			// Heartbeat восстановился — false alarm.
			b.dumpPending.store( false );
			Enter( idx, WatchdogState::HEALTHY, nowMs, "S2 recovered" );
		}
		else if ( inStateMs > 25000 )
		{
			// Timeout SUSPECTED → CONFIRMED по дизайну ≤25s.
			Enter( idx, WatchdogState::CONFIRMED_DEAD, nowMs, "SUSPECTED timeout 25s" );
		}
		break;
	}

	case WatchdogState::CONFIRMED_DEAD:
	{
		// Snapshot контекста для recovery.
		b.crashTimestampsMs.push_back( nowMs );
		TrimCrashWindow( idx, nowMs );

		if ( (int)b.crashTimestampsMs.size() >= m_cfg.maxCrashesPerWindow )
		{
			b.pendingKillForever.store( true );
			Enter( idx, WatchdogState::DEAD, nowMs, "crash loop" );
			break;
		}

		if ( b.reconnectsThisMatch >= m_cfg.maxReconnectsPerMatch )
		{
			b.pendingKillForever.store( true );
			Enter( idx, WatchdogState::DEAD, nowMs, "max reconnects per match" );
			break;
		}

		// Сбросить S3 — следующий dump должен сработать заново.
		b.dumpPending.store( false );

		// Переход в RELAUNCHING. RecoveryThread в orchestrator'е делает полный
		// kill+restart Steam ЭТОГО инстанса + LaunchDotaOnly + reinject DLLs
		// (Andromeda + ProxyHook). Никаких отдельных проб «вдруг steam ещё
		// жив» — каждый CONFIRMED_DEAD = full restart обоих процессов.
		b.pendingRelaunch.store( true );
		Enter( idx, WatchdogState::RELAUNCHING, nowMs, "trigger full recovery" );
		break;
	}

	case WatchdogState::RELAUNCHING:
	{
		// Ждём пока orchestrator вызовет OnRelaunchStarted с новым PID.
		// Тот переведёт нас в INJECTING. Тут только timeout guard.
		if ( inStateMs > 120000 )
		{
			b.crashTimestampsMs.push_back( nowMs ); // считаем за неудачу
			Enter( idx, WatchdogState::CONFIRMED_DEAD, nowMs, "RELAUNCH timeout 120s" );
		}
		break;
	}

	case WatchdogState::INJECTING:
	{
		// B5: 5s floor для inStateMs гарантирует что SM thread в DLL успел
		// сделать 2-3 polling cycle command_<pid>.json. Без этого race:
		// orchestrator пишет команду до того как DLL начала её опрашивать.
		if ( s.injected && s.dotaReady && s.heartbeatAgeMs >= 0 && s.heartbeatAgeMs < 10000
			&& inStateMs > 5000 )
		{
			b.pendingReconnect.store( true );
			b.reconnectsThisMatch++;
			b.lastReconnectIssuedMs = nowMs;
			Enter( idx, WatchdogState::RECONNECTING, nowMs, "DLL alive — issue reconnect" );
		}
		else if ( inStateMs > 60000 )
		{
			b.crashTimestampsMs.push_back( nowMs );
			Enter( idx, WatchdogState::CONFIRMED_DEAD, nowMs, "INJECT timeout 60s" );
		}
		break;
	}

	case WatchdogState::RECONNECTING:
	{
		// Reconnect command уже отправлен. Ждём resume heartbeat (DLL живая)
		// + (если wasInMatch) GAME_IN_PROGRESS. gc_ready НЕ проверяем — Steam
		// был только что перезапущен в RELAUNCHING/RecoveryThread, GC всё
		// равно поднимется раньше чем dota перейдёт в GAME_IN_PROGRESS.
		const bool heartbeatBack = ( s.heartbeatAgeMs >= 0 && s.heartbeatAgeMs < 10000 );
		const bool inMatch = ( s.gameStateId == DOTA_GAMERULES_STATE_GAME_IN_PROGRESS );
		const bool reconnected = heartbeatBack && ( !b.wasInMatch || inMatch );

		if ( reconnected )
		{
			Enter( idx, WatchdogState::IN_GAME, nowMs, "back in game" );
		}
		else if ( inStateMs > 90000 )
		{
			// 90s без resume — эскалируем в CONFIRMED_DEAD, оттуда пойдёт
			// ещё один full recovery cycle (если crash budget позволит).
			b.crashTimestampsMs.push_back( nowMs );
			Enter( idx, WatchdogState::CONFIRMED_DEAD, nowMs, "RECONNECT timeout 90s" );
		}
		break;
	}

	case WatchdogState::DEAD:
	{
		// Финал. Manual reset через CrashWatchdog::Reset.
		break;
	}
	}
}

// ── Acks ──

void CrashWatchdog::OnRelaunchStarted( int idx, DWORD newDotaPid )
{
	if ( idx < 0 || idx >= MAX_BOTS_INTERNAL ) return;
	std::lock_guard<std::mutex> lock( m_mutex );
	auto& b = m_bots[idx];
	if ( b.state == WatchdogState::RELAUNCHING )
	{
		uint64_t now = crash_watchdog::NowMs();
		if ( newDotaPid )
			b.pendingInject.store( true );
		Enter( idx, WatchdogState::INJECTING, now, "relaunch ack" );
	}
}

void CrashWatchdog::OnInjected( int /*idx*/ )
{
	// Ничего не делаем — Tick() сам перейдёт в RECONNECTING когда
	// snapshot.injected станет true и heartbeat поднимется.
}

void CrashWatchdog::OnReconnectIssued( int idx, uint64_t nowMs )
{
	if ( idx < 0 || idx >= MAX_BOTS_INTERNAL ) return;
	std::lock_guard<std::mutex> lock( m_mutex );
	m_bots[idx].lastReconnectIssuedMs = nowMs;
}

// ── Setters / Getters ──

void CrashWatchdog::SetHwidSeed( int idx, const std::string& seed )
{
	if ( idx < 0 || idx >= MAX_BOTS_INTERNAL ) return;
	std::lock_guard<std::mutex> lock( m_mutex );
	m_bots[idx].hwidSeed = seed;
}

std::string CrashWatchdog::GetHwidSeed( int idx ) const
{
	if ( idx < 0 || idx >= MAX_BOTS_INTERNAL ) return {};
	std::lock_guard<std::mutex> lock( m_mutex );
	return m_bots[idx].hwidSeed;
}

uint64_t CrashWatchdog::GetLastKnownLobbyId( int idx ) const
{
	if ( idx < 0 || idx >= MAX_BOTS_INTERNAL ) return 0;
	std::lock_guard<std::mutex> lock( m_mutex );
	return m_bots[idx].lastKnownLobbyId;
}

void CrashWatchdog::RestoreLobbyId( int idx, uint64_t lobbyId )
{
	if ( idx < 0 || idx >= MAX_BOTS_INTERNAL ) return;
	std::lock_guard<std::mutex> lock( m_mutex );
	m_bots[idx].lastKnownLobbyId = lobbyId;
}

WatchdogState CrashWatchdog::GetState( int idx ) const
{
	if ( idx < 0 || idx >= MAX_BOTS_INTERNAL ) return WatchdogState::HEALTHY;
	std::lock_guard<std::mutex> lock( m_mutex );
	return m_bots[idx].state;
}

const char* CrashWatchdog::GetStateStr( int idx ) const
{
	return crash_watchdog::StateName( GetState( idx ) );
}

int CrashWatchdog::GetCrashCount( int idx ) const
{
	if ( idx < 0 || idx >= MAX_BOTS_INTERNAL ) return 0;
	std::lock_guard<std::mutex> lock( m_mutex );
	return (int)m_bots[idx].crashTimestampsMs.size();
}

int CrashWatchdog::GetReconnectsThisMatch( int idx ) const
{
	if ( idx < 0 || idx >= MAX_BOTS_INTERNAL ) return 0;
	std::lock_guard<std::mutex> lock( m_mutex );
	return m_bots[idx].reconnectsThisMatch;
}

void CrashWatchdog::Reset( int idx )
{
	if ( idx < 0 || idx >= MAX_BOTS_INTERNAL ) return;
	std::lock_guard<std::mutex> lock( m_mutex );
	auto& b = m_bots[idx];
	b.state = WatchdogState::HEALTHY;
	b.stateEnteredMs = crash_watchdog::NowMs();
	b.crashTimestampsMs.clear();
	b.reconnectsThisMatch = 0;
	b.wasInMatch = false;
	b.pendingRelaunch.store( false );
	b.pendingInject.store( false );
	b.pendingReconnect.store( false );
	b.pendingKillForever.store( false );
	b.dumpPending.store( false );
}

// ── Dump watcher (S3) ──

void CrashWatchdog::StartDumpWatch( int idx )
{
	if ( !m_cfg.dumpWatchEnabled ) return;
	if ( idx < 0 || idx >= MAX_BOTS_INTERNAL ) return;

	auto& b = m_bots[idx];
	if ( b.dumpWatchThread.joinable() ) return; // уже запущен

	char dir[MAX_PATH];
	snprintf( dir, sizeof( dir ), "C:\\BotSteam\\%d\\dumps", idx );
	CreateDirectoryA( "C:\\BotSteam", nullptr );
	{
		char parent[MAX_PATH];
		snprintf( parent, sizeof( parent ), "C:\\BotSteam\\%d", idx );
		CreateDirectoryA( parent, nullptr );
	}
	CreateDirectoryA( dir, nullptr );

	HANDLE h = FindFirstChangeNotificationA(
		dir, FALSE,
		FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE );
	if ( h == INVALID_HANDLE_VALUE )
	{
		char msg[256];
		snprintf( msg, sizeof( msg ), "[watchdog] FindFirstChangeNotification failed for %s\n", dir );
		OutputDebugStringA( msg );
		return;
	}

	b.dumpDirHandle = h;
	b.dumpWatchStop.store( false );
	b.dumpWatchThread = std::thread( [this, idx]() { DumpWatcherLoop( idx ); } );
}

void CrashWatchdog::StopDumpWatch( int idx )
{
	if ( idx < 0 || idx >= MAX_BOTS_INTERNAL ) return;
	auto& b = m_bots[idx];
	b.dumpWatchStop.store( true );
	if ( b.dumpDirHandle )
	{
		FindCloseChangeNotification( b.dumpDirHandle );
		b.dumpDirHandle = nullptr;
	}
	if ( b.dumpWatchThread.joinable() )
		b.dumpWatchThread.join();
}

void CrashWatchdog::StopAllDumpWatch()
{
	for ( int i = 0; i < MAX_BOTS_INTERNAL; i++ )
		StopDumpWatch( i );
}

void CrashWatchdog::DumpWatcherLoop( int idx )
{
	auto& b = m_bots[idx];

	while ( !b.dumpWatchStop.load() )
	{
		HANDLE h = b.dumpDirHandle;
		if ( !h ) break;

		DWORD wait = WaitForSingleObject( h, 1000 ); // 1с poll чтобы быстро выйти при stop
		if ( b.dumpWatchStop.load() ) break;

		if ( wait == WAIT_OBJECT_0 )
		{
			// Сканируем директорию: ищем crash_dota2.exe_*.dmp (assert_*/gameoverlayui — игнор).
			char pattern[MAX_PATH];
			snprintf( pattern, sizeof( pattern ),
				"C:\\BotSteam\\%d\\dumps\\crash_dota2.exe_*.dmp", idx );

			WIN32_FIND_DATAA fd;
			HANDLE hFind = FindFirstFileA( pattern, &fd );
			if ( hFind != INVALID_HANDLE_VALUE )
			{
				// Берём самый свежий файл — если mtime в пределах 30с, считаем за crash.
				FILETIME now;
				GetSystemTimeAsFileTime( &now );
				ULARGE_INTEGER nowU;
				nowU.LowPart = now.dwLowDateTime;
				nowU.HighPart = now.dwHighDateTime;

				bool fresh = false;
				do
				{
					if ( fd.cFileName[0] == '.' ) continue;
					ULARGE_INTEGER ft;
					ft.LowPart = fd.ftLastWriteTime.dwLowDateTime;
					ft.HighPart = fd.ftLastWriteTime.dwHighDateTime;
					int64_t ageMs = (int64_t)( ( nowU.QuadPart - ft.QuadPart ) / 10000ULL );
					if ( ageMs >= 0 && ageMs < 30000 )
					{
						fresh = true;
						break;
					}
				} while ( FindNextFileA( hFind, &fd ) );
				FindClose( hFind );

				if ( fresh )
					b.dumpPending.store( true );
			}

			// Перезаряжаем notification.
			if ( !FindNextChangeNotification( h ) )
			{
				char msg[128];
				snprintf( msg, sizeof( msg ),
					"[watchdog] FindNextChangeNotification failed bot#%d\n", idx );
				OutputDebugStringA( msg );
				break;
			}
		}
		else if ( wait == WAIT_FAILED )
		{
			break;
		}
		// WAIT_TIMEOUT — просто loop.
	}
}
