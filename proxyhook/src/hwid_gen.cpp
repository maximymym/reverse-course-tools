#include "hwid_gen.h"

#include <Windows.h>
#include <bcrypt.h>
#include <ntstatus.h>
#include <cstdio>
#include <cstring>

#pragma comment(lib, "bcrypt.lib")

#ifndef NT_SUCCESS
#  define NT_SUCCESS(s) ( ((NTSTATUS)(s)) >= 0 )
#endif

namespace proxyhook
{

bool Sha256( const void* data, size_t size, uint8_t out[32] )
{
    BCRYPT_ALG_HANDLE hAlg = nullptr;
    if ( !NT_SUCCESS( BCryptOpenAlgorithmProvider( &hAlg, BCRYPT_SHA256_ALGORITHM, nullptr, 0 ) ) )
        return false;

    BCRYPT_HASH_HANDLE hHash = nullptr;
    bool ok = false;
    if ( NT_SUCCESS( BCryptCreateHash( hAlg, &hHash, nullptr, 0, nullptr, 0, 0 ) ) )
    {
        if ( NT_SUCCESS( BCryptHashData( hHash, (PUCHAR)data, (ULONG)size, 0 ) ) &&
             NT_SUCCESS( BCryptFinishHash( hHash, out, 32, 0 ) ) )
            ok = true;
        BCryptDestroyHash( hHash );
    }
    BCryptCloseAlgorithmProvider( hAlg, 0 );
    return ok;
}

// Expand seed → 64 bytes через два SHA-256 раунда (seed и seed+"|expand").
static bool ExpandSeed( const std::string& seed, uint8_t out64[64] )
{
    if ( !Sha256( seed.data(), seed.size(), out64 ) )
        return false;
    std::string seed2 = seed + "|expand";
    return Sha256( seed2.data(), seed2.size(), out64 + 32 );
}

static void FormatHex( const uint8_t* src, size_t n, char* dst, bool upper )
{
    const char* tab = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    for ( size_t i = 0; i < n; i++ )
    {
        dst[i * 2]     = tab[( src[i] >> 4 ) & 0xF];
        dst[i * 2 + 1] = tab[src[i] & 0xF];
    }
    dst[n * 2] = 0;
}

static std::string FormatMac( const uint8_t mac[6] )
{
    char buf[32];
    snprintf( buf, sizeof( buf ), "%02X-%02X-%02X-%02X-%02X-%02X",
        mac[0], mac[1], mac[2], mac[3], mac[4], mac[5] );
    return buf;
}

// "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx" (lowercase, no braces).
static std::string FormatGuidBody( const uint8_t b[16] )
{
    char buf[64];
    snprintf( buf, sizeof( buf ),
        "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
        b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15] );
    return buf;
}

// 10-char system serial: alphanumeric uppercase derived from bytes.
static std::string FormatSystemSerial( const uint8_t* bytes, size_t n )
{
    static const char* alphabet = "ABCDEFGHJKLMNPQRSTUVWXYZ0123456789"; // без I/O чтобы похоже на HP/Dell
    const size_t alphaLen = 33;
    std::string r;
    r.resize( 10 );
    for ( size_t i = 0; i < 10; i++ )
        r[i] = alphabet[bytes[i % n] % alphaLen];
    return r;
}

// ProductId "XXXXX-XXX-XXXXXXX-XXXXX" (22 digits + 3 dashes).
static std::string FormatProductId( const uint8_t* bytes, size_t n )
{
    char digits[25];
    for ( size_t i = 0; i < 20; i++ )
        digits[i] = '0' + ( bytes[i % n] % 10 );
    digits[20] = 0;

    char buf[32];
    snprintf( buf, sizeof( buf ), "%c%c%c%c%c-%c%c%c-%c%c%c%c%c%c%c-%c%c%c%c%c",
        digits[0], digits[1], digits[2], digits[3], digits[4],
        digits[5], digits[6], digits[7],
        digits[8], digits[9], digits[10], digits[11], digits[12], digits[13], digits[14],
        digits[15], digits[16], digits[17], digits[18], digits[19] );
    return buf;
}

bool DeriveHwid( const std::string& seed, HwidFakeValues& out )
{
    uint8_t buf[64]{};
    if ( !ExpandSeed( seed, buf ) )
        return false;

    // MAC: bytes[0:6]. Первый октет → local-admin bit 0x02, clear multicast bit 0x01.
    //   byte[0] &= 0xFE; byte[0] |= 0x02;
    for ( int i = 0; i < 6; i++ )
        out.mac[i] = buf[i];
    out.mac[0] &= 0xFE;
    out.mac[0] |= 0x02;
    out.macStr = FormatMac( out.mac.data() );

    // VolumeSerial — bytes[6:10] LE DWORD.
    out.volumeSerial =
        (uint32_t)buf[6]        |
        ( (uint32_t)buf[7] << 8 ) |
        ( (uint32_t)buf[8] << 16 ) |
        ( (uint32_t)buf[9] << 24 );
    if ( out.volumeSerial == 0 )
        out.volumeSerial = 0xCAFEBABE; // guard против нулевого (некоторые apps трактуют как ошибку)

    // DiskSerial — bytes[10:20] → 20 hex chars uppercase.
    char hex20[41];
    FormatHex( buf + 10, 10, hex20, /*upper*/ true );
    out.diskSerial = hex20;

    // MachineGuid — bytes[20:36] → GUID без braces, lowercase.
    out.machineGuid = FormatGuidBody( buf + 20 );

    // HwProfileGuid — bytes[36:52] → GUID с braces, lowercase.
    out.hwProfileGuid = "{" + FormatGuidBody( buf + 36 ) + "}";

    // SMBIOS UUID — bytes[16:32] raw + formatted string.
    for ( int i = 0; i < 16; i++ )
        out.smbiosUuid[i] = buf[16 + i];
    out.smbiosUuidStr = FormatGuidBody( out.smbiosUuid.data() );

    // SystemSerial — 10 chars из bytes[40:50].
    out.systemSerial = FormatSystemSerial( buf + 40, 10 );

    // BIOS vendor — зафиксирован как плашка от AMI (популярный vendor, не подозрителен).
    out.biosVendor = "American Megatrends Inc.";

    // BIOS version: F.<8 hex chars> — простой формат AMI обновлений.
    char bvBuf[16];
    FormatHex( buf + 50, 4, bvBuf, /*upper*/ true );
    out.biosVersion = std::string( "F." ) + bvBuf;

    // ProductId — bytes[54:64].
    out.productId = FormatProductId( buf + 54, 10 );

    return true;
}

} // namespace proxyhook
