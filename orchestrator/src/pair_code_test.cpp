// Standalone test exe for pair_code codec. No gtest dependency.
//
// Build (from orchestrator/):
//   cmake --build build --config Release --target pair_code_test
// Run:
//   build/Release/pair_code_test.exe
//
// Exit 0 = all PASS, 1 = at least one FAIL.

#include "pair_code.h"

#include <json.hpp>

#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>

namespace
{

int g_passed = 0;
int g_failed = 0;

void ReportPass( const char* name )
{
	std::printf( "PASS: %s\n", name );
	++g_passed;
}

void ReportFail( const char* name, const std::string& reason )
{
	std::printf( "FAIL: %s: %s\n", name, reason.c_str() );
	++g_failed;
}

#define EXPECT_TRUE( name, cond, reason ) \
	do { if ( !( cond ) ) { ReportFail( name, reason ); return; } } while ( 0 )

// 64-char hex (32-byte token) and 44-char base64 (~32-byte secret) test fixtures
const char* const kToken =
	"a1b2c3d4e5f60718293a4b5c6d7e8f9012345678abcdef0123456789abcdef01";
// 32 zero bytes -> base64 standard
const char* const kSecret = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=";
// Same secret in base64url (no padding)
const char* const kSecretUrl = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";

std::string EncodeOk()
{
	return pair_code::Encode(
		"144.31.85.217",
		5050,
		"dotafarm",
		kToken,
		"two-stand-prod",
		kSecret,
		"S",
		0 );
}

// --- Tests ---------------------------------------------------------------

void TestRoundTrip()
{
	const char* N = "round_trip";
	std::string code = EncodeOk();
	auto r = pair_code::Decode( code );
	EXPECT_TRUE( N, r.ok, std::string( "decode failed: " ) + pair_code::Describe( r.error ) );
	EXPECT_TRUE( N, r.decoded.version == 1, "version != 1" );
	EXPECT_TRUE( N, r.decoded.relayHost == "144.31.85.217", "relayHost mismatch" );
	EXPECT_TRUE( N, r.decoded.relayPort == 5050, "relayPort mismatch" );
	EXPECT_TRUE( N, r.decoded.userId == "dotafarm", "userId mismatch" );
	EXPECT_TRUE( N, r.decoded.userAuthToken == kToken, "tok mismatch" );
	EXPECT_TRUE( N, r.decoded.pairId == "two-stand-prod", "pid mismatch" );
	EXPECT_TRUE( N, r.decoded.pairSecret == kSecret, "sec mismatch" );
	EXPECT_TRUE( N, r.decoded.roleHint == "S", "rl mismatch" );
	EXPECT_TRUE( N, r.decoded.ttlSeconds == 0, "ttl mismatch" );
	ReportPass( N );
}

void TestPrefixMissing()
{
	const char* N = "prefix_missing";
	std::string code = EncodeOk();
	std::string broken = "WRONGPREFIX-" + code;
	auto r = pair_code::Decode( broken );
	EXPECT_TRUE( N, !r.ok, "expected fail" );
	EXPECT_TRUE( N, r.error == pair_code::Error::BadPrefix, "wrong error" );
	ReportPass( N );
}

void TestBadFormat()
{
	const char* N = "bad_format";
	// only 2 parts after the prefix
	std::string broken = "DOTAFARM-PAIR-1.payload";
	auto r = pair_code::Decode( broken );
	EXPECT_TRUE( N, !r.ok, "expected fail" );
	EXPECT_TRUE( N, r.error == pair_code::Error::BadFormat, "wrong error" );
	ReportPass( N );
}

void TestBadVersion()
{
	const char* N = "bad_version";
	std::string code = EncodeOk();
	// Replace "DOTAFARM-PAIR-1." with "DOTAFARM-PAIR-2."
	std::string broken = code;
	broken[14] = '2';  // the version digit
	auto r = pair_code::Decode( broken );
	EXPECT_TRUE( N, !r.ok, "expected fail" );
	EXPECT_TRUE( N, r.error == pair_code::Error::BadVersion, "wrong error" );
	ReportPass( N );
}

void TestCrcFail()
{
	const char* N = "crc_fail";
	std::string code = EncodeOk();
	// Flip last hex char so CRC mismatches.
	char& last = code.back();
	last = ( last == 'f' ? '0' : char( last + 1 ) );
	auto r = pair_code::Decode( code );
	EXPECT_TRUE( N, !r.ok, "expected fail" );
	EXPECT_TRUE( N, r.error == pair_code::Error::CrcFail, "wrong error" );
	ReportPass( N );
}

void TestBadBase64()
{
	const char* N = "bad_base64";
	// Construct a payload with illegal char '*'.
	std::string body  = "***";
	// Compute correct CRC over the (illegal) body so we hit base64 stage,
	// not CRC fail.
	std::string code = "DOTAFARM-PAIR-1.";
	code += body;
	code.push_back( '.' );
	// CRC8 of "***": compute manually using same algorithm.
	uint8_t crc = 0;
	for ( char c : body )
	{
		crc ^= uint8_t( c );
		for ( int i = 0; i < 8; ++i )
			crc = uint8_t( ( crc & 0x80 ) ? ( ( crc << 1 ) ^ 0x07 ) : ( crc << 1 ) );
	}
	const char* hex = "0123456789abcdef";
	code.push_back( hex[( crc >> 4 ) & 0xF] );
	code.push_back( hex[crc & 0xF] );

	auto r = pair_code::Decode( code );
	EXPECT_TRUE( N, !r.ok, "expected fail" );
	EXPECT_TRUE( N, r.error == pair_code::Error::BadBase64, "wrong error" );
	ReportPass( N );
}

// Helper: build a code from arbitrary JSON string (re-encodes through
// base64url + CRC). Used to inject malformed/missing-field payloads.
std::string BuildCode( const std::string& jsonStr )
{
	static const char* alpha =
		"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
	std::string body;
	const auto& d = jsonStr;
	size_t i = 0;
	while ( i + 3 <= d.size() )
	{
		uint32_t v = ( uint32_t( (uint8_t)d[i] ) << 16 )
				   | ( uint32_t( (uint8_t)d[i + 1] ) << 8 )
				   |   uint32_t( (uint8_t)d[i + 2] );
		body.push_back( alpha[( v >> 18 ) & 0x3F] );
		body.push_back( alpha[( v >> 12 ) & 0x3F] );
		body.push_back( alpha[( v >> 6 ) & 0x3F] );
		body.push_back( alpha[v & 0x3F] );
		i += 3;
	}
	size_t rem = d.size() - i;
	if ( rem == 1 )
	{
		uint32_t v = uint32_t( (uint8_t)d[i] ) << 16;
		body.push_back( alpha[( v >> 18 ) & 0x3F] );
		body.push_back( alpha[( v >> 12 ) & 0x3F] );
	}
	else if ( rem == 2 )
	{
		uint32_t v = ( uint32_t( (uint8_t)d[i] ) << 16 )
				   | ( uint32_t( (uint8_t)d[i + 1] ) << 8 );
		body.push_back( alpha[( v >> 18 ) & 0x3F] );
		body.push_back( alpha[( v >> 12 ) & 0x3F] );
		body.push_back( alpha[( v >> 6 ) & 0x3F] );
	}
	uint8_t crc = 0;
	for ( char c : body )
	{
		crc ^= uint8_t( c );
		for ( int i2 = 0; i2 < 8; ++i2 )
			crc = uint8_t( ( crc & 0x80 ) ? ( ( crc << 1 ) ^ 0x07 ) : ( crc << 1 ) );
	}
	const char* hex = "0123456789abcdef";
	std::string out = "DOTAFARM-PAIR-1.";
	out += body;
	out.push_back( '.' );
	out.push_back( hex[( crc >> 4 ) & 0xF] );
	out.push_back( hex[crc & 0xF] );
	return out;
}

void TestBadJson()
{
	const char* N = "bad_json";
	// "not-a-json" decodes from base64url cleanly (any non-empty byte seq is
	// valid base64url-decodable as bytes — we just want garbage that is NOT
	// JSON). Use literal bytes "x".
	std::string code = BuildCode( "this is not json {" );
	auto r = pair_code::Decode( code );
	EXPECT_TRUE( N, !r.ok, "expected fail" );
	EXPECT_TRUE( N, r.error == pair_code::Error::BadJson, "wrong error" );
	ReportPass( N );
}

void TestMissingField()
{
	const char* N = "missing_field";
	nlohmann::json j;
	j["v"]   = 1;
	j["rly"] = "127.0.0.1";
	j["p"]   = 5050;
	j["uid"] = "u";
	j["tok"] = kToken;
	j["pid"] = "pid";
	j["sec"] = kSecret;
	j["rl"]  = "S";
	j["iat"] = 1716700000;
	// missing "ttl"
	std::string code = BuildCode( j.dump() );
	auto r = pair_code::Decode( code );
	EXPECT_TRUE( N, !r.ok, "expected fail" );
	EXPECT_TRUE( N, r.error == pair_code::Error::MissingField, "wrong error" );
	ReportPass( N );
}

void TestBadFieldBadPort()
{
	const char* N = "bad_field_port_zero";
	nlohmann::json j;
	j["v"]   = 1;
	j["rly"] = "127.0.0.1";
	j["p"]   = 0;  // out of range (1..65535)
	j["uid"] = "u";
	j["tok"] = kToken;
	j["pid"] = "pid";
	j["sec"] = kSecret;
	j["rl"]  = "S";
	j["iat"] = 1716700000;
	j["ttl"] = 0;
	auto r = pair_code::Decode( BuildCode( j.dump() ) );
	EXPECT_TRUE( N, !r.ok, "expected fail" );
	EXPECT_TRUE( N, r.error == pair_code::Error::BadField, "wrong error" );
	ReportPass( N );
}

void TestBadFieldRoleHint()
{
	const char* N = "bad_field_role";
	nlohmann::json j;
	j["v"]   = 1;
	j["rly"] = "127.0.0.1";
	j["p"]   = 5050;
	j["uid"] = "u";
	j["tok"] = kToken;
	j["pid"] = "pid";
	j["sec"] = kSecret;
	j["rl"]  = "X";  // must be "M" or "S"
	j["iat"] = 1716700000;
	j["ttl"] = 0;
	auto r = pair_code::Decode( BuildCode( j.dump() ) );
	EXPECT_TRUE( N, !r.ok, "expected fail" );
	EXPECT_TRUE( N, r.error == pair_code::Error::BadField, "wrong error" );
	ReportPass( N );
}

void TestBadFieldShortToken()
{
	const char* N = "bad_field_short_token";
	nlohmann::json j;
	j["v"]   = 1;
	j["rly"] = "127.0.0.1";
	j["p"]   = 5050;
	j["uid"] = "u";
	j["tok"] = "short";  // < 32
	j["pid"] = "pid";
	j["sec"] = kSecret;
	j["rl"]  = "S";
	j["iat"] = 1716700000;
	j["ttl"] = 0;
	auto r = pair_code::Decode( BuildCode( j.dump() ) );
	EXPECT_TRUE( N, !r.ok, "expected fail" );
	EXPECT_TRUE( N, r.error == pair_code::Error::BadField, "wrong error" );
	ReportPass( N );
}

void TestBadFieldHostHasScheme()
{
	const char* N = "bad_field_host_scheme";
	nlohmann::json j;
	j["v"]   = 1;
	j["rly"] = "tcp://127.0.0.1";  // scheme not allowed
	j["p"]   = 5050;
	j["uid"] = "u";
	j["tok"] = kToken;
	j["pid"] = "pid";
	j["sec"] = kSecret;
	j["rl"]  = "S";
	j["iat"] = 1716700000;
	j["ttl"] = 0;
	auto r = pair_code::Decode( BuildCode( j.dump() ) );
	EXPECT_TRUE( N, !r.ok, "expected fail" );
	EXPECT_TRUE( N, r.error == pair_code::Error::BadField, "wrong error" );
	ReportPass( N );
}

void TestExpired()
{
	const char* N = "expired";
	nlohmann::json j;
	j["v"]   = 1;
	j["rly"] = "127.0.0.1";
	j["p"]   = 5050;
	j["uid"] = "u";
	j["tok"] = kToken;
	j["pid"] = "pid";
	j["sec"] = kSecret;
	j["rl"]  = "S";
	j["iat"] = 1000000;  // ~1970 epoch
	j["ttl"] = 60;       // 60 sec — long gone
	auto r = pair_code::Decode( BuildCode( j.dump() ) );
	EXPECT_TRUE( N, !r.ok, "expected fail" );
	EXPECT_TRUE( N, r.error == pair_code::Error::Expired, "wrong error" );
	ReportPass( N );
}

void TestWhitespaceTolerance()
{
	const char* N = "whitespace_tolerance";
	std::string code = EncodeOk();
	std::string wrapped = "\n\t  " + code + "  \r\n";
	auto r = pair_code::Decode( wrapped );
	EXPECT_TRUE( N, r.ok, std::string( "decode failed: " ) + pair_code::Describe( r.error ) );
	ReportPass( N );
}

void TestSecretBase64UrlAccepted()
{
	const char* N = "secret_base64url_accepted";
	std::string code = pair_code::Encode(
		"127.0.0.1", 5050, "u", kToken, "pid",
		kSecretUrl,  // no padding, base64url form
		"S", 0 );
	auto r = pair_code::Decode( code );
	EXPECT_TRUE( N, r.ok, std::string( "decode failed: " ) + pair_code::Describe( r.error ) );
	EXPECT_TRUE( N, r.decoded.pairSecret == kSecretUrl, "sec roundtrip mismatch" );
	ReportPass( N );
}

void TestRoleMasterAccepted()
{
	const char* N = "role_master_accepted";
	std::string code = pair_code::Encode(
		"127.0.0.1", 5050, "u", kToken, "pid", kSecret, "M", 0 );
	auto r = pair_code::Decode( code );
	EXPECT_TRUE( N, r.ok, std::string( "decode failed: " ) + pair_code::Describe( r.error ) );
	EXPECT_TRUE( N, r.decoded.roleHint == "M", "rl != M" );
	ReportPass( N );
}

void TestDescribeAllErrors()
{
	const char* N = "describe_covers_all_errors";
	using E = pair_code::Error;
	E all[] = {
		E::BadPrefix, E::BadFormat, E::BadVersion, E::CrcFail,
		E::BadBase64, E::BadJson, E::MissingField, E::BadField,
		E::Expired, E::SameMachine
	};
	for ( E e : all )
	{
		const char* s = pair_code::Describe( e );
		EXPECT_TRUE( N, s != nullptr && s[0] != '\0', "empty describe" );
	}
	ReportPass( N );
}

}  // namespace

int main()
{
	TestRoundTrip();
	TestPrefixMissing();
	TestBadFormat();
	TestBadVersion();
	TestCrcFail();
	TestBadBase64();
	TestBadJson();
	TestMissingField();
	TestBadFieldBadPort();
	TestBadFieldRoleHint();
	TestBadFieldShortToken();
	TestBadFieldHostHasScheme();
	TestExpired();
	TestWhitespaceTolerance();
	TestSecretBase64UrlAccepted();
	TestRoleMasterAccepted();
	TestDescribeAllErrors();

	std::printf( "\n[pair_code_test] %d passed, %d failed\n", g_passed, g_failed );
	return g_failed == 0 ? 0 : 1;
}
