#include "config.h"

#include <Windows.h>
#include <cstdio>
#include <cstdarg>
#include <fstream>
#include <sstream>
#include <mutex>

// nlohmann json — берём через orchestrator/deps/json.hpp
#include <json.hpp>
using json = nlohmann::json;

namespace proxyhook
{

std::string GetConfigPathForPid( unsigned long pid )
{
    char buf[MAX_PATH];
    snprintf( buf, sizeof( buf ), "C:\\temp\\andromeda\\proxy_%lu.json", pid );
    return buf;
}

bool ParseProxyUrl( const std::string& url, ProxyConfig& out )
{
    if ( url.empty() )
        return false;

    // scheme://
    size_t schemeEnd = url.find( "://" );
    if ( schemeEnd == std::string::npos )
        return false;
    out.scheme = url.substr( 0, schemeEnd );
    for ( auto& c : out.scheme )
        c = (char)tolower( (unsigned char)c );

    if ( out.scheme != "socks5" )
        return false; // Phase 1 — только socks5

    std::string rest = url.substr( schemeEnd + 3 );

    // user:pass@host:port
    size_t atPos = rest.find( '@' );
    std::string userinfo;
    std::string hostport;
    if ( atPos != std::string::npos )
    {
        userinfo = rest.substr( 0, atPos );
        hostport = rest.substr( atPos + 1 );

        size_t colonPos = userinfo.find( ':' );
        if ( colonPos != std::string::npos )
        {
            out.username = userinfo.substr( 0, colonPos );
            out.password = userinfo.substr( colonPos + 1 );
        }
        else
        {
            out.username = userinfo;
        }
    }
    else
    {
        hostport = rest;
    }

    // host:port (strip trailing /)
    size_t slashPos = hostport.find( '/' );
    if ( slashPos != std::string::npos )
        hostport = hostport.substr( 0, slashPos );

    size_t colonPos = hostport.rfind( ':' );
    if ( colonPos == std::string::npos )
        return false;

    out.host = hostport.substr( 0, colonPos );
    std::string portStr = hostport.substr( colonPos + 1 );
    long p = strtol( portStr.c_str(), nullptr, 10 );
    if ( p <= 0 || p > 65535 )
        return false;
    out.port = (uint16_t)p;

    if ( out.host.empty() )
        return false;

    out.enabled = true;
    out.rawUrl  = url;
    return true;
}

bool LoadConfigForCurrentProcess( FullConfig& out, std::string& errOut )
{
    std::string path = GetConfigPathForPid( GetCurrentProcessId() );

    std::ifstream file( path );
    if ( !file.is_open() )
    {
        errOut = "config missing: " + path;
        return false;
    }

    try
    {
        json j;
        file >> j;

        // ── Proxy (optional) ──
        if ( j.contains( "proxy" ) && j["proxy"].is_string() )
        {
            std::string url = j["proxy"].get<std::string>();
            if ( !url.empty() )
            {
                if ( ParseProxyUrl( url, out.proxy ) )
                {
                    if ( j.contains( "enable_udp" ) && j["enable_udp"].is_boolean() )
                        out.proxy.enableUdp = j["enable_udp"].get<bool>();

                    if ( j.contains( "bypass_hosts" ) && j["bypass_hosts"].is_array() )
                    {
                        for ( auto& h : j["bypass_hosts"] )
                        {
                            if ( h.is_string() )
                            {
                                std::string s = h.get<std::string>();
                                for ( auto& c : s )
                                    c = (char)tolower( (unsigned char)c );
                                out.proxy.bypassHosts.push_back( s );
                            }
                        }
                    }
                }
                else
                {
                    errOut = "cannot parse proxy url: " + url;
                    // Не fail целиком — возможно hwid включён
                }
            }
        }

        // ── HWID spoof (optional) ──
        if ( j.contains( "hwid_spoof" ) && j["hwid_spoof"].is_object() )
        {
            auto& h = j["hwid_spoof"];
            bool enabled = h.contains( "enabled" ) && h["enabled"].is_boolean() &&
                h["enabled"].get<bool>();
            std::string seed;
            if ( h.contains( "seed" ) && h["seed"].is_string() )
                seed = h["seed"].get<std::string>();

            if ( enabled && !seed.empty() )
            {
                out.hwid.enabled = true;
                out.hwid.seed = seed;
                if ( !DeriveHwid( seed, out.hwid.values ) )
                {
                    errOut = "DeriveHwid failed";
                    out.hwid.enabled = false;
                }
            }
        }

        // Успех если хотя бы одна фича включена.
        if ( !out.proxy.enabled && !out.hwid.enabled )
        {
            if ( errOut.empty() )
                errOut = "neither proxy nor hwid configured";
            return false;
        }

        return true;
    }
    catch ( const std::exception& e )
    {
        errOut = std::string( "json exception: " ) + e.what();
        return false;
    }
}

bool LoadConfigForCurrentProcess( ProxyConfig& out, std::string& errOut )
{
    FullConfig full;
    if ( !LoadConfigForCurrentProcess( full, errOut ) )
        return false;
    out = full.proxy;
    return out.enabled;
}

// ── DbgLog — open/write/close each call для надёжности в short-lived процессах ──

static std::mutex g_logMutex;

void DbgLog( const char* fmt, ... )
{
    std::lock_guard<std::mutex> lock( g_logMutex );

    CreateDirectoryA( "C:\\temp", nullptr );
    CreateDirectoryA( "C:\\temp\\andromeda", nullptr );

    char path[MAX_PATH];
    snprintf( path, sizeof( path ), "C:\\temp\\andromeda\\proxyhook_%lu.log",
        GetCurrentProcessId() );

    FILE* f = fopen( path, "a" );
    if ( !f ) return;

    SYSTEMTIME st;
    GetLocalTime( &st );
    fprintf( f, "[%02d:%02d:%02d.%03d] ",
        st.wHour, st.wMinute, st.wSecond, st.wMilliseconds );

    va_list args;
    va_start( args, fmt );
    vfprintf( f, fmt, args );
    va_end( args );

    fputc( '\n', f );
    fclose( f );

    // Также в debugger — удобно для DebugView
    char dbgBuf[1024];
    va_list args2;
    va_start( args2, fmt );
    vsnprintf( dbgBuf, sizeof( dbgBuf ) - 2, fmt, args2 );
    va_end( args2 );
    OutputDebugStringA( dbgBuf );
}

} // namespace proxyhook
