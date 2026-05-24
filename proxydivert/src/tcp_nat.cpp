#include "tcp_nat.h"

#include <WinSock2.h>

namespace proxydivert
{

namespace
{
TcpNat*    g_globalNat = nullptr;
std::mutex g_globalNatMu;
}

void SetGlobalTcpNat(TcpNat* nat)
{
    std::lock_guard<std::mutex> lk(g_globalNatMu);
    g_globalNat = nat;
}

TcpNatLookupResult TcpNatLookup(uint32_t clientIpNet, uint16_t clientPortNet)
{
    TcpNat* nat;
    {
        std::lock_guard<std::mutex> lk(g_globalNatMu);
        nat = g_globalNat;
    }
    if (!nat)
        return {};
    return nat->Lookup(clientIpNet, clientPortNet);
}

TcpNat::TcpNat(uint16_t relayPort)
    : m_relayPortNet(htons(relayPort))
    , m_lastGc(std::chrono::steady_clock::now())
{
}

bool TcpNat::OnOutbound(WINDIVERT_IPHDR* ip, WINDIVERT_TCPHDR* tcp,
                        uint32_t pid, uint32_t ifIdx)
{
    // Skip уже-loopback пакеты — иначе bouncing.
    if (ip->SrcAddr == htonl(0x7F000001) || ip->DstAddr == htonl(0x7F000001))
        return false;

    uint16_t clientPort = tcp->SrcPort;  // network order

    {
        std::lock_guard<std::mutex> lk(m_mu);
        auto& e = m_flows[clientPort];
        e.origSrcIp = ip->SrcAddr;
        e.origDstIp = ip->DstAddr;
        e.origDstPort = tcp->DstPort;
        e.pid = pid;
        e.ifIdx = ifIdx;
        e.lastSeen = std::chrono::steady_clock::now();
    }

    // Полный transparent NAT: src и dst оба → 127.0.0.1. Стек тогда
    // route'ит packet чисто через loopback driver, который доставит relay'у
    // на 127.0.0.1:relayPort. Reply от relay тоже будет loopback и
    // подхватится OnInbound.
    ip->SrcAddr = htonl(0x7F000001);
    ip->DstAddr = htonl(0x7F000001);
    tcp->DstPort = m_relayPortNet;
    return true;
}

bool TcpNat::OnInbound(WINDIVERT_IPHDR* ip, WINDIVERT_TCPHDR* tcp,
                       uint32_t* outIfIdx)
{
    // Reply пакеты от relay приходят как outbound loopback с
    // src=127.0.0.1:relayPort, dst=127.0.0.1:clientPort.
    if (tcp->SrcPort != m_relayPortNet)
        return false;
    if (ip->SrcAddr != htonl(0x7F000001))
        return false;

    uint16_t clientPort = tcp->DstPort;  // dst — это куда reply'нут клиенту

    uint32_t origSrcIp = 0;
    uint32_t origDstIp = 0;
    uint16_t origDstPort = 0;
    uint32_t ifIdx = 0;
    {
        std::lock_guard<std::mutex> lk(m_mu);
        auto it = m_flows.find(clientPort);
        if (it == m_flows.end())
            return false;
        origSrcIp = it->second.origSrcIp;
        origDstIp = it->second.origDstIp;
        origDstPort = it->second.origDstPort;
        ifIdx = it->second.ifIdx;
        it->second.lastSeen = std::chrono::steady_clock::now();
    }

    // Reply должен выглядеть как: src=origDst:origDstPort → dst=origSrc:clientPort.
    // Тогда TCP socket Steam'а (зарегистрирован как (origSrc:clientPort,
    // origDst:origDstPort)) получит payload.
    ip->SrcAddr = origDstIp;
    ip->DstAddr = origSrcIp;
    tcp->SrcPort = origDstPort;
    // tcp->DstPort = clientPort (уже)

    if (outIfIdx) *outIfIdx = ifIdx;
    return true;
}

TcpNatLookupResult TcpNat::Lookup(uint32_t /*clientIpNet*/, uint16_t clientPortNet) const
{
    TcpNatLookupResult r;
    std::lock_guard<std::mutex> lk(m_mu);
    auto it = m_flows.find(clientPortNet);
    if (it == m_flows.end())
        return r;

    r.found = true;
    r.origSrcIp = it->second.origSrcIp;
    r.origDstIp = it->second.origDstIp;
    r.origDstPort = it->second.origDstPort;
    r.pid = it->second.pid;
    return r;
}

size_t TcpNat::TrackedFlowCount() const
{
    std::lock_guard<std::mutex> lk(m_mu);
    return m_flows.size();
}

void TcpNat::GarbageCollect()
{
    auto now = std::chrono::steady_clock::now();
    if (now - m_lastGc < std::chrono::seconds(60))
        return;
    m_lastGc = now;

    std::lock_guard<std::mutex> lk(m_mu);
    auto cutoff = now - std::chrono::minutes(5);
    for (auto it = m_flows.begin(); it != m_flows.end(); )
    {
        if (it->second.lastSeen < cutoff)
            it = m_flows.erase(it);
        else
            ++it;
    }
}

} // namespace proxydivert
