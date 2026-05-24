#pragma once
// =====================================================================
// Server-side auth: POST /api/v1/auth with key + HWID
// HWID binds automatically on first use. No manual whitelist.
// =====================================================================

#include "hwid.h"

#include <Windows.h>
#include <winhttp.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>

#pragma comment(lib, "winhttp.lib")

namespace auth
{

static const wchar_t* SERVER_HOST = L"v1per.tech";
static const wchar_t* AUTH_PATH   = L"/api/v1/auth";

struct AuthResult
{
	bool        allowed = false;
	std::string message;
	std::string sessionToken;
	std::string hwidHash;
};

// Generate random hex nonce
inline std::string RandomNonce( int bytes = 16 )
{
	std::string hex;
	hex.reserve( bytes * 2 );
	for ( int i = 0; i < bytes; i++ )
	{
		BYTE b;
		BCryptGenRandom( nullptr, &b, 1, BCRYPT_USE_SYSTEM_PREFERRED_RNG );
		char buf[4];
		snprintf( buf, sizeof( buf ), "%02x", b );
		hex += buf;
	}
	return hex;
}

// Simple JSON value extractor (no deps)
inline std::string JsonGetString( const std::string& json, const char* key )
{
	std::string needle = std::string( "\"" ) + key + "\":\"";
	auto pos = json.find( needle );
	if ( pos == std::string::npos ) return {};
	pos += needle.size();
	auto end = json.find( '"', pos );
	if ( end == std::string::npos ) return {};
	return json.substr( pos, end - pos );
}

inline AuthResult Authenticate( const std::string& licenseKey )
{
	AuthResult result;
	result.hwidHash = hwid::Generate();

	// Build JSON body
	std::string nonce = RandomNonce();
	int64_t ts = (int64_t)time( nullptr );

	char body[512];
	snprintf( body, sizeof( body ),
		"{\"key\":\"%s\",\"hwid_hash\":\"%s\",\"stub_ver\":\"dotafarm-1.0\",\"timestamp\":%lld,\"nonce\":\"%s\"}",
		licenseKey.c_str(), result.hwidHash.c_str(), (long long)ts, nonce.c_str() );

	// WinHTTP POST
	HINTERNET hSession = WinHttpOpen( L"DotaFarm/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
		WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0 );
	if ( !hSession )
	{
		result.message = "WinHTTP init failed";
		return result;
	}

	HINTERNET hConnect = WinHttpConnect( hSession, SERVER_HOST, INTERNET_DEFAULT_HTTPS_PORT, 0 );
	if ( !hConnect )
	{
		result.message = "Cannot connect to server";
		WinHttpCloseHandle( hSession );
		return result;
	}

	HINTERNET hRequest = WinHttpOpenRequest( hConnect, L"POST", AUTH_PATH,
		nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE );
	if ( !hRequest )
	{
		result.message = "Request creation failed";
		WinHttpCloseHandle( hConnect );
		WinHttpCloseHandle( hSession );
		return result;
	}

	const wchar_t* headers = L"Content-Type: application/json\r\n";
	BOOL sent = WinHttpSendRequest( hRequest, headers, -1,
		(LPVOID)body, (DWORD)strlen( body ), (DWORD)strlen( body ), 0 );

	if ( !sent || !WinHttpReceiveResponse( hRequest, nullptr ) )
	{
		result.message = "Server unreachable";
		WinHttpCloseHandle( hRequest );
		WinHttpCloseHandle( hConnect );
		WinHttpCloseHandle( hSession );
		return result;
	}

	// Read response
	std::string response;
	DWORD bytesRead = 0;
	char buf[4096];
	while ( WinHttpReadData( hRequest, buf, sizeof( buf ) - 1, &bytesRead ) && bytesRead > 0 )
	{
		buf[bytesRead] = '\0';
		response += buf;
		bytesRead = 0;
	}

	WinHttpCloseHandle( hRequest );
	WinHttpCloseHandle( hConnect );
	WinHttpCloseHandle( hSession );

	// Parse response
	std::string status = JsonGetString( response, "status" );
	result.message = JsonGetString( response, "message" );
	result.sessionToken = JsonGetString( response, "session_token" );
	result.allowed = ( status == "allowed" );

	return result;
}

} // namespace auth
