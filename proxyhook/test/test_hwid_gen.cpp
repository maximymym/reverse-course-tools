// test_hwid_gen.cpp — standalone детерминизм-тест.
//
// Build (из proxyhook/):
//   cl /std:c++17 /EHsc /I src test/test_hwid_gen.cpp src/hwid_gen.cpp /link bcrypt.lib
//   test_hwid_gen.exe
//
// Expected output: "ALL PASSED".
// Падение ANY assert → exit 1.

#include "hwid_gen.h"

#include <cassert>
#include <cstdio>
#include <string>
#include <vector>
#include <unordered_set>

using namespace proxyhook;

static int g_failures = 0;

#define CHECK( cond, msg ) do { \
    if ( !( cond ) ) { \
        fprintf( stderr, "FAIL: %s (line %d): %s\n", msg, __LINE__, #cond ); \
        g_failures++; \
    } \
} while ( 0 )

// (1) Детерминизм: same seed → same values (N повторов).
static void TestDeterminism()
{
    const std::string seeds[] = {
        "76561198725850781_2026-04",
        "abc",
        "",
        "setup_0_testlogin_2026-04",
        "0_0-0",
    };
    for ( const auto& s : seeds )
    {
        HwidFakeValues first;
        if ( !DeriveHwid( s, first ) )
        {
            fprintf( stderr, "FAIL: DeriveHwid returned false for seed '%s'\n", s.c_str() );
            g_failures++;
            continue;
        }
        for ( int i = 0; i < 100; i++ )
        {
            HwidFakeValues next;
            DeriveHwid( s, next );
            CHECK( first.mac == next.mac,                  "mac stability" );
            CHECK( first.volumeSerial == next.volumeSerial, "volSerial stability" );
            CHECK( first.diskSerial == next.diskSerial,    "diskSerial stability" );
            CHECK( first.machineGuid == next.machineGuid,  "machineGuid stability" );
            CHECK( first.hwProfileGuid == next.hwProfileGuid, "hwProfileGuid stability" );
            CHECK( first.systemSerial == next.systemSerial,    "systemSerial stability" );
            CHECK( first.smbiosUuid == next.smbiosUuid,    "smbiosUuid stability" );
            CHECK( first.productId == next.productId,      "productId stability" );
            CHECK( first.macStr == next.macStr,            "macStr stability" );
        }
    }
}

// (2) Разные seeds → разные MAC/guid (с высокой вероятностью, коллизии в SHA-256 невозможны).
static void TestDivergence()
{
    std::vector<std::string> seeds = {
        "76561198725850781_2026-04",
        "76561198725850782_2026-04",
        "76561198725850781_2026-05",
        "76561198111111111_2026-04",
        "76561198999999999_2026-04",
    };
    std::unordered_set<std::string> macs, guids, vols, disks;
    for ( const auto& s : seeds )
    {
        HwidFakeValues v;
        DeriveHwid( s, v );
        macs.insert( v.macStr );
        guids.insert( v.machineGuid );
        vols.insert( std::to_string( v.volumeSerial ) );
        disks.insert( v.diskSerial );
    }
    CHECK( macs.size() == seeds.size(),  "all MACs unique" );
    CHECK( guids.size() == seeds.size(), "all MachineGuids unique" );
    CHECK( vols.size() == seeds.size(),  "all VolumeSerials unique" );
    CHECK( disks.size() == seeds.size(), "all DiskSerials unique" );
}

// (3) Format validation.
static void TestFormats()
{
    HwidFakeValues v;
    DeriveHwid( "76561198725850781_2026-04", v );

    // MAC: "XX-XX-XX-XX-XX-XX" (17 chars)
    CHECK( v.macStr.size() == 17, "macStr len" );
    CHECK( v.macStr[2] == '-' && v.macStr[5] == '-', "macStr dashes" );

    // Locally-administered bit set (0x02), multicast bit clear.
    CHECK( ( v.mac[0] & 0x02 ) != 0, "MAC LAA bit" );
    CHECK( ( v.mac[0] & 0x01 ) == 0, "MAC multicast bit clear" );

    // MachineGuid: "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx" 36 chars, lowercase, no braces.
    CHECK( v.machineGuid.size() == 36, "machineGuid len" );
    CHECK( v.machineGuid[8] == '-' && v.machineGuid[13] == '-' &&
           v.machineGuid[18] == '-' && v.machineGuid[23] == '-', "machineGuid dashes" );
    CHECK( v.machineGuid.front() != '{', "machineGuid no braces" );

    // HwProfileGuid: with braces, 38 chars.
    CHECK( v.hwProfileGuid.size() == 38, "hwProfileGuid len" );
    CHECK( v.hwProfileGuid.front() == '{' && v.hwProfileGuid.back() == '}',
           "hwProfileGuid braces" );

    // SMBIOS UUID string: 36 chars, no braces.
    CHECK( v.smbiosUuidStr.size() == 36, "smbiosUuidStr len" );

    // DiskSerial: 20 ASCII hex uppercase.
    CHECK( v.diskSerial.size() == 20, "diskSerial len" );
    for ( char c : v.diskSerial )
    {
        bool hex = ( c >= '0' && c <= '9' ) || ( c >= 'A' && c <= 'F' );
        CHECK( hex, "diskSerial hex-only" );
    }

    // SystemSerial: 10 chars alphanumeric uppercase (no I/O to look plausible).
    CHECK( v.systemSerial.size() == 10, "systemSerial len" );
    for ( char c : v.systemSerial )
    {
        bool ok = ( c >= 'A' && c <= 'Z' ) || ( c >= '0' && c <= '9' );
        CHECK( ok, "systemSerial alnum" );
        CHECK( c != 'I' && c != 'O', "systemSerial no I/O" );
    }

    // Non-zero volume serial.
    CHECK( v.volumeSerial != 0, "volSerial non-zero" );

    // ProductId: "XXXXX-XXX-XXXXXXX-XXXXX" — 22 digits + 3 dashes = 23 chars total.
    CHECK( v.productId.size() == 23, "productId len" );
    CHECK( v.productId[5] == '-' && v.productId[9] == '-' && v.productId[17] == '-',
           "productId dashes" );

    // BIOS
    CHECK( v.biosVendor == "American Megatrends Inc.", "biosVendor fixed" );
    CHECK( v.biosVersion.substr( 0, 2 ) == "F.", "biosVersion prefix" );

    printf( "Sample values for seed '76561198725850781_2026-04':\n" );
    printf( "  MAC        : %s\n",   v.macStr.c_str() );
    printf( "  VolSerial  : %08X\n", v.volumeSerial );
    printf( "  DiskSerial : %s\n",   v.diskSerial.c_str() );
    printf( "  MachineGUID: %s\n",   v.machineGuid.c_str() );
    printf( "  HwProfGUID : %s\n",   v.hwProfileGuid.c_str() );
    printf( "  SMBIOS UUID: %s\n",   v.smbiosUuidStr.c_str() );
    printf( "  SysSerial  : %s\n",   v.systemSerial.c_str() );
    printf( "  BIOS Vend  : %s\n",   v.biosVendor.c_str() );
    printf( "  BIOS Vers  : %s\n",   v.biosVersion.c_str() );
    printf( "  ProductId  : %s\n",   v.productId.c_str() );
}

// (4) Sha256 хорошо знакомая точка сравнения: пустая строка → e3b0c44...
static void TestSha256KnownVectors()
{
    uint8_t out[32];
    // SHA-256("") = e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855
    CHECK( Sha256( "", 0, out ), "Sha256 empty ok" );
    const uint8_t expected[] = {
        0xe3, 0xb0, 0xc4, 0x42, 0x98, 0xfc, 0x1c, 0x14,
        0x9a, 0xfb, 0xf4, 0xc8, 0x99, 0x6f, 0xb9, 0x24,
        0x27, 0xae, 0x41, 0xe4, 0x64, 0x9b, 0x93, 0x4c,
        0xa4, 0x95, 0x99, 0x1b, 0x78, 0x52, 0xb8, 0x55
    };
    bool match = memcmp( out, expected, 32 ) == 0;
    CHECK( match, "Sha256('') matches known vector" );

    // SHA-256("abc") = ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad
    CHECK( Sha256( "abc", 3, out ), "Sha256 abc ok" );
    const uint8_t expectedAbc[] = {
        0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea,
        0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
        0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c,
        0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad
    };
    match = memcmp( out, expectedAbc, 32 ) == 0;
    CHECK( match, "Sha256('abc') matches known vector" );
}

int main()
{
    TestSha256KnownVectors();
    TestDeterminism();
    TestDivergence();
    TestFormats();

    if ( g_failures == 0 )
    {
        printf( "\nALL PASSED\n" );
        return 0;
    }
    else
    {
        fprintf( stderr, "\n%d FAILURES\n", g_failures );
        return 1;
    }
}
