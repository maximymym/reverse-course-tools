#pragma once

#include <Windows.h>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <unordered_map>

#include "windivert/WinDivert.h"

namespace proxydivert
{

struct UdpNatLookupResult
{
    bool      found = false;
    uint32_t  origSrcIp = 0;
    uint32_t  origDstIp = 0;
    uint16_t  origDstPort = 0;
    uint32_t  pid = 0;
};

// Полный transparent NAT (как в tcp_nat.h). src+dst → 127.0.0.1, save mapping
// по clientPort. Reply от relay (loopback) NAT'ит обратно по сохранённому.
class UdpNat
{
public:
    explicit UdpNat(uint16_t relayPort);

    bool OnOutbound(WINDIVERT_IPHDR* ip, WINDIVERT_UDPHDR* udp,
                    uint32_t pid, uint32_t ifIdx);

    bool OnInbound(WINDIVERT_IPHDR* ip, WINDIVERT_UDPHDR* udp,
                   uint32_t* outIfIdx);

    UdpNatLookupResult Lookup(uint32_t clientIpNet, uint16_t clientPortNet) const;

    size_t TrackedFlowCount() const;
    void GarbageCollect();
    uint16_t RelayPortNet() const { return m_relayPortNet; }

private:
    struct FlowEntry
    {
        uint32_t origSrcIp = 0;
        uint32_t origDstIp = 0;
        uint16_t origDstPort = 0;
        uint32_t pid = 0;
        uint32_t ifIdx = 0;
        std::chrono::steady_clock::time_point lastSeen;
    };

    uint16_t m_relayPortNet;
    mutable std::mutex m_mu;
    std::unordered_map<uint16_t, FlowEntry> m_flows;
    std::chrono::steady_clock::time_point m_lastGc;
};

void SetGlobalUdpNat(UdpNat* nat);
UdpNatLookupResult UdpNatLookup(uint32_t clientIpNet, uint16_t clientPortNet);

} // namespace proxydivert
