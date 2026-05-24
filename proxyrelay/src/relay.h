#pragma once

#include <WinSock2.h>
#include <Windows.h>
#include <atomic>
#include <cstdint>
#include <functional>
#include <thread>

#include "pid_proxy_map.h"

namespace proxyrelay
{

using LogFn = std::function<void(const char* msg)>;

// Hook чтобы относительно loose-связать relay с DivertEngine: relay при
// accept'е zовёт LookupTcpFn(clientIp, clientPort) → (origDstIp, origDstPort, pid).
// Реализация — DivertEngine отдаёт указатель на свой TcpNat::Lookup wrapper.
using LookupTcpFn = std::function<bool(uint32_t clientIpNet, uint16_t clientPortNet,
                                       uint32_t* origDstIp, uint16_t* origDstPort,
                                       uint32_t* pid)>;

using LookupUdpFn = std::function<bool(uint32_t clientIpNet, uint16_t clientPortNet,
                                       uint32_t* origDstIp, uint16_t* origDstPort,
                                       uint32_t* pid)>;

class ProxyRelay
{
public:
    struct Config
    {
        // Слушаем 127.0.0.1:relayTcpPort (если 0 — auto-bind, читай из GetTcpPort()).
        uint16_t requestedTcpPort = 0;
        uint16_t requestedUdpPort = 0;
    };

    ProxyRelay();
    ~ProxyRelay();

    void SetLogger(LogFn fn);
    void SetTcpLookup(LookupTcpFn fn);
    void SetUdpLookup(LookupUdpFn fn);

    // Стартует listener'ы. После успеха — GetTcpPort() / GetUdpPort() возвращают
    // фактические порты (auto-bind).
    bool Start(const Config& cfg);
    void Stop();
    bool IsRunning() const { return m_running.load(); }

    uint16_t GetTcpPort() const { return m_tcpPort; }
    uint16_t GetUdpPort() const { return m_udpPort; }

    // Stats (для GUI Network panel)
    uint64_t TcpAccepted() const { return m_tcpAccepted.load(); }
    uint64_t TcpHandshakeOk() const { return m_tcpHandshakeOk.load(); }
    uint64_t TcpHandshakeFail() const { return m_tcpHandshakeFail.load(); }
    uint64_t UdpDatagramsIn() const { return m_udpDatagramsIn.load(); }
    uint64_t UdpDatagramsOut() const { return m_udpDatagramsOut.load(); }

private:
    void TcpAcceptLoop();
    void UdpRecvLoop();
    void HandleTcpClient(SOCKET clientSock,
        uint32_t clientIpNet, uint16_t clientPortNet);
    void Log(const char* fmt, ...);

    SOCKET m_tcpListen = INVALID_SOCKET;
    SOCKET m_udpListen = INVALID_SOCKET;
    uint16_t m_tcpPort = 0;
    uint16_t m_udpPort = 0;

    std::atomic<bool> m_running{ false };
    std::thread m_tcpThread;
    std::thread m_udpThread;

    LogFn       m_logger;
    LookupTcpFn m_lookupTcp;
    LookupUdpFn m_lookupUdp;

    std::atomic<uint64_t> m_tcpAccepted{ 0 };
    std::atomic<uint64_t> m_tcpHandshakeOk{ 0 };
    std::atomic<uint64_t> m_tcpHandshakeFail{ 0 };
    std::atomic<uint64_t> m_udpDatagramsIn{ 0 };
    std::atomic<uint64_t> m_udpDatagramsOut{ 0 };
};

} // namespace proxyrelay
