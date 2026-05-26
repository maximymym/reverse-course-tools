#pragma once

#include "config.h"
#include "steam_launcher.h"
#include "dota_launcher.h"
#include "dota_minifier.h"
#include "injector.h"
#include "gsi_server.h"
#include "proxy_service.h"
#include "singbox_supervisor.h"
#include "crash_watchdog.h"
#include "match_pairing_fsm.h"
#include "orchestrator_ipc.h"
#include "orchestrator_ipc_slave.h"
#include "orchestrator_relay_client.h"
#include "role_rotation.h"
#include "sync_start_coordinator.h"
#include "pair_code.h"

#include <deque>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include <mutex>
#include <chrono>
#include <thread>
#include <atomic>

static constexpr int MAX_BOTS = 5;

// ── Статус из DLL (status_<PID>.json, schema 2) ──
struct BotConn
{
	bool     gcReady = false;
	uint32_t gcMsgsTotal = 0;
	uint32_t clientVersion = 0;
	uint32_t pingDataBytes = 0;
};

struct BotParty
{
	uint64_t              id = 0;
	int                   size = 0;
	uint64_t              leaderSteamId = 0;
	bool                  isLeader = false;
	std::vector<uint64_t> memberSteamIds;
	uint64_t              pendingInviteId = 0;
};

struct BotQueue
{
	bool inQueue = false;
	int  resultCode = -1;
};

struct BotMatch
{
	bool     found = false;
	uint64_t lobbyId = 0;
};

// ── Per-PID health snapshot (из proxyhook_health_<PID>.json) ──
struct BotHealth
{
	bool        seen = false;            // файл прочитан хотя бы раз
	int64_t     writtenMs = 0;           // timestamp записи DLL'ой
	int64_t     ageMs = -1;              // насколько давно писала DLL

	// Proxy
	bool        proxyHookActive = false; // DLL proxy hook enabled (false если useTun2Socks)
	std::string proxyRawUrl;
	bool        probeOk = false;
	std::string probeExitIp;             // actual exit IP (из api.ipify.org)
	int64_t     probeLatencyMs = -1;
	int64_t     probeAgeMs = -1;         // насколько старый probe
	std::string probeError;
	std::string probeMode;               // "socks5" | "direct"
	uint64_t    socks5Ok = 0;
	uint64_t    socks5Fail = 0;

	// HWID
	bool        hwidEnabled = false;
	std::string hwidSeed;
	bool        machineGuidMatch = false;
	bool        macMatch = false;
	bool        volumeSerialMatch = false;
	bool        systemSerialMatch = false;
	bool        systemSerialPatched = false;  // OEM set SerialNumber в SMBIOS
	bool        criticalMatch = false;    // MachineGuid + MAC + VolSerial
	bool        allMatch = false;
	std::string observedMachineGuid;
	std::string observedMac;
	std::string observedSystemSerial;
	uint32_t    observedVolumeSerial = 0;
	std::string expectedMachineGuid;
	std::string expectedMac;
	std::string expectedSystemSerial;
	uint32_t    expectedVolumeSerial = 0;
};

struct BotInstance
{
	int         idx = 0;
	DWORD       steamPid = 0;
	DWORD       dotaPid = 0;
	bool        injected = false;
	bool        dotaReady = false;  // client.dll loaded

	// Health (из ProxyHook DLL) — берётся от dotaPid (приоритет) либо steamPid
	BotHealth   health;

	// ── Status JSON (DLL → orchestrator) ──
	int          schema = 0;         // 0 = no status yet, 1 = old, 2 = new
	std::string  state = "IDLE";     // SM state (back-compat)
	std::string  gameState;          // полный EDOTAGameState от GameRules
	int          gameStateId = -1;
	float        smStateSeconds = 0.f;
	bool         paused = false;      // DLL видит pause.flag
	std::string  hero;
	int          hp = 0;
	int          maxHp = 0;
	bool         alive = false;
	float        gameTime = 0.f;
	int          gamesPlayed = 0;
	std::string  role;
	uint64_t     ownSteamId = 0;
	int64_t      heartbeatMs = 0;     // wall-clock ms когда DLL писал
	int64_t      heartbeatAgeMs = -1; // насколько свежий heartbeat
	BotConn      conn{};
	BotParty     party{};
	BotQueue     queue{};
	BotMatch     match{};

	// ── GSI (Dota → orchestrator HTTP server) ──
	bool         gsiSeen = false;
	GsiSnapshot  gsi{};
	int64_t      gsiAgeMs = -1;

	std::chrono::steady_clock::time_point lastStatusUpdate{};
};

struct LogEntry
{
	std::string timestamp;
	std::string message;
};

class Orchestrator
{
public:
	enum class State
	{
		IDLE,
		LAUNCHING,        // Steam+Dota launching (unified)
		WAITING_READY,    // Waiting for client.dll in all instances
		INJECTING,
		WAITING_GC_READY, // Post-inject: ждём пока все DLL'ки прорезолвят GC handler
		RUNNING,
		STOPPING,
		ERROR_STATE
	};

	Orchestrator();

	// Initialize with config, resolve all paths to absolute
	bool Init( const std::string& configDir, const std::string& exeDir );

	// Стартовать ТОЛЬКО ProxyService (kernel redirect) — для случая когда
	// accounts.json ещё нет и Init() не вызван, но Steam через "Open Steam"
	// в setup wizard уже надо защитить от direct leak'а.
	void EarlyStartProxy( const std::string& exeDir );

	// Поднять GSI HTTP listener + установить cfg в Dota.
	// Можно дёргать повторно — GSI cfg идемпотентно перезаписывается.
	bool EnsureGsi();

	// Информация для GUI (последняя ошибка/диагностика GSI)
	struct GsiStatus
	{
		bool         running = false;
		unsigned     port = 0;
		uint64_t     totalRequests = 0;
		size_t       seenSteamIds = 0;
		std::string  cfgPath;
		std::string  lastError;
	};
	GsiStatus GetGsiStatus() const;

	// Kernel-level proxy redirect (WinDivert). Идёт лениво — стартуется при
	// первой регистрации root PID если useKernelRedirect=true.
	ProxyService& GetProxyService() { return m_proxy; }
	const ProxyService& GetProxyService() const { return m_proxy; }

	// Per-account SOCKS5 через tun2socks (sing-box) — основной proxy path.
	SingboxSupervisor& GetSingbox() { return m_singbox; }
	const SingboxSupervisor& GetSingbox() const { return m_singbox; }

	// Single-button: runs in background thread
	void StartFarm();
	void StopFarm();
	bool IsBusy() const { return m_busy.load(); }

	// Called every frame
	void MonitorTick();

	// Setup: launch Steam for a single account login
	DWORD LaunchSteamForLogin( int idx );
	void KillSteamProcess( DWORD pid );
	bool IsAccountSetUp( int idx );
	bool AreAllAccountsSetUp();

	// Getters for GUI
	State GetState() const { return m_state; }
	const char* GetStateStr() const;
	const BotInstance& GetBot( int idx ) const { return m_bots[idx]; }
	int GetBotCount() const { return m_nBotCount; }
	int GetTotalGames() const;
	const FarmConfig& GetConfig() const { return m_config; }
	FarmConfig& GetConfigMut() { return m_config; }
	const std::vector<LogEntry>& GetLog() const { return m_log; }
	void LogPublic( const char* msg );
	std::chrono::steady_clock::time_point GetStartTime() const { return m_startTime; }
	bool IsInitialized() const { return m_nBotCount > 0; }

public:
	// ── Pairing accessors (для GUI) ──
	struct PairingStatus
	{
		bool         enabled = false;
		bool         isMaster = false;
		bool         connected = false;
		int          clientCount = 0;       // только для master
		int64_t      lastPeerHbAgeMs = -1;
		PairingPhase phase = PairingPhase::IDLE;
		int          localFilled = 0;
		int          localTotal = 0;
		uint64_t     localMajorityLobby = 0;
		int          peerCount = 0;
		int          cancelStreak = 0;
		std::string  lastError;
		// Только при transport=relay: код последней ошибки от relay-сервиса
		// ("auth_failed" / "unknown_user" / "user_disabled" / ...) либо "" если
		// нет ошибок. Direct-режим всегда оставляет пустым.
		std::string  relayErrorCode;
		bool         usingRelay = false;
		std::string  currentStrategy;       // WIN/LOSE/DEBOOST в текущем матче
		std::string  nextStrategy;          // что будет в следующем
		std::deque<MatchRecord> history;    // последние 5

		// From RelayPeer::Snapshot (relay-mode только; в direct-режиме нули).
		uint64_t          msgSent = 0;
		uint64_t          msgRecv = 0;
		int64_t           rttMs   = -1;   // -1 = unknown / direct-mode

		// From SyncStartCoordinator::GetSnapshot()
		SyncStartSnapshot syncStart;
	};
	PairingStatus GetPairingStatus() const;

	// ── Pairing lifecycle controls (Wave 2 — used by GUI Wave 3) ─────────
	// Re-init pairing transports после изменения config (pair code apply, etc.)
	// Guard: refuse если state==RUNNING OR m_syncStart != IDLE.
	bool ReinitPairing();

	// Wrapper around StartFarm: gates на pair_complete + recent peer hb + handoff
	// в SyncStartCoordinator::Initiate. Backward-compat: при uxV2=false ИЛИ
	// pairing.enabled=false делегирует напрямую на StartFarm() (legacy path).
	// Returns true если запустился (legacy) ИЛИ если handshake инициирован.
	bool GuardedStartFarm();

	// Force reconnect (для GUI Reconnect button). В relay-mode bypass'ит backoff
	// через m_relayPeer->RequestReconnect; в direct-mode делает ReinitPairing.
	void RequestForceReconnect();

	// Disconnect runtime — останавливает peer'ов и FSM, не stop'ает фарм.
	void RequestDisconnect();

	// Apply decoded pair code + persist + re-init. Объединяет ApplyPairCode +
	// SavePairingConfigAtomic + ReinitPairing в один атомарный для GUI шаг.
	bool ApplyPairCodeAndReinit( const pair_code::Decoded& decoded );

	// Текущий pair code на основе live config (для Generate dialog).
	// Master генерит код для slave (roleHint="S"), и наоборот. Возвращает ""
	// если конфиг не заполнен (отсутствуют relayHost / userId / pairSecret / ...).
	std::string GenerateCurrentPairCode() const;

	// Force override для следующего матча (Force WIN / Force LOSE кнопки в GUI).
	void SetTeamStrategyOverride( const std::string& mode );  // "auto"|"WIN"|"LOSE"|"DEBOOST"
	const std::string& GetTeamStrategyMode() const { return m_config.teamStrategyMode; }

	// ── Minifier on-the-fly apply/revert (для GUI кнопки) ──
	// Apply ко всем enabled-аккаунтам (force-override JSON enabled-флага +
	// force VPK). Revert — RevertAll + revert VPK если был применён.
	// IsApplied — true если хотя бы у одного бота есть активный backup ИЛИ
	// VPK marker present. IsMinifierBusy — true пока bg thread работает
	// (apply/revert non-blocking, VPK subprocess 10-30s).
	bool IsMinifierAppliedToAnyBot() const { return m_minifier.IsAppliedToAnyBot(); }
	bool IsMinifierBusy() const { return m_minifierBusy.load(); }
	void ApplyMinifierAll();   // non-blocking: spawns detached thread
	void RevertMinifierAll();  // non-blocking: spawns detached thread

private:
	void Log( const char* fmt, ... );
	void ReadStatusFiles();
	void ReadGsiSnapshots();
	void ReadHealthFiles();
	void ReadMatchPendingFiles();
	void HandlePairingDecision( const PairingDecision& d );
	// Единая точка приёма peer-сообщений (вызывается из любого транспорта:
	// OrchestratorIpc / SlavePeer / RelayPeer). Routes к m_pairing для
	// match-pending координации либо в match_result reconciliation handler.
	void OnPairingMessage( const PeerMsg& m );
	// Routes outgoing json через активный transport (relay / slave / master ipc).
	// Используется m_pairing.onBroadcast и m_syncStart.broadcast.
	void SendPairingMessage_( const nlohmann::json& msg );
	// Поднимает pairing transports исходя из текущего m_config.pairing.
	// Extracted из Init() ради переиспользования в ReinitPairing().
	void InitPairing_();
	void TickPostGameDetection();
	std::string ResolveStrategyForNextMatch();
	void RestartInstance( int idx );
	void BuildPartyMembers( int excludeIdx, uint64_t* out, int& count );
	void CreateProfileDirs();
	void EnsureTempDirs();
	void StartFarmThread();  // actual work (runs on bg thread)
	bool KillMutexWithRetry( DWORD pid, int retries = 3 );

	// Crash recovery: фоновая детачнутая задача для одного бота.
	// RELAUNCHING фаза: kill steam+dota инстанса → sleep 5s → LaunchInstance
	// (Steam restart) → poll alive → LaunchDotaOnly → reinject DLLs (Andromeda
	// + ProxyHook) → переход в INJECTING. Длинная (30-90s).
	void RecoveryThread( int idx );
	void IssueReconnect( int idx );          // RECONNECTING (быстро)

	std::string ResolvePath( const std::string& path );

	// GSI
	std::string  m_gsiCfgPath;
	std::string  m_gsiInstallError;
	bool         m_gsiInstalled = false;

	FarmConfig      m_config;
	std::string     m_exeDir;  // directory containing DotaFarm.exe
	State           m_state = State::IDLE;
	BotInstance     m_bots[MAX_BOTS];
	int             m_nBotCount = 0;

	SteamLauncher     m_steamLauncher;
	DotaLauncher      m_dotaLauncher;
	Injector          m_injector;
	ProxyService      m_proxy;
	SingboxSupervisor m_singbox;
	CrashWatchdog     m_watchdog;
	DotaMinifier      m_minifier;

	// ── Pairing 5v5 self-play (опционально, cfg.pairing.enabled) ──
	// Direct mode: master использует m_ipc, slave — m_slavePeer.
	// Relay mode:  ОБЕ роли используют m_relayPeer (m_ipc/m_slavePeer не стартуют).
	OrchestratorIpc            m_ipc;
	std::unique_ptr<SlavePeer> m_slavePeer;
	std::unique_ptr<RelayPeer> m_relayPeer;
	MatchPairingFsm            m_pairing;
	SyncStartCoordinator       m_syncStart;
	RoleRotation             m_roleRotation;
	std::string              m_currentStrategy;        // "WIN"|"LOSE"|"DEBOOST" в текущем матче
	bool                     m_postGameHandled = false;
	uint64_t                 m_lastHandledMatchId = 0;
	int                      m_pauseUntilMatchCancelMs = 0;  // если streak ≥ max — long pause
	uint64_t                 m_pauseUntilMs = 0;
	// Mapping pid → botIdx для match_pending файлов.
	std::map<DWORD, int>     m_pidToBotIdx;

	std::vector<LogEntry> m_log;
	std::chrono::steady_clock::time_point m_startTime{};
	std::chrono::steady_clock::time_point m_lastMonitorTick{};

	std::atomic<bool> m_busy{ false };
	std::thread       m_workerThread;

	// Minifier apply/revert — long-running (VPK subprocess 10-30s). Кнопка в GUI
	// триггерит detached thread, флаг отображается в UI как "APPLYING..." пока
	// thread не завершит работу.
	std::atomic<bool> m_minifierBusy{ false };

	// B.2: sequential launch — ротация по кругу между StartFarm вызовами.
	// Persists внутри процесса, между запусками orchestrator сбрасывается.
	int               m_nextSequentialIdx = 0;

	mutable std::mutex m_mutex;

	// CR-FIX 2026-05-26: serialize RecoveryThread'ы. Без него 2 параллельных
	// recovery race'ятся в LaunchDotaOnly — оба видят одну новую dota2.exe
	// (FindDotaPids ∖ excludeSet) и оба возвращают её как "свою". Симптом в
	// DotaFarm.log: "#1: recovery: dota2.exe PID X spawned" + "#3: recovery:
	// dota2.exe PID X spawned" — одинаковый PID, второй inevitably exit'ит
	// → watchdog → DEAD. Также concurrent shader recompile на WARP usurp'ит
	// все GPU слоты разом, → terminal crash recurrence. Serialization OK для
	// фермы: 5 одновременных crashes — recover 1 за раз с ~120s каждый,
	// итого ~10 мин полной восстановки. Лучше чем фейк-DEAD за 5 мин.
	std::mutex        m_recoveryMx;
};
