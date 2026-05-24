#pragma once

#include <string>
#include <vector>
#include <cstdint>

#include "hwid_gen.h"

namespace proxyhook
{

struct ProxyConfig
{
    bool        enabled = false;

    // Parsed URL components
    std::string scheme;       // "socks5" (http/socks4 not supported)
    std::string username;
    std::string password;
    std::string host;
    uint16_t    port = 0;

    // Features
    bool        enableUdp = true;
    std::vector<std::string> bypassHosts;

    // Raw URL (for logging)
    std::string rawUrl;
};

// Per-PID HWID spoof config. Если seed пустой — HWID hook'и не ставятся.
struct HwidConfig
{
    bool            enabled = false;
    std::string     seed;       // "<steamId>_<YYYY-MM>"
    HwidFakeValues  values;     // derived from seed at config load
};

struct FullConfig
{
    ProxyConfig  proxy;
    HwidConfig   hwid;
};

// Parse "socks5://user:pass@host:port" → ProxyConfig. Returns true on success.
bool ParseProxyUrl( const std::string& url, ProxyConfig& out );

// Load proxy_<PID>.json for the current process. Returns false if file missing
// or BOTH proxy field empty AND hwid.enabled=false — caller should treat as "no hook".
// Iff hwid.enabled=true but proxy is empty: proxy.enabled stays false, но hwid готов.
bool LoadConfigForCurrentProcess( FullConfig& out, std::string& errOut );

// Backward-compat thin wrapper (старый API для existing callers).
bool LoadConfigForCurrentProcess( ProxyConfig& out, std::string& errOut );

// Helper: proxy_<PID>.json path in 'C:/temp/andromeda/'.
std::string GetConfigPathForPid( unsigned long pid );

// Simple debug log → C:\temp\andromeda\proxyhook_<PID>.log (line-buffered).
void DbgLog( const char* fmt, ... );

} // namespace proxyhook
