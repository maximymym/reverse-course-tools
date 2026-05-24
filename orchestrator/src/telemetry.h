#pragma once
// =====================================================================
// Telemetry: upload debug logs to server for remote diagnostics.
// POST /api/v1/log with session_token or key+hwid fallback.
// =====================================================================

#include <Windows.h>
#include <winhttp.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <filesystem>

#pragma comment(lib, "winhttp.lib")

namespace telemetry
{

static const wchar_t* SERVER_HOST = L"v1per.tech";
static const wchar_t* LOG_PATH    = L"/api/v1/log";

// Read last N bytes of a file (tail)
inline std::string ReadTail( const std::string& path, size_t maxBytes = 200000 )
{
	std::ifstream f( path, std::ios::binary | std::ios::ate );
	if ( !f.is_open() ) return {};
	auto size = f.tellg();
	if ( size <= 0 ) return {};
	size_t readSize = ( (size_t)size > maxBytes ) ? maxBytes : (size_t)size;
	f.seekg( -(std::streamoff)readSize, std::ios::end );
	std::string buf( readSize, '\0' );
	f.read( buf.data(), readSize );
	return buf;
}

// Escape string for JSON (minimal: backslash, quote, control chars)
inline std::string JsonEscape( const std::string& s )
{
	std::string out;
	out.reserve( s.size() + s.size() / 8 );
	for ( unsigned char c : s )
	{
		switch ( c )
		{
		case '\\': out += "\\\\"; break;
		case '"':  out += "\\\""; break;
		case '\n': out += "\\n";  break;
		case '\r': out += "\\r";  break;
		case '\t': out += "\\t";  break;
		default:
			if ( c < 0x20 )
			{
				char buf[8];
				snprintf( buf, sizeof( buf ), "\\u%04x", c );
				out += buf;
			}
			else
				out += (char)c;
		}
	}
	return out;
}

// Collect all logs: DotaFarm.log + all debug_*.log (last 200KB each)
inline std::string CollectLogs( const std::string& logDir )
{
	std::ostringstream ss;

	// 1. Orchestrator log
	std::string orchLog = logDir + "\\DotaFarm.log";
	std::string orchContent = ReadTail( orchLog, 100000 );
	if ( !orchContent.empty() )
	{
		ss << "===== DotaFarm.log =====\n" << orchContent << "\n\n";
	}

	// 2. All debug_*.log files
	try
	{
		namespace fs = std::filesystem;
		for ( auto& entry : fs::directory_iterator( logDir ) )
		{
			auto fname = entry.path().filename().string();
			if ( fname.rfind( "debug_", 0 ) == 0 && fname.size() > 10 )
			{
				std::string content = ReadTail( entry.path().string(), 150000 );
				if ( !content.empty() )
				{
					// Extract just the [GC] and [StateMachine] lines to save space
					std::istringstream lines( content );
					std::string line;
					std::ostringstream filtered;
					int count = 0;
					while ( std::getline( lines, line ) )
					{
						if ( line.find( "[GC]" ) != std::string::npos ||
							 line.find( "[StateMachine]" ) != std::string::npos ||
							 line.find( "[InstanceConfig]" ) != std::string::npos ||
							 line.find( "[CGCMessageHandler]" ) != std::string::npos ||
							 line.find( "[reconnect]" ) != std::string::npos ||
							 line.find( "[CrashLog" ) != std::string::npos ||
							 line.find( "Exception " ) != std::string::npos ||
							 line.find( "CallStack" ) != std::string::npos ||
							 line.find( "ERROR" ) != std::string::npos ||
							 line.find( "CRASH" ) != std::string::npos )
						{
							// Skip schema dump lines
							if ( line.find( "class " ) != std::string::npos ||
								 line.find( "enum " ) != std::string::npos ||
								 line.find( "\tbool " ) != std::string::npos ||
								 line.find( "\tfloat" ) != std::string::npos ||
								 line.find( "\tint32" ) != std::string::npos ||
								 line.find( "\tuint" ) != std::string::npos ||
								 line.find( "k_EMsg" ) != std::string::npos ||
								 line.find( "k_ECustom" ) != std::string::npos )
								continue;
							filtered << line << "\n";
							count++;
						}
					}
					if ( count > 0 )
					{
						ss << "===== " << fname << " (" << count << " lines) =====\n";
						ss << filtered.str() << "\n";
					}
				}
			}
		}
	}
	catch ( ... ) {}

	// 2b. Bot brain log (Lua controller + actions) — С:\temp\andromeda\botbrain.log.
	// Один файл на машину (все инстансы пишут туда, последний инстанс перезатирает).
	// Последние 300 КБ чтобы захватить несколько минут тиков (log ~500 байт/сек на инстанс).
	{
		std::string brainLog = "C:\\temp\\andromeda\\botbrain.log";
		std::string brainContent = ReadTail( brainLog, 300000 );
		if ( !brainContent.empty() )
		{
			ss << "===== botbrain.log (" << brainContent.size() << " bytes) =====\n"
			   << brainContent << "\n\n";
		}
	}

	// 3. List crash dumps (path + size) — actual upload of .dmp is separate
	try
	{
		namespace fs = std::filesystem;
		std::ostringstream dumps;
		int dumpCount = 0;
		for ( auto& entry : fs::directory_iterator( logDir ) )
		{
			auto fname = entry.path().filename().string();
			if ( fname.rfind( "dump_", 0 ) == 0 && fname.size() > 4
				&& fname.substr( fname.size() - 4 ) == ".dmp" )
			{
				auto sz = (uint64_t)entry.file_size();
				dumps << "  " << fname << " (" << sz << " bytes)\n";
				dumpCount++;
			}
		}
		if ( dumpCount > 0 )
		{
			ss << "===== Crash dumps (" << dumpCount << ") =====\n";
			ss << dumps.str() << "\n";
		}
	}
	catch ( ... ) {}

	return ss.str();
}

// Upload a single binary file (minidump) as multipart POST.
inline bool UploadDump( const std::string& sessionToken, const std::string& keyValue,
	const std::string& hwidHash, const std::string& dumpPath )
{
	std::ifstream f( dumpPath, std::ios::binary | std::ios::ate );
	if ( !f.is_open() ) return false;
	auto size = (size_t)f.tellg();
	if ( size == 0 || size > 50 * 1024 * 1024 ) return false; // cap 50MB
	f.seekg( 0, std::ios::beg );
	std::vector<char> buf( size );
	f.read( buf.data(), size );

	// Build multipart/form-data body
	std::string boundary = "----DotaFarmBoundary7XYZ";
	std::string filename = std::filesystem::path( dumpPath ).filename().string();

	std::ostringstream head;
	head << "--" << boundary << "\r\n";
	if ( !sessionToken.empty() )
		head << "Content-Disposition: form-data; name=\"session_token\"\r\n\r\n" << sessionToken << "\r\n--" << boundary << "\r\n";
	else
	{
		head << "Content-Disposition: form-data; name=\"key_value\"\r\n\r\n" << keyValue << "\r\n--" << boundary << "\r\n";
		head << "Content-Disposition: form-data; name=\"hwid_hash\"\r\n\r\n" << hwidHash << "\r\n--" << boundary << "\r\n";
	}
	head << "Content-Disposition: form-data; name=\"dump\"; filename=\"" << filename << "\"\r\n";
	head << "Content-Type: application/octet-stream\r\n\r\n";

	std::string tail = "\r\n--" + boundary + "--\r\n";

	std::string body = head.str();
	body.insert( body.end(), buf.begin(), buf.end() );
	body += tail;

	HINTERNET hSession = WinHttpOpen( L"DotaFarm/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
		WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0 );
	if ( !hSession ) return false;
	HINTERNET hConnect = WinHttpConnect( hSession, SERVER_HOST, INTERNET_DEFAULT_HTTPS_PORT, 0 );
	if ( !hConnect ) { WinHttpCloseHandle( hSession ); return false; }
	HINTERNET hRequest = WinHttpOpenRequest( hConnect, L"POST", L"/api/v1/dump",
		nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE );
	if ( !hRequest ) { WinHttpCloseHandle( hConnect ); WinHttpCloseHandle( hSession ); return false; }

	std::wstring ct = L"Content-Type: multipart/form-data; boundary=" +
		std::wstring( boundary.begin(), boundary.end() ) + L"\r\n";

	BOOL sent = WinHttpSendRequest( hRequest, ct.c_str(), (DWORD)-1,
		(LPVOID)body.data(), (DWORD)body.size(), (DWORD)body.size(), 0 );

	bool ok = false;
	if ( sent && WinHttpReceiveResponse( hRequest, nullptr ) )
	{
		DWORD statusCode = 0, statusSize = sizeof( statusCode );
		WinHttpQueryHeaders( hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
			WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusSize, WINHTTP_NO_HEADER_INDEX );
		ok = ( statusCode == 200 );
	}

	WinHttpCloseHandle( hRequest );
	WinHttpCloseHandle( hConnect );
	WinHttpCloseHandle( hSession );
	return ok;
}

// POST log to server. Returns true on success.
inline bool UploadLog( const std::string& sessionToken, const std::string& keyValue,
					   const std::string& hwidHash, const std::string& logContent )
{
	if ( logContent.empty() ) return false;

	// Build JSON body
	std::string escaped = JsonEscape( logContent );

	std::string body;
	if ( !sessionToken.empty() )
	{
		body = "{\"session_token\":\"" + sessionToken + "\",\"log\":\"" + escaped + "\"}";
	}
	else if ( !keyValue.empty() && !hwidHash.empty() )
	{
		body = "{\"key_value\":\"" + keyValue + "\",\"hwid_hash\":\"" + hwidHash + "\",\"log\":\"" + escaped + "\"}";
	}
	else
		return false;

	// Truncate if over 2MB
	if ( body.size() > 1900000 )
		body.resize( 1900000 );

	// WinHTTP POST
	HINTERNET hSession = WinHttpOpen( L"DotaFarm/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
		WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0 );
	if ( !hSession ) return false;

	HINTERNET hConnect = WinHttpConnect( hSession, SERVER_HOST, INTERNET_DEFAULT_HTTPS_PORT, 0 );
	if ( !hConnect ) { WinHttpCloseHandle( hSession ); return false; }

	HINTERNET hRequest = WinHttpOpenRequest( hConnect, L"POST", LOG_PATH,
		nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE );
	if ( !hRequest ) { WinHttpCloseHandle( hConnect ); WinHttpCloseHandle( hSession ); return false; }

	const wchar_t* headers = L"Content-Type: application/json\r\n";
	BOOL sent = WinHttpSendRequest( hRequest, headers, -1,
		(LPVOID)body.c_str(), (DWORD)body.size(), (DWORD)body.size(), 0 );

	bool ok = false;
	if ( sent && WinHttpReceiveResponse( hRequest, nullptr ) )
	{
		DWORD statusCode = 0, statusSize = sizeof( statusCode );
		WinHttpQueryHeaders( hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
			WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusSize, WINHTTP_NO_HEADER_INDEX );
		ok = ( statusCode == 200 );
	}

	WinHttpCloseHandle( hRequest );
	WinHttpCloseHandle( hConnect );
	WinHttpCloseHandle( hSession );
	return ok;
}

} // namespace telemetry
