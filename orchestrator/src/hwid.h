#pragma once
// =====================================================================
// HWID generation (standalone, no external crypto deps)
// SHA256 via Windows CNG (bcrypt.dll)
// =====================================================================

#include <Windows.h>
#include <intrin.h>
#include <winioctl.h>
#include <Sddl.h>
#include <bcrypt.h>
#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "advapi32.lib")

namespace hwid {

// ── SHA256 via CNG ──
static std::string SHA256Hex( const void* data, size_t len )
{
	BCRYPT_ALG_HANDLE hAlg = nullptr;
	BCRYPT_HASH_HANDLE hHash = nullptr;
	UCHAR hash[32]{};

	BCryptOpenAlgorithmProvider( &hAlg, BCRYPT_SHA256_ALGORITHM, nullptr, 0 );
	BCryptCreateHash( hAlg, &hHash, nullptr, 0, nullptr, 0, 0 );
	BCryptHashData( hHash, (PUCHAR)data, (ULONG)len, 0 );
	BCryptFinishHash( hHash, hash, 32, 0 );
	BCryptDestroyHash( hHash );
	BCryptCloseAlgorithmProvider( hAlg, 0 );

	char hex[65]{};
	for ( int i = 0; i < 32; i++ )
		snprintf( hex + i * 2, 3, "%02x", hash[i] );
	return hex;
}

// ── Components (copied from deploy/stub/src/hwid.h) ──

static std::string GetCPUBrand()
{
	char brand[49]{};
	int info[4];
	__cpuid( info, 0x80000000 );
	if ( (unsigned)info[0] < 0x80000004u ) return "";
	__cpuid( info, 0x80000002 ); memcpy( brand, info, 16 );
	__cpuid( info, 0x80000003 ); memcpy( brand + 16, info, 16 );
	__cpuid( info, 0x80000004 ); memcpy( brand + 32, info, 16 );
	int len = 48;
	while ( len > 0 && ( brand[len - 1] == ' ' || brand[len - 1] == '\0' ) ) len--;
	return std::string( brand, len );
}

static std::string GetCPUIDSignature()
{
	int info[4];
	__cpuid( info, 1 );
	char buf[16];
	snprintf( buf, sizeof( buf ), "%08X", (unsigned)info[0] );
	return buf;
}

static std::string GetLogicalCPUCount()
{
	SYSTEM_INFO si{};
	GetSystemInfo( &si );
	char buf[8];
	snprintf( buf, sizeof( buf ), "%u", si.dwNumberOfProcessors );
	return buf;
}

static std::string GetTotalRAMGB()
{
	MEMORYSTATUSEX ms{};
	ms.dwLength = sizeof( ms );
	if ( !GlobalMemoryStatusEx( &ms ) ) return "";
	unsigned long long gb = ( ms.ullTotalPhys + ( 1ULL << 29 ) ) >> 30;
	char buf[8];
	snprintf( buf, sizeof( buf ), "%llu", gb );
	return buf;
}

static std::string GetGPUDescription()
{
	HKEY hKey;
	if ( RegOpenKeyExA( HKEY_LOCAL_MACHINE,
		"SYSTEM\\CurrentControlSet\\Control\\Class\\{4d36e968-e325-11ce-bfc1-08002be10318}\\0000",
		0, KEY_READ, &hKey ) != ERROR_SUCCESS )
		return "";
	char desc[256]{};
	DWORD size = sizeof( desc );
	RegQueryValueExA( hKey, "DriverDesc", nullptr, nullptr, (LPBYTE)desc, &size );
	RegCloseKey( hKey );
	return desc;
}

static std::string GetDiskModel()
{
	HANDLE h = CreateFileA( "\\\\.\\PhysicalDrive0", 0,
		FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr );
	if ( h == INVALID_HANDLE_VALUE ) return "";

	STORAGE_PROPERTY_QUERY query{};
	query.PropertyId = StorageDeviceProperty;
	query.QueryType = PropertyStandardQuery;

	char buf[1024]{};
	DWORD bytesRet = 0;
	std::string result;

	if ( DeviceIoControl( h, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof( query ),
		buf, sizeof( buf ), &bytesRet, nullptr ) )
	{
		auto* desc = reinterpret_cast<STORAGE_DEVICE_DESCRIPTOR*>( buf );
		if ( desc->VendorIdOffset && desc->VendorIdOffset < bytesRet )
			result += std::string( buf + desc->VendorIdOffset );
		result += ':';
		if ( desc->ProductIdOffset && desc->ProductIdOffset < bytesRet )
			result += std::string( buf + desc->ProductIdOffset );
		while ( !result.empty() && ( result.back() == ' ' || result.back() == ':' ) )
			result.pop_back();
	}
	CloseHandle( h );
	return result;
}

static std::string GetMachineSID()
{
	HANDLE hToken = nullptr;
	if ( !OpenProcessToken( GetCurrentProcess(), TOKEN_QUERY, &hToken ) ) return "";

	DWORD tokenSize = 0;
	GetTokenInformation( hToken, TokenUser, nullptr, 0, &tokenSize );
	if ( !tokenSize ) { CloseHandle( hToken ); return ""; }

	std::vector<uint8_t> tokenBuf( tokenSize );
	if ( !GetTokenInformation( hToken, TokenUser, tokenBuf.data(), tokenSize, &tokenSize ) )
	{ CloseHandle( hToken ); return ""; }
	CloseHandle( hToken );

	auto* tokenUser = reinterpret_cast<TOKEN_USER*>( tokenBuf.data() );
	LPSTR sidStr = nullptr;
	if ( !ConvertSidToStringSidA( tokenUser->User.Sid, &sidStr ) ) return "";

	std::string fullSid( sidStr );
	LocalFree( sidStr );

	auto lastDash = fullSid.rfind( '-' );
	if ( lastDash == std::string::npos ) return fullSid;
	return fullSid.substr( 0, lastDash );
}

static std::string GetBaseboardInfo()
{
	DWORD size = GetSystemFirmwareTable( 'RSMB', 0, nullptr, 0 );
	if ( !size || size > 1024 * 1024 ) return "";

	std::vector<uint8_t> buf( size );
	if ( GetSystemFirmwareTable( 'RSMB', 0, buf.data(), size ) != size ) return "";

	if ( size < 8 ) return "";
	const uint8_t* data = buf.data() + 8;
	const uint8_t* end = buf.data() + size;

	while ( data + 4 <= end )
	{
		uint8_t type = data[0];
		uint8_t length = data[1];

		if ( type == 2 && length >= 0x08 )
		{
			uint8_t mfgIdx = data[0x04];
			uint8_t prodIdx = data[0x05];
			const uint8_t* strArea = data + length;

			auto getString = [&]( uint8_t idx ) -> std::string {
				if ( idx == 0 ) return "";
				const uint8_t* p = strArea;
				uint8_t cur = 1;
				while ( p < end ) {
					const uint8_t* strStart = p;
					while ( p < end && *p != 0 ) p++;
					if ( cur == idx ) return std::string( (const char*)strStart, p - strStart );
					if ( p + 1 < end && *( p + 1 ) == 0 ) break;
					p++; cur++;
				}
				return "";
			};

			return getString( mfgIdx ) + ":" + getString( prodIdx );
		}

		const uint8_t* strArea = data + length;
		while ( strArea + 1 < end && !( strArea[0] == 0 && strArea[1] == 0 ) )
			strArea++;
		data = strArea + 2;
	}
	return "";
}

// ── Generate HWID hash ──
inline std::string Generate()
{
	std::string raw;
	raw += GetCPUBrand();        raw += '|';
	raw += GetCPUIDSignature();  raw += '|';
	raw += GetLogicalCPUCount(); raw += '|';
	raw += GetTotalRAMGB();      raw += '|';
	raw += GetGPUDescription();  raw += '|';
	raw += GetDiskModel();       raw += '|';
	raw += GetMachineSID();      raw += '|';
	raw += GetBaseboardInfo();

	return SHA256Hex( raw.c_str(), raw.size() );
}

// ── Whitelist check ──
inline bool IsAllowed( const std::string& hwid, const std::vector<std::string>& whitelist )
{
	for ( auto& h : whitelist )
		if ( h == hwid )
			return true;
	return false;
}

} // namespace hwid
