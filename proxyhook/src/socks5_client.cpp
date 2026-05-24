#include "socks5_client.h"
#include <WS2tcpip.h>
#include <cstring>

#pragma comment(lib, "ws2_32.lib")

namespace proxyhook
{

// Читает ровно N байт. Использует select() с timeout перед каждым recv —
// работает даже для overlapped сокетов (Steam/Dota их используют), где обычный
// blocking recv() мгновенно возвращает WSAEWOULDBLOCK если данных ещё нет.
static bool RecvAll( SOCKET s, void* buf, size_t n )
{
    char* p = (char*)buf;
    size_t got = 0;
    while ( got < n )
    {
        // Ждём пока появятся данные (max 15s)
        fd_set rfd;
        FD_ZERO( &rfd );
        FD_SET( s, &rfd );
        timeval tv{ 15, 0 };
        int sr = select( 0, &rfd, nullptr, nullptr, &tv );
        if ( sr != 1 )
            return false; // timeout or error

        int r = recv( s, p + got, (int)( n - got ), 0 );
        if ( r > 0 )
        {
            got += (size_t)r;
            continue;
        }
        if ( r == 0 )
            return false; // peer closed
        int e = WSAGetLastError();
        if ( e == WSAEWOULDBLOCK || e == WSAEINTR )
            continue; // спин, но select уже подождал — редкий случай
        return false;
    }
    return true;
}

// Отправляет ровно N байт. Для overlapped сокетов send() может возвращать
// WSAEWOULDBLOCK когда TCP buffer полон — делаем select() на запись.
static bool SendAll( SOCKET s, const void* buf, size_t n )
{
    const char* p = (const char*)buf;
    size_t sent = 0;
    while ( sent < n )
    {
        int r = send( s, p + sent, (int)( n - sent ), 0 );
        if ( r > 0 )
        {
            sent += (size_t)r;
            continue;
        }
        if ( r == 0 )
            return false;
        int e = WSAGetLastError();
        if ( e == WSAEWOULDBLOCK || e == WSAEINTR )
        {
            fd_set wfd;
            FD_ZERO( &wfd );
            FD_SET( s, &wfd );
            timeval tv{ 15, 0 };
            if ( select( 0, nullptr, &wfd, nullptr, &tv ) != 1 )
                return false;
            continue;
        }
        return false;
    }
    return true;
}

bool ResolveHost( const char* hostName, uint16_t portHostOrder, sockaddr_in* out )
{
    memset( out, 0, sizeof( *out ) );
    out->sin_family = AF_INET;
    out->sin_port = htons( portHostOrder );

    // Сначала пробуем как literal IPv4
    IN_ADDR ia{};
    if ( inet_pton( AF_INET, hostName, &ia ) == 1 )
    {
        out->sin_addr = ia;
        return true;
    }

    // DNS lookup. ВАЖНО: getaddrinfo у нас захукан, но в DllMain пока hook не установлен,
    // и на прокси мы обычно передаём литеральный IP. Всё равно fallback через getaddrinfo.
    ADDRINFOA hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    ADDRINFOA* res = nullptr;
    if ( getaddrinfo( hostName, nullptr, &hints, &res ) != 0 || !res )
        return false;

    sockaddr_in* first = (sockaddr_in*)res->ai_addr;
    out->sin_addr = first->sin_addr;
    freeaddrinfo( res );
    return true;
}

SOCKET ConnectTcp( const sockaddr_in& addr, int timeoutMs )
{
    SOCKET s = socket( AF_INET, SOCK_STREAM, IPPROTO_TCP );
    if ( s == INVALID_SOCKET )
        return INVALID_SOCKET;

    // Non-blocking для connect с timeout
    u_long nb = 1;
    ioctlsocket( s, FIONBIO, &nb );

    int r = connect( s, (const sockaddr*)&addr, sizeof( addr ) );
    if ( r == SOCKET_ERROR && WSAGetLastError() != WSAEWOULDBLOCK )
    {
        closesocket( s );
        return INVALID_SOCKET;
    }

    fd_set wfd;
    FD_ZERO( &wfd );
    FD_SET( s, &wfd );
    timeval tv{};
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = ( timeoutMs % 1000 ) * 1000;

    r = select( 0, nullptr, &wfd, nullptr, &tv );
    if ( r != 1 )
    {
        closesocket( s );
        return INVALID_SOCKET;
    }

    int soErr = 0;
    int soErrLen = sizeof( soErr );
    if ( getsockopt( s, SOL_SOCKET, SO_ERROR, (char*)&soErr, &soErrLen ) != 0 || soErr != 0 )
    {
        closesocket( s );
        return INVALID_SOCKET;
    }

    // back to blocking
    nb = 0;
    ioctlsocket( s, FIONBIO, &nb );

    // Таймауты на recv/send чтобы ничего не висло навечно
    DWORD tmo = 15000;
    setsockopt( s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tmo, sizeof( tmo ) );
    setsockopt( s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tmo, sizeof( tmo ) );

    return s;
}

// ── Greeting + auth ──

static Socks5Status Greeting( SOCKET s, const ProxyConfig& cfg )
{
    // Methods: 0x00 (no auth) + 0x02 (userpass) если есть креды
    bool offerUserpass = !cfg.username.empty();
    uint8_t req[4];
    req[0] = 0x05; // VER
    req[1] = offerUserpass ? 2 : 1;
    req[2] = 0x00;
    req[3] = 0x02;

    size_t reqLen = offerUserpass ? 4 : 3;
    if ( !SendAll( s, req, reqLen ) )
    {
        extern void DbgLog( const char* fmt, ... );
        DbgLog( "socks5 greeting: send failed (wsa=%d)", WSAGetLastError() );
        return Socks5Status::GreetingFailed;
    }

    uint8_t resp[2];
    if ( !RecvAll( s, resp, 2 ) )
    {
        extern void DbgLog( const char* fmt, ... );
        DbgLog( "socks5 greeting: recv failed (wsa=%d)", WSAGetLastError() );
        return Socks5Status::GreetingFailed;
    }

    if ( resp[0] != 0x05 )
        return Socks5Status::ProtocolError;

    uint8_t method = resp[1];
    if ( method == 0xFF )
        return Socks5Status::AuthRequired;

    if ( method == 0x00 )
        return Socks5Status::OK;

    if ( method == 0x02 )
    {
        // RFC 1929 userpass
        if ( cfg.username.empty() )
            return Socks5Status::AuthRequired;

        uint8_t userLen = (uint8_t)( cfg.username.size() > 255 ? 255 : cfg.username.size() );
        uint8_t passLen = (uint8_t)( cfg.password.size() > 255 ? 255 : cfg.password.size() );

        std::string pkt;
        pkt.push_back( (char)0x01 );         // VER (auth subneg)
        pkt.push_back( (char)userLen );
        pkt.append( cfg.username.data(), userLen );
        pkt.push_back( (char)passLen );
        pkt.append( cfg.password.data(), passLen );

        if ( !SendAll( s, pkt.data(), pkt.size() ) )
            return Socks5Status::IoError;

        uint8_t authResp[2];
        if ( !RecvAll( s, authResp, 2 ) )
            return Socks5Status::IoError;

        if ( authResp[0] != 0x01 )
            return Socks5Status::ProtocolError;
        if ( authResp[1] != 0x00 )
            return Socks5Status::AuthFailed;

        return Socks5Status::OK;
    }

    return Socks5Status::AuthRequired;
}

// Парсит и отбрасывает BND.ADDR / BND.PORT ответа CONNECT/UDP_ASSOCIATE.
// Возвращает true если успешно, и пишет BND endpoint в bndOut (если != NULL).
static bool ReadBoundAddr( SOCKET s, sockaddr_in* bndOut )
{
    extern void DbgLog( const char* fmt, ... );
    uint8_t hdr[4];
    if ( !RecvAll( s, hdr, 4 ) )
    {
        DbgLog( "socks5 reply: recv header failed (wsa=%d)", WSAGetLastError() );
        return false;
    }
    // hdr[0]=VER, hdr[1]=REP, hdr[2]=RSV, hdr[3]=ATYP
    if ( hdr[0] != 0x05 )
    {
        DbgLog( "socks5 reply: bad VER=0x%02X", hdr[0] );
        return false;
    }
    if ( hdr[1] != 0x00 )
    {
        // REP codes: 0=OK, 1=general fail, 2=not allowed, 3=net unreach,
        // 4=host unreach, 5=conn refused, 6=TTL expired, 7=cmd not supp, 8=atyp not supp
        static const char* repNames[] = {
            "OK", "general-fail", "not-allowed", "net-unreach",
            "host-unreach", "conn-refused", "ttl-expired", "cmd-not-supp", "atyp-not-supp"
        };
        const char* name = ( hdr[1] < 9 ) ? repNames[hdr[1]] : "unknown";
        DbgLog( "socks5 reply: REP=0x%02X (%s) ATYP=0x%02X", hdr[1], name, hdr[3] );
        return false; // REP != success
    }

    uint32_t ipv4Net = 0;
    uint16_t portNet = 0;
    switch ( hdr[3] )
    {
    case 0x01: // IPv4
    {
        uint8_t addr[4];
        if ( !RecvAll( s, addr, 4 ) ) return false;
        memcpy( &ipv4Net, addr, 4 );
        break;
    }
    case 0x03: // Domain name
    {
        uint8_t dlen = 0;
        if ( !RecvAll( s, &dlen, 1 ) ) return false;
        char tmp[256];
        if ( dlen > 0 && !RecvAll( s, tmp, dlen ) ) return false;
        break;
    }
    case 0x04: // IPv6
    {
        uint8_t addr6[16];
        if ( !RecvAll( s, addr6, 16 ) ) return false;
        break;
    }
    default:
        return false;
    }

    uint8_t portBuf[2];
    if ( !RecvAll( s, portBuf, 2 ) ) return false;
    memcpy( &portNet, portBuf, 2 );

    if ( bndOut )
    {
        memset( bndOut, 0, sizeof( *bndOut ) );
        bndOut->sin_family = AF_INET;
        bndOut->sin_addr.s_addr = ipv4Net;
        bndOut->sin_port = portNet;
    }
    return true;
}

// ── TCP CONNECT ──

Socks5Status Socks5TcpHandshake(
    SOCKET proxySock,
    const ProxyConfig& cfg,
    const char* dstHost,
    uint32_t dstIpv4Net,
    uint16_t dstPortNet )
{
    Socks5Status g = Greeting( proxySock, cfg );
    if ( g != Socks5Status::OK )
        return g;

    // CONNECT request
    std::string req;
    req.push_back( (char)0x05 ); // VER
    req.push_back( (char)0x01 ); // CMD=CONNECT
    req.push_back( (char)0x00 ); // RSV

    if ( dstHost && *dstHost )
    {
        size_t hlen = strlen( dstHost );
        if ( hlen > 255 ) hlen = 255;
        req.push_back( (char)0x03 ); // ATYP=DOMAIN
        req.push_back( (char)hlen );
        req.append( dstHost, hlen );
    }
    else
    {
        req.push_back( (char)0x01 ); // ATYP=IPv4
        req.append( (const char*)&dstIpv4Net, 4 );
    }
    req.append( (const char*)&dstPortNet, 2 );

    if ( !SendAll( proxySock, req.data(), req.size() ) )
        return Socks5Status::IoError;

    if ( !ReadBoundAddr( proxySock, nullptr ) )
        return Socks5Status::ConnectRequestFailed;

    return Socks5Status::OK;
}

// ── UDP ASSOCIATE ──

Socks5UdpAssocResult Socks5UdpAssociate( const ProxyConfig& cfg )
{
    Socks5UdpAssocResult r;

    sockaddr_in pxAddr{};
    if ( !ResolveHost( cfg.host.c_str(), cfg.port, &pxAddr ) )
    {
        r.status = Socks5Status::ProxyConnectFailed;
        return r;
    }

    SOCKET ctrl = ConnectTcp( pxAddr, 10000 );
    if ( ctrl == INVALID_SOCKET )
    {
        r.status = Socks5Status::ProxyConnectFailed;
        return r;
    }

    Socks5Status g = Greeting( ctrl, cfg );
    if ( g != Socks5Status::OK )
    {
        closesocket( ctrl );
        r.status = g;
        return r;
    }

    // UDP ASSOCIATE request — DST.ADDR/PORT = 0 (мы не знаем свой source port)
    uint8_t req[10] = {
        0x05, 0x03, 0x00, 0x01,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00
    };
    if ( !SendAll( ctrl, req, 10 ) )
    {
        closesocket( ctrl );
        r.status = Socks5Status::IoError;
        return r;
    }

    sockaddr_in bnd{};
    if ( !ReadBoundAddr( ctrl, &bnd ) )
    {
        closesocket( ctrl );
        r.status = Socks5Status::ConnectRequestFailed;
        return r;
    }

    // Если proxy вернул 0.0.0.0 как BND.ADDR — используем сам IP proxy
    if ( bnd.sin_addr.s_addr == 0 )
        bnd.sin_addr = pxAddr.sin_addr;

    r.status = Socks5Status::OK;
    r.controlSocket = ctrl;
    r.relayEndpoint = bnd;
    return r;
}

// ── UDP wrap / unwrap ──

size_t Socks5WrapUdp(
    uint8_t* buf, size_t bufLen,
    const char* dstHost,
    uint32_t dstIpv4Net,
    uint16_t dstPortNet,
    const void* data, size_t dataLen )
{
    size_t hdrLen = 0;
    if ( dstHost && *dstHost )
        hdrLen = 4 /*RSV FRAG ATYP DLEN*/ + strlen( dstHost ) + 2;
    else
        hdrLen = 3 /*RSV FRAG ATYP*/ + 4 + 2;

    if ( bufLen < hdrLen + dataLen )
        return 0;

    size_t o = 0;
    buf[o++] = 0x00;
    buf[o++] = 0x00;
    buf[o++] = 0x00; // FRAG=0

    if ( dstHost && *dstHost )
    {
        size_t hl = strlen( dstHost );
        if ( hl > 255 ) return 0;
        buf[o++] = 0x03;          // ATYP=DOMAIN
        buf[o++] = (uint8_t)hl;
        memcpy( buf + o, dstHost, hl ); o += hl;
    }
    else
    {
        buf[o++] = 0x01;          // ATYP=IPv4
        memcpy( buf + o, &dstIpv4Net, 4 ); o += 4;
    }
    memcpy( buf + o, &dstPortNet, 2 ); o += 2;

    memcpy( buf + o, data, dataLen ); o += dataLen;
    return o;
}

size_t Socks5UnwrapUdp(
    const uint8_t* buf, size_t bufLen,
    char* hostOut,
    uint32_t* ipv4NetOut,
    uint16_t* portNetOut,
    size_t* payloadOffOut,
    size_t* payloadLenOut )
{
    if ( bufLen < 4 ) return 0;
    if ( buf[0] != 0 || buf[1] != 0 ) return 0; // RSV
    // buf[2] = FRAG. Мы не поддерживаем FRAG != 0 — отбрасываем.
    if ( buf[2] != 0 ) return 0;

    uint8_t atyp = buf[3];
    size_t o = 4;

    if ( hostOut ) hostOut[0] = 0;
    if ( ipv4NetOut ) *ipv4NetOut = 0;

    switch ( atyp )
    {
    case 0x01: // IPv4
    {
        if ( bufLen < o + 4 + 2 ) return 0;
        if ( ipv4NetOut )
            memcpy( ipv4NetOut, buf + o, 4 );
        o += 4;
        break;
    }
    case 0x03: // DOMAIN
    {
        if ( bufLen < o + 1 ) return 0;
        uint8_t dl = buf[o++];
        if ( bufLen < o + dl + 2 ) return 0;
        if ( hostOut && dl < 256 )
        {
            memcpy( hostOut, buf + o, dl );
            hostOut[dl] = 0;
        }
        o += dl;
        break;
    }
    case 0x04: // IPv6 — skip
    {
        if ( bufLen < o + 16 + 2 ) return 0;
        o += 16;
        break;
    }
    default:
        return 0;
    }

    if ( portNetOut )
        memcpy( portNetOut, buf + o, 2 );
    o += 2;

    if ( payloadOffOut ) *payloadOffOut = o;
    if ( payloadLenOut ) *payloadLenOut = bufLen - o;
    return o;
}

} // namespace proxyhook
