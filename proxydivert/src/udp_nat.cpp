#include "udp_nat.h"

#include <WinSock2.h>

namespace proxydivert
{

namespace
{
UdpNat*    g_globalNat = nullptr;
std::mutex g_globalNatMu;
}

void SetGlobalUdpNat(UdpNat* nat)
{
    std::lock_guard<std::mutex> lk(g_globalNatMu);
    g_globalNat = nat;
}

UdpNatLookupResult UdpNatLookup(uint32_t clientIpNet, uint16_t clientPortNet)
{
    UdpNat* nat;
    {
        std::lock_guard<std::mutex> lk(g_globalNatMu);
        nat = g_globalNat;
    }
    if (!nat)
        return {};
    return nat->Lookup(clientIpNet, clientPortNet);
}

UdpNat::UdpNat(uint16_t relayPort)
    : m_relayPortNet(htons(relayPort))
    , m_lastGc(std::chrono::steady_clock::now())
{
}

bool UdpNat::OnOutbound(WINDIVERT_IPHDR* ip, WINDIVERT_UDPHDR* udp,
                        uint32_t pid, uint32_t ifIdx)
{
    if (ip->SrcAddr == htonl(0x7F000001) || ip->DstAddr == htonl(0x7F000001))
        return false;

    uint16_t clientPort = udp->SrcPort;
    {
        std::lock_guard<std::mutex> lk(m_mu);
        auto& e = m_flows[clientPort];
        e.origSrcIp = ip->SrcAddr;
        e.origDstIp = ip->DstAddr;
        e.origDstPort = udp->DstPort;
        e.pid = pid;
        e.ifIdx = ifIdx;
        e.lastSeen = std::chrono::steady_clock::now();
    }

    ip->SrcAddr = htonl(0x7F000001);
    ip->DstAddr = htonl(0x7F000001);
    udp->DstPort = m_relayPortNet;
    return true;
}

bool UdpNat::OnInbound(WINDIVERT_IPHDR* ip, WINDIVERT_UDPHDR* udp,
                       uint32_t* outIfIdx)
{
    if (udp->SrcPort != m_relayPortNet) return false;
    if (ip->SrcAddr != htonl(0x7F000001)) return false;

    uint16_t clientPort = udp->DstPort;

    uint32_t origSrcIp = 0;
    uint32_t origDstIp = 0;
    uint16_t origDstPort = 0;
    uint32_t ifIdx = 0;
    {
        std::lock_guard<std::mutex> lk(m_mu);
        auto it = m_flows.find(clientPort);
        if (it == m_flows.end()) return false;
        origSrcIp = it->second.origSrcIp;
        origDstIp = it->second.origDstIp;
        origDstPort = it->second.origDstPort;
        ifIdx = it->second.ifIdx;
        it->second.lastSeen = std::chrono::steady_clock::now();
    }

    ip->SrcAddr = origDstIp;
    ip->DstAddr = origSrcIp;
    udp->SrcPort = origDstPort;

    if (outIfIdx) *outIfIdx = ifIdx;
    return true;
}

UdpNatLookupResult UdpNat::Lookup(uint32_t /*clientIpNet*/, uint16_t clientPortNet) const
{
    UdpNatLookupResult r;
    std::lock_guard<std::mutex> lk(m_mu);
    auto it = m_flows.find(clientPortNet);
    if (it == m_flows.end()) return r;

    r.found = true;
    r.origSrcIp = it->second.origSrcIp;
    r.origDstIp = it->second.origDstIp;
    r.origDstPort = it->second.origDstPort;
    r.pid = it->second.pid;
    return r;
}

size_t UdpNat::TrackedFlowCount() const
{
    std::lock_guard<std::mutex> lk(m_mu);
    return m_flows.size();
}

void UdpNat::GarbageCollect()
{
    auto now = std::chrono::steady_clock::now();
    if (now - m_lastGc < std::chrono::seconds(60)) return;
    m_lastGc = now;

    std::lock_guard<std::mutex> lk(m_mu);
    auto cutoff = now - std::chrono::minutes(2);
    for (auto it = m_flows.begin(); it != m_flows.end(); )
    {
        if (it->second.lastSeen < cutoff) it = m_flows.erase(it);
        else ++it;
    }
}

} // namespace proxydivert
