#include "relay.h"

#include "../../proxyhook/src/socks5_client.h"

#include <WinSock2.h>
#include <WS2tcpip.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <thread>
#include <unordered_map>
#include <mutex>

#pragma comment(lib, "ws2_32.lib")

namespace proxyrelay
{

namespace
{
// Per-PID UDP ASSOCIATE control sockets — держим control socket открытым
// пока PID жив, иначе proxy обрывает relay.
struct UdpAssoc
{
    SOCKET       control = INVALID_SOCKET;
    sockaddr_in  relayEndpoint{};
    SOCKET       upstreamUdp = INVALID_SOCKET;  // socket to send to relayEndpoint
    uint16_t     upstreamLocalPort = 0;
};

std::mutex g_udpAssocMu;
std::unordered_map<DWORD, UdpAssoc> g_udpAssocByPid;

// Обратная карта: какой PID использует upstream UDP socket (по local port)
// — нужно чтобы reply path знал куда возвращать.
std::unordered_map<uint16_t, DWORD> g_upstreamPortToPid;

// Per-(pid, clientPort) → upstream sock — для multiplex'а нескольких UDP
// клиентов одного процесса через один control. Здесь упрощаем: один
// control + один upstream socket per PID, на upstream получаем reply,
// unwrap, sendto клиенту обратно.
}

ProxyRelay::ProxyRelay() = default;

ProxyRelay::~ProxyRelay()
{
    Stop();
}

void ProxyRelay::SetLogger(LogFn fn)
{
    m_logger = std::move(fn);
}

void ProxyRelay::SetTcpLookup(LookupTcpFn fn)
{
    m_lookupTcp = std::move(fn);
}

void ProxyRelay::SetUdpLookup(LookupUdpFn fn)
{
    m_lookupUdp = std::move(fn);
}

void ProxyRelay::Log(const char* fmt, ...)
{
    if (!m_logger) return;
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    m_logger(buf);
}

bool ProxyRelay::Start(const Config& cfg)
{
    if (m_running.load())
        return true;

    // WSAStartup — orchestrator уже мог инициализировать (ws2_32 idempotent).
    WSADATA wsa{};
    WSAStartup(MAKEWORD(2, 2), &wsa);

    // ── TCP listener ──
    m_tcpListen = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (m_tcpListen == INVALID_SOCKET)
    {
        Log("[relay] socket(TCP) failed: %d", WSAGetLastError());
        return false;
    }
    BOOL reuse = TRUE;
    setsockopt(m_tcpListen, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(cfg.requestedTcpPort);
    if (bind(m_tcpListen, (sockaddr*)&addr, sizeof(addr)) != 0)
    {
        Log("[relay] bind(TCP %u) failed: %d", cfg.requestedTcpPort, WSAGetLastError());
        closesocket(m_tcpListen);
        m_tcpListen = INVALID_SOCKET;
        return false;
    }
    int alen = sizeof(addr);
    getsockname(m_tcpListen, (sockaddr*)&addr, &alen);
    m_tcpPort = ntohs(addr.sin_port);

    if (listen(m_tcpListen, SOMAXCONN) != 0)
    {
        Log("[relay] listen(TCP) failed: %d", WSAGetLastError());
        closesocket(m_tcpListen);
        m_tcpListen = INVALID_SOCKET;
        return false;
    }

    // ── UDP listener ──
    m_udpListen = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (m_udpListen == INVALID_SOCKET)
    {
        Log("[relay] socket(UDP) failed: %d", WSAGetLastError());
        closesocket(m_tcpListen);
        m_tcpListen = INVALID_SOCKET;
        return false;
    }
    addr.sin_port = htons(cfg.requestedUdpPort);
    if (bind(m_udpListen, (sockaddr*)&addr, sizeof(addr)) != 0)
    {
        Log("[relay] bind(UDP %u) failed: %d", cfg.requestedUdpPort, WSAGetLastError());
        closesocket(m_tcpListen);
        closesocket(m_udpListen);
        m_tcpListen = m_udpListen = INVALID_SOCKET;
        return false;
    }
    alen = sizeof(addr);
    getsockname(m_udpListen, (sockaddr*)&addr, &alen);
    m_udpPort = ntohs(addr.sin_port);

    m_running.store(true);
    m_tcpThread = std::thread(&ProxyRelay::TcpAcceptLoop, this);
    m_udpThread = std::thread(&ProxyRelay::UdpRecvLoop, this);

    Log("[relay] listening: tcp=127.0.0.1:%u udp=127.0.0.1:%u", m_tcpPort, m_udpPort);
    return true;
}

void ProxyRelay::Stop()
{
    if (!m_running.exchange(false))
        return;

    if (m_tcpListen != INVALID_SOCKET)
    {
        closesocket(m_tcpListen);
        m_tcpListen = INVALID_SOCKET;
    }
    if (m_udpListen != INVALID_SOCKET)
    {
        closesocket(m_udpListen);
        m_udpListen = INVALID_SOCKET;
    }

    if (m_tcpThread.joinable()) m_tcpThread.join();
    if (m_udpThread.joinable()) m_udpThread.join();

    {
        std::lock_guard<std::mutex> lk(g_udpAssocMu);
        for (auto& kv : g_udpAssocByPid)
        {
            if (kv.second.control != INVALID_SOCKET) closesocket(kv.second.control);
            if (kv.second.upstreamUdp != INVALID_SOCKET) closesocket(kv.second.upstreamUdp);
        }
        g_udpAssocByPid.clear();
        g_upstreamPortToPid.clear();
    }
}

void ProxyRelay::TcpAcceptLoop()
{
    while (m_running.load())
    {
        sockaddr_in peer{};
        int plen = sizeof(peer);
        SOCKET s = accept(m_tcpListen, (sockaddr*)&peer, &plen);
        if (s == INVALID_SOCKET)
        {
            if (!m_running.load()) break;
            int e = WSAGetLastError();
            if (e == WSAEINTR || e == WSAENOTSOCK) break;
            Log("[relay] TCP accept err=%d", e);
            Sleep(100);
            continue;
        }

        m_tcpAccepted.fetch_add(1, std::memory_order_relaxed);

        // Move client handling off accept thread.
        std::thread(&ProxyRelay::HandleTcpClient, this,
            s, peer.sin_addr.s_addr, peer.sin_port).detach();
    }
}

// Идемпотентность: если клиент шлёт SOCKS5 greeting (0x05 0x01..0x04) — это
// значит ProxyHook.dll уже сделал handshake, и мы его обернём в double-proxy.
// Тогда вместо handshake — passthrough к upstream.
//
// MVP: для упрощения, идемпотентности нет в первой итерации. ProxyHook
// должен быть disabled когда WinDivert engine активен (флаг
// keepProxyHookFallback в config). Двойной прокси произойдёт только если
// ProxyHook включён и driver тоже работает — тогда оба перехватят, double
// SOCKS5 → fail. Это пользователю в гайде объяснено.
void ProxyRelay::HandleTcpClient(SOCKET clientSock,
    uint32_t clientIpNet, uint16_t clientPortNet)
{
    auto closeAll = [&]() { closesocket(clientSock); };

    if (!m_lookupTcp)
    {
        Log("[relay] TCP: no lookup fn");
        closeAll();
        m_tcpHandshakeFail.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    uint32_t origDstIp = 0;
    uint16_t origDstPort = 0;
    uint32_t pid = 0;
    if (!m_lookupTcp(clientIpNet, clientPortNet, &origDstIp, &origDstPort, &pid))
    {
        // Tuple не найден — клиент попал на наш порт напрямую (никакой NAT
        // не сработал). Нечего делать — закрываем.
        Log("[relay] TCP: no NAT mapping for %u:%u",
            ntohl(clientIpNet), ntohs(clientPortNet));
        closeAll();
        m_tcpHandshakeFail.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    proxyhook::ProxyConfig pcfg;
    if (!Global().Get(pid, pcfg) || !pcfg.enabled)
    {
        // PID не зарегистрирован для proxy — direct connect к origDst как
        // fallback, чтобы не убить connection. Это даёт leak по IP, но лучше
        // чем глухой fail.
        Log("[relay] TCP: PID %u no proxy → direct fallback", pid);
        sockaddr_in d{};
        d.sin_family = AF_INET;
        d.sin_addr.s_addr = origDstIp;
        d.sin_port = origDstPort;
        SOCKET upstream = proxyhook::ConnectTcp(d, 10000);
        if (upstream == INVALID_SOCKET)
        {
            closeAll();
            m_tcpHandshakeFail.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        // Splice
        std::thread([clientSock, upstream]()
        {
            char buf[65536];
            while (true)
            {
                int r = recv(clientSock, buf, sizeof(buf), 0);
                if (r <= 0) break;
                int sent = 0;
                while (sent < r)
                {
                    int w = send(upstream, buf + sent, r - sent, 0);
                    if (w <= 0) break;
                    sent += w;
                }
            }
            shutdown(upstream, SD_BOTH);
            closesocket(clientSock);
        }).detach();
        std::thread([clientSock, upstream]()
        {
            char buf[65536];
            while (true)
            {
                int r = recv(upstream, buf, sizeof(buf), 0);
                if (r <= 0) break;
                int sent = 0;
                while (sent < r)
                {
                    int w = send(clientSock, buf + sent, r - sent, 0);
                    if (w <= 0) break;
                    sent += w;
                }
            }
            shutdown(clientSock, SD_BOTH);
            closesocket(upstream);
        }).detach();
        return;
    }

    // Resolve proxy address
    sockaddr_in proxyAddr{};
    if (!proxyhook::ResolveHost(pcfg.host.c_str(), pcfg.port, &proxyAddr))
    {
        Log("[relay] TCP: resolve %s failed", pcfg.host.c_str());
        closeAll();
        m_tcpHandshakeFail.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    SOCKET upstream = proxyhook::ConnectTcp(proxyAddr, 10000);
    if (upstream == INVALID_SOCKET)
    {
        Log("[relay] TCP: connect proxy %s:%u failed", pcfg.host.c_str(), pcfg.port);
        closeAll();
        m_tcpHandshakeFail.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    auto status = proxyhook::Socks5TcpHandshake(
        upstream, pcfg, /*dstHost*/nullptr, origDstIp, origDstPort);
    if (status != proxyhook::Socks5Status::OK)
    {
        Log("[relay] TCP: SOCKS5 handshake fail status=%d for PID %u dst=%u:%u",
            (int)status, pid,
            ntohl(origDstIp), ntohs(origDstPort));
        closesocket(upstream);
        closeAll();
        m_tcpHandshakeFail.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    m_tcpHandshakeOk.fetch_add(1, std::memory_order_relaxed);

    // Bidirectional splice. 2 threads, 64KB buffers each.
    std::thread([clientSock, upstream]()
    {
        char buf[65536];
        while (true)
        {
            int r = recv(clientSock, buf, sizeof(buf), 0);
            if (r <= 0) break;
            int sent = 0;
            while (sent < r)
            {
                int w = send(upstream, buf + sent, r - sent, 0);
                if (w <= 0) goto done;
                sent += w;
            }
        }
done:
        shutdown(upstream, SD_BOTH);
        closesocket(clientSock);
    }).detach();

    std::thread([clientSock, upstream]()
    {
        char buf[65536];
        while (true)
        {
            int r = recv(upstream, buf, sizeof(buf), 0);
            if (r <= 0) break;
            int sent = 0;
            while (sent < r)
            {
                int w = send(clientSock, buf + sent, r - sent, 0);
                if (w <= 0) goto done;
                sent += w;
            }
        }
done:
        shutdown(clientSock, SD_BOTH);
        closesocket(upstream);
    }).detach();
}

// UDP recv loop — смотрит datagram'ы которые WinDivert завернул на 127.0.0.1
// :relayUdpPort. Для каждого: lookup (origDst, pid), получаем/создаём
// per-PID UDP ASSOCIATE control + upstream socket, оборачиваем datagram в
// SOCKS5 UDP header, шлём.
//
// Reply path: на каждый upstream socket держим recv-thread, который при
// получении SOCKS5-обёрнутого reply дёргает sendto обратно клиенту.
void ProxyRelay::UdpRecvLoop()
{
    constexpr int MAX = 65536;
    auto buf = std::make_unique<char[]>(MAX);

    while (m_running.load())
    {
        sockaddr_in from{};
        int flen = sizeof(from);
        int r = recvfrom(m_udpListen, buf.get(), MAX, 0, (sockaddr*)&from, &flen);
        if (r <= 0)
        {
            if (!m_running.load()) break;
            int e = WSAGetLastError();
            if (e == WSAEINTR || e == WSAENOTSOCK) break;
            Sleep(10);
            continue;
        }

        m_udpDatagramsIn.fetch_add(1, std::memory_order_relaxed);

        if (!m_lookupUdp) continue;

        uint32_t origDstIp = 0;
        uint16_t origDstPort = 0;
        uint32_t pid = 0;
        if (!m_lookupUdp(from.sin_addr.s_addr, from.sin_port,
                         &origDstIp, &origDstPort, &pid))
            continue;

        proxyhook::ProxyConfig pcfg;
        if (!Global().Get(pid, pcfg) || !pcfg.enabled)
            continue;

        // Get or create per-PID assoc.
        UdpAssoc assoc;
        bool needCreate = false;
        {
            std::lock_guard<std::mutex> lk(g_udpAssocMu);
            auto it = g_udpAssocByPid.find(pid);
            if (it == g_udpAssocByPid.end())
                needCreate = true;
            else
                assoc = it->second;
        }

        if (needCreate)
        {
            auto a = proxyhook::Socks5UdpAssociate(pcfg);
            if (a.status != proxyhook::Socks5Status::OK)
            {
                Log("[relay] UDP: ASSOCIATE failed status=%d for PID %u", (int)a.status, pid);
                continue;
            }
            UdpAssoc na;
            na.control = a.controlSocket;
            na.relayEndpoint = a.relayEndpoint;
            na.upstreamUdp = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            if (na.upstreamUdp == INVALID_SOCKET)
            {
                closesocket(a.controlSocket);
                continue;
            }
            sockaddr_in laddr{};
            laddr.sin_family = AF_INET;
            laddr.sin_addr.s_addr = htonl(INADDR_ANY);
            laddr.sin_port = 0;
            bind(na.upstreamUdp, (sockaddr*)&laddr, sizeof(laddr));
            int llen = sizeof(laddr);
            getsockname(na.upstreamUdp, (sockaddr*)&laddr, &llen);
            na.upstreamLocalPort = ntohs(laddr.sin_port);

            {
                std::lock_guard<std::mutex> lk(g_udpAssocMu);
                g_udpAssocByPid[pid] = na;
                g_upstreamPortToPid[na.upstreamLocalPort] = pid;
            }
            assoc = na;

            // Spawn reply thread for this assoc.
            SOCKET upstream = na.upstreamUdp;
            uint16_t clientSrcPort = from.sin_port;
            uint32_t clientSrcIp = from.sin_addr.s_addr;
            // Capture relay socket for sendto back.
            SOCKET relayUdp = m_udpListen;
            std::thread([upstream, relayUdp, clientSrcIp, clientSrcPort, this]()
            {
                constexpr int RMAX = 65536;
                auto rbuf = std::make_unique<char[]>(RMAX);
                while (m_running.load())
                {
                    sockaddr_in rfrom{};
                    int rflen = sizeof(rfrom);
                    int rr = recvfrom(upstream, rbuf.get(), RMAX, 0,
                        (sockaddr*)&rfrom, &rflen);
                    if (rr <= 0) break;

                    char hostOut[256] = {};
                    uint32_t origIp = 0;
                    uint16_t origPort = 0;
                    size_t payloadOff = 0;
                    size_t payloadLen = 0;
                    size_t hdrLen = proxyhook::Socks5UnwrapUdp(
                        (const uint8_t*)rbuf.get(), (size_t)rr,
                        hostOut, &origIp, &origPort, &payloadOff, &payloadLen);
                    if (hdrLen == 0) continue;

                    // Forward unwrapped payload back to client. From the
                    // client's point of view it must appear to come from
                    // origDst — but we send via raw UDP socket back to
                    // (clientIp, clientPort). The WinDivert inbound rule
                    // catches our src port == relayUdpPort and rewrites src
                    // back to origDst.
                    sockaddr_in to{};
                    to.sin_family = AF_INET;
                    to.sin_addr.s_addr = clientSrcIp;
                    to.sin_port = clientSrcPort;
                    sendto(relayUdp, rbuf.get() + payloadOff, (int)payloadLen,
                        0, (sockaddr*)&to, sizeof(to));
                }
                shutdown(upstream, SD_BOTH);
            }).detach();
        }

        // Wrap and send.
        constexpr int WMAX = 65536;
        auto wbuf = std::make_unique<uint8_t[]>(WMAX);
        size_t wlen = proxyhook::Socks5WrapUdp(
            wbuf.get(), WMAX, /*dstHost*/nullptr,
            origDstIp, origDstPort, buf.get(), (size_t)r);
        if (wlen == 0) continue;

        sendto(assoc.upstreamUdp, (const char*)wbuf.get(), (int)wlen, 0,
            (sockaddr*)&assoc.relayEndpoint, sizeof(assoc.relayEndpoint));
        m_udpDatagramsOut.fetch_add(1, std::memory_order_relaxed);
    }
}

} // namespace proxyrelay
