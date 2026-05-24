#pragma once

#include <Windows.h>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

namespace proxydivert
{

// Logger sink — orchestrator подкидывает свой через SetLogger.
using LogFn = std::function<void(const char* msg)>;

class TcpNat;
class UdpNat;

// Главный движок: открывает WinDivert handle, читает пакеты, передаёт их в
// TcpNat / UdpNat, реинжектит. PID watchlist thread-safe.
//
// Единственный длинный поток внутри — recv loop. Остановка через Stop()
// блокирует пока поток не вернёт управление (WinDivertShutdown пробуждает
// зависший WinDivertRecv).
class DivertEngine
{
public:
    struct Config
    {
        // Куда заворачиваем TCP перехваты. 127.0.0.1:relayTcpPort.
        uint16_t relayTcpPort = 0;
        // Аналогично для UDP. 127.0.0.1:relayUdpPort.
        uint16_t relayUdpPort = 0;
        // Сколько максимум packets в kernel queue (default 8192).
        uint32_t queueLength = 8192;
        // Сколько ms packet может ждать в queue (default 2000).
        uint32_t queueTimeMs = 2000;
        // Размер kernel queue в bytes (default 16 MB).
        uint32_t queueSizeBytes = 16 * 1024 * 1024;
    };

    struct Stats
    {
        std::atomic<uint64_t> packetsTotal{ 0 };
        std::atomic<uint64_t> tcpRedirectedOut{ 0 };
        std::atomic<uint64_t> tcpRedirectedIn{ 0 };
        std::atomic<uint64_t> udpRedirectedOut{ 0 };
        std::atomic<uint64_t> udpRedirectedIn{ 0 };
        std::atomic<uint64_t> passthrough{ 0 };
        std::atomic<uint64_t> dropped{ 0 };
        std::atomic<uint64_t> reinjectFails{ 0 };
    };

    DivertEngine();
    ~DivertEngine();

    DivertEngine(const DivertEngine&) = delete;
    DivertEngine& operator=(const DivertEngine&) = delete;

    void SetLogger(LogFn fn);

    // Запускает engine. После успеха — все TCP/UDP outbound пакеты watched
    // PID'ов будут заворачиваться. Возвращает false если WinDivert handle не
    // открылся (нет admin / driver не загрузился / Defender блокирует).
    bool Start(const Config& cfg);

    // Останавливает engine, ждёт recv thread, закрывает handle. Idempotent.
    void Stop();

    bool IsRunning() const { return m_running.load(); }

    // PID watchlist — pre-existing root PIDs orchestrator знает (steam.exe).
    // Children (steamwebhelper, dota2) добавляются позже либо через
    // AddWatchedPid из polling thread, либо через named-pipe IPC из ProxyHook.
    void AddWatchedPid(DWORD pid);
    void RemoveWatchedPid(DWORD pid);
    bool IsWatchedPid(DWORD pid) const;
    std::vector<DWORD> GetWatchedPids() const;
    size_t GetWatchedPidCount() const;

    const Stats& GetStats() const { return m_stats; }

private:
    void RecvLoop();
    void FlowLoop();
    void Log(const char* fmt, ...);

    Config       m_cfg{};
    HANDLE       m_handle = INVALID_HANDLE_VALUE;
    HANDLE       m_flowHandle = INVALID_HANDLE_VALUE;  // FLOW layer для PID discovery
    std::atomic<bool> m_running{ false };
    std::atomic<bool> m_stopRequested{ false };
    std::thread       m_recvThread;
    std::thread       m_flowThread;

    mutable std::mutex          m_pidMutex;
    std::unordered_set<DWORD>   m_watchedPids;

    std::unique_ptr<TcpNat> m_tcpNat;
    std::unique_ptr<UdpNat> m_udpNat;

    LogFn  m_logger;
    Stats  m_stats;
};

// Глобальный FlowMap (заполняется DivertEngine::FlowLoop) — отображает
// (proto, localPort) → PID для outbound коннектов. NETWORK layer NAT lookup'ит
// сюда чтобы решить — наш ли это PID. Если не наш → passthrough без NAT
// (иначе заворачиваем весь трафик системы и роняем интернет в loop).
//
// proto: IPPROTO_TCP=6, IPPROTO_UDP=17.
DWORD FlowLookupPid(uint8_t proto, uint16_t localPortNet);

} // namespace proxydivert
