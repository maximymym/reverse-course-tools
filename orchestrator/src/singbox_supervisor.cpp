#include "singbox_supervisor.h"

// Winsock / IPHelper headers: winsock2.h must precede windows.h include chain.
// singbox_supervisor.h transitively includes <Windows.h>, so we guard with
// WIN32_LEAN_AND_MEAN (set in CMake) — and include winsock2 FIRST here.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>

#include <json.hpp>

#include <cstdarg>
#include <cstdio>
#include <fstream>
#include <regex>
#include <sstream>
#include <vector>

#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ws2_32.lib")

using json = nlohmann::json;

// ── URL parser: socks5://user:pass@host:port → fields ─────────────
// Поддерживаем "socks5://" и "http://". Пароль может содержать любой символ
// кроме '@'. URL-encoded не поддерживается (не встречается в наших конфигах).
struct ProxyUrl
{
	std::string scheme;     // "socks5" | "http"
	std::string host;
	uint16_t    port = 0;
	std::string user;
	std::string pass;
	bool        valid = false;
};

static ProxyUrl ParseProxyUrl( const std::string& url )
{
	ProxyUrl out;
	size_t schemeEnd = url.find( "://" );
	if ( schemeEnd == std::string::npos )
		return out;

	out.scheme = url.substr( 0, schemeEnd );
	std::string rest = url.substr( schemeEnd + 3 );

	// user:pass@host:port — if '@' present, split
	size_t at = rest.rfind( '@' );
	std::string authority;
	std::string userinfo;
	if ( at != std::string::npos )
	{
		userinfo = rest.substr( 0, at );
		authority = rest.substr( at + 1 );
	}
	else
	{
		authority = rest;
	}

	if ( !userinfo.empty() )
	{
		size_t colon = userinfo.find( ':' );
		if ( colon != std::string::npos )
		{
			out.user = userinfo.substr( 0, colon );
			out.pass = userinfo.substr( colon + 1 );
		}
		else
		{
			out.user = userinfo;
		}
	}

	// authority = host:port
	size_t colon = authority.rfind( ':' );
	if ( colon == std::string::npos )
		return out;
	out.host = authority.substr( 0, colon );
	try
	{
		int p = std::stoi( authority.substr( colon + 1 ) );
		if ( p <= 0 || p > 65535 )
			return out;
		out.port = (uint16_t)p;
	}
	catch ( ... )
	{
		return out;
	}

	if ( out.host.empty() ) return out;
	out.valid = true;
	return out;
}

// ── Escape backslashes для process_path_regex (Go regex, backslash = '\\\\\\\\' в JSON) ──
// process_path_regex интерпретируется как Go regex2. В Windows пути сам regex
// должен иметь escaped backslash: \\\\, что в JSON string кодируется как \\\\\\\\
// (4 backslash = 2 в регексе = 1 литеральный '\'). nlohmann::json сделает \\
// автоматически когда сериализует строку — нам надо передать в string 2 '\'.
static std::string MakeBotSteamRegex( int idx )
{
	char buf[128];
	// Regex matches: "...\\BotSteam\\<idx>\\..."
	// В C++ строке '\\\\' = два '\', что regex трактует как '\'.
	snprintf( buf, sizeof( buf ), "(?i)BotSteam\\\\%d\\\\", idx );
	return buf;
}

// Matches BotDota paths: hardlinked dota2.exe living at
// <volume>:/BotDota/<idx>/game/bin/win64/dota2.exe. Regex ищет "BotDota/<idx>/"
// в процессном path. Case-insensitive т.к. Windows paths бывают разного регистра.
static std::string MakeBotDotaRegex( int idx )
{
	char buf[128];
	snprintf( buf, sizeof( buf ), "(?i)BotDota\\\\%d\\\\", idx );
	return buf;
}

SingboxSupervisor::SingboxSupervisor() = default;

// Detect physical NIC — первый UP IPv4 adapter с gateway, не VPN/TUN/loopback.
// Используется для auto_detect_interface: false — чтобы sing-box TUN direct
// outbound не выбрал AmneziaVPN adapter как default (фейл предыдущей итерации).
std::string SingboxSupervisor::DetectDefaultInterface()
{
	if ( !m_defaultInterface.empty() )
		return m_defaultInterface; // cached

	ULONG bufLen = 0;
	GetAdaptersAddresses( AF_INET, GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_SKIP_MULTICAST,
		nullptr, nullptr, &bufLen );
	if ( bufLen == 0 )
		return {};

	std::vector<BYTE> buf( bufLen );
	auto* adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>( buf.data() );
	if ( GetAdaptersAddresses( AF_INET, GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_SKIP_MULTICAST,
		nullptr, adapters, &bufLen ) != NO_ERROR )
		return {};

	auto containsCaseInsensitive = []( const std::wstring& haystack, const wchar_t* needle ) -> bool
	{
		std::wstring lc = haystack;
		for ( auto& c : lc ) c = (wchar_t)towlower( c );
		std::wstring n = needle;
		for ( auto& c : n ) c = (wchar_t)towlower( c );
		return lc.find( n ) != std::wstring::npos;
	};

	// Исключаем явные non-physical интерфейсы.
	const wchar_t* excludeSubs[] = {
		L"tun", L"tap", L"amnezia", L"wireguard", L"tailscale",
		L"openvpn", L"expressvpn", L"nordvpn", L"loopback",
		L"virtualbox", L"vmware", L"hyper-v", L"bluetooth", L"wintun",
		L"dotafarm-tun" // наш же TUN если orchestrator перезапускается
	};

	for ( auto a = adapters; a; a = a->Next )
	{
		if ( a->OperStatus != IfOperStatusUp ) continue;
		if ( a->IfType == IF_TYPE_SOFTWARE_LOOPBACK ) continue;
		if ( a->IfType == IF_TYPE_TUNNEL ) continue;

		std::wstring friendly = a->FriendlyName ? a->FriendlyName : L"";
		std::wstring desc = a->Description ? a->Description : L"";

		bool excluded = false;
		for ( auto sub : excludeSubs )
		{
			if ( containsCaseInsensitive( friendly, sub ) ||
				 containsCaseInsensitive( desc, sub ) )
			{
				excluded = true; break;
			}
		}
		if ( excluded ) continue;

		// Должен иметь gateway — иначе это non-routable (link-local)
		if ( !a->FirstGatewayAddress ) continue;

		// Получили candidate — конвертим FriendlyName в UTF-8 для sing-box JSON
		if ( friendly.empty() ) continue;
		int needed = WideCharToMultiByte( CP_UTF8, 0, friendly.c_str(), -1,
			nullptr, 0, nullptr, nullptr );
		if ( needed <= 0 ) continue;
		std::string out( needed, 0 );
		WideCharToMultiByte( CP_UTF8, 0, friendly.c_str(), -1,
			out.data(), needed, nullptr, nullptr );
		if ( !out.empty() && out.back() == 0 ) out.pop_back();

		m_defaultInterface = out;
		return out;
	}
	return {};
}

SingboxSupervisor::~SingboxSupervisor()
{
	Stop();
}

void SingboxSupervisor::SetLogger( LogFn fn )
{
	m_logger = std::move( fn );
}

void SingboxSupervisor::Log( const char* fmt, ... )
{
	if ( !m_logger ) return;
	char buf[512];
	va_list args;
	va_start( args, fmt );
	vsnprintf( buf, sizeof( buf ), fmt, args );
	va_end( args );
	m_logger( buf );
}

bool SingboxSupervisor::BuildConfig( const std::vector<AccountConfig>& accounts,
	const std::string& workDir,
	std::string& outJson,
	size_t& outOutbounds )
{
	json root;
	root["log"] = {
		{ "level", "info" },
		{ "output", workDir + "\\singbox.log" },
		{ "timestamp", true }
	};

	// ── TUN inbound ────────────────────────────────────────────
	// auto_route:true — sing-box становится системным default route (добавляет
	// routes 1.0.0.0/8..224.0.0.0/8 через TUN). process_path_regex матчит
	// Steam-процессы → outbound acc<N>; остальное → outbound "direct", который
	// благодаря auto_detect_interface обходит свой собственный TUN и идёт
	// через реальный NIC / сохранённую оригинальную default gateway.
	// Таким образом не-Steam трафик (browser, Tailscale, AmneziaVPN) работает
	// нормально, а Steam-paths правильно обслуживаются прокси.
	json tun = {
		{ "type", "tun" },
		{ "tag", "tun-in" },
		{ "interface_name", "dotafarm-tun" },
		{ "address", json::array( { "198.18.0.1/30", "fdfe:dcba:9876::1/126" } ) },
		{ "mtu", 1500 },
		{ "auto_route", true },
		{ "strict_route", false },
		{ "endpoint_independent_nat", true },
		{ "stack", "system" }
	};
	root["inbounds"] = json::array( { tun } );

	// ── Outbounds ─────────────────────────────────────────────
	json outbounds = json::array();
	size_t numProxied = 0;

	// DNS outbound (через proxy — иначе DNS leak).
	// Добавляем fake direct тоже для route fallback.
	for ( size_t i = 0; i < accounts.size(); ++i )
	{
		const auto& acc = accounts[i];
		// Per-account proxy toggle: если выключен — не создаём outbound для
		// этого аккаунта, его трафик пойдёт через `direct` default rule (тот же
		// путь, что и для аккаунтов без proxy строки вообще).
		if ( acc.proxy.empty() || !acc.proxyEnabled )
		{
			continue;
		}

		ProxyUrl pu = ParseProxyUrl( acc.proxy );
		if ( !pu.valid )
		{
			m_lastError = "invalid proxy url for account " + std::to_string( i ) + ": " + acc.proxy;
			Log( "[singbox] %s", m_lastError.c_str() );
			continue;
		}

		std::string tag = "acc" + std::to_string( i );

		if ( pu.scheme == "socks5" || pu.scheme == "socks" )
		{
			json ob = {
				{ "type", "socks" },
				{ "tag", tag },
				{ "server", pu.host },
				{ "server_port", pu.port },
				{ "version", "5" }
			};
			if ( !pu.user.empty() )
			{
				ob["username"] = pu.user;
				ob["password"] = pu.pass;
			}
			outbounds.push_back( ob );
			++numProxied;
		}
		else if ( pu.scheme == "http" || pu.scheme == "https" )
		{
			json ob = {
				{ "type", "http" },
				{ "tag", tag },
				{ "server", pu.host },
				{ "server_port", pu.port }
			};
			if ( !pu.user.empty() )
			{
				ob["username"] = pu.user;
				ob["password"] = pu.pass;
			}
			if ( pu.scheme == "https" )
				ob["tls"] = { { "enabled", true } };
			outbounds.push_back( ob );
			++numProxied;
		}
		else
		{
			Log( "[singbox] unsupported scheme '%s' for account %zu — skipped",
				pu.scheme.c_str(), i );
		}
	}

	// direct (fallback для всех не-Steam процессов).
	// bind_interface: явно привязываем direct-socket к physical NIC, чтобы
	// auto-detect не выбрал сам TUN или VPN-адаптер. Без этого — предыдущая
	// итерация сломала интернет машины (VPN interface стал default).
	{
		json direct = {
			{ "type", "direct" },
			{ "tag", "direct" }
		};
		if ( !m_defaultInterface.empty() )
			direct["bind_interface"] = m_defaultInterface;
		outbounds.push_back( direct );
	}

	// block — для явных блоков
	outbounds.push_back( {
		{ "type", "block" },
		{ "tag", "block" }
	} );

	root["outbounds"] = outbounds;

	// ── DNS ────────────────────────────────────────────────────
	// Если есть хоть один outbound-прокси — пускаем DNS через первый из них,
	// чтобы не было DNS leak'а (real IP резолверу и в итоге плейн в payload).
	// Fallback — системный DNS через direct.
	{
		json dnsServers = json::array();
		if ( numProxied > 0 )
		{
			// Найти первый acc тэг
			for ( size_t i = 0; i < accounts.size(); ++i )
			{
				if ( accounts[i].proxy.empty() ) continue;
				ProxyUrl pu = ParseProxyUrl( accounts[i].proxy );
				if ( !pu.valid ) continue;
				std::string tag = "acc" + std::to_string( i );
				dnsServers.push_back( {
					{ "tag", "proxy_dns" },
					{ "address", "tcp://1.1.1.1" },
					{ "detour", tag }
				} );
				break;
			}
		}
		dnsServers.push_back( {
			{ "tag", "direct_dns" },
			{ "address", "8.8.8.8" },
			{ "detour", "direct" }
		} );

		json dnsRules = json::array();
		// Для Steam+Dota процессов каждого аккаунта — DNS через per-account proxy.
		// Regex объединяет BotSteam\<N>\ и BotDota\<N>\ — Steam и Dota обслуживаются
		// одним outbound'ом, DNS тоже через тот же detour (чтобы не было DNS leak).
		for ( size_t i = 0; i < accounts.size(); ++i )
		{
			if ( accounts[i].proxy.empty() ) continue;
			ProxyUrl pu = ParseProxyUrl( accounts[i].proxy );
			if ( !pu.valid ) continue;
			dnsRules.push_back( {
				{ "process_path_regex", json::array( {
					MakeBotSteamRegex( (int)i ),
					MakeBotDotaRegex( (int)i )
				} ) },
				{ "server", "proxy_dns" }
			} );
		}

		root["dns"] = {
			{ "servers", dnsServers },
			{ "rules", dnsRules },
			{ "strategy", "ipv4_only" },
			{ "final", "direct_dns" }
		};
	}

	// ── Route rules ───────────────────────────────────────────
	// process_path_regex per-account объединяет Steam+Dota paths:
	//   BotSteam\<N>\.* OR BotDota\<N>\.*  → outbound acc<N>
	// Оба пути уникальны per-account благодаря hardlink'ам (Phase A/B).
	// Final: direct — весь остальной трафик через physical NIC (bind_interface).
	json routeRules = json::array();
	for ( size_t i = 0; i < accounts.size(); ++i )
	{
		if ( accounts[i].proxy.empty() ) continue;
		ProxyUrl pu = ParseProxyUrl( accounts[i].proxy );
		if ( !pu.valid ) continue;
		std::string tag = "acc" + std::to_string( i );
		routeRules.push_back( {
			{ "process_path_regex", json::array( {
				MakeBotSteamRegex( (int)i ),
				MakeBotDotaRegex( (int)i )
			} ) },
			{ "outbound", tag }
		} );
	}

	// auto_detect_interface=false + default_interface — форсируем physical NIC
	// для любого socket не имеющего outbound-specific bind_interface. При VPN
	// активном это КРИТИЧНО: auto-detect выбрал бы VPN adapter → трафик
	// зацикливался бы внутри TUN → весь интернет машины ложился.
	json routeObj = {
		{ "rules", routeRules },
		{ "final", "direct" }
	};
	if ( !m_defaultInterface.empty() )
	{
		routeObj["auto_detect_interface"] = false;
		routeObj["default_interface"] = m_defaultInterface;
	}
	else
	{
		// Fallback — если NIC detection fail, auto_detect_interface=true это
		// risk, но иначе sing-box вообще не поднимется (нужен какой-то default).
		routeObj["auto_detect_interface"] = true;
	}
	root["route"] = routeObj;

	// ── Experimental (optional): ClashAPI для отладки ─────────
	// root["experimental"] = { { "clash_api", { { "external_controller", "127.0.0.1:9090" } } } };

	outJson = root.dump( 2 );
	outOutbounds = numProxied;
	return true;
}

bool SingboxSupervisor::Start( const std::vector<AccountConfig>& accounts,
	const std::string& exeDir,
	const std::string& workDir )
{
	if ( m_running.load() )
	{
		Log( "[singbox] already running — skip Start" );
		return true;
	}

	std::lock_guard<std::mutex> lock( m_mutex );

	m_exeDir = exeDir;
	m_workDir = workDir;
	m_accounts = accounts;

	// Путь к sing-box.exe: рядом с DotaFarm.exe
	m_singboxExePath = exeDir + "\\sing-box.exe";
	if ( GetFileAttributesA( m_singboxExePath.c_str() ) == INVALID_FILE_ATTRIBUTES )
	{
		m_lastError = "sing-box.exe not found next to DotaFarm.exe: " + m_singboxExePath;
		Log( "[singbox] %s", m_lastError.c_str() );
		return false;
	}

	// wintun.dll (не ошибка если нет — sing-box сам ругнётся, но логим)
	std::string wintunPath = exeDir + "\\wintun.dll";
	if ( GetFileAttributesA( wintunPath.c_str() ) == INVALID_FILE_ATTRIBUTES )
	{
		Log( "[singbox] WARN: wintun.dll not found next to sing-box.exe — TUN will fail" );
	}

	// Обеспечиваем workDir
	CreateDirectoryA( workDir.c_str(), nullptr );
	m_configPath = workDir + "\\singbox.json";
	m_logPath    = workDir + "\\singbox.log";

	// КРИТИЧНО: убираем leftover routes от предыдущей сессии ДО build config.
	// Если current start fails (no proxies / spawn error) — leftover split-default
	// routes не будут перехватывать трафик → internet не уходит в мёртвый TUN.
	CleanupStaleRoutes();

	// Detect physical NIC для bind_interface в direct outbound.
	std::string nic = DetectDefaultInterface();
	if ( !nic.empty() )
		Log( "[singbox] physical NIC detected: %s", nic.c_str() );
	else
		Log( "[singbox] WARN: no physical NIC detected — falling back to auto_detect_interface" );

	std::string configJson;
	if ( !BuildConfig( accounts, workDir, configJson, m_outbounds ) )
		return false;

	// Если нет ни одного proxy-outbound — sing-box бесполезен, не стартуем.
	if ( m_outbounds == 0 )
	{
		m_lastError = "no accounts with proxy set — sing-box not started";
		Log( "[singbox] %s", m_lastError.c_str() );
		return false;
	}

	{
		std::ofstream f( m_configPath, std::ios::binary | std::ios::trunc );
		if ( !f.is_open() )
		{
			m_lastError = "failed to write config: " + m_configPath;
			Log( "[singbox] %s", m_lastError.c_str() );
			return false;
		}
		f << configJson;
	}

	Log( "[singbox] config written: %s (outbounds=%zu)", m_configPath.c_str(), m_outbounds );

	if ( !SpawnProcess() )
		return false;

	m_running = true;
	return true;
}

bool SingboxSupervisor::SpawnProcess()
{
	// JobObject — sing-box умирает когда orchestrator умирает (даже при kill -9).
	m_jobObject = CreateJobObjectA( nullptr, nullptr );
	if ( !m_jobObject )
	{
		m_lastError = "CreateJobObject failed";
		return false;
	}

	JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
	info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
	SetInformationJobObject( m_jobObject, JobObjectExtendedLimitInformation, &info, sizeof( info ) );

	// Command line: sing-box.exe run -c <config>
	std::string cmdLine = "\"" + m_singboxExePath + "\" run -c \"" + m_configPath + "\"";

	STARTUPINFOA si{};
	si.cb = sizeof( si );
	si.dwFlags = STARTF_USESHOWWINDOW;
	si.wShowWindow = SW_HIDE;
	PROCESS_INFORMATION pi{};

	BOOL ok = CreateProcessA(
		nullptr,
		const_cast<char*>( cmdLine.c_str() ),
		nullptr, nullptr,
		FALSE,
		CREATE_SUSPENDED | CREATE_NO_WINDOW | CREATE_BREAKAWAY_FROM_JOB,
		nullptr,
		m_exeDir.c_str(),
		&si, &pi
	);

	if ( !ok )
	{
		DWORD err = GetLastError();
		char buf[128];
		snprintf( buf, sizeof( buf ), "CreateProcess sing-box failed: %lu", err );
		m_lastError = buf;
		Log( "[singbox] %s", m_lastError.c_str() );
		CloseHandle( m_jobObject );
		m_jobObject = nullptr;
		return false;
	}

	// Присваиваем job перед ResumeThread — чтобы уже первая инструкция
	// выполнилась под job (и если exit — kill cascade сработает).
	if ( !AssignProcessToJobObject( m_jobObject, pi.hProcess ) )
	{
		DWORD err = GetLastError();
		char buf[128];
		snprintf( buf, sizeof( buf ), "AssignProcessToJobObject failed: %lu", err );
		m_lastError = buf;
		Log( "[singbox] WARN: %s (continuing without job control)", m_lastError.c_str() );
		// Не фатально — sing-box запустится, но parent crash не убьёт его.
	}

	ResumeThread( pi.hThread );
	CloseHandle( pi.hThread );

	m_processHandle = pi.hProcess;
	m_pid = pi.dwProcessId;

	Log( "[singbox] spawned pid=%lu config=%s outbounds=%zu",
		m_pid, m_configPath.c_str(), m_outbounds );

	// Дать 2 сек на старт и проверить что процесс жив.
	for ( int i = 0; i < 20; ++i )
	{
		Sleep( 100 );
		DWORD ec = 0;
		if ( GetExitCodeProcess( m_processHandle, &ec ) && ec != STILL_ACTIVE )
		{
			char buf[128];
			snprintf( buf, sizeof( buf ), "sing-box exited early with code %lu (check singbox.log)", ec );
			m_lastError = buf;
			Log( "[singbox] %s", m_lastError.c_str() );
			CloseHandle( m_processHandle );
			m_processHandle = nullptr;
			m_pid = 0;
			CloseHandle( m_jobObject );
			m_jobObject = nullptr;
			return false;
		}
	}

	// Split-default routes: 0.0.0.0/1 + 128.0.0.0/1 через TUN. auto_route sing-box'а
	// добавляет только 0.0.0.0/0 на Windows, что имеет ту же длину префикса как
	// VPN gateways (WireGuard/Amnezia/OpenVPN тоже ставят /0). При одинаковой
	// длине префикса Windows выбирает по interface metric, которая у VPN может
	// быть ниже. Split-default `/1` короче по bitmask но длиннее по prefix match
	// → всегда выигрывает longest-prefix у /0, гарантируя что трафик идёт в TUN.
	{
		char cmd[256];
		DWORD exitCode = 0;
		STARTUPINFOA si2{}; si2.cb = sizeof( si2 );
		si2.dwFlags = STARTF_USESHOWWINDOW; si2.wShowWindow = SW_HIDE;
		PROCESS_INFORMATION pi2{};

		// Delete stale первым — если previous orchestrator не cleanup'нулся
		snprintf( cmd, sizeof( cmd ), "route DELETE 0.0.0.0 MASK 128.0.0.0" );
		if ( CreateProcessA( nullptr, cmd, nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
			nullptr, nullptr, &si2, &pi2 ) )
		{
			WaitForSingleObject( pi2.hProcess, 5000 );
			CloseHandle( pi2.hProcess ); CloseHandle( pi2.hThread );
		}
		snprintf( cmd, sizeof( cmd ), "route DELETE 128.0.0.0 MASK 128.0.0.0" );
		if ( CreateProcessA( nullptr, cmd, nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
			nullptr, nullptr, &si2, &pi2 ) )
		{
			WaitForSingleObject( pi2.hProcess, 5000 );
			CloseHandle( pi2.hProcess ); CloseHandle( pi2.hThread );
		}

		// Add split-default через TUN gateway 198.18.0.2 с metric 1.
		snprintf( cmd, sizeof( cmd ), "route ADD 0.0.0.0 MASK 128.0.0.0 198.18.0.2 METRIC 1" );
		if ( CreateProcessA( nullptr, cmd, nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
			nullptr, nullptr, &si2, &pi2 ) )
		{
			WaitForSingleObject( pi2.hProcess, 5000 );
			GetExitCodeProcess( pi2.hProcess, &exitCode );
			CloseHandle( pi2.hProcess ); CloseHandle( pi2.hThread );
			Log( "[singbox] route ADD 0.0.0.0/1 via TUN: exit=%lu", exitCode );
		}
		snprintf( cmd, sizeof( cmd ), "route ADD 128.0.0.0 MASK 128.0.0.0 198.18.0.2 METRIC 1" );
		if ( CreateProcessA( nullptr, cmd, nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
			nullptr, nullptr, &si2, &pi2 ) )
		{
			WaitForSingleObject( pi2.hProcess, 5000 );
			GetExitCodeProcess( pi2.hProcess, &exitCode );
			CloseHandle( pi2.hProcess ); CloseHandle( pi2.hThread );
			Log( "[singbox] route ADD 128.0.0.0/1 via TUN: exit=%lu", exitCode );
		}
	}

	m_lastError.clear();
	return true;
}

void SingboxSupervisor::CleanupStaleRoutes()
{
	const char* cleanupCmds[] = {
		"route DELETE 0.0.0.0 MASK 128.0.0.0",
		"route DELETE 128.0.0.0 MASK 128.0.0.0"
	};
	for ( auto c : cleanupCmds )
	{
		STARTUPINFOA si2{}; si2.cb = sizeof( si2 );
		si2.dwFlags = STARTF_USESHOWWINDOW; si2.wShowWindow = SW_HIDE;
		PROCESS_INFORMATION pi2{};
		char cmd[256];
		strncpy( cmd, c, sizeof( cmd ) );
		if ( CreateProcessA( nullptr, cmd, nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
			nullptr, nullptr, &si2, &pi2 ) )
		{
			WaitForSingleObject( pi2.hProcess, 3000 );
			CloseHandle( pi2.hProcess ); CloseHandle( pi2.hThread );
		}
	}
}

void SingboxSupervisor::Stop()
{
	std::lock_guard<std::mutex> lock( m_mutex );

	if ( !m_running.load() && !m_processHandle && !m_jobObject )
		return;

	if ( m_processHandle )
	{
		// Graceful: просто terminate. sing-box корректно снимет TUN адаптер.
		TerminateProcess( m_processHandle, 0 );
		WaitForSingleObject( m_processHandle, 3000 );
		CloseHandle( m_processHandle );
		m_processHandle = nullptr;
	}

	if ( m_jobObject )
	{
		CloseHandle( m_jobObject );
		m_jobObject = nullptr;
	}

	// Clean up split-default routes to avoid leaving dead TUN routes after shutdown.
	{
		const char* cleanupCmds[] = {
			"route DELETE 0.0.0.0 MASK 128.0.0.0",
			"route DELETE 128.0.0.0 MASK 128.0.0.0"
		};
		for ( auto c : cleanupCmds )
		{
			STARTUPINFOA si2{}; si2.cb = sizeof( si2 );
			si2.dwFlags = STARTF_USESHOWWINDOW; si2.wShowWindow = SW_HIDE;
			PROCESS_INFORMATION pi2{};
			char cmd[256];
			strncpy( cmd, c, sizeof( cmd ) );
			if ( CreateProcessA( nullptr, cmd, nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
				nullptr, nullptr, &si2, &pi2 ) )
			{
				WaitForSingleObject( pi2.hProcess, 3000 );
				CloseHandle( pi2.hProcess ); CloseHandle( pi2.hThread );
			}
		}
	}

	m_pid = 0;
	m_outbounds = 0;
	m_running = false;
	Log( "[singbox] stopped" );
}

bool SingboxSupervisor::ReloadConfig( const std::vector<AccountConfig>& accounts )
{
	Stop();
	return Start( accounts, m_exeDir, m_workDir );
}

SingboxSupervisor::Stats SingboxSupervisor::GetStats() const
{
	std::lock_guard<std::mutex> lock( m_mutex );
	Stats s;
	s.running    = m_running.load();
	s.pid        = m_pid;
	s.outbounds  = m_outbounds;
	s.configPath = m_configPath;
	s.logPath    = m_logPath;
	s.lastError  = m_lastError;
	return s;
}
