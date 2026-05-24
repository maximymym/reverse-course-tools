#include "hooks.h"
#include "health.h"

#include <WS2tcpip.h>
#include <MinHook.h>
#include <cstring>

#pragma comment(lib, "ws2_32.lib")

namespace proxyhook
{

// ── Originals ──
using fn_sendto_t      = int (WSAAPI*)( SOCKET, const char*, int, int, const sockaddr*, int );
using fn_WSASendTo_t   = int (WSAAPI*)( SOCKET, LPWSABUF, DWORD, LPDWORD, DWORD, const sockaddr*, int,
                                        LPWSAOVERLAPPED, LPWSAOVERLAPPED_COMPLETION_ROUTINE );
using fn_recvfrom_t    = int (WSAAPI*)( SOCKET, char*, int, int, sockaddr*, int* );
using fn_WSARecvFrom_t = int (WSAAPI*)( SOCKET, LPWSABUF, DWORD, LPDWORD, LPDWORD, sockaddr*, LPINT,
                                        LPWSAOVERLAPPED, LPWSAOVERLAPPED_COMPLETION_ROUTINE );

static fn_sendto_t      o_sendto = nullptr;
static fn_WSASendTo_t   o_WSASendTo = nullptr;
static fn_recvfrom_t    o_recvfrom = nullptr;
static fn_WSARecvFrom_t o_WSARecvFrom = nullptr;

// ── Helpers ──

static bool IsUdpSocket( SOCKET s )
{
    int type = 0; int tlen = sizeof( type );
    if ( getsockopt( s, SOL_SOCKET, SO_TYPE, (char*)&type, &tlen ) != 0 )
        return false;
    return type == SOCK_DGRAM;
}

// Установить или получить UDP session для сокета. Возвращает false если udp
// disabled или proxy не поддерживает UDP ASSOCIATE.
static bool EnsureUdpSession( SOCKET s, HookState::UdpSession& out )
{
    auto& st = g_state();
    if ( !st.cfg.enableUdp )
        return false;

    std::lock_guard<std::mutex> lock( st.udpMutex );
    auto it = st.udpSessions.find( s );
    if ( it != st.udpSessions.end() && it->second.established )
    {
        out = it->second;
        return true;
    }

    // Открываем новую UDP ASSOCIATE session
    Socks5UdpAssocResult r = Socks5UdpAssociate( st.cfg );
    if ( r.status != Socks5Status::OK )
    {
        GetHealthCounters().udpFail.fetch_add( 1, std::memory_order_relaxed );
        DbgLog( "UDP ASSOCIATE failed st=%d (disabling UDP for this socket)", (int)r.status );
        // Пометить сокет как failed чтобы дальше не пытаться
        HookState::UdpSession failed;
        failed.established = false;
        st.udpSessions[s] = failed;
        return false;
    }

    GetHealthCounters().udpOk.fetch_add( 1, std::memory_order_relaxed );
    HookState::UdpSession us;
    us.controlSocket  = r.controlSocket;
    us.relayEndpoint  = r.relayEndpoint;
    us.established    = true;
    st.udpSessions[s] = us;
    out = us;

    DbgLog( "UDP ASSOCIATE OK: relay=%u.%u.%u.%u:%u",
        us.relayEndpoint.sin_addr.S_un.S_un_b.s_b1,
        us.relayEndpoint.sin_addr.S_un.S_un_b.s_b2,
        us.relayEndpoint.sin_addr.S_un.S_un_b.s_b3,
        us.relayEndpoint.sin_addr.S_un.S_un_b.s_b4,
        ntohs( us.relayEndpoint.sin_port ) );
    return true;
}

// Cleanup UDP session при закрытии сокета (вызывается из h_closesocket)
void CleanupUdpSession( SOCKET s )
{
    auto& st = g_state();
    std::lock_guard<std::mutex> lock( st.udpMutex );
    auto it = st.udpSessions.find( s );
    if ( it != st.udpSessions.end() )
    {
        if ( it->second.controlSocket != INVALID_SOCKET )
            closesocket( it->second.controlSocket );
        st.udpSessions.erase( it );
    }
}

// ── sendto / WSASendTo ──

static int WSAAPI h_sendto( SOCKET s, const char* buf, int len, int flags,
    const sockaddr* to, int tolen )
{
    if ( !to || tolen < (int)sizeof( sockaddr_in ) || to->sa_family != AF_INET ||
         !IsUdpSocket( s ) )
        return o_sendto( s, buf, len, flags, to, tolen );

    const sockaddr_in* in4 = (const sockaddr_in*)to;
    uint32_t ipv4Net = in4->sin_addr.s_addr;

    // Bypass: loopback, broadcast, etc
    if ( IsBypassIpv4( ipv4Net ) )
        return o_sendto( s, buf, len, flags, to, tolen );

    HookState::UdpSession us;
    if ( !EnsureUdpSession( s, us ) )
    {
        // UDP relay недоступен — fallback прямая отправка (IP утекает, но хоть что-то)
        return o_sendto( s, buf, len, flags, to, tolen );
    }

    // Wrap в SOCKS5 UDP header
    std::string realHost;
    bool isFake = LookupFakeIp( ipv4Net, realHost );

    uint8_t tmp[65536];
    size_t wrapped = Socks5WrapUdp( tmp, sizeof( tmp ),
        isFake ? realHost.c_str() : nullptr,
        isFake ? 0 : ipv4Net,
        in4->sin_port,
        buf, (size_t)len );
    if ( wrapped == 0 )
    {
        WSASetLastError( WSAEMSGSIZE );
        return SOCKET_ERROR;
    }

    int r = o_sendto( s, (const char*)tmp, (int)wrapped, flags,
        (const sockaddr*)&us.relayEndpoint, (int)sizeof( us.relayEndpoint ) );
    if ( r <= 0 )
        return r;
    // Приложение ожидает что sendto вернёт исходный len данных, не wrapped
    return len;
}

static int WSAAPI h_WSASendTo( SOCKET s, LPWSABUF bufs, DWORD nBufs, LPDWORD pSent,
    DWORD flags, const sockaddr* to, int tolen,
    LPWSAOVERLAPPED ov, LPWSAOVERLAPPED_COMPLETION_ROUTINE cb )
{
    if ( !to || tolen < (int)sizeof( sockaddr_in ) || to->sa_family != AF_INET ||
         !IsUdpSocket( s ) )
        return o_WSASendTo( s, bufs, nBufs, pSent, flags, to, tolen, ov, cb );

    const sockaddr_in* in4 = (const sockaddr_in*)to;
    uint32_t ipv4Net = in4->sin_addr.s_addr;
    if ( IsBypassIpv4( ipv4Net ) )
        return o_WSASendTo( s, bufs, nBufs, pSent, flags, to, tolen, ov, cb );

    HookState::UdpSession us;
    if ( !EnsureUdpSession( s, us ) )
        return o_WSASendTo( s, bufs, nBufs, pSent, flags, to, tolen, ov, cb );

    // Сцепить все буферы в плоский payload
    DWORD total = 0;
    for ( DWORD i = 0; i < nBufs; i++ ) total += bufs[i].len;
    if ( total > 60000 )
    {
        WSASetLastError( WSAEMSGSIZE );
        return SOCKET_ERROR;
    }

    std::string payload;
    payload.reserve( total );
    for ( DWORD i = 0; i < nBufs; i++ )
        payload.append( bufs[i].buf, bufs[i].len );

    std::string realHost;
    bool isFake = LookupFakeIp( ipv4Net, realHost );

    uint8_t tmp[65536];
    size_t wrapped = Socks5WrapUdp( tmp, sizeof( tmp ),
        isFake ? realHost.c_str() : nullptr,
        isFake ? 0 : ipv4Net,
        in4->sin_port,
        payload.data(), payload.size() );
    if ( wrapped == 0 )
    {
        WSASetLastError( WSAEMSGSIZE );
        return SOCKET_ERROR;
    }

    // Чтобы не мучиться с OVERLAPPED wrapping — делаем blocking sendto
    int sent = o_sendto( s, (const char*)tmp, (int)wrapped, (int)flags,
        (const sockaddr*)&us.relayEndpoint, (int)sizeof( us.relayEndpoint ) );
    if ( sent <= 0 )
        return SOCKET_ERROR;

    if ( pSent ) *pSent = total;
    return 0;
}

// ── recvfrom / WSARecvFrom ──

static int WSAAPI h_recvfrom( SOCKET s, char* buf, int len, int flags,
    sockaddr* from, int* fromlen )
{
    if ( !IsUdpSocket( s ) )
        return o_recvfrom( s, buf, len, flags, from, fromlen );

    auto& st = g_state();
    bool hasSession = false;
    {
        std::lock_guard<std::mutex> lock( st.udpMutex );
        auto it = st.udpSessions.find( s );
        hasSession = ( it != st.udpSessions.end() && it->second.established );
    }
    if ( !hasSession )
        return o_recvfrom( s, buf, len, flags, from, fromlen );

    // Принимаем как есть от относительного recvfrom, потом unwrap
    uint8_t tmp[65536];
    sockaddr_in fromRelay{};
    int flen = sizeof( fromRelay );
    int r = o_recvfrom( s, (char*)tmp, (int)sizeof( tmp ), flags,
        (sockaddr*)&fromRelay, &flen );
    if ( r <= 0 )
        return r;

    char hostOut[256] = {};
    uint32_t ipv4 = 0;
    uint16_t portNet = 0;
    size_t payloadOff = 0, payloadLen = 0;

    size_t hdrLen = Socks5UnwrapUdp( tmp, (size_t)r,
        hostOut, &ipv4, &portNet, &payloadOff, &payloadLen );
    if ( hdrLen == 0 )
    {
        // Not a SOCKS5 wrapped packet — return as-is (разумно для loopback bypass)
        if ( r > len ) r = len;
        memcpy( buf, tmp, r );
        if ( from && fromlen && *fromlen >= (int)sizeof( sockaddr_in ) )
        {
            memcpy( from, &fromRelay, sizeof( fromRelay ) );
            *fromlen = sizeof( sockaddr_in );
        }
        return r;
    }

    size_t copyLen = ( payloadLen > (size_t)len ) ? (size_t)len : payloadLen;
    memcpy( buf, tmp + payloadOff, copyLen );

    if ( from && fromlen && *fromlen >= (int)sizeof( sockaddr_in ) )
    {
        sockaddr_in* sa = (sockaddr_in*)from;
        memset( sa, 0, sizeof( *sa ) );
        sa->sin_family = AF_INET;
        sa->sin_port = portNet;
        if ( hostOut[0] )
        {
            // Смотрим — это наш fake или новый hostname. Для возврата
            // приложению нужен IP, так что выделяем fake IP.
            sa->sin_addr.s_addr = AllocFakeIpForHost( hostOut );
        }
        else
        {
            sa->sin_addr.s_addr = ipv4;
        }
        *fromlen = sizeof( sockaddr_in );
    }

    return (int)copyLen;
}

static int WSAAPI h_WSARecvFrom( SOCKET s, LPWSABUF bufs, DWORD nBufs, LPDWORD pRecvd,
    LPDWORD pFlags, sockaddr* from, LPINT fromlen,
    LPWSAOVERLAPPED ov, LPWSAOVERLAPPED_COMPLETION_ROUTINE cb )
{
    if ( !IsUdpSocket( s ) )
        return o_WSARecvFrom( s, bufs, nBufs, pRecvd, pFlags, from, fromlen, ov, cb );

    auto& st = g_state();
    bool hasSession = false;
    {
        std::lock_guard<std::mutex> lock( st.udpMutex );
        auto it = st.udpSessions.find( s );
        hasSession = ( it != st.udpSessions.end() && it->second.established );
    }
    if ( !hasSession )
        return o_WSARecvFrom( s, bufs, nBufs, pRecvd, pFlags, from, fromlen, ov, cb );

    // Принимаем в свой буфер, unwrap'аем, копируем в WSABUFs
    uint8_t tmp[65536];
    sockaddr_in fromRelay{};
    int flen = sizeof( fromRelay );
    int r = o_recvfrom( s, (char*)tmp, (int)sizeof( tmp ),
        pFlags ? (int)*pFlags : 0,
        (sockaddr*)&fromRelay, &flen );
    if ( r <= 0 )
        return ( r == 0 ) ? 0 : SOCKET_ERROR;

    char hostOut[256] = {};
    uint32_t ipv4 = 0;
    uint16_t portNet = 0;
    size_t payloadOff = 0, payloadLen = 0;

    size_t hdrLen = Socks5UnwrapUdp( tmp, (size_t)r,
        hostOut, &ipv4, &portNet, &payloadOff, &payloadLen );
    if ( hdrLen == 0 )
    {
        // Raw — скопируем как есть
        DWORD copied = 0;
        const uint8_t* p = tmp;
        DWORD rem = (DWORD)r;
        for ( DWORD i = 0; i < nBufs && rem > 0; i++ )
        {
            DWORD n = ( bufs[i].len < rem ) ? bufs[i].len : rem;
            memcpy( bufs[i].buf, p, n );
            p += n; copied += n; rem -= n;
        }
        if ( pRecvd ) *pRecvd = copied;
        if ( from && fromlen && *fromlen >= (int)sizeof( sockaddr_in ) )
        {
            memcpy( from, &fromRelay, sizeof( fromRelay ) );
            *fromlen = sizeof( sockaddr_in );
        }
        return 0;
    }

    DWORD copied = 0;
    const uint8_t* p = tmp + payloadOff;
    DWORD rem = (DWORD)payloadLen;
    for ( DWORD i = 0; i < nBufs && rem > 0; i++ )
    {
        DWORD n = ( bufs[i].len < rem ) ? bufs[i].len : rem;
        memcpy( bufs[i].buf, p, n );
        p += n; copied += n; rem -= n;
    }
    if ( pRecvd ) *pRecvd = copied;

    if ( from && fromlen && *fromlen >= (int)sizeof( sockaddr_in ) )
    {
        sockaddr_in* sa = (sockaddr_in*)from;
        memset( sa, 0, sizeof( *sa ) );
        sa->sin_family = AF_INET;
        sa->sin_port = portNet;
        if ( hostOut[0] )
            sa->sin_addr.s_addr = AllocFakeIpForHost( hostOut );
        else
            sa->sin_addr.s_addr = ipv4;
        *fromlen = sizeof( sockaddr_in );
    }

    return 0;
}

// ── Install ──

static bool HookUdp( LPCSTR fn, LPVOID detour, LPVOID* pOrig )
{
    HMODULE h = GetModuleHandleA( "ws2_32.dll" );
    if ( !h ) return false;
    void* target = GetProcAddress( h, fn );
    if ( !target )
    {
        DbgLog( "HookUdp: proc not found: %s", fn );
        return false;
    }
    MH_STATUS st = MH_CreateHook( target, detour, pOrig );
    if ( st != MH_OK )
    {
        DbgLog( "HookUdp: MH_CreateHook %s = %d", fn, st );
        return false;
    }
    st = MH_EnableHook( target );
    if ( st != MH_OK )
    {
        DbgLog( "HookUdp: MH_EnableHook %s = %d", fn, st );
        return false;
    }
    return true;
}

bool InstallUdpHooks()
{
    if ( !g_state().cfg.enableUdp )
    {
        DbgLog( "InstallUdpHooks: UDP disabled by config" );
        return true; // не ошибка
    }

    bool ok = true;
    ok &= HookUdp( "sendto",      (LPVOID)h_sendto,      (LPVOID*)&o_sendto );
    ok &= HookUdp( "WSASendTo",   (LPVOID)h_WSASendTo,   (LPVOID*)&o_WSASendTo );
    ok &= HookUdp( "recvfrom",    (LPVOID)h_recvfrom,    (LPVOID*)&o_recvfrom );
    ok &= HookUdp( "WSARecvFrom", (LPVOID)h_WSARecvFrom, (LPVOID*)&o_WSARecvFrom );
    DbgLog( "InstallUdpHooks: %s", ok ? "OK" : "FAILED" );
    return ok;
}

} // namespace proxyhook
