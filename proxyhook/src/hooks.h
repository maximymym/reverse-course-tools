#pragma once

#include <WinSock2.h>
#include <Windows.h>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

#include "config.h"
#include "socks5_client.h"

namespace proxyhook
{

// ── Глобальное состояние загруженного hook ──
struct HookState
{
    bool         initialized = false;
    ProxyConfig  cfg;
    HwidConfig   hwid;

    // Fake IP mapping для remote-DNS. Для каждого резолвнутого hostname
    // возвращаем фейковый 127.30.x.x IP. В connect() hook при виде такого IP
    // смотрим mapping и отправляем CONNECT по hostname (ATYP=0x03).
    //
    // 127.30.0.1 .. 127.30.255.254 ~ 65k mappings. После выхода за предел —
    // wrap around (существующие mappings перетираются, но Steam rarely держит
    // 65k одновременных резолвов).
    std::mutex                                    fakeIpMutex;
    std::unordered_map<uint32_t, std::string>     fakeIpToHost;     // key = network-order IPv4
    std::unordered_map<std::string, uint32_t>     hostToFakeIp;
    uint32_t                                      nextFakeIpCounter = 1;

    // Socket registry для UDP — сохраняем UDP ASSOCIATE sessions per-socket.
    // Каждый SOCK_DGRAM socket при первом sendto получает свой control channel
    // + relay endpoint. Cleanup в closesocket.
    struct UdpSession
    {
        SOCKET      controlSocket = INVALID_SOCKET;
        sockaddr_in relayEndpoint{};
        bool        established = false;
    };
    std::mutex                              udpMutex;
    std::unordered_map<SOCKET, UdpSession>  udpSessions;

    // Путь до ProxyHook.dll (из GetModuleFileName) — нужен hooks_process
    // для inject в child-процессы.
    std::string  selfDllPath;
};

HookState& g_state();

// Получить (или создать) fake IP для hostname. Возвращает network-order IP.
// (legacy — fake IP strategy, не используется при real-IP режиме.)
uint32_t AllocFakeIpForHost( const std::string& host );

// Посмотреть — это наш fake IP или нет. Если да — вернёт real hostname.
bool LookupFakeIp( uint32_t ipv4Net, std::string& hostOut );

// Real IP → hostname mapping. Заполняется в h_getaddrinfo из native resolver.
// h_connect использует для SOCKS5 CONNECT через hostname (ATYP=DOMAIN) когда
// возможно — тогда DNS резолвится на proxy side, если не возможно — по IPv4.
void RegisterIpToHost( uint32_t ipv4Net, const std::string& host );
bool LookupIpHost( uint32_t ipv4Net, std::string& hostOut );

// Установить все хуки (TCP + UDP + process). Вызывается из DllMain.
bool InstallAllHooks();
void UninstallAllHooks();

// Hook installers (явно разделены, вызываются из InstallAllHooks в нужном порядке)
bool InstallTcpHooks();
bool InstallUdpHooks();
bool InstallProcessHooks();
// HWID per-process spoof (registry + network + storage + SMBIOS).
// No-op если g_state().hwid.enabled=false.
bool InstallHwidHooks();

// Bypass check: host попадает в whitelist?
bool IsBypassHost( const char* hostName );
bool IsBypassIpv4( uint32_t ipv4Net );

} // namespace proxyhook
