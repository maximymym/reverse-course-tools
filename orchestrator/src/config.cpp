#include "config.h"
#include "crypto/sealed_file.h"
#include "pair_code.h"
#include <json.hpp>
#include <fstream>
#include <sstream>
#include <cctype>
#include <algorithm>
#include <Windows.h>

using json = nlohmann::json;

namespace config
{

static std::string TrimWS( const std::string& s )
{
	size_t b = 0, e = s.size();
	while ( b < e && std::isspace( (unsigned char)s[b] ) ) b++;
	while ( e > b && std::isspace( (unsigned char)s[e - 1] ) ) e--;
	return s.substr( b, e - b );
}

std::string NormalizeProxyString( const std::string& raw )
{
	std::string s = TrimWS( raw );
	if ( s.empty() ) return "";

	// Уже канонический url со scheme — оставляем (lowercase scheme).
	if ( s.find( "://" ) != std::string::npos )
	{
		size_t schemeEnd = s.find( "://" );
		std::string scheme = s.substr( 0, schemeEnd );
		std::transform( scheme.begin(), scheme.end(), scheme.begin(),
			[]( unsigned char c ) { return (char)std::tolower( c ); } );
		return scheme + s.substr( schemeEnd );
	}

	// Без scheme — считаем SOCKS5. Варианты:
	//   host:port                         (2 части)
	//   user:pass@host:port               (@ присутствует)
	//   host:port:user:pass               (3+ частей через :)
	//   host:port:user                    (3 частей, pass пусто)

	size_t atPos = s.find( '@' );
	if ( atPos != std::string::npos )
	{
		// user:pass@host:port style
		return "socks5://" + s;
	}

	// Считаем двоеточия
	int colons = 0;
	for ( char c : s ) if ( c == ':' ) colons++;

	if ( colons == 1 )
	{
		// host:port — без auth
		return "socks5://" + s;
	}

	if ( colons >= 3 )
	{
		// host:port:user:pass (возможно с доп. ':' в pass — берём первые три)
		size_t p1 = s.find( ':' );
		size_t p2 = s.find( ':', p1 + 1 );
		size_t p3 = s.find( ':', p2 + 1 );
		std::string host = s.substr( 0, p1 );
		std::string port = s.substr( p1 + 1, p2 - p1 - 1 );
		std::string user = s.substr( p2 + 1, p3 - p2 - 1 );
		std::string pass = s.substr( p3 + 1 );  // всё что после — пароль
		return "socks5://" + user + ":" + pass + "@" + host + ":" + port;
	}

	if ( colons == 2 )
	{
		// host:port:user (pass пустой)
		size_t p1 = s.find( ':' );
		size_t p2 = s.find( ':', p1 + 1 );
		std::string host = s.substr( 0, p1 );
		std::string port = s.substr( p1 + 1, p2 - p1 - 1 );
		std::string user = s.substr( p2 + 1 );
		return "socks5://" + user + "@" + host + ":" + port;
	}

	// Странный формат — возвращаем как есть (DLL залогирует parse fail).
	return s;
}

// accounts.json is stored as AES-256-GCM sealed envelope ("DFRM" + IV + tag + ct).
// On first load after upgrade a legacy plain-text accounts.json is migrated:
// parsed, re-written sealed.
bool LoadAccounts( const std::string& path, FarmConfig& cfg )
{
	std::string raw;
	bool hadMagic = false;
	bool unsealOk = sealed_file::UnsealFileToString( path, raw, hadMagic );

	// Diagnostic to DotaFarm.log so we can see WHY accounts disappeared after
	// "update" — the most common cause is HWID-derived key mismatch (sealed
	// file from old build / different env doesn't decrypt).
	{
		FILE* f = fopen( "C:\\temp\\andromeda\\config_load.log", "a" );
		if ( f )
		{
			SYSTEMTIME st; GetLocalTime( &st );
			fprintf( f, "[%04d-%02d-%02d %02d:%02d:%02d] LoadAccounts path=%s hadMagic=%d unsealOk=%d rawLen=%zu\n",
				st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
				path.c_str(), (int)hadMagic, (int)unsealOk, raw.size() );
			fclose( f );
		}
	}

	// hadMagic && !unsealOk ⇒ tag mismatch (HWID changed / corrupted) — fail loud.
	if ( hadMagic && !unsealOk )
		return false;

	// No magic AND no bytes ⇒ file simply missing.
	if ( !hadMagic && raw.empty() )
		return false;

	const bool needsMigration = !hadMagic;  // legacy plain JSON on disk

	try
	{
		json j = json::parse( raw );

		if ( j.contains( "steam_exe" ) )
			cfg.steamExe = j["steam_exe"].get<std::string>();

		if ( j.contains( "dota_app_id" ) )
			cfg.dotaAppId = j["dota_app_id"].get<int>();

		if ( j.contains( "launch_args" ) )
			cfg.launchArgs = j["launch_args"].get<std::string>();

		if ( j.contains( "instances" ) )
		{
			cfg.accounts.clear();
			for ( auto& inst : j["instances"] )
			{
				AccountConfig acc;
				if ( inst.contains( "ipc_name" ) )  acc.ipcName = inst["ipc_name"].get<std::string>();
				if ( inst.contains( "login" ) )      acc.login = inst["login"].get<std::string>();
				if ( inst.contains( "steam_id" ) )   acc.steamId = inst["steam_id"].get<uint64_t>();
				if ( inst.contains( "persona" ) )    acc.persona = inst["persona"].get<std::string>();
				if ( inst.contains( "userchooser" ) ) acc.userchooser = inst["userchooser"].get<bool>();
				if ( inst.contains( "enabled" ) )    acc.enabled = inst["enabled"].get<bool>();
				if ( inst.contains( "proxy" ) )      acc.proxy = NormalizeProxyString( inst["proxy"].get<std::string>() );
				// Per-account toggles. Default true если поле отсутствует
				// (юзеры с accounts.json от старых версий).
				if ( inst.contains( "proxy_enabled" ) )      acc.proxyEnabled = inst["proxy_enabled"].get<bool>();
				if ( inst.contains( "hwid_spoof_enabled" ) ) acc.hwidSpoofEnabled = inst["hwid_spoof_enabled"].get<bool>();
				if ( inst.contains( "password" ) )   acc.password = inst["password"].get<std::string>();
				cfg.accounts.push_back( acc );
			}
		}

		if ( needsMigration )
			SaveAccounts( path, cfg ); // re-write sealed; ignore failure (next save will retry)

		// Diagnostic: how many accounts loaded, how many have non-empty passwords.
		{
			int withPw = 0;
			for ( auto& a : cfg.accounts ) if ( !a.password.empty() ) withPw++;
			FILE* f = fopen( "C:\\temp\\andromeda\\config_load.log", "a" );
			if ( f )
			{
				SYSTEMTIME st; GetLocalTime( &st );
				fprintf( f, "[%04d-%02d-%02d %02d:%02d:%02d] LoadAccounts parsed: %zu accounts, %d with password, %zu without\n",
					st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
					cfg.accounts.size(), withPw, cfg.accounts.size() - withPw );
				fclose( f );
			}
		}

		return true;
	}
	catch ( ... )
	{
		return false;
	}
}

bool LoadFarmSettings( const std::string& path, FarmConfig& cfg )
{
	std::ifstream file( path );
	if ( !file.is_open() )
		return false;

	try
	{
		json j;
		file >> j;

		// Heroes: array or single string
		if ( j.contains( "heroes" ) && j["heroes"].is_array() )
		{
			cfg.heroes.clear();
			for ( auto& h : j["heroes"] )
				cfg.heroes.push_back( h.get<std::string>() );
		}
		else if ( j.contains( "hero" ) )
		{
			cfg.heroes.clear();
			cfg.heroes.push_back( j["hero"].get<std::string>() );
		}

		if ( j.contains( "region" ) )      cfg.region = j["region"].get<uint32_t>();
		if ( j.contains( "game_mode" ) )   cfg.gameMode = j["game_mode"].get<uint32_t>();
		if ( j.contains( "dll_path" ) )    cfg.dllPath = j["dll_path"].get<std::string>();
		if ( j.contains( "handle_exe" ) )  cfg.handleExe = j["handle_exe"].get<std::string>();
		if ( j.contains( "profiles_dir" ) ) cfg.profilesDir = j["profiles_dir"].get<std::string>();

		// Anti-ban
		if ( j.contains( "spoofer_exe" ) )         cfg.spooferExe = j["spoofer_exe"].get<std::string>();
		if ( j.contains( "spoofer_timeout_ms" ) )  cfg.spooferTimeoutMs = j["spoofer_timeout_ms"].get<int>();
		if ( j.contains( "launch_mode" ) )         cfg.launchMode = j["launch_mode"].get<std::string>();
		if ( j.contains( "launch_cooldown_sec" ) ) cfg.launchCooldownSec = j["launch_cooldown_sec"].get<int>();

		if ( j.contains( "proxyhook_dll" ) )       cfg.proxyHookDllPath = j["proxyhook_dll"].get<std::string>();

		// HWID Spoof Mode — единый ключ. Backward-compat: если spoof_mode
		// отсутствует, но есть старые булевые spoofer_enabled / hwid_spoof_enabled,
		// конвертируем на лету. Если только spoof_mode — он и побеждает.
		if ( j.contains( "spoof_mode" ) && j["spoof_mode"].is_string() )
		{
			cfg.spoofMode = SpoofModeFromString( j["spoof_mode"].get<std::string>() );
		}
		else
		{
			bool legacyFullPC = j.contains( "spoofer_enabled" ) && j["spoofer_enabled"].get<bool>();
			bool legacySteam  = j.contains( "hwid_spoof_enabled" ) && j["hwid_spoof_enabled"].get<bool>();
			if ( legacyFullPC && legacySteam ) cfg.spoofMode = SpoofMode::Both;
			else if ( legacyFullPC )           cfg.spoofMode = SpoofMode::FullPC;
			else if ( legacySteam )            cfg.spoofMode = SpoofMode::SteamOnly;
			else if ( j.contains( "spoofer_enabled" ) || j.contains( "hwid_spoof_enabled" ) )
			{
				// Оба ключа явно false → юзер отключил всё.
				cfg.spoofMode = SpoofMode::Off;
			}
			// Иначе оставляем default из FarmConfig (SteamOnly).
		}

		// Network engines. Defaults в FarmConfig — оба false ("не трогаем сеть
		// юзера"). use_tun2socks=true создаёт WinTUN адаптер который становится
		// default route и ломает существующий VPN; use_kernel_redirect — dead
		// path (TCP seq mismatch).
		if ( j.contains( "use_tun2socks" ) )       cfg.useTun2Socks = j["use_tun2socks"].get<bool>();
		if ( j.contains( "use_kernel_redirect" ) ) cfg.useKernelRedirect = j["use_kernel_redirect"].get<bool>();
		if ( j.contains( "keep_proxyhook_fallback" ) ) cfg.keepProxyHookFallback = j["keep_proxyhook_fallback"].get<bool>();

		// ── CPU cores per instance ──
		// Top-level int "cores_per_instance": 0 = no limit. Применяется в
		// dota_launcher.cpp после спавна dota2.exe.
		if ( j.contains( "cores_per_instance" ) && j["cores_per_instance"].is_number_integer() )
			cfg.coresPerInstance = j["cores_per_instance"].get<int>();

		// ── Window tiler ──
		// Секция window_tile { layout, force_windowed, auto_on_launch, padding,
		// monitor_index }. Если секция отсутствует — defaults из FarmConfig.
		if ( j.contains( "window_tile" ) && j["window_tile"].is_object() )
		{
			auto& wt = j["window_tile"];
			if ( wt.contains( "layout" ) )           cfg.tileLayout = wt["layout"].get<std::string>();
			if ( wt.contains( "force_windowed" ) )   cfg.tileForceWindowed = wt["force_windowed"].get<bool>();
			if ( wt.contains( "auto_on_launch" ) )   cfg.tileAutoOnLaunch = wt["auto_on_launch"].get<bool>();
			if ( wt.contains( "padding" ) )          cfg.tilePadding = wt["padding"].get<int>();
			if ( wt.contains( "monitor_index" ) )    cfg.tileMonitorIndex = wt["monitor_index"].get<int>();
		}

		// ── Pairing (5v5 self-play, см. match_pairing_fsm.h) ──
		// Если секции нет — defaults из FarmConfig::PairingConfig (enabled=false).
		if ( j.contains( "pairing" ) && j["pairing"].is_object() )
		{
			auto& pr = j["pairing"];
			auto& dst = cfg.pairing;
			if ( pr.contains( "enabled" ) )                  dst.enabled = pr["enabled"].get<bool>();
			if ( pr.contains( "transport" ) )                dst.transport = pr["transport"].get<std::string>();
			if ( pr.contains( "role" ) )                     dst.role = pr["role"].get<std::string>();
			if ( pr.contains( "pair_secret" ) )              dst.pairSecret = pr["pair_secret"].get<std::string>();
			if ( pr.contains( "master_ip" ) )                dst.masterIp = pr["master_ip"].get<std::string>();
			if ( pr.contains( "ipc_port" ) )                 dst.ipcPort = (uint16_t)pr["ipc_port"].get<int>();
			if ( pr.contains( "user_id" ) )                  dst.userId = pr["user_id"].get<std::string>();
			if ( pr.contains( "user_auth_token" ) )          dst.userAuthToken = pr["user_auth_token"].get<std::string>();
			if ( pr.contains( "pair_id" ) )                  dst.pairId = pr["pair_id"].get<std::string>();
			if ( pr.contains( "relay_host" ) )               dst.relayHost = pr["relay_host"].get<std::string>();
			if ( pr.contains( "relay_port" ) )               dst.relayPort = (uint16_t)pr["relay_port"].get<int>();
			// Получить user_id + user_auth_token: связаться с admin'ом relay'я,
			// получить выданный token (формат: <user_id> + <32-char hex>),
			// вписать сюда. При transport=relay они обязательны — Init() откажет
			// стартовать pairing если они пустые.
			if ( pr.contains( "match_sync_timeout_s" ) )     dst.matchSyncTimeoutS = pr["match_sync_timeout_s"].get<int>();
			if ( pr.contains( "max_consecutive_cancels" ) )  dst.maxConsecutiveCancels = pr["max_consecutive_cancels"].get<int>();
			if ( pr.contains( "post_game_winner_grace_s" ) ) dst.postGameWinnerGraceS = pr["post_game_winner_grace_s"].get<int>();
			if ( pr.contains( "uxV2" ) )                     dst.uxV2 = pr["uxV2"].get<bool>();
		}

		if ( j.contains( "team_strategy_mode" ) && j["team_strategy_mode"].is_string() )
			cfg.teamStrategyMode = j["team_strategy_mode"].get<std::string>();

		// ── Minifier (см. docs/MINIFIER_RESEARCH.md) ──
		// Если секции нет — defaults из MinifierConfig{} (enabled=false).
		if ( j.contains( "minifier" ) && j["minifier"].is_object() )
		{
			auto& mn = j["minifier"];
			auto& dst = cfg.minifier;
			if ( mn.contains( "enabled" ) )                dst.enabled = mn["enabled"].get<bool>();
			if ( mn.contains( "apply_launch_options" ) )   dst.applyLaunchOptions = mn["apply_launch_options"].get<bool>();
			if ( mn.contains( "apply_autoexec" ) )         dst.applyAutoexec = mn["apply_autoexec"].get<bool>();
			if ( mn.contains( "apply_video_txt" ) )        dst.applyVideoTxt = mn["apply_video_txt"].get<bool>();
			// VPK_DISABLED 2026-05-17: kill-switch. VPK patches убраны полностью
			// после повторной gameinfo.gi регрессии у юзеров. Конфиг читаем для
			// отладки (увидим если юзер пытается включить через config), но в
			// struct ставим false независимо. Defensive cleanup в orchestrator
			// Init сносит leftover pak'и от старых сборок безусловно.
			bool _userTriedVpk = false;
			if ( mn.contains( "apply_vpk_patches" ) )
				_userTriedVpk = mn["apply_vpk_patches"].get<bool>();
			dst.applyVpkPatches      = false;
			dst.vpkAttemptedFromConfig = _userTriedVpk;
			// vpk_fix_launch_options тоже принудительно off.
			dst.vpkFixLaunchOptions  = false;
			if ( mn.contains( "apply_d3d_hook" ) )         dst.applyD3dHook = mn["apply_d3d_hook"].get<bool>();
			if ( mn.contains( "fps_max" ) )                dst.fpsMax = mn["fps_max"].get<int>();
			if ( mn.contains( "resolution_width" ) )       dst.resolutionWidth = mn["resolution_width"].get<int>();
			if ( mn.contains( "resolution_height" ) )      dst.resolutionHeight = mn["resolution_height"].get<int>();

			// Bundle G2 — Egezenn/dota2-minify VPK integration
			if ( mn.contains( "vpk_preset" ) )             dst.vpkPreset = mn["vpk_preset"].get<std::string>();
			// Bundle H — predпочитаем PyInstaller standalone .exe; pythonExe = legacy fallback
			if ( mn.contains( "wrapper_exe" ) )            dst.wrapperExe = mn["wrapper_exe"].get<std::string>();
			if ( mn.contains( "python_exe" ) )             dst.pythonExe = mn["python_exe"].get<std::string>();
			if ( mn.contains( "wrapper_script" ) )         dst.wrapperScript = mn["wrapper_script"].get<std::string>();
			// vpk_fix_launch_options — VPK_DISABLED, читать не нужно (force OFF выше).
			if ( mn.contains( "vpk_subprocess_timeout_ms" ) ) dst.vpkSubprocessTimeoutMs = mn["vpk_subprocess_timeout_ms"].get<int>();

			// Bundle RAM — render backend
			if ( mn.contains( "render_backend" ) )
				dst.renderBackend = mn["render_backend"].get<std::string>();

			// Bundle MEMRED (2026-05-16) — memreduct-style system-wide reclaim.
			// Заменяет legacy periodic_ws_trim / ws_trim_interval_s.
			// Migration: если новой секции "mem_reclaim" нет, но старые ключи
			// присутствуют — мигрируем enabled/interval. EmptyAllWorkingSets
			// при миграции остаётся OFF (явный фикс stutter'а — см. план).
			if ( mn.contains( "mem_reclaim" ) && mn["mem_reclaim"].is_object() )
			{
				auto& mr = mn["mem_reclaim"];
				if ( mr.contains( "enabled" ) )
					dst.memReclaimEnabled = mr["enabled"].get<bool>();
				if ( mr.contains( "interval_s" ) )
					dst.memReclaimIntervalS = mr["interval_s"].get<int>();
				if ( mr.contains( "auto_threshold_pct" ) )
					dst.memReclaimAutoThreshold = mr["auto_threshold_pct"].get<int>();
				if ( mr.contains( "purge_standby" ) )
					dst.memReclaimPurgeStandby = mr["purge_standby"].get<bool>();
				if ( mr.contains( "purge_low_prio_standby" ) )
					dst.memReclaimPurgeLowPrioStandby = mr["purge_low_prio_standby"].get<bool>();
				if ( mr.contains( "flush_modified" ) )
					dst.memReclaimFlushModified = mr["flush_modified"].get<bool>();
				if ( mr.contains( "combine_pages" ) )
					dst.memReclaimCombinePages = mr["combine_pages"].get<bool>();
				if ( mr.contains( "empty_all_working_sets" ) )
					dst.memReclaimEmptyAllWorkingSets = mr["empty_all_working_sets"].get<bool>();
			}
			else if ( mn.contains( "periodic_ws_trim" ) || mn.contains( "ws_trim_interval_s" ) )
			{
				// Legacy migration path. Только enabled + interval.
				if ( mn.contains( "periodic_ws_trim" ) )
					dst.memReclaimEnabled = mn["periodic_ws_trim"].get<bool>();
				if ( mn.contains( "ws_trim_interval_s" ) )
					dst.memReclaimIntervalS = mn["ws_trim_interval_s"].get<int>();
				dst.memReclaimEmptyAllWorkingSets = false;  // hard OFF
				dst.memReclaimMigratedFromLegacy  = true;
			}
		}

		// ── Crash recovery (см. tools/dota2/RECONNECT_DESIGN.md §4) ──
		// Если секции нет — defaults из CrashRecoveryConfig{} (всё enabled).
		if ( j.contains( "crash_recovery" ) && j["crash_recovery"].is_object() )
		{
			auto& cr = j["crash_recovery"];
			auto& dst = cfg.crashRecovery;
			if ( cr.contains( "enabled" ) )                     dst.enabled = cr["enabled"].get<bool>();
			if ( cr.contains( "heartbeat_suspect_s" ) )         dst.heartbeatSuspectS = cr["heartbeat_suspect_s"].get<int>();
			if ( cr.contains( "heartbeat_confirm_s" ) )         dst.heartbeatConfirmS = cr["heartbeat_confirm_s"].get<int>();
			if ( cr.contains( "dump_watch_enabled" ) )          dst.dumpWatchEnabled = cr["dump_watch_enabled"].get<bool>();
			if ( cr.contains( "max_reconnects_per_match" ) )    dst.maxReconnectsPerMatch = cr["max_reconnects_per_match"].get<int>();
			if ( cr.contains( "max_crashes_per_window" ) )      dst.maxCrashesPerWindow = cr["max_crashes_per_window"].get<int>();
			if ( cr.contains( "crash_window_min" ) )            dst.crashWindowMin = cr["crash_window_min"].get<int>();
			if ( cr.contains( "reconnect_cooldown_s" ) )        dst.reconnectCooldownS = cr["reconnect_cooldown_s"].get<int>();
			if ( cr.contains( "loading_state_grace_s" ) )       dst.loadingStateGraceS = cr["loading_state_grace_s"].get<int>();
			if ( cr.contains( "respoof_hwid_on_relaunch" ) )    dst.respoofHwidOnRelaunch = cr["respoof_hwid_on_relaunch"].get<bool>();
			if ( cr.contains( "steam_relaunch_enabled" ) )      dst.steamRelaunchEnabled = cr["steam_relaunch_enabled"].get<bool>();
			if ( cr.contains( "gc_ready_timeout_s" ) )          dst.gcReadyTimeoutS = cr["gc_ready_timeout_s"].get<int>();
			if ( cr.contains( "max_steam_relaunches_per_match" ) ) dst.maxSteamRelaunchesPerMatch = cr["max_steam_relaunches_per_match"].get<int>();
		}

		return true;
	}
	catch ( ... )
	{
		return false;
	}
}

bool SaveAccounts( const std::string& path, const FarmConfig& cfg )
{
	try
	{
		json j;
		j["steam_exe"] = cfg.steamExe;
		j["dota_app_id"] = cfg.dotaAppId;
		j["launch_args"] = cfg.launchArgs;

		json instances = json::array();
		for ( const auto& acc : cfg.accounts )
		{
			json inst;
			inst["ipc_name"] = acc.ipcName;
			inst["login"] = acc.login;
			inst["steam_id"] = acc.steamId;
			inst["persona"] = acc.persona;
			inst["userchooser"] = acc.userchooser;
			inst["enabled"] = acc.enabled;
			inst["proxy"] = NormalizeProxyString( acc.proxy );
			inst["proxy_enabled"] = acc.proxyEnabled;
			inst["hwid_spoof_enabled"] = acc.hwidSpoofEnabled;
			if ( !acc.password.empty() )
				inst["password"] = acc.password;
			instances.push_back( inst );
		}
		j["instances"] = instances;

		// Seal plain JSON under HWID-derived AES-256-GCM key.
		// LEGACY_PLAIN: previous `std::ofstream file(path); file << j.dump(4);`
		// path is no longer human-readable on disk.
		std::string serialized = j.dump( 4 );
		return sealed_file::SealFile( path, serialized );
	}
	catch ( ... )
	{
		return false;
	}
}

bool SaveFarmBoolSetting( const std::string& path, const std::string& key, bool value )
{
	json j;
	{
		std::ifstream in( path );
		if ( in.is_open() )
		{
			try { in >> j; }
			catch ( ... ) { return false; }
		}
		else
		{
			j = json::object();
		}
	}
	if ( !j.is_object() ) return false;
	j[key] = value;

	std::ofstream out( path );
	if ( !out.is_open() ) return false;
	out << j.dump( 4 );
	return true;
}

bool SaveFarmStringSetting( const std::string& path, const std::string& key, const std::string& value )
{
	json j;
	{
		std::ifstream in( path );
		if ( in.is_open() )
		{
			try { in >> j; }
			catch ( ... ) { return false; }
		}
		else
		{
			j = json::object();
		}
	}
	if ( !j.is_object() ) return false;
	j[key] = value;

	std::ofstream out( path );
	if ( !out.is_open() ) return false;
	out << j.dump( 4 );
	return true;
}

bool SaveFarmIntSetting( const std::string& path, const std::string& key, int value )
{
	json j;
	{
		std::ifstream in( path );
		if ( in.is_open() )
		{
			try { in >> j; }
			catch ( ... ) { return false; }
		}
		else
		{
			j = json::object();
		}
	}
	if ( !j.is_object() ) return false;
	j[key] = value;

	std::ofstream out( path );
	if ( !out.is_open() ) return false;
	out << j.dump( 4 );
	return true;
}

// ── Atomic RMW helpers (паттерн из strategy_writer.cpp) ──────────────
// LockFileEx над выделенным lock-файлом рядом с farm.json. Сам JSON
// заменяется через MoveFileExA → лочить его handle бесполезно, поэтому
// держим отдельный <path>.lock.
namespace
{

class FileLock
{
public:
	FileLock( const std::string& path )
	{
		m_h = CreateFileA( path.c_str(), GENERIC_READ | GENERIC_WRITE,
			FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
			FILE_ATTRIBUTE_NORMAL, nullptr );
		if ( m_h == INVALID_HANDLE_VALUE ) return;

		OVERLAPPED ovl{};
		if ( !LockFileEx( m_h, LOCKFILE_EXCLUSIVE_LOCK, 0,
				MAXDWORD, MAXDWORD, &ovl ) )
		{
			CloseHandle( m_h );
			m_h = INVALID_HANDLE_VALUE;
		}
	}

	~FileLock()
	{
		if ( m_h != INVALID_HANDLE_VALUE )
		{
			OVERLAPPED ovl{};
			UnlockFileEx( m_h, 0, MAXDWORD, MAXDWORD, &ovl );
			CloseHandle( m_h );
		}
	}

	bool ok() const { return m_h != INVALID_HANDLE_VALUE; }

	FileLock( const FileLock& ) = delete;
	FileLock& operator=( const FileLock& ) = delete;

private:
	HANDLE m_h = INVALID_HANDLE_VALUE;
};

static bool WriteAtomicFile( const std::string& finalPath,
                             const std::string& tmpPath,
                             const std::string& body )
{
	{
		std::ofstream f( tmpPath, std::ios::binary | std::ios::trunc );
		if ( !f ) return false;
		f << body;
		f.flush();
		if ( !f ) return false;
	}
	// Force write-back cache to disk before rename. Без этого на VM с writeback
	// cache данные могут не дойти до сектора до того как MoveFileEx обновит
	// журнал FS → power-loss даёт переименованный, но zero-length файл.
	{
		HANDLE h = CreateFileA( tmpPath.c_str(), GENERIC_WRITE,
			FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
			OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr );
		if ( h != INVALID_HANDLE_VALUE )
		{
			FlushFileBuffers( h );
			CloseHandle( h );
		}
	}
	if ( !MoveFileExA( tmpPath.c_str(), finalPath.c_str(),
			MOVEFILE_REPLACE_EXISTING ) )
	{
		// Explicit cleanup: иначе orphan .tmp накапливается при sharing
		// violation (AV-сканер локает) до следующего успешного save.
		DeleteFileA( tmpPath.c_str() );
		return false;
	}
	return true;
}

} // anonymous namespace

bool SavePairingConfigAtomic( const std::string& farmJsonPath,
                              const FarmConfig::PairingConfig& cfg )
{
	FileLock lk( farmJsonPath + ".lock" );
	if ( !lk.ok() ) return false;

	// RMW: читаем существующий farm.json в json, чтобы НЕ потерять прочие
	// секции (heroes, accounts, crash_recovery, minifier и т.д.). Если файла
	// нет — начинаем с пустого object'а.
	json j = json::object();
	{
		std::ifstream in( farmJsonPath );
		if ( in.is_open() )
		{
			try
			{
				in >> j;
				if ( !j.is_object() ) j = json::object();
			}
			catch ( ... )
			{
				// Битый файл — отказываемся писать поверх, чтобы не уничтожить
				// state юзера. Caller увидит false и может сделать backup/restore.
				return false;
			}
		}
	}

	json& pr = j["pairing"];
	pr["enabled"]                  = cfg.enabled;
	pr["transport"]                = cfg.transport;
	pr["role"]                     = cfg.role;
	pr["user_id"]                  = cfg.userId;
	pr["user_auth_token"]          = cfg.userAuthToken;
	pr["pair_id"]                  = cfg.pairId;
	pr["pair_secret"]              = cfg.pairSecret;
	pr["master_ip"]                = cfg.masterIp;
	pr["ipc_port"]                 = (int)cfg.ipcPort;
	pr["relay_host"]               = cfg.relayHost;
	pr["relay_port"]               = (int)cfg.relayPort;
	pr["match_sync_timeout_s"]     = cfg.matchSyncTimeoutS;
	pr["max_consecutive_cancels"]  = cfg.maxConsecutiveCancels;
	pr["post_game_winner_grace_s"] = cfg.postGameWinnerGraceS;
	pr["uxV2"]                     = cfg.uxV2;

	std::string body;
	try
	{
		body = j.dump( 4 );
	}
	catch ( ... )
	{
		return false;
	}

	return WriteAtomicFile( farmJsonPath, farmJsonPath + ".tmp", body );
}

void ApplyPairCode( const pair_code::Decoded& src,
                    FarmConfig::PairingConfig& dst )
{
	dst.transport     = "relay";
	dst.relayHost     = src.relayHost;
	dst.relayPort     = src.relayPort;
	dst.userId        = src.userId;
	dst.userAuthToken = src.userAuthToken;
	dst.pairId        = src.pairId;
	dst.pairSecret    = src.pairSecret;
	dst.role          = ( src.roleHint == "M" ) ? "master" : "slave";
	// НЕ трогаем: enabled, masterIp, ipcPort, matchSyncTimeoutS,
	//              maxConsecutiveCancels, postGameWinnerGraceS, uxV2.
	// Caller (Orchestrator::ApplyPairCodeAndReinit) сам выставит
	// enabled=true и uxV2=true.
}

std::vector<std::pair<std::string, VdfAccount>> ParseLoginUsersVdf( const std::string& steamDir )
{
	std::vector<std::pair<std::string, VdfAccount>> result;

	std::string path = steamDir + "\\config\\loginusers.vdf";
	std::ifstream file( path );
	if ( !file.is_open() ) return result;

	// Simple line-by-line VDF parser (not full spec, but enough for loginusers.vdf)
	std::string line;
	uint64_t currentSteamId = 0;
	std::string currentLogin;
	std::string currentPersona;

	auto extractQuoted = []( const std::string& s ) -> std::string
	{
		auto first = s.find( '"' );
		if ( first == std::string::npos ) return {};
		auto second = s.find( '"', first + 1 );
		if ( second == std::string::npos ) return {};
		// Check for second value (key-value pair: "key" "value")
		auto third = s.find( '"', second + 1 );
		if ( third == std::string::npos ) return s.substr( first + 1, second - first - 1 );
		auto fourth = s.find( '"', third + 1 );
		if ( fourth == std::string::npos ) return s.substr( first + 1, second - first - 1 );
		return s.substr( third + 1, fourth - third - 1 );
	};

	auto extractKey = []( const std::string& s ) -> std::string
	{
		auto first = s.find( '"' );
		if ( first == std::string::npos ) return {};
		auto second = s.find( '"', first + 1 );
		if ( second == std::string::npos ) return {};
		return s.substr( first + 1, second - first - 1 );
	};

	bool inUser = false;
	int braceDepth = 0;

	while ( std::getline( file, line ) )
	{
		std::string trimmed = line;
		size_t start = trimmed.find_first_not_of( " \t" );
		if ( start != std::string::npos ) trimmed = trimmed.substr( start );

		if ( trimmed == "{" )
		{
			braceDepth++;
			if ( braceDepth == 2 ) inUser = true;
			continue;
		}
		if ( trimmed == "}" )
		{
			if ( braceDepth == 2 && inUser && currentSteamId != 0 && !currentLogin.empty() )
			{
				VdfAccount acc;
				acc.steamId = currentSteamId;
				acc.persona = currentPersona;
				result.emplace_back( currentLogin, acc );
				currentSteamId = 0;
				currentLogin.clear();
				currentPersona.clear();
			}
			inUser = ( braceDepth > 2 );
			braceDepth--;
			continue;
		}

		if ( braceDepth == 1 )
		{
			// Top-level key = steam_id (e.g. "76561198725850781")
			std::string key = extractKey( trimmed );
			if ( key.size() > 10 )
				currentSteamId = strtoull( key.c_str(), nullptr, 10 );
		}
		else if ( braceDepth == 2 && inUser )
		{
			std::string key = extractKey( trimmed );
			std::string val = extractQuoted( trimmed );
			if ( key == "AccountName" )  currentLogin = val;
			if ( key == "PersonaName" )  currentPersona = val;
		}
	}

	return result;
}

} // namespace config
