#include "proxy_service.h"

#include "../../proxydivert/src/tcp_nat.h"
#include "../../proxydivert/src/udp_nat.h"
#include "../../proxyrelay/src/pid_proxy_map.h"
#include "../../proxyhook/src/config.h"

#include <TlHelp32.h>
#include <cstdarg>
#include <cstdio>
#include <unordered_map>
#include <unordered_set>
#include <vector>

ProxyService::ProxyService() = default;

ProxyService::~ProxyService()
{
    Stop();
}

void ProxyService::SetLogger(LogFn fn)
{
    m_logger = std::move(fn);
}

void ProxyService::Log(const char* fmt, ...)
{
    if (!m_logger) return;
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    m_logger(buf);
}

bool ProxyService::Start()
{
    if (m_running.load())
        return true;

    auto loggerWrap = [this](const char* m) { Log("%s", m); };
    m_divert.SetLogger(loggerWrap);
    m_relay.SetLogger(loggerWrap);

    // Wire relay → NAT lookups (loose coupling, no direct dependency).
    m_relay.SetTcpLookup([](uint32_t ip, uint16_t port,
        uint32_t* dIp, uint16_t* dPort, uint32_t* pid)
    {
        auto r = proxydivert::TcpNatLookup(ip, port);
        if (!r.found) return false;
        *dIp = r.origDstIp;
        *dPort = r.origDstPort;
        *pid = r.pid;
        return true;
    });
    m_relay.SetUdpLookup([](uint32_t ip, uint16_t port,
        uint32_t* dIp, uint16_t* dPort, uint32_t* pid)
    {
        auto r = proxydivert::UdpNatLookup(ip, port);
        if (!r.found) return false;
        *dIp = r.origDstIp;
        *dPort = r.origDstPort;
        *pid = r.pid;
        return true;
    });

    proxyrelay::ProxyRelay::Config rcfg{};
    rcfg.requestedTcpPort = 0;  // auto
    rcfg.requestedUdpPort = 0;
    if (!m_relay.Start(rcfg))
    {
        Log("[proxy] relay.Start failed");
        return false;
    }

    proxydivert::DivertEngine::Config dcfg{};
    dcfg.relayTcpPort = m_relay.GetTcpPort();
    dcfg.relayUdpPort = m_relay.GetUdpPort();
    if (!m_divert.Start(dcfg))
    {
        Log("[proxy] divert.Start failed — stopping relay");
        m_relay.Stop();
        return false;
    }

    m_stopRequested.store(false);
    m_running.store(true);
    m_pipeStopEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    m_pollThread = std::thread(&ProxyService::ChildPollLoop, this);
    m_pipeThread = std::thread(&ProxyService::ChildPipeLoop, this);

    Log("[proxy] service started: tcp=%u udp=%u",
        m_relay.GetTcpPort(), m_relay.GetUdpPort());
    return true;
}

void ProxyService::Stop()
{
    if (!m_running.exchange(false))
        return;

    m_stopRequested.store(true);
    if (m_pipeStopEvent) SetEvent(m_pipeStopEvent);

    if (m_pollThread.joinable())
        m_pollThread.join();
    if (m_pipeThread.joinable())
        m_pipeThread.join();

    if (m_pipeStopEvent)
    {
        CloseHandle(m_pipeStopEvent);
        m_pipeStopEvent = nullptr;
    }

    m_divert.Stop();
    m_relay.Stop();

    {
        std::lock_guard<std::mutex> lk(m_rootsMu);
        m_roots.clear();
    }

    Log("[proxy] service stopped");
}

void ProxyService::AddRootPid(DWORD pid, const std::string& proxyUrl)
{
    if (!pid) return;

    proxyhook::ProxyConfig pcfg;
    if (!proxyUrl.empty() && !proxyhook::ParseProxyUrl(proxyUrl, pcfg))
    {
        Log("[proxy] AddRootPid: invalid proxy url '%s'", proxyUrl.c_str());
        return;
    }

    {
        std::lock_guard<std::mutex> lk(m_rootsMu);
        for (auto& r : m_roots)
        {
            if (r.pid == pid) { r.proxy = proxyUrl; goto registered; }
        }
        m_roots.push_back({ pid, proxyUrl });
    registered:;
    }

    proxyrelay::Global().Set(pid, pcfg);
    m_divert.AddWatchedPid(pid);
    Log("[proxy] root +PID %lu (proxy=%s)", pid,
        proxyUrl.empty() ? "<none>" : proxyUrl.c_str());
}

void ProxyService::RemoveRootPid(DWORD pid)
{
    {
        std::lock_guard<std::mutex> lk(m_rootsMu);
        for (auto it = m_roots.begin(); it != m_roots.end(); )
        {
            if (it->pid == pid) it = m_roots.erase(it);
            else ++it;
        }
    }
    proxyrelay::Global().Clear(pid);
    m_divert.RemoveWatchedPid(pid);
}

void ProxyService::AddChildPid(DWORD pid, DWORD inheritedFromRoot)
{
    if (!pid) return;

    std::string proxyUrl;
    {
        std::lock_guard<std::mutex> lk(m_rootsMu);
        for (auto& r : m_roots)
        {
            if (r.pid == inheritedFromRoot)
            {
                proxyUrl = r.proxy;
                break;
            }
        }
    }

    proxyhook::ProxyConfig pcfg;
    if (!proxyUrl.empty()) proxyhook::ParseProxyUrl(proxyUrl, pcfg);
    proxyrelay::Global().Set(pid, pcfg);
    m_divert.AddWatchedPid(pid);
}

// Polling thread — каждые 500ms строит дерево от каждого root PID и обновляет
// watchlist. Ловит steamwebhelper.exe / gameoverlayui.exe / dota2.exe / ...
//
// CreateToolhelp32Snapshot не даёт parent-child прямо; вместо этого отдаёт
// PROCESSENTRY32.th32ParentProcessID. Walk: для каждого процесса проверяем
// принадлежит ли его ancestry к root-set.
void ProxyService::ChildPollLoop()
{
    std::unordered_set<DWORD> known;

    while (!m_stopRequested.load())
    {
        std::unordered_set<DWORD> roots;
        {
            std::lock_guard<std::mutex> lk(m_rootsMu);
            for (auto& r : m_roots) roots.insert(r.pid);
        }

        if (roots.empty())
        {
            Sleep(500);
            continue;
        }

        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap == INVALID_HANDLE_VALUE)
        {
            Sleep(500);
            continue;
        }

        // Build PID → parent map.
        std::unordered_map<DWORD, DWORD> parentOf;
        PROCESSENTRY32 pe{};
        pe.dwSize = sizeof(pe);
        if (Process32First(snap, &pe))
        {
            do
            {
                parentOf[pe.th32ProcessID] = pe.th32ParentProcessID;
            } while (Process32Next(snap, &pe));
        }
        CloseHandle(snap);

        // For each known PID, find its root ancestor (if any).
        std::unordered_set<DWORD> live;
        for (auto& kv : parentOf)
        {
            DWORD pid = kv.first;
            DWORD cur = pid;
            for (int hops = 0; hops < 16; hops++)
            {
                if (roots.count(cur))
                {
                    live.insert(pid);
                    if (!known.count(pid))
                    {
                        AddChildPid(pid, cur);
                        known.insert(pid);
                    }
                    break;
                }
                auto it = parentOf.find(cur);
                if (it == parentOf.end()) break;
                if (it->second == cur || it->second == 0) break;
                cur = it->second;
            }
        }

        // Покинули watchlist — процессы умерли.
        for (auto it = known.begin(); it != known.end(); )
        {
            if (!live.count(*it))
            {
                proxyrelay::Global().Clear(*it);
                m_divert.RemoveWatchedPid(*it);
                it = known.erase(it);
            }
            else
            {
                ++it;
            }
        }

        // Также: root PID мёртв → удаляем его регистрацию.
        std::vector<DWORD> rootsDead;
        {
            std::lock_guard<std::mutex> lk(m_rootsMu);
            for (auto& r : m_roots)
                if (!parentOf.count(r.pid)) rootsDead.push_back(r.pid);
        }
        for (DWORD p : rootsDead)
            RemoveRootPid(p);

        Sleep(500);
    }
}

// Pipe \\.\pipe\DotaFarmChildPid — ProxyHook.dll шлёт "<parentPid> <childPid>\n"
// при каждом CreateProcess. Обрабатываем sequentially (one client at a time —
// ConnectNamedPipe + Disconnect + recreate). Latency ~5ms, лучше чем 500ms
// polling fallback.
void ProxyService::ChildPipeLoop()
{
    constexpr const char* PIPE_NAME = "\\\\.\\pipe\\DotaFarmChildPid";

    while (!m_stopRequested.load())
    {
        HANDLE hPipe = CreateNamedPipeA(
            PIPE_NAME,
            PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
            PIPE_UNLIMITED_INSTANCES,
            0, 4096, 0, nullptr);
        if (hPipe == INVALID_HANDLE_VALUE)
        {
            Sleep(500);
            continue;
        }

        OVERLAPPED ov{};
        ov.hEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);

        BOOL connected = ConnectNamedPipe(hPipe, &ov);
        DWORD err = GetLastError();
        if (!connected && err != ERROR_IO_PENDING && err != ERROR_PIPE_CONNECTED)
        {
            CloseHandle(ov.hEvent);
            CloseHandle(hPipe);
            Sleep(100);
            continue;
        }

        if (err == ERROR_IO_PENDING)
        {
            HANDLE waits[2] = { ov.hEvent, m_pipeStopEvent };
            DWORD w = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
            if (w == WAIT_OBJECT_0 + 1)
            {
                CancelIoEx(hPipe, &ov);
                CloseHandle(ov.hEvent);
                CloseHandle(hPipe);
                break;
            }
        }

        CloseHandle(ov.hEvent);

        // Read message
        char buf[64] = {};
        DWORD got = 0;
        ReadFile(hPipe, buf, sizeof(buf) - 1, &got, nullptr);
        if (got > 0)
        {
            DWORD parentPid = 0, childPid = 0;
            if (sscanf_s(buf, "%lu %lu", &parentPid, &childPid) == 2 && childPid)
            {
                AddChildPid(childPid, parentPid);
            }
        }

        DisconnectNamedPipe(hPipe);
        CloseHandle(hPipe);
    }
}

ProxyService::Stats ProxyService::GetStats() const
{
    Stats s;
    s.running = m_running.load();
    s.relayTcpPort = m_relay.GetTcpPort();
    s.relayUdpPort = m_relay.GetUdpPort();
    s.watchedPids = m_divert.GetWatchedPidCount();
    s.registeredPids = proxyrelay::Global().Size();

    auto& d = m_divert.GetStats();
    s.pktTotal     = d.packetsTotal.load();
    s.tcpRedirOut  = d.tcpRedirectedOut.load();
    s.tcpRedirIn   = d.tcpRedirectedIn.load();
    s.udpRedirOut  = d.udpRedirectedOut.load();
    s.udpRedirIn   = d.udpRedirectedIn.load();
    s.passthrough  = d.passthrough.load();

    s.tcpAccepted      = m_relay.TcpAccepted();
    s.tcpHandshakeOk   = m_relay.TcpHandshakeOk();
    s.tcpHandshakeFail = m_relay.TcpHandshakeFail();
    s.udpDgIn          = m_relay.UdpDatagramsIn();
    s.udpDgOut         = m_relay.UdpDatagramsOut();
    return s;
}
