#pragma once

#include <WinSock2.h>
#include <cstdint>
#include <string>
#include "config.h"

namespace proxyhook
{

// Результат handshake
enum class Socks5Status
{
    OK = 0,
    ProxyConnectFailed,
    GreetingFailed,
    AuthRequired,
    AuthFailed,
    ConnectRequestFailed,
    ProtocolError,
    IoError,
};

struct Socks5UdpAssocResult
{
    Socks5Status status = Socks5Status::IoError;
    // TCP control channel — приложение должно держать его открытым
    // пока использует UDP relay. При закрытии proxy обрывает UDP relay.
    SOCKET       controlSocket = INVALID_SOCKET;
    // Endpoint куда приложение шлёт UDP datagrams (BND.ADDR / BND.PORT)
    sockaddr_in  relayEndpoint{};
};

// Выполнить полный handshake на уже открытом и подключённом к proxy TCP сокете:
//   - greeting (methods)
//   - userpass auth если требуется
//   - CONNECT request к dstAddr/dstPort (или по hostname если dstHost != nullptr)
// После успеха data flows обычным send/recv через этот же сокет.
//
// Если dstHost != nullptr — ATYP=0x03 (domain name), IPv4 игнорируется.
// Иначе dstIpv4Net = network-order ipv4, ATYP=0x01.
Socks5Status Socks5TcpHandshake(
    SOCKET                     proxySock,
    const ProxyConfig&         cfg,
    const char*                dstHost,        // NULL для IPv4
    uint32_t                   dstIpv4Net,
    uint16_t                   dstPortNet );

// UDP ASSOCIATE через указанный proxy. Возвращает control socket + relay endpoint.
// На вход — ProxyConfig (где host:port прокси). Функция сама резолвит и коннектит.
// Blocking, но с SO_RCVTIMEO = 10s.
Socks5UdpAssocResult Socks5UdpAssociate( const ProxyConfig& cfg );

// Обёртка UDP datagram в SOCKS5 UDP header:
//   +----+------+------+----------+----------+----------+
//   |RSV | FRAG | ATYP | DST.ADDR | DST.PORT |   DATA   |
//   +----+------+------+----------+----------+----------+
//     2    1      1      4/16/N      2         N
// Возвращает размер записанных байт (0 при ошибке). buf должен быть размером
// >= dataLen + 22.
size_t Socks5WrapUdp(
    uint8_t*      buf,
    size_t        bufLen,
    const char*   dstHost,       // NULL для IPv4
    uint32_t      dstIpv4Net,
    uint16_t      dstPortNet,
    const void*   data,
    size_t        dataLen );

// Распаковка SOCKS5 UDP header из принятого datagram. На выход пишет
// destHost (если ATYP=0x03), destIpv4/Port, и указатель на payload.
// Возвращает offset данных от начала buf (0 при ошибке).
size_t Socks5UnwrapUdp(
    const uint8_t* buf,
    size_t         bufLen,
    char*          hostOut,      // buffer >= 256
    uint32_t*      ipv4NetOut,
    uint16_t*      portNetOut,
    size_t*        payloadOffOut,
    size_t*        payloadLenOut );

// Утилиты
bool ResolveHost( const char* hostName, uint16_t portHostOrder, sockaddr_in* out );
SOCKET ConnectTcp( const sockaddr_in& addr, int timeoutMs );

} // namespace proxyhook
