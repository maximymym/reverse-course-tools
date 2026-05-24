#include "pid_proxy_map.h"

namespace proxyrelay
{

void PidProxyMap::Set(DWORD pid, const proxyhook::ProxyConfig& cfg)
{
    std::lock_guard<std::mutex> lk(m_mu);
    m_map[pid] = cfg;
}

void PidProxyMap::Clear(DWORD pid)
{
    std::lock_guard<std::mutex> lk(m_mu);
    m_map.erase(pid);
}

bool PidProxyMap::Get(DWORD pid, proxyhook::ProxyConfig& out) const
{
    std::lock_guard<std::mutex> lk(m_mu);
    auto it = m_map.find(pid);
    if (it == m_map.end())
        return false;
    out = it->second;
    return true;
}

size_t PidProxyMap::Size() const
{
    std::lock_guard<std::mutex> lk(m_mu);
    return m_map.size();
}

PidProxyMap& Global()
{
    static PidProxyMap inst;
    return inst;
}

} // namespace proxyrelay
