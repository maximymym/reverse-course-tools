// ProxyHook.dll — per-process SOCKS5 redirector
//
// Читает C:\temp\andromeda\proxy_<PID>.json при загрузке. Если proxy поле
// непустое и парсится как "socks5://..." — устанавливает WinSock hooks
// (TCP connect, DNS, UDP sendto/recvfrom) и child-process hooks
// (CreateProcessA/W → auto-inject в child).
//
// Если конфиг отсутствует или невалидный — DLL no-op (hook'и не ставятся),
// чтобы случайный инжект не ломал процесс.

#include "hooks.h"
#include "health.h"

#include <WinSock2.h>
#include <Windows.h>
#include <MinHook.h>

#pragma comment(lib, "ws2_32.lib")

namespace proxyhook
{

// ── Singleton state ──
static HookState g_hookState;
HookState& g_state() { return g_hookState; }

// ── Fake IP registry ──

uint32_t AllocFakeIpForHost( const std::string& host )
{
    auto& st = g_state();
    std::lock_guard<std::mutex> lock( st.fakeIpMutex );

    auto it = st.hostToFakeIp.find( host );
    if ( it != st.hostToFakeIp.end() )
        return it->second;

    // 127.30.b.c where counter encodes b and c
    uint32_t counter = st.nextFakeIpCounter++;
    if ( counter > 0xFFFE ) // safeguard: 127.30.0.1 .. 127.30.255.254
    {
        st.nextFakeIpCounter = 1;
        counter = 1;
    }

    uint8_t b = ( counter >> 8 ) & 0xFF;
    uint8_t c = counter & 0xFF;

    // Network-order IPv4: 127.30.b.c → bytes {127, 30, b, c}
    uint32_t ipv4Net = 0;
    uint8_t* p = (uint8_t*)&ipv4Net;
    p[0] = 127; p[1] = 30; p[2] = b; p[3] = c;

    st.hostToFakeIp[host]      = ipv4Net;
    st.fakeIpToHost[ipv4Net]   = host;
    return ipv4Net;
}

bool LookupFakeIp( uint32_t ipv4Net, std::string& hostOut )
{
    auto& st = g_state();
    // Быстрая проверка диапазона — 127.30.x.x
    uint8_t* p = (uint8_t*)&ipv4Net;
    if ( p[0] != 127 || ( p[1] != 30 && p[1] != 31 ) )
        return false;

    std::lock_guard<std::mutex> lock( st.fakeIpMutex );
    auto it = st.fakeIpToHost.find( ipv4Net );
    if ( it == st.fakeIpToHost.end() )
        return false;
    hostOut = it->second;
    return true;
}

// Real IP ↔ hostname reverse mapping. Использует ту же fakeIpToHost мапу
// (семантика "IP → hostname"), но без fake-range ограничения.
void RegisterIpToHost( uint32_t ipv4Net, const std::string& host )
{
    auto& st = g_state();
    std::lock_guard<std::mutex> lock( st.fakeIpMutex );
    st.fakeIpToHost[ipv4Net] = host;
}

bool LookupIpHost( uint32_t ipv4Net, std::string& hostOut )
{
    auto& st = g_state();
    std::lock_guard<std::mutex> lock( st.fakeIpMutex );
    auto it = st.fakeIpToHost.find( ipv4Net );
    if ( it == st.fakeIpToHost.end() )
        return false;
    hostOut = it->second;
    return true;
}

// ── Install all ──

bool InstallAllHooks()
{
    MH_STATUS mhInit = MH_Initialize();
    if ( mhInit != MH_OK && mhInit != MH_ERROR_ALREADY_INITIALIZED )
    {
        DbgLog( "MH_Initialize failed: %d", mhInit );
        return false;
    }

    bool tcp = true, udp = true, proc = true;
    if ( g_state().cfg.enabled )
    {
        tcp  = InstallTcpHooks();
        udp  = InstallUdpHooks(); // no-op если enable_udp=false
        proc = InstallProcessHooks();
    }
    bool hwid = InstallHwidHooks(); // no-op если hwid.enabled=false

    DbgLog( "InstallAllHooks: tcp=%d udp=%d process=%d hwid=%d",
        tcp, udp, proc, hwid );

    // Успех если proxy (TCP) поднялся ИЛИ hwid активен. Иначе DLL не имеет цели.
    return ( g_state().cfg.enabled ? tcp : false ) || g_state().hwid.enabled;
}

void UninstallAllHooks()
{
    MH_DisableHook( MH_ALL_HOOKS );
    MH_Uninitialize();
}

// ── Init worker thread ──
//
// DllMain должен вернуться быстро и не делать тяжёлую работу (loader-lock).
// Запускаем отдельный поток который:
//   1) грузит WS2_32 (LoadLibraryA("ws2_32.dll"))
//   2) читает config
//   3) ставит hooks

static DWORD WINAPI InitThreadProc( LPVOID )
{
    // Ensure WinSock loaded (для GetProcAddress в hooks)
    WSADATA wsa;
    WSAStartup( MAKEWORD( 2, 2 ), &wsa );

    auto& st = g_state();

    // 1) Сохраняем путь к самому себе (для child-inject)
    HMODULE hSelf = nullptr;
    GetModuleHandleExA(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        (LPCSTR)&InitThreadProc, &hSelf );
    if ( hSelf )
    {
        char path[MAX_PATH];
        DWORD n = GetModuleFileNameA( hSelf, path, MAX_PATH );
        if ( n > 0 && n < MAX_PATH )
            st.selfDllPath = path;
    }

    // 2) Читаем конфиг (proxy + hwid)
    std::string err;
    FullConfig full;
    if ( !LoadConfigForCurrentProcess( full, err ) )
    {
        DbgLog( "init: config load failed — %s (no-op)", err.c_str() );
        return 0;
    }
    st.cfg = full.proxy;
    st.hwid = full.hwid;

    DbgLog( "init: proxy_enabled=%d hwid_enabled=%d proxy=%s:%u udp=%d self=%s",
        (int)st.cfg.enabled, (int)st.hwid.enabled,
        st.cfg.host.c_str(), st.cfg.port,
        (int)st.cfg.enableUdp,
        st.selfDllPath.c_str() );

    // 3) Ставим hooks
    if ( !InstallAllHooks() )
    {
        DbgLog( "init: InstallAllHooks failed" );
        return 0;
    }

    st.initialized = true;
    DbgLog( "init: hooks active, ready" );

    // Health thread — self-diagnostic для orchestrator'а
    StartHealthThread();
    return 0;
}

} // namespace proxyhook

// ── DllMain ──
BOOL WINAPI DllMain( HINSTANCE hInst, DWORD reason, LPVOID )
{
    if ( reason == DLL_PROCESS_ATTACH )
    {
        DisableThreadLibraryCalls( hInst );
        proxyhook::DbgLog( "DllMain DLL_PROCESS_ATTACH (pid=%lu, base=%p)",
            GetCurrentProcessId(), hInst );
        HANDLE t = CreateThread( nullptr, 0, proxyhook::InitThreadProc, nullptr, 0, nullptr );
        if ( t )
        {
            proxyhook::DbgLog( "DllMain: init thread launched" );
            CloseHandle( t );
        }
        else
        {
            proxyhook::DbgLog( "DllMain: CreateThread FAILED err=%lu", GetLastError() );
        }
    }
    else if ( reason == DLL_PROCESS_DETACH )
    {
        proxyhook::DbgLog( "DllMain DLL_PROCESS_DETACH" );
        if ( proxyhook::g_state().initialized )
            proxyhook::UninstallAllHooks();
    }
    return TRUE;
}
