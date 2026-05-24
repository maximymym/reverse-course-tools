#include "divert_engine.h"
#include "tcp_nat.h"
#include "udp_nat.h"

#include "windivert/WinDivert.h"

#include <WinSock2.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <mutex>
#include <unordered_map>

namespace proxydivert
{

namespace
{
// (proto<<16 | localPortNet) → PID
std::mutex                              g_flowMu;
std::unordered_map<uint32_t, DWORD>     g_flowMap;

uint32_t MakeFlowKey(uint8_t proto, uint16_t localPortNet)
{
    return ((uint32_t)proto << 16) | (uint32_t)localPortNet;
}
}

DWORD FlowLookupPid(uint8_t proto, uint16_t localPortNet)
{
    std::lock_guard<std::mutex> lk(g_flowMu);
    auto it = g_flowMap.find(MakeFlowKey(proto, localPortNet));
    return it == g_flowMap.end() ? 0 : it->second;
}

DivertEngine::DivertEngine() = default;

DivertEngine::~DivertEngine()
{
    Stop();
}

void DivertEngine::SetLogger(LogFn fn)
{
    m_logger = std::move(fn);
}

void DivertEngine::Log(const char* fmt, ...)
{
    if (!m_logger) return;
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    m_logger(buf);
}

bool DivertEngine::Start(const Config& cfg)
{
    if (m_running.load())
        return true;

    m_cfg = cfg;
    if (m_cfg.relayTcpPort == 0 || m_cfg.relayUdpPort == 0)
    {
        Log("[divert] Start: relay ports not set (tcp=%u udp=%u)",
            m_cfg.relayTcpPort, m_cfg.relayUdpPort);
        return false;
    }

    // Filter:
    //   - outbound: ловим то что watched PID отправляет — но WinDivert на
    //     LAYER_NETWORK НЕ имеет .Process.Id (это есть только на FLOW/SOCKET
    //     слоях). Поэтому фильтруем по PID в user-mode после recv.
    //     Зато NETWORK слой даёт modify+reinject что нам и нужно.
    //   - inbound: ловим только пакеты от 127.0.0.1:relayPort обратно.
    //
    // Фильтр исключает loopback траффик пользователя, но пускает наш relay
    // <-> upstream proxy на loopback-out (если бы он был — на самом деле
    // прокси внешний, поэтому loopback нам мешает только дёргать сам себя).
    // Filter:
    //   - outbound non-loopback (tcp/udp): kандидаты на NAT
    //   - outbound loopback от relay (src=127.0.0.1, srcPort=relayPort): reply
    //     которое надо превратить обратно в inbound к клиенту
    char filter[1024];
    snprintf(filter, sizeof(filter),
        "(outbound and not loopback and (tcp or udp)) or "
        "(outbound and loopback and ip.SrcAddr == 127.0.0.1 and "
            "((tcp.SrcPort == %u) or (udp.SrcPort == %u)))",
        m_cfg.relayTcpPort, m_cfg.relayUdpPort);

    m_handle = WinDivertOpen(filter, WINDIVERT_LAYER_NETWORK, /*priority*/0, /*flags*/0);
    if (m_handle == INVALID_HANDLE_VALUE)
    {
        DWORD err = GetLastError();
        Log("[divert] WinDivertOpen(NETWORK) failed: err=%lu (need admin? driver loaded?)", err);
        return false;
    }

    WinDivertSetParam(m_handle, WINDIVERT_PARAM_QUEUE_LENGTH, m_cfg.queueLength);
    WinDivertSetParam(m_handle, WINDIVERT_PARAM_QUEUE_TIME,   m_cfg.queueTimeMs);
    WinDivertSetParam(m_handle, WINDIVERT_PARAM_QUEUE_SIZE,   m_cfg.queueSizeBytes);

    // FLOW layer (sniff-only) — даёт PID на каждый новый connection.
    // NETWORK layer не имеет PID, поэтому без FLOW мы не могли бы фильтровать
    // по PID и заворачивали ВЕСЬ outbound трафик системы → коллапс интернета.
    m_flowHandle = WinDivertOpen(
        "outbound and (tcp or udp)",
        WINDIVERT_LAYER_FLOW,
        /*priority*/-100,  // ниже NETWORK
        WINDIVERT_FLAG_SNIFF | WINDIVERT_FLAG_RECV_ONLY);
    if (m_flowHandle == INVALID_HANDLE_VALUE)
    {
        DWORD err = GetLastError();
        Log("[divert] WinDivertOpen(FLOW) failed: err=%lu — closing NETWORK", err);
        WinDivertClose(m_handle);
        m_handle = INVALID_HANDLE_VALUE;
        return false;
    }

    m_tcpNat = std::make_unique<TcpNat>(m_cfg.relayTcpPort);
    m_udpNat = std::make_unique<UdpNat>(m_cfg.relayUdpPort);
    SetGlobalTcpNat(m_tcpNat.get());
    SetGlobalUdpNat(m_udpNat.get());

    m_stopRequested.store(false);
    m_running.store(true);
    m_flowThread = std::thread(&DivertEngine::FlowLoop, this);
    m_recvThread = std::thread(&DivertEngine::RecvLoop, this);

    Log("[divert] started: tcp_relay=127.0.0.1:%u udp_relay=127.0.0.1:%u",
        m_cfg.relayTcpPort, m_cfg.relayUdpPort);
    return true;
}

void DivertEngine::Stop()
{
    if (!m_running.exchange(false))
        return;

    m_stopRequested.store(true);

    if (m_handle != INVALID_HANDLE_VALUE)
        WinDivertShutdown(m_handle, WINDIVERT_SHUTDOWN_BOTH);
    if (m_flowHandle != INVALID_HANDLE_VALUE)
        WinDivertShutdown(m_flowHandle, WINDIVERT_SHUTDOWN_BOTH);

    if (m_recvThread.joinable())
        m_recvThread.join();
    if (m_flowThread.joinable())
        m_flowThread.join();

    if (m_handle != INVALID_HANDLE_VALUE)
    {
        WinDivertClose(m_handle);
        m_handle = INVALID_HANDLE_VALUE;
    }
    if (m_flowHandle != INVALID_HANDLE_VALUE)
    {
        WinDivertClose(m_flowHandle);
        m_flowHandle = INVALID_HANDLE_VALUE;
    }

    {
        std::lock_guard<std::mutex> lk(g_flowMu);
        g_flowMap.clear();
    }

    SetGlobalTcpNat(nullptr);
    SetGlobalUdpNat(nullptr);
    m_tcpNat.reset();
    m_udpNat.reset();

    Log("[divert] stopped (pkts=%llu tcp_out=%llu tcp_in=%llu udp_out=%llu udp_in=%llu pass=%llu)",
        (unsigned long long)m_stats.packetsTotal.load(),
        (unsigned long long)m_stats.tcpRedirectedOut.load(),
        (unsigned long long)m_stats.tcpRedirectedIn.load(),
        (unsigned long long)m_stats.udpRedirectedOut.load(),
        (unsigned long long)m_stats.udpRedirectedIn.load(),
        (unsigned long long)m_stats.passthrough.load());
}

void DivertEngine::AddWatchedPid(DWORD pid)
{
    if (!pid) return;
    bool added = false;
    {
        std::lock_guard<std::mutex> lk(m_pidMutex);
        added = m_watchedPids.insert(pid).second;
    }
    if (added)
        Log("[divert] watch +PID %lu (total=%zu)", pid, GetWatchedPidCount());
}

void DivertEngine::RemoveWatchedPid(DWORD pid)
{
    bool removed = false;
    {
        std::lock_guard<std::mutex> lk(m_pidMutex);
        removed = m_watchedPids.erase(pid) > 0;
    }
    if (removed)
        Log("[divert] watch -PID %lu (total=%zu)", pid, GetWatchedPidCount());
}

bool DivertEngine::IsWatchedPid(DWORD pid) const
{
    std::lock_guard<std::mutex> lk(m_pidMutex);
    return m_watchedPids.count(pid) > 0;
}

std::vector<DWORD> DivertEngine::GetWatchedPids() const
{
    std::lock_guard<std::mutex> lk(m_pidMutex);
    return std::vector<DWORD>(m_watchedPids.begin(), m_watchedPids.end());
}

size_t DivertEngine::GetWatchedPidCount() const
{
    std::lock_guard<std::mutex> lk(m_pidMutex);
    return m_watchedPids.size();
}

// FLOW layer events — sniff connection establishment, заполняем
// (proto, localPortNet) → PID cache. NETWORK loop ниже использует cache
// чтобы решить — наш ли это PID. Без этого мы заворачивали бы ВЕСЬ outbound
// и относили в loop через relay → upstream → NAT → relay (коллапс интернета).
void DivertEngine::FlowLoop()
{
    // FLOW layer возвращает только WINDIVERT_ADDRESS — buffer ignored, но
    // некоторые версии WinDivert требуют non-NULL pointer. Передаём dummy.
    UINT8 dummy[16];
    uint64_t evtTotal = 0;
    uint64_t lastLogged = 0;
    while (!m_stopRequested.load())
    {
        WINDIVERT_ADDRESS addr{};
        UINT recvLen = 0;
        if (!WinDivertRecv(m_flowHandle, dummy, sizeof(dummy), &recvLen, &addr))
        {
            if (m_stopRequested.load()) break;
            DWORD err = GetLastError();
            if (err == ERROR_NO_DATA) continue;
            if (err == ERROR_INSUFFICIENT_BUFFER) continue;  // expected for FLOW
            Log("[divert] FLOW recv err=%lu", err);
            Sleep(50);
            continue;
        }

        DWORD pid = addr.Flow.ProcessId;
        uint8_t proto = addr.Flow.Protocol;
        uint16_t localPortNet = htons(addr.Flow.LocalPort);

        evtTotal++;
        if (evtTotal - lastLogged >= 50 || lastLogged == 0)
        {
            lastLogged = evtTotal;
            Log("[divert] FLOW evt #%llu: event=%u pid=%lu proto=%u lport=%u "
                "(map size=%zu)",
                (unsigned long long)evtTotal, (unsigned)addr.Event,
                pid, proto, ntohs(localPortNet),
                g_flowMap.size());
        }

        if (addr.Event == 1 /* WINDIVERT_EVENT_FLOW_ESTABLISHED */)
        {
            std::lock_guard<std::mutex> lk(g_flowMu);
            g_flowMap[MakeFlowKey(proto, localPortNet)] = pid;
        }
        else if (addr.Event == 2 /* WINDIVERT_EVENT_FLOW_DELETED */)
        {
            std::lock_guard<std::mutex> lk(g_flowMu);
            g_flowMap.erase(MakeFlowKey(proto, localPortNet));
        }
    }
}

// NETWORK loop — modify+reinject. Per packet: lookup PID через FlowMap, и
// если PID не watched → passthrough (зеркальный send без NAT). Чужой трафик
// проходит сквозь без задержки.
void DivertEngine::RecvLoop()
{
    // 65536 потому что LSO/TSO offload может отдавать packets > 16K.
    // Меньше → ERROR_INSUFFICIENT_BUFFER (122) и spam в логе.
    constexpr UINT MAX_PKT = 65536;
    auto packet = std::make_unique<UINT8[]>(MAX_PKT);

    auto lastStats = std::chrono::steady_clock::now();

    while (!m_stopRequested.load())
    {
        // Periodic stats log — раз в 5 секунд
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now - lastStats).count() >= 5)
        {
            lastStats = now;
            Log("[divert] stats: pkts=%llu tcp_out=%llu tcp_in=%llu udp_out=%llu udp_in=%llu pass=%llu flow_map=%zu watched=%zu",
                (unsigned long long)m_stats.packetsTotal.load(),
                (unsigned long long)m_stats.tcpRedirectedOut.load(),
                (unsigned long long)m_stats.tcpRedirectedIn.load(),
                (unsigned long long)m_stats.udpRedirectedOut.load(),
                (unsigned long long)m_stats.udpRedirectedIn.load(),
                (unsigned long long)m_stats.passthrough.load(),
                g_flowMap.size(), GetWatchedPidCount());
        }

        WINDIVERT_ADDRESS addr{};
        UINT recvLen = 0;
        if (!WinDivertRecv(m_handle, packet.get(), MAX_PKT, &recvLen, &addr))
        {
            DWORD err = GetLastError();
            if (m_stopRequested.load())
                break;
            if (err == ERROR_NO_DATA)
                continue;
            Log("[divert] WinDivertRecv err=%lu — sleeping 100ms", err);
            Sleep(100);
            continue;
        }

        m_stats.packetsTotal.fetch_add(1, std::memory_order_relaxed);

        PWINDIVERT_IPHDR  ip = nullptr;
        PWINDIVERT_TCPHDR tcp = nullptr;
        PWINDIVERT_UDPHDR udp = nullptr;
        UINT8 proto = 0;
        WinDivertHelperParsePacket(packet.get(), recvLen,
            &ip, nullptr, &proto, nullptr, nullptr,
            &tcp, &udp, nullptr, nullptr, nullptr, nullptr);

        bool modified = false;

        if (ip)
        {
            // Loopback-replies от relay: src=127.0.0.1, srcPort=relayPort.
            // Это reply на наш NAT'нутый outbound. Меняем src обратно на
            // origDst, dst на origSrc, reinject как inbound через original
            // ifIdx (чтобы стек дотащил до Steam socket'а через правильный
            // interface — VPN, Wi-Fi etc.).
            bool isLoopbackReply = addr.Loopback &&
                ip->SrcAddr == htonl(0x7F000001) &&
                ((tcp && tcp->SrcPort == htons(m_cfg.relayTcpPort)) ||
                 (udp && udp->SrcPort == htons(m_cfg.relayUdpPort)));

            if (isLoopbackReply && tcp)
            {
                uint32_t ifIdx = 0;
                if (m_tcpNat->OnInbound(ip, tcp, &ifIdx))
                {
                    addr.Outbound = 0;
                    addr.Loopback = 0;
                    addr.Network.IfIdx = ifIdx;
                    modified = true;
                    m_stats.tcpRedirectedIn.fetch_add(1, std::memory_order_relaxed);
                }
            }
            else if (isLoopbackReply && udp)
            {
                uint32_t ifIdx = 0;
                if (m_udpNat->OnInbound(ip, udp, &ifIdx))
                {
                    addr.Outbound = 0;
                    addr.Loopback = 0;
                    addr.Network.IfIdx = ifIdx;
                    modified = true;
                    m_stats.udpRedirectedIn.fetch_add(1, std::memory_order_relaxed);
                }
            }
            else if (addr.Outbound && tcp)
            {
                DWORD pid = FlowLookupPid(/*IPPROTO_TCP*/6, tcp->SrcPort);
                if (pid && IsWatchedPid(pid))
                {
                    uint32_t ifIdx = addr.Network.IfIdx;
                    if (m_tcpNat->OnOutbound(ip, tcp, pid, ifIdx))
                    {
                        addr.Loopback = 1;  // reinject через loopback driver
                        modified = true;
                        m_stats.tcpRedirectedOut.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            }
            else if (addr.Outbound && udp)
            {
                DWORD pid = FlowLookupPid(/*IPPROTO_UDP*/17, udp->SrcPort);
                if (pid && IsWatchedPid(pid))
                {
                    uint32_t ifIdx = addr.Network.IfIdx;
                    if (m_udpNat->OnOutbound(ip, udp, pid, ifIdx))
                    {
                        addr.Loopback = 1;
                        modified = true;
                        m_stats.udpRedirectedOut.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            }
        }

        if (modified)
        {
            // После modify checksums надо пересчитать.
            WinDivertHelperCalcChecksums(packet.get(), recvLen, &addr, 0);
        }
        else
        {
            m_stats.passthrough.fetch_add(1, std::memory_order_relaxed);
        }

        UINT sentLen = 0;
        if (!WinDivertSend(m_handle, packet.get(), recvLen, &sentLen, &addr))
        {
            m_stats.reinjectFails.fetch_add(1, std::memory_order_relaxed);
        }

        m_tcpNat->GarbageCollect();
        m_udpNat->GarbageCollect();
    }
}

} // namespace proxydivert
