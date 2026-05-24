#pragma once

// CrashWatchdog — per-instance FSM, который замечает падение dota2.exe
// в активной катке и инициирует восстановление (relaunch + reinject + reconnect).
//
// Дизайн: tools/dota2/RECONNECT_DESIGN.md
//
// Использование (orchestrator side):
//   m_watchdog.Tick(now_ms, idx, snapshot, cfg)
//   if ( m_watchdog.ShouldRelaunch(idx) )   { ... LaunchDotaOnly(...); m_watchdog.OnRelaunchStarted(idx, newDotaPid); }
//   if ( m_watchdog.ShouldInject(idx) )     { ... inject ProxyHook+Andromeda ...; m_watchdog.OnInjected(idx); }
//   if ( m_watchdog.ShouldReconnect(idx) )  { WriteCommandFile(...);             m_watchdog.OnReconnectIssued(idx); }
//   if ( m_watchdog.ShouldKillForever(idx) ){ ... mark instance dead ...; }
//
// Watchdog НЕ выполняет действий сам — он только меняет state и сообщает
// orchestrator'у "пора". Орchestrator выполняет действия на background thread,
// потом сообщает результат через OnXxx().

#include <Windows.h>
#include <string>
#include <deque>
#include <mutex>
#include <atomic>
#include <thread>
#include <cstdint>

// ── Конфиг (из farm.json -> FarmConfig::CrashRecovery) ──
struct CrashRecoveryConfig
{
	bool enabled              = true;
	int  heartbeatSuspectS    = 15;   // S2: SUSPECTED после freeze ≥ N сек
	int  heartbeatConfirmS    = 40;   // S2: CONFIRMED после freeze ≥ N сек
	bool dumpWatchEnabled     = true; // S3: FindFirstChangeNotification на dumps/
	int  maxReconnectsPerMatch = 3;
	int  maxCrashesPerWindow  = 3;
	int  crashWindowMin       = 5;    // окно для max_crashes
	int  reconnectCooldownS   = 10;
	int  loadingStateGraceS   = 90;   // S2 grace при LOADING/HERO_SELECTION
	bool respoofHwidOnRelaunch = false;

	// Steam relaunch policy. Каждый recovery (RELAUNCHING) ВСЕГДА начинается
	// с kill+restart Steam ИМЕННО этого инстанса (по m_bots[i].steamPid +
	// path-prefix C:\BotSteam\<idx>\), затем LaunchDotaOnly + reinject DLL.
	// Раньше пытались отдельный gc_ready timeout — оказалось ненадёжно,
	// Steam client часто застревает в auth_pending после crash dota, поэтому
	// безусловный restart стабильнее.
	// steamRelaunchEnabled=false → старое поведение (только LaunchDotaOnly,
	// без kill steam). По умолчанию включено.
	// gcReadyTimeoutS / maxSteamRelaunchesPerMatch — поля сохранены для
	// конфиг-обратной совместимости, в FSM сейчас не задействованы (бюджет
	// recovery ограничивается общим maxCrashesPerWindow).
	bool steamRelaunchEnabled         = true;
	int  gcReadyTimeoutS              = 30;
	int  maxSteamRelaunchesPerMatch   = 2;
};

// ── Snapshot одного бота (минимальные поля что нужны watchdog'у) ──
// Заполняется orchestrator::MonitorTick из BotInstance перед Tick().
struct WatchdogSnapshot
{
	DWORD       dotaPid       = 0;     // 0 = no process
	DWORD       steamPid      = 0;
	bool        injected      = false; // Andromeda DLL injected?
	bool        dotaReady     = false; // client.dll loaded?

	int64_t     heartbeatAgeMs = -1;   // -1 = no heartbeat ever; >=0 = age в ms
	int         gameStateId    = -1;   // EDOTAGameState (см. dota schema)
	std::string smState;               // "PLAYING", "FINDING_MATCH" и т.п.
	bool        paused         = false;
	bool        gcReady        = false;

	uint64_t    lobbyId        = 0;    // 0 = not in match
};

// ── FSM state ──
enum class WatchdogState
{
	HEALTHY,           // dota жива, heartbeat свежий
	SUSPECTED_DEAD,    // S2 > heartbeatSuspectS ИЛИ dump pending
	CONFIRMED_DEAD,    // S1 ИЛИ S2>confirmS ИЛИ S3+S2>5s
	RELAUNCHING,       // recovery thread: kill steam+dota → relaunch steam → relaunch dota
	INJECTING,         // ждём DLL injected + status_<pid>.json
	RECONNECTING,      // отправили reconnect_to_match, ждём heartbeat resume → IN_GAME
	IN_GAME,           // успешно вернулись (или never crashed)
	DEAD               // crash loop — manual restart
};

// ── Per-bot recovery context ──
struct WatchdogBot
{
	WatchdogState state = WatchdogState::HEALTHY;
	uint64_t      stateEnteredMs = 0;

	// crashes в скользящем окне crashWindowMin минут
	std::deque<uint64_t> crashTimestampsMs;

	// текущая катка (для max_reconnects_per_match)
	uint64_t lastKnownLobbyId = 0;
	int      reconnectsThisMatch = 0;
	bool     wasInMatch = false;        // snapshot момент: gameStateId==GAME_IN_PROGRESS

	// HWID seed на момент первого инжекта — переиспользуем при relaunch
	std::string hwidSeed;

	// S3: dump watcher thread per-bot
	std::atomic<bool> dumpPending{ false };
	std::thread       dumpWatchThread;
	std::atomic<bool> dumpWatchStop{ false };
	HANDLE            dumpDirHandle = nullptr; // FindFirstChangeNotification handle

	// pending action flags (одноразовые: orchestrator выгребает через ShouldXxx и квитирует Onxxx)
	std::atomic<bool> pendingRelaunch{ false };
	std::atomic<bool> pendingInject{ false };
	std::atomic<bool> pendingReconnect{ false };
	std::atomic<bool> pendingKillForever{ false };

	// Для timeout/cooldown
	uint64_t lastReconnectIssuedMs = 0;
};

class CrashWatchdog
{
public:
	CrashWatchdog();
	~CrashWatchdog();

	void SetConfig( const CrashRecoveryConfig& cfg ) { m_cfg = cfg; }
	const CrashRecoveryConfig& GetConfig() const { return m_cfg; }

	// Главная точка: вызывать каждый MonitorTick (~2с) per-bot.
	// nowMs = system clock (не steady) — должен быть в той же шкале что и
	// snapshot.heartbeatAgeMs measurement base.
	void Tick( uint64_t nowMs, int botIdx, const WatchdogSnapshot& s );

	// ── Action queries (orchestrator polls these) ──
	bool ShouldRelaunch( int idx )      { return Pop( idx, &WatchdogBot::pendingRelaunch ); }
	bool ShouldInject( int idx )        { return Pop( idx, &WatchdogBot::pendingInject ); }
	bool ShouldReconnect( int idx )     { return Pop( idx, &WatchdogBot::pendingReconnect ); }
	bool ShouldKillForever( int idx )   { return Pop( idx, &WatchdogBot::pendingKillForever ); }

	// ── Acks (orchestrator notifies progress) ──
	void OnRelaunchStarted( int idx, DWORD newDotaPid );
	void OnInjected( int idx );
	void OnReconnectIssued( int idx, uint64_t nowMs );

	// ── Setters (orchestrator передаёт init state) ──
	// hwidSeed запомнить чтобы переиспользовать при relaunch (антибан).
	void SetHwidSeed( int idx, const std::string& seed );
	std::string GetHwidSeed( int idx ) const;

	// C1: last-known lobby_id для бота (обновляется в Tick когда snapshot
	// показывает live match). Reconnect использует как fallback если
	// bot.match.lobbyId потерян. 0 = нет известного match'а.
	uint64_t GetLastKnownLobbyId( int idx ) const;

	// C3: restore lobby_id из persisted match_state_<idx>.json при старте
	// orchestrator'а. Используется после crash самого orchestrator'а.
	void RestoreLobbyId( int idx, uint64_t lobbyId );

	// Старт DumpWatcher thread'а для бота (idx) — отдельный handle на BotSteam\<idx>\dumps.
	void StartDumpWatch( int idx );
	void StopDumpWatch( int idx );
	void StopAllDumpWatch();

	// Snapshot текущего FSM state (для GUI).
	WatchdogState GetState( int idx ) const;
	const char*   GetStateStr( int idx ) const;
	int           GetCrashCount( int idx ) const;
	int           GetReconnectsThisMatch( int idx ) const;

	// Reset один бот в HEALTHY (например, юзер нажал "manual restart" в GUI).
	void Reset( int idx );

private:
	// Pop pending flag (atomic exchange to false).
	bool Pop( int idx, std::atomic<bool> WatchdogBot::* member );

	// Перейти в новое состояние, лог через callback.
	void Enter( int idx, WatchdogState newState, uint64_t nowMs, const char* reason );

	// Slide window: убрать старые crash timestamps.
	void TrimCrashWindow( int idx, uint64_t nowMs );

	// DumpWatcher thread body (per-bot). Слушает FindNextChangeNotification.
	void DumpWatcherLoop( int idx );

	// 5 ботов max (см. orchestrator.h MAX_BOTS).
	static constexpr int MAX_BOTS_INTERNAL = 8;
	WatchdogBot           m_bots[MAX_BOTS_INTERNAL];
	mutable std::mutex    m_mutex; // защищает state/queue/seed; pendingXxx — atomic
	CrashRecoveryConfig   m_cfg;
};

// ── Helper: write reconnect command для Andromeda DLL ──
// Файл C:\temp\andromeda\command_<pid>.json:
//   {"cmd":"reconnect_to_match","lobby_id":<id>,"ts":<epoch_ms>}
// DLL опрашивает в SM tick и дёргает panorama event.
namespace crash_watchdog
{
bool WriteCommandFile( DWORD pid, const std::string& jsonContent );
bool WriteReconnectCommand( DWORD pid, uint64_t lobbyId );

// C3: atomic-write helper (tmp + flush + MoveFileExA REPLACE_EXISTING).
// Используется persisted match_state_<idx>.json для переживания restart'а
// orchestrator'а — иначе reconnect mid-match после crash самого orchestrator'а
// невозможен (lobby_id потерян).
bool AtomicWriteFile( const char* dstPath, const std::string& content );

// Текущее system time в ms (epoch).
uint64_t NowMs();

// Имя FSM state (для логов / GUI).
const char* StateName( WatchdogState s );
} // namespace crash_watchdog
