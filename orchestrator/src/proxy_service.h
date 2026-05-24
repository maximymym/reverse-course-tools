#pragma once

#include <Windows.h>
#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../../proxydivert/src/divert_engine.h"
#include "../../proxyrelay/src/relay.h"

// Тонкий фасад поверх ProxyDivert + ProxyRelay. Orchestrator работает с ним
// через AddSteamPid / RemoveSteamPid; child PID discovery (steamwebhelper,
// dota2, ...) делается ChildPidPoller'ом внутри.
class ProxyService
{
public:
    using LogFn = std::function<void(const char* msg)>;

    ProxyService();
    ~ProxyService();

    void SetLogger(LogFn fn);

    // Стартует Divert engine + ProxyRelay + child PID polling thread.
    bool Start();
    void Stop();
    bool IsRunning() const { return m_running.load(); }

    // Регистрация PID Steam (root). Все children этого PID будут добавлены в
    // watchlist автоматически. proxyUrl — "socks5://user:pass@host:port".
    void AddRootPid(DWORD pid, const std::string& proxyUrl);
    void RemoveRootPid(DWORD pid);

    // Прямое добавление PID в watchlist (для fast-path IPC от ProxyHook).
    void AddChildPid(DWORD pid, DWORD inheritedFromRoot);

    // Stats для GUI Network panel.
    struct Stats
    {
        bool        running = false;
        uint16_t    relayTcpPort = 0;
        uint16_t    relayUdpPort = 0;
        size_t      watchedPids = 0;
        size_t      registeredPids = 0;
        uint64_t    pktTotal = 0;
        uint64_t    tcpRedirOut = 0;
        uint64_t    tcpRedirIn = 0;
        uint64_t    udpRedirOut = 0;
        uint64_t    udpRedirIn = 0;
        uint64_t    passthrough = 0;
        uint64_t    tcpAccepted = 0;
        uint64_t    tcpHandshakeOk = 0;
        uint64_t    tcpHandshakeFail = 0;
        uint64_t    udpDgIn = 0;
        uint64_t    udpDgOut = 0;
    };
    Stats GetStats() const;

private:
    void ChildPollLoop();
    void ChildPipeLoop();
    void Log(const char* fmt, ...);

    proxydivert::DivertEngine m_divert;
    proxyrelay::ProxyRelay    m_relay;

    std::atomic<bool>  m_running{ false };
    std::atomic<bool>  m_stopRequested{ false };
    std::thread        m_pollThread;
    std::thread        m_pipeThread;
    HANDLE             m_pipeStopEvent = nullptr;

    LogFn  m_logger;

    struct RootEntry
    {
        DWORD       pid;
        std::string proxy;
    };
    mutable std::mutex m_rootsMu;
    std::vector<RootEntry> m_roots;
};
