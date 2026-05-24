#pragma once

#include <Windows.h>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <unordered_map>

#include "windivert/WinDivert.h"

namespace proxydivert
{

// Public lookup result — used by ProxyRelay to recover (origDst, pid, origSrc)
// from (clientPort) of an accepted relay socket.
struct TcpNatLookupResult
{
    bool      found = false;
    uint32_t  origSrcIp = 0;     // network order — оригинальный src IP клиента (10.8.1.2 etc.)
    uint32_t  origDstIp = 0;     // network order
    uint16_t  origDstPort = 0;   // network order
    uint32_t  pid = 0;
};

// 4-tuple NAT для TCP. Outbound пакет watched PID'а перенаправляется на
// 127.0.0.1:relayPort, при этом original (dstIp, dstPort) сохраняется по
// ключу (clientIp, clientPort).
//
// Inbound пакет от relay'а имеет:
//   - srcIp = 127.0.0.1, srcPort = relayPort
//   - dstIp = clientIp, dstPort = clientPort
// Чтобы клиентский TCP стек принял ответ как от реального сервера, переписываем:
//   - srcIp = origDstIp, srcPort = origDstPort
class TcpNat
{
public:
    explicit TcpNat(uint16_t relayPort);

    // Outbound: меняем src→127.0.0.1, dst→127.0.0.1:relayPort. Сохраняем
    // mapping (clientPort) → (origSrc, origDst, ifIdx, pid). Caller должен
    // выставить addr.Loopback=1 после этого.
    bool OnOutbound(WINDIVERT_IPHDR* ip, WINDIVERT_TCPHDR* tcp,
                    uint32_t pid, uint32_t ifIdx);

    // Reply от relay (loopback): src=127.0.0.1:relayPort, dst=127.0.0.1:clientPort.
    // Меняем src→origDst, dst→origSrc. Caller должен выставить addr.Outbound=0,
    // addr.Loopback=0, addr.Network.IfIdx=GetSavedIfIdx(...).
    bool OnInbound(WINDIVERT_IPHDR* ip, WINDIVERT_TCPHDR* tcp,
                   uint32_t* outIfIdx);

    // Lookup по (clientIp, clientPort) — для ProxyRelay при accept'е.
    TcpNatLookupResult Lookup(uint32_t clientIpNet, uint16_t clientPortNet) const;

    size_t TrackedFlowCount() const;

    // GC старых mapping'ов (idle > 5 мин). Можно дёргать каждый recv tick —
    // внутри throttle 60 сек.
    void GarbageCollect();

    uint16_t RelayPortNet() const { return m_relayPortNet; }

private:
    // Ключ — clientPort (network order). Это уникально per-host. SrcIp у нас
    // меняется на 127.0.0.1, так что не надо его в ключ.
    struct FlowEntry
    {
        uint32_t origSrcIp = 0;       // network order — 10.8.1.2 etc.
        uint32_t origDstIp = 0;
        uint16_t origDstPort = 0;
        uint32_t pid = 0;
        uint32_t ifIdx = 0;            // оригинальный outbound interface
        std::chrono::steady_clock::time_point lastSeen;
    };

    uint16_t m_relayPortNet;  // network order

    mutable std::mutex m_mu;
    std::unordered_map<uint16_t, FlowEntry> m_flows;  // key = clientPort (net order)
    std::chrono::steady_clock::time_point m_lastGc;
};

// Глобальный singleton — DivertEngine выставляет при старте, ProxyRelay
// читает при accept. Обёртка нужна потому что они в разных translation units
// и без зависимости от DivertEngine.
void SetGlobalTcpNat(TcpNat* nat);
TcpNatLookupResult TcpNatLookup(uint32_t clientIpNet, uint16_t clientPortNet);

} // namespace proxydivert
