#include "hooks.h"
#include "health.h"

#include <WS2tcpip.h>
#include <MinHook.h>
#include <cstring>

#pragma comment(lib, "ws2_32.lib")

namespace proxyhook
{

// ── Originals (заполняются в InstallTcpHooks) ──
using fn_connect_t             = int  (WSAAPI*)( SOCKET, const sockaddr*, int );
using fn_WSAConnect_t          = int  (WSAAPI*)( SOCKET, const sockaddr*, int, LPWSABUF, LPWSABUF, LPQOS, LPQOS );
using fn_getaddrinfo_t         = int  (WSAAPI*)( PCSTR, PCSTR, const ADDRINFOA*, PADDRINFOA* );
using fn_GetAddrInfoW_t        = int  (WSAAPI*)( PCWSTR, PCWSTR, const ADDRINFOW*, PADDRINFOW* );
using fn_freeaddrinfo_t        = void (WSAAPI*)( PADDRINFOA );
using fn_FreeAddrInfoW_t       = void (WSAAPI*)( PADDRINFOW );
using fn_gethostbyname_t       = hostent* (WSAAPI*)( const char* );
using fn_WSAConnectByNameA_t   = BOOL (WSAAPI*)( SOCKET, LPSTR, LPSTR, LPDWORD, PSOCKADDR, LPDWORD, PSOCKADDR, const timeval*, LPWSAOVERLAPPED );
using fn_WSAConnectByNameW_t   = BOOL (WSAAPI*)( SOCKET, LPWSTR, LPWSTR, LPDWORD, PSOCKADDR, LPDWORD, PSOCKADDR, const timeval*, LPWSAOVERLAPPED );
using fn_closesocket_t         = int  (WSAAPI*)( SOCKET );

static fn_connect_t             o_connect = nullptr;
static fn_WSAConnect_t          o_WSAConnect = nullptr;
static fn_getaddrinfo_t         o_getaddrinfo = nullptr;
static fn_GetAddrInfoW_t        o_GetAddrInfoW = nullptr;
static fn_freeaddrinfo_t        o_freeaddrinfo = nullptr;
static fn_FreeAddrInfoW_t       o_FreeAddrInfoW = nullptr;
static fn_gethostbyname_t       o_gethostbyname = nullptr;
static fn_WSAConnectByNameA_t   o_WSAConnectByNameA = nullptr;
static fn_WSAConnectByNameW_t   o_WSAConnectByNameW = nullptr;
static fn_closesocket_t         o_closesocket = nullptr;

// ConnectEx — lives in mswsock.dll, но приложения обычно получают её через
// WSAIoctl(SIO_GET_EXTENSION_FUNCTION_POINTER, WSAID_CONNECTEX). Steam/Dota
// используют именно ConnectEx через IOCP.
using fn_ConnectEx_t = BOOL ( PASCAL* )(
    SOCKET, const sockaddr*, int, PVOID, DWORD, LPDWORD, LPOVERLAPPED );
static fn_ConnectEx_t           o_ConnectEx = nullptr;

using fn_WSAIoctl_t = int (WSAAPI*)(
    SOCKET, DWORD, LPVOID, DWORD, LPVOID, DWORD, LPDWORD, LPWSAOVERLAPPED, LPWSAOVERLAPPED_COMPLETION_ROUTINE );
static fn_WSAIoctl_t            o_WSAIoctl = nullptr;

// Gethostbyname thread-local cache (stub он возвращает hostent* в TLS, воспроизводим)
struct TlsHostent
{
    hostent   he;
    uint32_t  addr;        // network-order IPv4
    char*     addrList[2]; // pointers into addr
    char      hname[256];
};
static thread_local TlsHostent g_tlsHost;

// ── Hooks ──

// ── connect/WSAConnect helpers ──

static int DoSocks5Redirect( SOCKET s, const sockaddr* dst, int dstLen )
{
    if ( !dst || dstLen < (int)sizeof( sockaddr_in ) || dst->sa_family != AF_INET )
    {
        // Non-IPv4 — fallback на оригинал (IPv6, UNIX, etc не hook'аем)
        return o_connect( s, dst, dstLen );
    }

    const sockaddr_in* in4 = (const sockaddr_in*)dst;
    uint32_t ipv4Net = in4->sin_addr.s_addr;

    // Bypass check
    if ( IsBypassIpv4( ipv4Net ) )
    {
        DbgLog( "connect: bypass (%u.%u.%u.%u:%u)",
            in4->sin_addr.S_un.S_un_b.s_b1, in4->sin_addr.S_un.S_un_b.s_b2,
            in4->sin_addr.S_un.S_un_b.s_b3, in4->sin_addr.S_un.S_un_b.s_b4,
            ntohs( in4->sin_port ) );
        return o_connect( s, dst, dstLen );
    }

    // Defense: bypass если dest == сам proxy. Иначе если приложение видит
    // HTTP_PROXY env (или configured proxy) и коннектит к нему напрямую,
    // наш hook сделает proxy-over-proxy handshake и запутается.
    {
        sockaddr_in pxAddr{};
        if ( ResolveHost( g_state().cfg.host.c_str(), g_state().cfg.port, &pxAddr ) )
        {
            if ( pxAddr.sin_addr.s_addr == ipv4Net && pxAddr.sin_port == in4->sin_port )
            {
                DbgLog( "connect: bypass (dest == proxy itself) %u.%u.%u.%u:%u",
                    in4->sin_addr.S_un.S_un_b.s_b1, in4->sin_addr.S_un.S_un_b.s_b2,
                    in4->sin_addr.S_un.S_un_b.s_b3, in4->sin_addr.S_un.S_un_b.s_b4,
                    ntohs( in4->sin_port ) );
                return o_connect( s, dst, dstLen );
            }
        }
    }

    // Real-IP → hostname lookup. Если нашли — SOCKS5 CONNECT через hostname
    // (remote DNS на proxy side). Иначе по IPv4.
    std::string realHost;
    bool isFake = LookupIpHost( ipv4Net, realHost );

    // Подключаемся к прокси (НЕ к оригинальному IP!)
    sockaddr_in proxyAddr{};
    if ( !ResolveHost( g_state().cfg.host.c_str(), g_state().cfg.port, &proxyAddr ) )
    {
        DbgLog( "connect: proxy host resolve failed: %s", g_state().cfg.host.c_str() );
        WSASetLastError( WSAENETUNREACH );
        return SOCKET_ERROR;
    }

    // ВАЖНО: s — это socket, переданный приложением. Оно ждёт что connect()
    // соединит его с dst. Мы коннектим через тот же socket к proxy.
    // Если приложение сделало socket() с не-BLOCKING флагом — RecvAll/SendAll
    // в handshake будут падать. Для простоты переводим socket в blocking на
    // время handshake. На ЛЮБОМ exit пути (error/success) возвращаем blocking
    // mode в дефолт (0 = blocking) — WinSock API не даёт прочитать текущий
    // FIONBIO, поэтому явный restore в blocking — best effort. Приложения,
    // использующие WSAEventSelect/WSAAsyncSelect для auto-non-blocking,
    // не задеваются: эти API перепишут режим при следующей операции.
    struct BlockingGuard
    {
        SOCKET s;
        bool   armed;
        ~BlockingGuard()
        {
            if ( armed )
            {
                u_long zero = 0;
                ioctlsocket( s, FIONBIO, &zero );
            }
        }
    };
    u_long nb = 0;
    ioctlsocket( s, FIONBIO, &nb ); // set blocking
    BlockingGuard guard{ s, true };

    int cr = o_connect( s, (const sockaddr*)&proxyAddr, sizeof( proxyAddr ) );
    if ( cr != 0 )
    {
        DbgLog( "connect: connect-to-proxy failed (WSA=%d)", WSAGetLastError() );
        return cr;
    }

    // Handshake
    Socks5Status st;
    if ( isFake )
    {
        DbgLog( "connect: fake-ip -> hostname '%s':%u", realHost.c_str(), ntohs( in4->sin_port ) );
        st = Socks5TcpHandshake( s, g_state().cfg,
            realHost.c_str(), 0, in4->sin_port );
    }
    else
    {
        DbgLog( "connect: raw IPv4 (%u.%u.%u.%u:%u) — no hostname mapping",
            in4->sin_addr.S_un.S_un_b.s_b1, in4->sin_addr.S_un.S_un_b.s_b2,
            in4->sin_addr.S_un.S_un_b.s_b3, in4->sin_addr.S_un.S_un_b.s_b4,
            ntohs( in4->sin_port ) );
        st = Socks5TcpHandshake( s, g_state().cfg,
            nullptr, ipv4Net, in4->sin_port );
    }

    if ( st != Socks5Status::OK )
    {
        GetHealthCounters().socks5Fail.fetch_add( 1, std::memory_order_relaxed );
        DbgLog( "connect: SOCKS5 handshake failed st=%d", (int)st );
        WSASetLastError( WSAECONNREFUSED );
        return SOCKET_ERROR;
    }

    GetHealthCounters().socks5Ok.fetch_add( 1, std::memory_order_relaxed );

    // Логируем успех (без перегруза лога — только первые N)
    {
        static std::atomic<int> logged{ 0 };
        if ( logged.fetch_add( 1, std::memory_order_relaxed ) < 50 )
        {
            if ( isFake )
                DbgLog( "connect: OK via proxy (host='%s':%u)", realHost.c_str(), ntohs( in4->sin_port ) );
            else
                DbgLog( "connect: OK via proxy (%u.%u.%u.%u:%u)",
                    in4->sin_addr.S_un.S_un_b.s_b1, in4->sin_addr.S_un.S_un_b.s_b2,
                    in4->sin_addr.S_un.S_un_b.s_b3, in4->sin_addr.S_un.S_un_b.s_b4,
                    ntohs( in4->sin_port ) );
        }
    }
    return 0;
}

static int WSAAPI h_connect( SOCKET s, const sockaddr* name, int namelen )
{
    // Skip non-INET
    if ( !name || namelen < 2 )
        return o_connect( s, name, namelen );
    if ( name->sa_family != AF_INET )
        return o_connect( s, name, namelen );

    // Проверяем что это TCP socket (UDP связан с sendto, не connect)
    int type = 0; int tlen = sizeof( type );
    if ( getsockopt( s, SOL_SOCKET, SO_TYPE, (char*)&type, &tlen ) == 0 && type != SOCK_STREAM )
        return o_connect( s, name, namelen );

    return DoSocks5Redirect( s, name, namelen );
}

static int WSAAPI h_WSAConnect( SOCKET s, const sockaddr* name, int namelen,
    LPWSABUF ci, LPWSABUF co, LPQOS q1, LPQOS q2 )
{
    if ( !name || namelen < 2 || name->sa_family != AF_INET )
        return o_WSAConnect( s, name, namelen, ci, co, q1, q2 );

    int type = 0; int tlen = sizeof( type );
    if ( getsockopt( s, SOL_SOCKET, SO_TYPE, (char*)&type, &tlen ) == 0 && type != SOCK_STREAM )
        return o_WSAConnect( s, name, namelen, ci, co, q1, q2 );

    int r = DoSocks5Redirect( s, name, namelen );
    return r;
}

// ConnectEx — overlapped TCP connect. Steam/Dota использует его через IOCP.
// Выполняем SOCKS5 handshake **синхронно** (как и для regular connect), потом
// сигналим completion порт вручную через PostQueuedCompletionStatus — но это
// сложно. Для MVP: делаем блокирующий handshake и возвращаем TRUE + сразу
// completion. Steam ждёт overlapped result — не пойдёт без IOCP post.
//
// Упрощение для текущей задачи: redirect через SOCKS5 synchronously, затем
// если успех — signal overlapped event чтобы caller'ский WaitForMultipleObjects
// разбудился.
static BOOL PASCAL h_ConnectEx( SOCKET s, const sockaddr* name, int namelen,
    PVOID sendBuf, DWORD sendBufLen, LPDWORD bytesSent, LPOVERLAPPED ov )
{
    // Skip non-INET
    if ( !name || namelen < 2 || name->sa_family != AF_INET )
        return o_ConnectEx( s, name, namelen, sendBuf, sendBufLen, bytesSent, ov );

    // Synchronous SOCKS5 redirect through same socket
    int r = DoSocks5Redirect( s, name, namelen );
    if ( r != 0 )
    {
        WSASetLastError( WSAECONNREFUSED );
        return FALSE;
    }

    // Если caller подал sendBuf — отправим сразу
    DWORD sent = 0;
    if ( sendBuf && sendBufLen > 0 )
    {
        int sr = send( s, (const char*)sendBuf, (int)sendBufLen, 0 );
        if ( sr > 0 ) sent = (DWORD)sr;
    }
    if ( bytesSent ) *bytesSent = sent;

    // Сигналим overlapped event чтобы GetQueuedCompletionStatus / WaitForSingleObject разбудился
    if ( ov )
    {
        ov->Internal = 0;         // NTSTATUS SUCCESS
        ov->InternalHigh = sent;
        if ( ov->hEvent )
            SetEvent( ov->hEvent );
    }

    return TRUE;
}

// WSAIoctl — приложения получают ConnectEx extension function pointer через него.
// Если вопрос про WSAID_CONNECTEX — возвращаем НАШ wrapper.
static const GUID kConnectExGuid = { 0x25a207b9, 0xddf3, 0x4660,
    { 0x8e, 0xe9, 0x76, 0xe5, 0x8c, 0x74, 0x06, 0x3e } };

static int WSAAPI h_WSAIoctl( SOCKET s, DWORD code, LPVOID in, DWORD inLen,
    LPVOID out, DWORD outLen, LPDWORD bytesRet,
    LPWSAOVERLAPPED ov, LPWSAOVERLAPPED_COMPLETION_ROUTINE cb )
{
    int r = o_WSAIoctl( s, code, in, inLen, out, outLen, bytesRet, ov, cb );

    // SIO_GET_EXTENSION_FUNCTION_POINTER = 0xC8000006
    if ( r == 0 && code == 0xC8000006 && in && inLen == sizeof( GUID ) && out && outLen >= sizeof( void* ) )
    {
        GUID g = *(GUID*)in;
        if ( memcmp( &g, &kConnectExGuid, sizeof( GUID ) ) == 0 )
        {
            // Сохраняем оригинал, подменяем указатель на наш hook
            if ( !o_ConnectEx )
                o_ConnectEx = *(fn_ConnectEx_t*)out;
            *(fn_ConnectEx_t*)out = &h_ConnectEx;
            DbgLog( "WSAIoctl: ConnectEx pointer intercepted (orig=%p -> hook=%p)",
                (void*)o_ConnectEx, (void*)&h_ConnectEx );
        }
    }

    return r;
}

// ── getaddrinfo / gethostbyname — fake IP strategy ──

static bool IsIpLiteral( const char* name )
{
    // Является ли строка уже IP-адресом (IPv4)?
    IN_ADDR ia{};
    return inet_pton( AF_INET, name, &ia ) == 1;
}

static int WSAAPI h_getaddrinfo( PCSTR pNodeName, PCSTR pServiceName,
    const ADDRINFOA* pHints, PADDRINFOA* ppResult )
{
    // Сначала РЕАЛЬНЫЙ resolve через native resolver (= DNS leak к реальному
    // интернету, но без этого Steam не делает connect вообще).
    int r = o_getaddrinfo( pNodeName, pServiceName, pHints, ppResult );

    if ( r != 0 || !ppResult || !*ppResult )
        return r;

    if ( !pNodeName || IsIpLiteral( pNodeName ) || IsBypassHost( pNodeName ) )
        return r;

    // Регистрируем mapping real_ip → hostname для всех IPv4 в ответе.
    // В h_connect, если dest IP есть в mapping — делаем SOCKS5 CONNECT
    // через hostname (ATYP=DOMAIN), иначе через IPv4.
    int mapped = 0;
    for ( ADDRINFOA* p = *ppResult; p; p = p->ai_next )
    {
        if ( p->ai_family != AF_INET || !p->ai_addr ) continue;
        sockaddr_in* sa = (sockaddr_in*)p->ai_addr;
        RegisterIpToHost( sa->sin_addr.s_addr, pNodeName );
        if ( mapped == 0 )
        {
            DbgLog( "getaddrinfo: '%s' -> %u.%u.%u.%u (mapped for SOCKS5 remote-DNS)",
                pNodeName,
                sa->sin_addr.S_un.S_un_b.s_b1, sa->sin_addr.S_un.S_un_b.s_b2,
                sa->sin_addr.S_un.S_un_b.s_b3, sa->sin_addr.S_un.S_un_b.s_b4 );
        }
        mapped++;
    }
    return 0;
}

static int WSAAPI h_GetAddrInfoW( PCWSTR pNodeName, PCWSTR pServiceName,
    const ADDRINFOW* pHints, PADDRINFOW* ppResult )
{
    if ( !pNodeName )
        return o_GetAddrInfoW( pNodeName, pServiceName, pHints, ppResult );

    int r = o_GetAddrInfoW( pNodeName, pServiceName, pHints, ppResult );
    if ( r != 0 || !ppResult || !*ppResult )
        return r;

    char narrow[512];
    int nn = WideCharToMultiByte( CP_UTF8, 0, pNodeName, -1, narrow, sizeof( narrow ), nullptr, nullptr );
    if ( nn <= 0 )
        return r;

    if ( IsIpLiteral( narrow ) || IsBypassHost( narrow ) )
        return r;

    int mapped = 0;
    for ( ADDRINFOW* p = *ppResult; p; p = p->ai_next )
    {
        if ( p->ai_family != AF_INET || !p->ai_addr ) continue;
        sockaddr_in* sa = (sockaddr_in*)p->ai_addr;
        RegisterIpToHost( sa->sin_addr.s_addr, narrow );
        mapped++;
    }
    if ( mapped > 0 )
        DbgLog( "GetAddrInfoW: '%s' -> %d IPs mapped", narrow, mapped );
    return 0;
}

// freeaddrinfo — распознаём наш heap-allocated блок: ai_next==null и он был выделен
// ровно одним HeapAlloc(sizeof(ADDRINFOA) + sizeof(sockaddr_in)). Проверять "свои"
// через HeapSize — если получилось, это наш. Иначе делегируем системному freeaddrinfo.
static void WSAAPI h_freeaddrinfo( PADDRINFOA ai )
{
    if ( !ai )
        return;
    if ( ai->ai_next == nullptr &&
         ai->ai_addr == (sockaddr*)( ai + 1 ) )
    {
        SIZE_T sz = HeapSize( GetProcessHeap(), 0, ai );
        if ( sz != (SIZE_T)-1 &&
             sz >= sizeof( ADDRINFOA ) + sizeof( sockaddr_in ) )
        {
            HeapFree( GetProcessHeap(), 0, ai );
            return;
        }
    }
    if ( o_freeaddrinfo )
        o_freeaddrinfo( ai );
}

static void WSAAPI h_FreeAddrInfoW( PADDRINFOW ai )
{
    if ( !ai )
        return;
    if ( ai->ai_next == nullptr &&
         ai->ai_addr == (sockaddr*)( ai + 1 ) )
    {
        SIZE_T sz = HeapSize( GetProcessHeap(), 0, ai );
        if ( sz != (SIZE_T)-1 &&
             sz >= sizeof( ADDRINFOW ) + sizeof( sockaddr_in ) )
        {
            HeapFree( GetProcessHeap(), 0, ai );
            return;
        }
    }
    if ( o_FreeAddrInfoW )
        o_FreeAddrInfoW( ai );
}

static hostent* WSAAPI h_gethostbyname( const char* name )
{
    if ( !name || IsIpLiteral( name ) || IsBypassHost( name ) )
        return o_gethostbyname( name );

    uint32_t fake = AllocFakeIpForHost( name );

    strncpy( g_tlsHost.hname, name, sizeof( g_tlsHost.hname ) - 1 );
    g_tlsHost.hname[sizeof( g_tlsHost.hname ) - 1] = 0;
    g_tlsHost.addr = fake;
    g_tlsHost.addrList[0] = (char*)&g_tlsHost.addr;
    g_tlsHost.addrList[1] = nullptr;

    g_tlsHost.he.h_name      = g_tlsHost.hname;
    g_tlsHost.he.h_aliases   = nullptr;
    g_tlsHost.he.h_addrtype  = AF_INET;
    g_tlsHost.he.h_length    = 4;
    g_tlsHost.he.h_addr_list = g_tlsHost.addrList;

    DbgLog( "gethostbyname: '%s' -> fake", name );
    return &g_tlsHost.he;
}

// WSAConnectByName: резолвит hostname + CONNECT в одной операции.
// Реализуем через: socket() (если нужен) → connect() который будет перехвачен.
// Но WinSock API гарантирует что func сама вызывает connect — её внутренняя связка
// не проходит через exported connect(). Поэтому полностью повторяем: getaddrinfo →
// connect → return.
static BOOL WSAAPI h_WSAConnectByNameA( SOCKET s, LPSTR nodename, LPSTR servicename,
    LPDWORD locAddrLen, PSOCKADDR locAddr, LPDWORD remAddrLen, PSOCKADDR remAddr,
    const timeval* tmo, LPWSAOVERLAPPED ov )
{
    if ( !nodename || !servicename )
    {
        if ( o_WSAConnectByNameA )
            return o_WSAConnectByNameA( s, nodename, servicename, locAddrLen, locAddr,
                remAddrLen, remAddr, tmo, ov );
        return FALSE;
    }

    if ( IsIpLiteral( nodename ) || IsBypassHost( nodename ) )
    {
        if ( o_WSAConnectByNameA )
            return o_WSAConnectByNameA( s, nodename, servicename, locAddrLen, locAddr,
                remAddrLen, remAddr, tmo, ov );
    }

    long portL = strtol( servicename, nullptr, 10 );
    if ( portL <= 0 || portL >= 65536 ) portL = 0;

    // Connect socket к proxy, handshake по hostname
    sockaddr_in proxyAddr{};
    if ( !ResolveHost( g_state().cfg.host.c_str(), g_state().cfg.port, &proxyAddr ) )
    {
        WSASetLastError( WSAEHOSTUNREACH );
        return FALSE;
    }

    if ( o_connect( s, (const sockaddr*)&proxyAddr, sizeof( proxyAddr ) ) != 0 )
        return FALSE;

    Socks5Status st = Socks5TcpHandshake( s, g_state().cfg,
        nodename, 0, htons( (uint16_t)portL ) );
    if ( st != Socks5Status::OK )
    {
        GetHealthCounters().socks5Fail.fetch_add( 1, std::memory_order_relaxed );
        WSASetLastError( WSAECONNREFUSED );
        return FALSE;
    }
    GetHealthCounters().socks5Ok.fetch_add( 1, std::memory_order_relaxed );

    // remAddr (если просили) — просто напишем fake
    if ( remAddr && remAddrLen && *remAddrLen >= (DWORD)sizeof( sockaddr_in ) )
    {
        sockaddr_in* sa = (sockaddr_in*)remAddr;
        sa->sin_family = AF_INET;
        sa->sin_port = htons( (uint16_t)portL );
        sa->sin_addr.s_addr = AllocFakeIpForHost( nodename );
        *remAddrLen = sizeof( sockaddr_in );
    }

    DbgLog( "WSAConnectByNameA: '%s:%s' OK via proxy", nodename, servicename );
    return TRUE;
}

static BOOL WSAAPI h_WSAConnectByNameW( SOCKET s, LPWSTR nodename, LPWSTR servicename,
    LPDWORD locAddrLen, PSOCKADDR locAddr, LPDWORD remAddrLen, PSOCKADDR remAddr,
    const timeval* tmo, LPWSAOVERLAPPED ov )
{
    char nn[512], sn[64];
    int r1 = WideCharToMultiByte( CP_UTF8, 0, nodename ? nodename : L"", -1, nn, sizeof( nn ), nullptr, nullptr );
    int r2 = WideCharToMultiByte( CP_UTF8, 0, servicename ? servicename : L"", -1, sn, sizeof( sn ), nullptr, nullptr );
    if ( r1 <= 0 || r2 <= 0 )
    {
        if ( o_WSAConnectByNameW )
            return o_WSAConnectByNameW( s, nodename, servicename, locAddrLen, locAddr,
                remAddrLen, remAddr, tmo, ov );
        return FALSE;
    }
    return h_WSAConnectByNameA( s, nn, sn, locAddrLen, locAddr, remAddrLen, remAddr, tmo, ov );
}

// ── closesocket: cleanup UDP session если есть ──
static int WSAAPI h_closesocket( SOCKET s )
{
    // UDP session cleanup — вынесено в hooks_udp для централизации
    extern void CleanupUdpSession( SOCKET );
    CleanupUdpSession( s );
    return o_closesocket( s );
}

// ── Install ──

static bool Hook( LPCSTR mod, LPCSTR fn, LPVOID detour, LPVOID* pOrig )
{
    HMODULE h = GetModuleHandleA( mod );
    if ( !h )
        h = LoadLibraryA( mod );
    if ( !h )
    {
        DbgLog( "Hook: module not loaded: %s", mod );
        return false;
    }

    void* target = GetProcAddress( h, fn );
    if ( !target )
    {
        DbgLog( "Hook: GetProcAddress failed: %s!%s", mod, fn );
        return false;
    }

    MH_STATUS s = MH_CreateHook( target, detour, pOrig );
    if ( s != MH_OK )
    {
        DbgLog( "Hook: MH_CreateHook %s!%s failed: %d", mod, fn, s );
        return false;
    }

    s = MH_EnableHook( target );
    if ( s != MH_OK )
    {
        DbgLog( "Hook: MH_EnableHook %s!%s failed: %d", mod, fn, s );
        return false;
    }
    return true;
}

bool InstallTcpHooks()
{
    // Required — без них нет смысла продолжать
    bool required = true;
    required &= Hook( "ws2_32.dll", "connect",         (LPVOID)h_connect,         (LPVOID*)&o_connect );
    required &= Hook( "ws2_32.dll", "WSAConnect",      (LPVOID)h_WSAConnect,      (LPVOID*)&o_WSAConnect );
    required &= Hook( "ws2_32.dll", "WSAIoctl",        (LPVOID)h_WSAIoctl,        (LPVOID*)&o_WSAIoctl );
    required &= Hook( "ws2_32.dll", "getaddrinfo",     (LPVOID)h_getaddrinfo,     (LPVOID*)&o_getaddrinfo );
    required &= Hook( "ws2_32.dll", "GetAddrInfoW",    (LPVOID)h_GetAddrInfoW,    (LPVOID*)&o_GetAddrInfoW );

    // Optional — могут отсутствовать или быть forwarder'ами на некоторых билдах Windows
    (void)Hook( "ws2_32.dll", "freeaddrinfo",     (LPVOID)h_freeaddrinfo,     (LPVOID*)&o_freeaddrinfo );
    (void)Hook( "ws2_32.dll", "FreeAddrInfoW",    (LPVOID)h_FreeAddrInfoW,    (LPVOID*)&o_FreeAddrInfoW );
    (void)Hook( "ws2_32.dll", "gethostbyname",    (LPVOID)h_gethostbyname,    (LPVOID*)&o_gethostbyname );
    (void)Hook( "ws2_32.dll", "WSAConnectByNameA", (LPVOID)h_WSAConnectByNameA, (LPVOID*)&o_WSAConnectByNameA );
    (void)Hook( "ws2_32.dll", "WSAConnectByNameW", (LPVOID)h_WSAConnectByNameW, (LPVOID*)&o_WSAConnectByNameW );
    (void)Hook( "ws2_32.dll", "closesocket",      (LPVOID)h_closesocket,      (LPVOID*)&o_closesocket );

    DbgLog( "InstallTcpHooks: required=%s", required ? "OK" : "FAILED" );
    return required;
}

// ── Bypass ──

static bool HostMatches( const std::string& pattern, const char* host )
{
    if ( pattern.empty() ) return false;
    // простое substring (loginusers.vdf тоже lowercased, и паттерны тоже)
    std::string lower;
    for ( const char* p = host; *p; p++ )
        lower.push_back( (char)tolower( (unsigned char)*p ) );
    return lower.find( pattern ) != std::string::npos;
}

bool IsBypassHost( const char* hostName )
{
    if ( !hostName || !*hostName )
        return false;
    for ( auto& p : g_state().cfg.bypassHosts )
        if ( HostMatches( p, hostName ) )
            return true;
    // built-in: loopback hosts
    if ( _stricmp( hostName, "localhost" ) == 0 ||
         _strnicmp( hostName, "127.", 4 ) == 0 )
        return true;
    return false;
}

bool IsBypassIpv4( uint32_t ipv4Net )
{
    uint32_t h = ntohl( ipv4Net );
    uint8_t a = ( h >> 24 ) & 0xFF;

    // 127.0.0.0/8 loopback — НО наши fake IP тоже в 127.30.0.0/16.
    // Отличаем: 127.30.x.x и 127.31.x.x резервируем для нас, остальные 127.x.x.x = bypass.
    if ( a == 127 )
    {
        uint8_t b = ( h >> 16 ) & 0xFF;
        if ( b == 30 || b == 31 )
            return false; // наш fake
        return true;      // loopback bypass
    }

    // 0.0.0.0 / INADDR_ANY — bypass (listen bind etc)
    if ( h == 0 )
        return true;

    return false;
}

} // namespace proxyhook
