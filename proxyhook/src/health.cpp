// health.cpp — self-diagnostic thread для ProxyHook DLL.
//
// Каждые ~15с пишет JSON на диск:
//   C:\temp\andromeda\proxyhook_health_<PID>.json
//
// Содержит:
//   - proxy.exit_ip      — actual exit IP из api.ipify.org через SOCKS5
//   - proxy.probe_ok     — SOCKS5 tunnel рабочий (handshake + HTTP 200)
//   - proxy.latency_ms   — время probe'а
//   - socks5_ok/fail     — cumulative handshake counters (incrementятся из hooks_tcp)
//   - hwid.observed.*    — результаты вызова hooked APIs изнутри DLL
//   - hwid.*_match       — true если observed == expected
//
// Orchestrator читает эти файлы per-PID и показывает в GUI.

#include "health.h"
#include "hooks.h"
#include "config.h"
#include "hwid_gen.h"
#include "socks5_client.h"

#include <WinSock2.h>
#include <WS2tcpip.h>
#include <Windows.h>
#include <iphlpapi.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <chrono>
#include <fstream>

#include <json.hpp>
using json = nlohmann::json;

#pragma comment(lib, "iphlpapi.lib")

namespace proxyhook
{

// ── Singleton counters ──
static HealthCounters g_counters;
HealthCounters& GetHealthCounters() { return g_counters; }

// ── Helpers ──

static int64_t NowMs()
{
    FILETIME ft;
    GetSystemTimeAsFileTime( &ft );
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    // 100-ns since 1601 → ms since 1970
    return (int64_t)( u.QuadPart / 10000ULL - 11644473600000ULL );
}

static std::string Ipv4ToStr( uint32_t ipv4Net )
{
    uint8_t* p = (uint8_t*)&ipv4Net;
    char buf[32];
    snprintf( buf, sizeof( buf ), "%u.%u.%u.%u", p[0], p[1], p[2], p[3] );
    return buf;
}

// ── Proxy HTTP probe через SOCKS5 → api.ipify.org:80 ──
//
// Возвращает exit IP или "" при ошибке.

struct ProbeResult
{
    bool        ok = false;
    std::string exitIp;
    int64_t     latencyMs = -1;
    std::string error;
    std::string mode;     // "socks5" или "direct"
};

// HTTP GET через уже установленный SOCKS5 tunnel (socket).
// Возвращает body string.
static bool HttpGetSimple( SOCKET sock, const char* host, const char* path,
    std::string& bodyOut, std::string& errOut )
{
    char req[512];
    int n = snprintf( req, sizeof( req ),
        "GET %s HTTP/1.0\r\n"
        "Host: %s\r\n"
        "User-Agent: ProxyHook/1.0\r\n"
        "Accept: */*\r\n"
        "Connection: close\r\n"
        "\r\n",
        path, host );

    if ( n <= 0 || n >= (int)sizeof( req ) )
    {
        errOut = "req too big";
        return false;
    }

    int sent = send( sock, req, n, 0 );
    if ( sent != n )
    {
        errOut = "send failed";
        return false;
    }

    // Recv всё до close
    std::string resp;
    resp.reserve( 4096 );
    char buf[4096];
    while ( true )
    {
        int r = recv( sock, buf, sizeof( buf ), 0 );
        if ( r > 0 )
            resp.append( buf, r );
        else
            break;
        if ( resp.size() > 256 * 1024 )
            break; // safety cap
    }

    if ( resp.empty() )
    {
        errOut = "empty response";
        return false;
    }

    // Separate status/headers/body
    size_t headerEnd = resp.find( "\r\n\r\n" );
    if ( headerEnd == std::string::npos )
    {
        errOut = "malformed http";
        return false;
    }
    std::string status = resp.substr( 0, resp.find( "\r\n" ) );
    if ( status.find( "200" ) == std::string::npos )
    {
        errOut = "http status: " + status.substr( 0, 32 );
        return false;
    }

    bodyOut = resp.substr( headerEnd + 4 );
    // Trim trailing whitespace
    while ( !bodyOut.empty() &&
        ( bodyOut.back() == '\r' || bodyOut.back() == '\n' || bodyOut.back() == ' ' ) )
        bodyOut.pop_back();

    return true;
}

static ProbeResult DoProxyProbe()
{
    ProbeResult res;
    const auto& cfg = g_state().cfg;

    int64_t t0 = NowMs();

    // Два пути:
    //  (A) cfg.enabled → hook активен, SOCKS5 к api.ipify.org через прокси
    //  (B) cfg.enabled=false → direct connect к api.ipify.org.
    //      При useTun2Socks трафик попадёт в sing-box TUN → per-account outbound
    //      → exit IP = per-account proxy. Если TUN не ловит (leak) — видим real IP.
    //      В обоих случаях результат = "что VALVE реально видит от этого процесса".
    SOCKET sock = INVALID_SOCKET;

    if ( cfg.enabled )
    {
        res.mode = "socks5";
        sockaddr_in proxyAddr{};
        if ( !ResolveHost( cfg.host.c_str(), cfg.port, &proxyAddr ) )
        {
            res.error = "resolve proxy host failed";
            return res;
        }
        sock = ConnectTcp( proxyAddr, 5000 );
        if ( sock == INVALID_SOCKET )
        {
            res.error = "connect-to-proxy failed";
            return res;
        }
        Socks5Status st = Socks5TcpHandshake( sock, cfg,
            "api.ipify.org", 0, htons( 80 ) );
        if ( st != Socks5Status::OK )
        {
            char ebuf[64];
            snprintf( ebuf, sizeof( ebuf ), "handshake fail st=%d", (int)st );
            res.error = ebuf;
            closesocket( sock );
            return res;
        }
    }
    else
    {
        res.mode = "direct";
        sockaddr_in target{};
        if ( !ResolveHost( "api.ipify.org", 80, &target ) )
        {
            res.error = "resolve api.ipify.org failed";
            return res;
        }
        sock = ConnectTcp( target, 5000 );
        if ( sock == INVALID_SOCKET )
        {
            res.error = "connect api.ipify.org failed";
            return res;
        }
    }

    // HTTP GET — api.ipify.org возвращает plain-text IP в body
    DWORD rcvTimeout = 5000;
    setsockopt( sock, SOL_SOCKET, SO_RCVTIMEO, (char*)&rcvTimeout, sizeof( rcvTimeout ) );

    std::string body, err;
    bool httpOk = HttpGetSimple( sock, "api.ipify.org", "/", body, err );
    closesocket( sock );

    if ( !httpOk )
    {
        res.error = "http: " + err;
        return res;
    }

    // Extract IP — ipify.org возвращает plain "8.8.8.8"
    // Валидируем: только digits и dots, <= 15 chars, parseable as IPv4.
    if ( body.size() < 7 || body.size() > 15 )
    {
        res.error = "body len invalid: " + std::to_string( body.size() );
        return res;
    }
    for ( char c : body )
    {
        if ( !( c >= '0' && c <= '9' ) && c != '.' )
        {
            res.error = "body not IPv4-literal";
            return res;
        }
    }

    in_addr ia{};
    if ( inet_pton( AF_INET, body.c_str(), &ia ) != 1 )
    {
        res.error = "inet_pton fail";
        return res;
    }

    res.exitIp = body;
    res.ok = true;
    res.latencyMs = NowMs() - t0;
    return res;
}

// ── HWID self-check: вызываем hooked APIs изнутри DLL, сравниваем с expected ──

struct HwidObserved
{
    std::string machineGuid;        // из RegQueryValueExW
    bool        machineGuidOk = false;

    std::string macStr;             // из GetAdaptersInfo первый non-loopback
    bool        macOk = false;

    uint32_t    volumeSerial = 0;   // из GetVolumeInformationW C:
    bool        volumeSerialOk = false;

    bool        smbiosSerialMatch = false;  // SystemSerial через GetSystemFirmwareTable
    std::string smbiosSerialObserved;
};

static void CheckHwidMachineGuid( HwidObserved& out )
{
    HKEY h = nullptr;
    if ( RegOpenKeyExW( HKEY_LOCAL_MACHINE,
            L"SOFTWARE\\Microsoft\\Cryptography", 0,
            KEY_READ | KEY_WOW64_64KEY, &h ) != ERROR_SUCCESS )
        return;

    wchar_t buf[128] = {};
    DWORD sz = sizeof( buf );
    DWORD type = 0;
    LSTATUS r = RegQueryValueExW( h, L"MachineGuid", nullptr, &type,
        (BYTE*)buf, &sz );
    RegCloseKey( h );

    if ( r != ERROR_SUCCESS || type != REG_SZ )
        return;

    // RegQueryValueExW не гарантирует null-terminator если reg value ровно
    // заполнил буфер. WideCharToMultiByte(-1) пойдёт читать мимо → AV. Форсим
    // терминатор в последней wchar cell.
    constexpr size_t kBufChars = sizeof( buf ) / sizeof( wchar_t );
    buf[kBufChars - 1] = 0;

    char narrow[128] = {};
    WideCharToMultiByte( CP_UTF8, 0, buf, -1, narrow, sizeof( narrow ), nullptr, nullptr );
    narrow[sizeof( narrow ) - 1] = 0;
    out.machineGuid = narrow;
    out.machineGuidOk = true;
}

static void CheckHwidMac( HwidObserved& out )
{
    ULONG sz = 0;
    GetAdaptersInfo( nullptr, &sz );
    if ( sz == 0 ) return;
    std::vector<uint8_t> buf( sz );
    PIP_ADAPTER_INFO head = (PIP_ADAPTER_INFO)buf.data();
    if ( GetAdaptersInfo( head, &sz ) != NO_ERROR ) return;

    for ( PIP_ADAPTER_INFO p = head; p; p = p->Next )
    {
        if ( p->AddressLength == 6 )
        {
            uint8_t* m = p->Address;
            char str[32];
            snprintf( str, sizeof( str ), "%02X-%02X-%02X-%02X-%02X-%02X",
                m[0], m[1], m[2], m[3], m[4], m[5] );
            out.macStr = str;
            out.macOk = true;
            return; // первый адаптер
        }
    }
}

static void CheckHwidVolumeSerial( HwidObserved& out )
{
    DWORD serial = 0;
    if ( GetVolumeInformationW( L"C:\\", nullptr, 0, &serial, nullptr, nullptr, nullptr, 0 ) )
    {
        out.volumeSerial = serial;
        out.volumeSerialOk = true;
    }
}

static void CheckHwidSmbios( HwidObserved& out )
{
    // Получаем RSMB через GetSystemFirmwareTable — наш hook подменит поля
    UINT sz = GetSystemFirmwareTable( 'RSMB', 0, nullptr, 0 );
    if ( sz == 0 || sz > 64 * 1024 )
        return;
    std::vector<uint8_t> buf( sz );
    UINT got = GetSystemFirmwareTable( 'RSMB', 0, buf.data(), (DWORD)sz );
    if ( got == 0 || got > sz )
        return;

    // Парсим Type 1 (System) → string[SerialNumber@0x07]
    struct RawHdr { BYTE CallMethod; BYTE Maj; BYTE Min; BYTE DmiRev; DWORD Length; };
    if ( got < sizeof( RawHdr ) ) return;
    auto rh = (RawHdr*)buf.data();
    if ( rh->Length > got - sizeof( RawHdr ) ) return;

    uint8_t* tbl = buf.data() + sizeof( RawHdr );
    uint8_t* end = tbl + rh->Length;

    uint8_t* p = tbl;
    int safety = 0;
    while ( p + 4 <= end && safety++ < 512 )
    {
        uint8_t type = p[0];
        uint8_t length = p[1];
        if ( length < 4 ) break;
        uint8_t* formattedEnd = p + length;
        if ( formattedEnd > end ) break;

        if ( type == 1 && length >= 0x18 )
        {
            // SerialNumber offset 0x07 — 1-based string index
            int serialIdx = p[0x07];
            if ( serialIdx > 0 )
            {
                // Find Nth string after formattedEnd
                uint8_t* s = formattedEnd;
                int idx = 1;
                while ( s < end - 1 )
                {
                    uint8_t* sEnd = s;
                    while ( sEnd < end && *sEnd != 0 ) sEnd++;
                    if ( sEnd == s ) break;
                    if ( idx == serialIdx )
                    {
                        out.smbiosSerialObserved.assign( (const char*)s, sEnd - s );
                        break;
                    }
                    s = sEnd + 1;
                    idx++;
                }
            }
            break;
        }

        // Skip strings
        uint8_t* s = formattedEnd;
        while ( s < end - 1 )
        {
            if ( s[0] == 0 && s[1] == 0 ) { s += 2; break; }
            s++;
        }
        p = s;
        if ( type == 127 ) break;
    }

    const auto& expected = g_state().hwid.values.systemSerial;
    if ( !expected.empty() && out.smbiosSerialObserved == expected )
        out.smbiosSerialMatch = true;
}

static HwidObserved DoHwidSelfCheck()
{
    HwidObserved o;
    if ( !g_state().hwid.enabled )
        return o;
    CheckHwidMachineGuid( o );
    CheckHwidMac( o );
    CheckHwidVolumeSerial( o );
    CheckHwidSmbios( o );
    return o;
}

// ── JSON writer ──

static void WriteHealthJson( const ProbeResult& probe,
    const HwidObserved& hw, int64_t probeMs, bool probeRan )
{
    const auto& st = g_state();
    const auto& fake = st.hwid.values;

    json j;
    j["pid"]         = (uint64_t)GetCurrentProcessId();
    j["written_ms"]  = NowMs();

    // ── Proxy ──
    json jp;
    jp["enabled"]    = st.cfg.enabled;
    if ( st.cfg.enabled )
    {
        jp["raw_url"] = st.cfg.rawUrl;
        jp["host"]    = st.cfg.host;
        jp["port"]    = st.cfg.port;
    }
    auto& ctr = g_counters;
    jp["socks5_ok"]    = (uint64_t)ctr.socks5Ok.load();
    jp["socks5_fail"]  = (uint64_t)ctr.socks5Fail.load();
    jp["udp_ok"]       = (uint64_t)ctr.udpOk.load();
    jp["udp_fail"]     = (uint64_t)ctr.udpFail.load();

    if ( probeRan )
    {
        jp["last_probe_ms"]       = probeMs;
        jp["last_probe_ok"]       = probe.ok;
        jp["last_probe_exit_ip"]  = probe.exitIp;
        jp["last_probe_latency"]  = probe.latencyMs;
        jp["last_probe_mode"]     = probe.mode;
        if ( !probe.ok )
            jp["last_probe_error"] = probe.error;
    }
    j["proxy"] = jp;

    // ── HWID ──
    json jh;
    jh["enabled"] = st.hwid.enabled;
    if ( st.hwid.enabled )
    {
        jh["seed"] = st.hwid.seed;

        json exp;
        exp["machine_guid"]   = fake.machineGuid;
        exp["mac"]            = fake.macStr;
        exp["volume_serial"]  = fake.volumeSerial;
        exp["system_serial"]  = fake.systemSerial;
        jh["expected"] = exp;

        json obs;
        obs["machine_guid"]   = hw.machineGuid;
        obs["mac"]            = hw.macStr;
        obs["volume_serial"]  = hw.volumeSerial;
        obs["system_serial"]  = hw.smbiosSerialObserved;
        jh["observed"] = obs;

        bool gMatch = hw.machineGuidOk   && hw.machineGuid   == fake.machineGuid;
        bool mMatch = hw.macOk           && hw.macStr        == fake.macStr;
        bool vMatch = hw.volumeSerialOk  && hw.volumeSerial  == fake.volumeSerial;
        bool sMatch = hw.smbiosSerialMatch;

        jh["machine_guid_match"]  = gMatch;
        jh["mac_match"]           = mMatch;
        jh["volume_serial_match"] = vMatch;
        jh["system_serial_match"] = sMatch;

        // SystemSerial часто is not-set by OEM (observed="") — наш hook
        // не может patch'ить несуществующую строку без resize'а blob. Поэтому
        // SystemSerial — optional check. Critical 3 = MachineGuid + MAC + VolSerial,
        // которые Valve smurf detector кластеризует.
        jh["system_serial_patched"]   = !hw.smbiosSerialObserved.empty();
        jh["critical_match"]          = gMatch && mMatch && vMatch;
        jh["all_match"]               = gMatch && mMatch && vMatch && sMatch;
    }
    j["hwid"] = jh;

    // Atomic write
    char path[MAX_PATH];
    snprintf( path, sizeof( path ),
        "C:\\temp\\andromeda\\proxyhook_health_%lu.json",
        GetCurrentProcessId() );
    char tmpPath[MAX_PATH];
    snprintf( tmpPath, sizeof( tmpPath ), "%s.tmp", path );

    CreateDirectoryA( "C:\\temp", nullptr );
    CreateDirectoryA( "C:\\temp\\andromeda", nullptr );

    std::ofstream f( tmpPath );
    if ( !f.is_open() ) return;
    f << j.dump( 2 );
    f.close();

    MoveFileExA( tmpPath, path, MOVEFILE_REPLACE_EXISTING );
}

// ── Main thread ──
//
// Запускается с задержкой 5с (ждём пока приложение полностью стартанёт и
// hooks прогреются) — затем каждые 15с пишет health JSON.

static DWORD WINAPI HealthThreadProc( LPVOID )
{
    // Инициальная пауза — Steam/Dota ещё не прогрузились
    Sleep( 5000 );

    int iter = 0;
    while ( true )
    {
        // Probe ВСЕГДА раз в 2 итерации (каждые 30с) — даже при proxy hook off.
        // При useTun2Socks direct connect попадёт в sing-box TUN → per-account exit IP.
        // Это главный source of truth "что Valve реально видит от этого процесса".
        bool runProbe = ( iter % 2 == 0 );
        ProbeResult probe;
        int64_t probeMs = 0;
        if ( runProbe )
        {
            probe = DoProxyProbe();
            probeMs = NowMs();
            DbgLog( "health probe: mode=%s ok=%d exit=%s lat=%lldms err='%s'",
                probe.mode.c_str(), (int)probe.ok, probe.exitIp.c_str(),
                (long long)probe.latencyMs, probe.error.c_str() );
        }

        HwidObserved hw = DoHwidSelfCheck();
        if ( g_state().hwid.enabled && iter < 3 )
        {
            // Логируем первые 3 итерации — чтобы в DbgLog видно было
            DbgLog( "health hwid: mg='%s'(ok=%d) mac='%s'(ok=%d) vol=0x%08X(ok=%d) sysser='%s'(match=%d)",
                hw.machineGuid.c_str(), (int)hw.machineGuidOk,
                hw.macStr.c_str(), (int)hw.macOk,
                hw.volumeSerial, (int)hw.volumeSerialOk,
                hw.smbiosSerialObserved.c_str(), (int)hw.smbiosSerialMatch );
        }

        WriteHealthJson( probe, hw, probeMs, runProbe );

        iter++;
        Sleep( 15000 );
    }
}

void StartHealthThread()
{
    HANDLE t = CreateThread( nullptr, 0, HealthThreadProc, nullptr, 0, nullptr );
    if ( t )
    {
        DbgLog( "health: thread started" );
        CloseHandle( t );
    }
    else
    {
        DbgLog( "health: CreateThread failed err=%lu", GetLastError() );
    }
}

} // namespace proxyhook
