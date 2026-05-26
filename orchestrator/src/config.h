#pragma once

#include "crash_watchdog.h"
#include "dota_minifier.h"

#include <string>
#include <vector>
#include <cstdint>

// Forward decl — реальная definition в pair_code.h (Task T1).
// Используется только в API config::ApplyPairCode (по reference), поэтому
// header-include здесь не нужен — реализация в config.cpp включит pair_code.h.
namespace pair_code { struct Decoded; }

// Spoof Mode — единый селектор HWID-режима для фермы.
// Off       — никакого спуфа, легитимный HWID
// SteamOnly — per-process IAT хуки через ProxyHook.dll. Каждый Steam инстанс
//             видит уникальный fake HWID, derived от steamId. User-mode, без
//             driver, без admin прав на загрузку sys'ника
// FullPC    — machine-wide kernel driver через HwidSpoofer.exe + spoofer.sys.
//             Все процессы на ПК видят fake HWID. Один HWID на всю ферму.
//             Требует admin + driver signing работает через KDU
// Both      — defense in depth: driver спуфит базу, поверх per-process хуки
//             разводят 5 ботов между собой, чтобы у каждого был свой HWID
enum class SpoofMode
{
	Off       = 0,
	SteamOnly = 1,
	FullPC    = 2,
	Both      = 3,
};

inline const char* SpoofModeToString( SpoofMode m )
{
	switch ( m )
	{
		case SpoofMode::Off:       return "off";
		case SpoofMode::SteamOnly: return "steam_only";
		case SpoofMode::FullPC:    return "full_pc";
		case SpoofMode::Both:      return "both";
	}
	return "off";
}

inline SpoofMode SpoofModeFromString( const std::string& s )
{
	if ( s == "off" )         return SpoofMode::Off;
	if ( s == "steam_only" )  return SpoofMode::SteamOnly;
	if ( s == "full_pc" )     return SpoofMode::FullPC;
	if ( s == "both" )        return SpoofMode::Both;
	return SpoofMode::SteamOnly;  // default
}

struct AccountConfig
{
	std::string ipcName;
	std::string login;
	uint64_t    steamId = 0;
	std::string persona;
	bool        userchooser = false;
	bool        enabled = true;  // can be toggled in dashboard

	// Per-account IPv4 proxy (SOCKS5/HTTP). Формат:
	//   socks5://user:pass@host:port
	//   http://user:pass@host:port
	//   "" = no proxy (default)
	// Применяется к Steam+Dota через Proxifier rule или env vars.
	std::string proxy;

	// Per-account toggle для proxy. Если false — прокси НЕ применяется даже если
	// `proxy` строка заполнена. Позволяет юзеру на лету отключать прокси для
	// одного аккаунта без очистки поля. Default true.
	bool        proxyEnabled = true;

	// Per-account toggle "применять выбранный SpoofMode для этого аккаунта".
	// Имеет смысл только при SpoofMode == SteamOnly или Both (per-process hooks).
	// При FullPC machine-wide — flag игнорируется, driver всё равно спуфит ВСЁ.
	// При Off — flag игнорируется. Default true.
	bool        hwidSpoofEnabled = true;

	// Steam password — используется ТОЛЬКО при первом логине через
	// `steam.exe -login <user> <pass>`. После успешного логина Steam сохраняет
	// токен в loginusers.vdf (RememberPassword=1), пароль больше не нужен.
	// Можно оставить в json — Steam всё равно проигнорирует если токен свежий.
	// WARNING: хранится в plaintext, защищай файл ACL'ом.
	std::string password;
};

struct FarmConfig
{
	// Accounts
	std::string            steamExe = "C:\\Program Files (x86)\\Steam\\steam.exe";
	std::vector<AccountConfig> accounts;
	int                    dotaAppId = 570;
	// 2026-05-22: расширенный low-spec launch profile (low-spec-researcher findings):
	// -noprewarm — skip shader precompilation (для каждого бота отдельно копит, на 5 vCPU = долго)
	// -low — process priority BelowNormal (если 5 ботов на 4 vCPU, scheduler distribute по приоритету)
	// -dxwarp — force WARP software renderer без detection (наш кейс на дедике)
	// -uselogdir — изолированные log dirs per instance
	// -nohltv -novr — отключение GOTV demos и VR
	// -favor_consistent_framerate — preferred predictable framerate vs peak FPS
	std::string            launchArgs = "-novid -nojoy -w 640 -h 480 -windowed -noprewarm -low -dxwarp -uselogdir -nohltv -novr -favor_consistent_framerate +fps_max 15 -console";

	// Farm settings
	std::vector<std::string> heroes;  // per-bot heroes (fallback: skeleton_king)
	uint32_t               region = 0x80;   // Russia
	uint32_t               gameMode = 0x02; // All Pick
	std::string            profilesDir = "C:\\BotProfiles";

	// Paths
	std::string            dllPath;
	std::string            handleExe = "C:\\temp\\handle64.exe";
	std::string            configDir;

	// ProxyHook.dll — per-account Winsock redirector. Если путь указан и у
	// аккаунта в accounts[i].proxy непустая строка — Steam launch идёт с
	// CREATE_SUSPENDED, DLL инжектится до первого Winsock call, child
	// процессы (steamwebhelper, dota2) auto-inherit через CreateProcess hook.
	std::string            proxyHookDllPath = "ProxyHook.dll";

	// Единый HWID Spoof Mode. Заменяет старые независимые булевые флаги
	// spoofer_enabled (FullPC) + hwid_spoof_enabled (per-process). Backward-
	// compat миграция реализована в config.cpp::LoadFarmSettings.
	//
	// Default: SteamOnly. Это user-mode безопасный режим: каждый бот получает
	// уникальный HWID без admin-прав на driver, без риска сломать Windows
	// апдейтом. FullPC и Both требуют HwidSpoofer.exe + spoofer.sys в dist/.
	SpoofMode              spoofMode = SpoofMode::SteamOnly;

	// HwidSpoofer.exe — driver-orchestrator. Запускается если spoofMode ==
	// FullPC || Both, перед первым LaunchInstance. Один раз на запуск фермы,
	// seed = MakeMachineSeed() (один HWID на всю ферму).
	std::string            spooferExe;
	int                    spooferTimeoutMs = 45000;

	// Удобные предикаты: проверка активности каждой ветки спуфа.
	// IsFullPCSpoofEnabled — машинный driver-спуф нужно запустить перед фермой
	// IsSteamSpoofEnabled  — per-process IAT хуки в Steam/Dota инстансах
	bool IsFullPCSpoofEnabled() const
	{
		return spoofMode == SpoofMode::FullPC || spoofMode == SpoofMode::Both;
	}
	bool IsSteamSpoofEnabled() const
	{
		return spoofMode == SpoofMode::SteamOnly || spoofMode == SpoofMode::Both;
	}

	// Launch mode: "parallel" (default, текущее поведение) или "sequential"
	// (1 бот за раз, с cooldown между запусками — используется как antiban
	// fallback: 5 одновременных логинов = мощный smurf-сигнал).
	std::string            launchMode = "parallel";
	int                    launchCooldownSec = 600; // 10 мин между sequential запусками

	// Crash recovery (см. tools/dota2/RECONNECT_DESIGN.md). Defaults в
	// CrashRecoveryConfig::default_init — все enabled с разумными порогами.
	CrashRecoveryConfig    crashRecovery{};

	// Minifier — урезание Dota 2 graphics/network settings для self-play фермы.
	// Безопасный путь: launch options + autoexec.cfg + video.txt с backup/revert.
	// VPK patching и D3D hook оставлены за флагами (НЕ реализованы — VAC risk).
	// См. docs/MINIFIER_RESEARCH.md.
	MinifierConfig         minifier{};

	// ── CPU affinity per dota2.exe instance ──
	// Глобальный лимит "сколько ядер давать каждому окну Доты". 0 = без лимита
	// (всё система). Иначе orchestrator после спавна dota2.exe считает affinity
	// mask на N последовательных ядер и шифтит её для каждого следующего
	// инстанса, чтобы 5 ботов не дрались за одни и те же ядра.
	// Применяется через SetProcessAffinityMask (см. dota_launcher.cpp).
	int                    coresPerInstance = 0;

	// ── Window tiler ──
	// Раскладка 5 окон Dota в grid на одном дисплее. См. window_tiler.h.
	// layout: "grid_3_2" | "grid_2_2_1" | "strip_5" | "cascade"
	std::string            tileLayout = "grid_3_2";
	bool                   tileForceWindowed = false;
	bool                   tileAutoOnLaunch = false; // авто-tile когда все боты dotaReady
	int                    tilePadding = 4;          // в пикселях между окнами
	int                    tileMonitorIndex = -1;    // -1 = primary

	// ── Парный 5v5 self-play (Bundle C) ──
	// Master orchestrator поднимает TCP listener, slave подключается и они
	// обмениваются lobby_id'ами после того как у каждого 5/5 ботов сработал
	// ReadyUp. См. tools/dota2/orchestrator/src/match_pairing_fsm.h.
	struct PairingConfig
	{
		bool        enabled               = false;
		// Транспорт между master и slave orchestrator'ами:
		//   "direct" — master поднимает TCP listener, slave коннектится напрямую
		//              (loopback для тестов, LAN для двух машин в одной сети).
		//   "relay"  — оба клиента коннектятся к relay-сервису на VPS (NAT-traversal).
		// Backward-compat: missing поле в farm.json → "direct".
		std::string transport             = "direct";
		std::string role                  = "master";   // "master" | "slave"
		std::string pairSecret            = "change-me-32chars-min";

		// ── direct mode ──
		std::string masterIp              = "127.0.0.1";
		uint16_t    ipcPort               = 5050;

		// ── relay mode ──
		// Multi-tenant relay (Bundle D server обслуживает много пользователей).
		// userId + userAuthToken выдаются admin'ом relay-сервиса; relay
		// проверяет их по своей user db и при auth-fail закрывает соединение
		// с {"type":"error","body":{"code":"auth_failed",...}}.
		// pairId — общий идентификатор пары, scoped в namespace user_id
		// (две разные пары могут иметь одинаковый pair_id если они у разных users).
		// relayHost/relayPort — endpoint relay-сервиса на VPS.
		std::string userId                = "";   // выдан admin'ом relay'я
		std::string userAuthToken         = "";   // 32+ char token
		std::string pairId                = "default-pair";
		std::string relayHost             = "";
		uint16_t    relayPort             = 5050;

		// ── common ──
		int         matchSyncTimeoutS     = 15;
		int         maxConsecutiveCancels = 3;
		int         postGameWinnerGraceS  = 60;

		// Opt-in для нового UX (Pair Code workflow + Sync Start). При true GUI
		// показывает упрощённый pair-code flow вместо ручного редактирования
		// relay/user/pair полей. Default false — старый UX по-прежнему доступен.
		bool        uxV2                  = false;
	};
	PairingConfig pairing;

	// "auto" → RoleRotation определяет следующий strategy (WIN ↔ LOSE).
	// "WIN" / "LOSE" / "DEBOOST" → forced override (используется кнопками в GUI).
	std::string teamStrategyMode = "auto";

	// ── Per-account SOCKS5 через tun2socks (sing-box) ──
	// Если true — orchestrator спавнит sing-box.exe рядом с DotaFarm.exe.
	// sing-box поднимает WinTUN адаптер, становится default route, и матчит
	// Steam-процессы по process_path_regex "...\\BotSteam\\<N>\\..." → per-account
	// SOCKS5 outbound. Не-Steam процессы идут через sing-box'овский "direct"
	// outbound, который обходит TUN через реальный NIC. Это единственный рабочий
	// способ (user-mode ProxyHook не ловит AFD-direct Steam'а; WinDivert
	// termination-relay ломает TCP seq numbers).
	// Default false: WinTUN перехватывает ВЕСЬ трафик системы и становится
	// default route → ломает существующий VPN/маршрутизацию пользователя.
	// Включать только осознанно, через UI checkbox или farm.json.
	bool                   useTun2Socks = false;

	// ── Kernel-level proxy redirect (WinDivert) ──
	// DEAD PATH: не работает из-за TCP seq mismatch при packet-level termination.
	// Оставлен в репо на случай UDP-only сценариев. Держать false.
	bool                   useKernelRedirect = false;
	// Если true — ProxyHook.dll по-прежнему инжектится как user-mode fallback.
	// При useTun2Socks=true ProxyHook бесполезен (TUN уже ловит всё) — НЕ инжектим.
	bool                   keepProxyHookFallback = false;

	// Get hero for bot index (cycles if not enough heroes)
	std::string GetHero( int idx ) const
	{
		if ( heroes.empty() ) return "npc_dota_hero_skeleton_king";
		return heroes[idx % heroes.size()];
	}
};

namespace config
{

bool LoadAccounts( const std::string& path, FarmConfig& cfg );
bool LoadFarmSettings( const std::string& path, FarmConfig& cfg );
bool SaveAccounts( const std::string& path, const FarmConfig& cfg );

// Patch single top-level bool в farm.json. Best-effort: парсит существующий
// JSON, перезаписывает (или вставляет) поле, пишет обратно. Если файл
// невалидный — возвращает false и НЕ создаёт новый. Используется UI
// toggles чтобы сохранить выбор юзера между перезапусками без перезаписи
// прочих секций (heroes, minifier, crash_recovery и т.д.).
bool SaveFarmBoolSetting( const std::string& path, const std::string& key, bool value );

// Аналог для строковых полей (например spoof_mode = "steam_only"). Семантика
// идентична SaveFarmBoolSetting.
bool SaveFarmStringSetting( const std::string& path, const std::string& key, const std::string& value );

// Аналог для int-полей (например cores_per_instance). Семантика идентична.
bool SaveFarmIntSetting( const std::string& path, const std::string& key, int value );

// Atomic RMW write секции "pairing" в farm.json.
// Использует LockFileEx + MoveFileEx (паттерн strategy_writer). Сохраняет
// все НЕ-pairing секции (heroes, accounts, crash_recovery, minifier, и т.п.).
// Сериализуется JSON с indent=4 (читабельно для пользователя).
bool SavePairingConfigAtomic( const std::string& farmJsonPath,
                              const FarmConfig::PairingConfig& cfg );

// Применяет decoded pair code к существующему PairingConfig.
// Merge: relayHost, relayPort, userId, userAuthToken, pairId, pairSecret,
// role (на основе roleHint), transport="relay".
// Preserve: enabled, masterIp, ipcPort, matchSyncTimeoutS,
// maxConsecutiveCancels, postGameWinnerGraceS, uxV2.
// roleHint == "M" → role="master", иначе → role="slave".
void ApplyPairCode( const pair_code::Decoded& src, FarmConfig::PairingConfig& dst );

// Нормализует проксю в канонический socks5://user:pass@host:port формат.
// Принимает:
//   - "socks5://user:pass@host:port"           → as-is
//   - "http://user:pass@host:port"             → as-is (не поддерживается DLL, но сохраняем)
//   - "host:port:user:pass"                    → "socks5://user:pass@host:port"
//   - "host:port"                              → "socks5://host:port" (без auth)
//   - "user:pass@host:port"                    → "socks5://user:pass@host:port"
//   - "" или whitespace                        → ""
std::string NormalizeProxyString( const std::string& raw );

// Parse Steam loginusers.vdf — returns map of AccountName → {steamId, persona}
struct VdfAccount
{
	uint64_t    steamId = 0;
	std::string persona;
};
std::vector<std::pair<std::string, VdfAccount>> ParseLoginUsersVdf( const std::string& steamDir );

} // namespace config
