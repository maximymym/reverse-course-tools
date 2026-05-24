#pragma once

#include <Windows.h>
#include <mutex>
#include <unordered_map>

#include "../../proxyhook/src/config.h"

namespace proxyrelay
{

// Потокобезопасная карта PID → ProxyConfig. Orchestrator выставляет при
// LaunchInstance, чистит при exit. Relay читает при accept'е (TCP) или при
// первом UDP datagram'е от PID.
class PidProxyMap
{
public:
    void Set(DWORD pid, const proxyhook::ProxyConfig& cfg);
    void Clear(DWORD pid);
    bool Get(DWORD pid, proxyhook::ProxyConfig& out) const;
    size_t Size() const;

private:
    mutable std::mutex m_mu;
    std::unordered_map<DWORD, proxyhook::ProxyConfig> m_map;
};

PidProxyMap& Global();

} // namespace proxyrelay
