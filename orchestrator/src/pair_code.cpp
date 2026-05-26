#include "pair_code.h"

#include <json.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")

namespace pair_code
{

namespace
{

constexpr const char* kPrefix = "DOTAFARM-PAIR-";
constexpr size_t      kPrefixLen = 14;  // strlen("DOTAFARM-PAIR-")
constexpr int         kMajorVersion = 1;

// ── base64url (RFC 4648 §5, no '=' padding) ─────────────────────────────
static const char kB64UrlAlphabet[] =
	"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

std::string Base64UrlEncode( const std::vector<uint8_t>& data )
{
	std::string out;
	out.reserve( ( data.size() * 4 + 2 ) / 3 );
	size_t i = 0;
	const size_t n = data.size();
	while ( i + 3 <= n )
	{
		uint32_t v = ( uint32_t( data[i] ) << 16 )
				   | ( uint32_t( data[i + 1] ) << 8 )
				   |   uint32_t( data[i + 2] );
		out.push_back( kB64UrlAlphabet[( v >> 18 ) & 0x3F] );
		out.push_back( kB64UrlAlphabet[( v >> 12 ) & 0x3F] );
		out.push_back( kB64UrlAlphabet[( v >> 6 ) & 0x3F] );
		out.push_back( kB64UrlAlphabet[v & 0x3F] );
		i += 3;
	}
	const size_t rem = n - i;
	if ( rem == 1 )
	{
		uint32_t v = uint32_t( data[i] ) << 16;
		out.push_back( kB64UrlAlphabet[( v >> 18 ) & 0x3F] );
		out.push_back( kB64UrlAlphabet[( v >> 12 ) & 0x3F] );
		// no padding
	}
	else if ( rem == 2 )
	{
		uint32_t v = ( uint32_t( data[i] ) << 16 )
				   | ( uint32_t( data[i + 1] ) << 8 );
		out.push_back( kB64UrlAlphabet[( v >> 18 ) & 0x3F] );
		out.push_back( kB64UrlAlphabet[( v >> 12 ) & 0x3F] );
		out.push_back( kB64UrlAlphabet[( v >> 6 ) & 0x3F] );
	}
	return out;
}

bool Base64UrlDecode( const std::string& s, std::vector<uint8_t>& out )
{
	out.clear();
	out.reserve( ( s.size() * 3 ) / 4 );

	auto charVal = []( char c ) -> int
	{
		if ( c >= 'A' && c <= 'Z' ) return c - 'A';
		if ( c >= 'a' && c <= 'z' ) return c - 'a' + 26;
		if ( c >= '0' && c <= '9' ) return c - '0' + 52;
		if ( c == '-' ) return 62;
		if ( c == '_' ) return 63;
		return -1;
	};

	uint32_t buf = 0;
	int      bits = 0;
	for ( char c : s )
	{
		if ( c == '=' ) return false;  // strict: no padding allowed
		int v = charVal( c );
		if ( v < 0 ) return false;
		buf = ( buf << 6 ) | uint32_t( v );
		bits += 6;
		if ( bits >= 8 )
		{
			bits -= 8;
			out.push_back( uint8_t( ( buf >> bits ) & 0xFF ) );
		}
	}
	// Trailing bits (if any) MUST be zero per RFC 4648 §3.5; we don't strictly
	// enforce that — but the leftover count must be 0, 2, or 4. 1/3/5 = error.
	if ( bits == 1 || bits == 3 || bits == 5 ) return false;
	return true;
}

// ── CRC8-CCITT (poly 0x07, init 0x00) ────────────────────────────────────
uint8_t Crc8( const std::string& s )
{
	uint8_t crc = 0x00;
	for ( char c : s )
	{
		crc ^= uint8_t( c );
		for ( int i = 0; i < 8; ++i )
		{
			if ( crc & 0x80 )
				crc = uint8_t( ( crc << 1 ) ^ 0x07 );
			else
				crc = uint8_t( crc << 1 );
		}
	}
	return crc;
}

std::string Crc8Hex( const std::string& s )
{
	uint8_t c = Crc8( s );
	const char* hex = "0123456789abcdef";
	std::string out;
	out.push_back( hex[( c >> 4 ) & 0xF] );
	out.push_back( hex[c & 0xF] );
	return out;
}

bool ParseHex2( const std::string& s, uint8_t& out )
{
	if ( s.size() != 2 ) return false;
	auto val = []( char c, int& v ) -> bool
	{
		if ( c >= '0' && c <= '9' ) { v = c - '0'; return true; }
		if ( c >= 'a' && c <= 'f' ) { v = c - 'a' + 10; return true; }
		if ( c >= 'A' && c <= 'F' ) { v = c - 'A' + 10; return true; }
		return false;
	};
	int hi = 0, lo = 0;
	if ( !val( s[0], hi ) || !val( s[1], lo ) ) return false;
	out = uint8_t( ( hi << 4 ) | lo );
	return true;
}

std::string TrimWs( const std::string& s )
{
	size_t b = 0, e = s.size();
	while ( b < e && std::isspace( (unsigned char)s[b] ) ) ++b;
	while ( e > b && std::isspace( (unsigned char)s[e - 1] ) ) --e;
	return s.substr( b, e - b );
}

int64_t NowUnixMs()
{
	using namespace std::chrono;
	return duration_cast< milliseconds >(
		system_clock::now().time_since_epoch() ).count();
}

}  // namespace

const char* Describe( Error e )
{
	switch ( e )
	{
		case Error::BadPrefix:    return "Pair code does not start with DOTAFARM-PAIR-";
		case Error::BadFormat:    return "Pair code must have version.payload.crc8 structure";
		case Error::BadVersion:   return "Unsupported pair code version";
		case Error::CrcFail:      return "Pair code CRC8 mismatch (corrupted in transit)";
		case Error::BadBase64:    return "Pair code payload is not valid base64url";
		case Error::BadJson:      return "Pair code payload is not valid JSON";
		case Error::MissingField: return "Pair code payload missing required field";
		case Error::BadField:     return "Pair code payload has invalid field value";
		case Error::Expired:      return "Pair code has expired";
		case Error::SameMachine:  return "Pair code was generated on this machine";
	}
	return "Unknown pair code error";
}

std::string Encode(
	const std::string& relayHost,
	uint16_t           relayPort,
	const std::string& userId,
	const std::string& userAuthToken,
	const std::string& pairId,
	const std::string& pairSecret,
	const std::string& roleHint,
	int                ttlSeconds )
{
	nlohmann::json j;
	j["v"]   = kMajorVersion;
	j["rly"] = relayHost;
	j["p"]   = int( relayPort );
	j["uid"] = userId;
	j["tok"] = userAuthToken;
	j["pid"] = pairId;
	j["sec"] = pairSecret;
	j["rl"]  = roleHint.empty() ? std::string( "S" ) : roleHint;
	j["iat"] = NowUnixMs() / 1000;       // seconds, per schema
	j["ttl"] = ttlSeconds;

	std::string compact = j.dump();      // no indent → compact
	std::vector<uint8_t> bytes( compact.begin(), compact.end() );
	std::string body = Base64UrlEncode( bytes );
	std::string crc  = Crc8Hex( body );

	std::string out;
	out.reserve( kPrefixLen + 2 + body.size() + 1 + crc.size() + 1 );
	out.append( kPrefix );
	out.push_back( '1' );
	out.push_back( '.' );
	out.append( body );
	out.push_back( '.' );
	out.append( crc );
	return out;
}

DecodeResult Decode( const std::string& codeIn )
{
	DecodeResult r;
	std::string code = TrimWs( codeIn );

	// ── prefix ──
	if ( code.size() < kPrefixLen
	  || code.compare( 0, kPrefixLen, kPrefix ) != 0 )
	{
		r.error = Error::BadPrefix;
		return r;
	}
	std::string tail = code.substr( kPrefixLen );

	// ── split version.body.crc on '.' (exactly 3 parts) ──
	size_t d1 = tail.find( '.' );
	if ( d1 == std::string::npos ) { r.error = Error::BadFormat; return r; }
	size_t d2 = tail.find( '.', d1 + 1 );
	if ( d2 == std::string::npos ) { r.error = Error::BadFormat; return r; }
	if ( tail.find( '.', d2 + 1 ) != std::string::npos )
	{
		r.error = Error::BadFormat;
		return r;
	}

	std::string verPart  = tail.substr( 0, d1 );
	std::string bodyPart = tail.substr( d1 + 1, d2 - d1 - 1 );
	std::string crcPart  = tail.substr( d2 + 1 );

	if ( verPart.empty() || bodyPart.empty() || crcPart.empty() )
	{
		r.error = Error::BadFormat;
		return r;
	}

	// ── version ──
	if ( verPart != "1" )
	{
		r.error = Error::BadVersion;
		return r;
	}

	// ── crc ──
	uint8_t crcGiven = 0;
	if ( !ParseHex2( crcPart, crcGiven ) )
	{
		r.error = Error::BadFormat;
		return r;
	}
	uint8_t crcCalc = Crc8( bodyPart );
	if ( crcGiven != crcCalc )
	{
		r.error = Error::CrcFail;
		return r;
	}

	// ── base64url ──
	std::vector<uint8_t> bytes;
	if ( !Base64UrlDecode( bodyPart, bytes ) )
	{
		r.error = Error::BadBase64;
		return r;
	}
	std::string jsonStr( bytes.begin(), bytes.end() );

	// ── json ──
	nlohmann::json j;
	try
	{
		j = nlohmann::json::parse( jsonStr );
	}
	catch ( const std::exception& )
	{
		r.error = Error::BadJson;
		return r;
	}
	if ( !j.is_object() )
	{
		r.error = Error::BadJson;
		return r;
	}

	const char* required[] = {
		"v", "rly", "p", "uid", "tok", "pid", "sec", "rl", "iat", "ttl" };
	for ( const char* k : required )
	{
		if ( !j.contains( k ) )
		{
			r.error = Error::MissingField;
			return r;
		}
	}

	try
	{
		if ( !j["v"].is_number_integer() )       { r.error = Error::BadField; return r; }
		int v = j["v"].get< int >();
		if ( v != kMajorVersion )                { r.error = Error::BadVersion; return r; }

		if ( !j["rly"].is_string() )             { r.error = Error::BadField; return r; }
		std::string rly = j["rly"].get< std::string >();
		if ( rly.empty() )                       { r.error = Error::BadField; return r; }
		// host must not embed a scheme prefix
		if ( rly.find( "://" ) != std::string::npos ) { r.error = Error::BadField; return r; }

		if ( !j["p"].is_number_integer() )       { r.error = Error::BadField; return r; }
		int p = j["p"].get< int >();
		if ( p < 1 || p > 65535 )                { r.error = Error::BadField; return r; }

		if ( !j["uid"].is_string() )             { r.error = Error::BadField; return r; }
		std::string uid = j["uid"].get< std::string >();
		if ( uid.empty() )                       { r.error = Error::BadField; return r; }

		if ( !j["tok"].is_string() )             { r.error = Error::BadField; return r; }
		std::string tok = j["tok"].get< std::string >();
		if ( tok.size() < 32 )                   { r.error = Error::BadField; return r; }

		if ( !j["pid"].is_string() )             { r.error = Error::BadField; return r; }
		std::string pid = j["pid"].get< std::string >();
		if ( pid.empty() )                       { r.error = Error::BadField; return r; }

		if ( !j["sec"].is_string() )             { r.error = Error::BadField; return r; }
		std::string sec = j["sec"].get< std::string >();
		// sec is base64(32+ bytes). 32 bytes → 44 chars w/ padding, 43 w/o.
		// Accept either; just check decoded length is ≥ 32 bytes.
		{
			std::string secB64 = sec;
			// Strip any trailing '=' for tolerant decode of standard base64.
			while ( !secB64.empty() && secB64.back() == '=' ) secB64.pop_back();
			// Replace '+'→'-' and '/'→'_' so we can reuse url decoder, then
			// try; if that fails, also try original (allows pure base64url).
			std::string secUrl = secB64;
			for ( char& c : secUrl )
			{
				if ( c == '+' ) c = '-';
				else if ( c == '/' ) c = '_';
			}
			std::vector<uint8_t> secBytes;
			bool decOk = Base64UrlDecode( secUrl, secBytes );
			if ( !decOk || secBytes.size() < 32 )
			{
				r.error = Error::BadField;
				return r;
			}
		}

		if ( !j["rl"].is_string() )              { r.error = Error::BadField; return r; }
		std::string rl = j["rl"].get< std::string >();
		if ( rl != "M" && rl != "S" )            { r.error = Error::BadField; return r; }

		if ( !j["iat"].is_number_integer() )     { r.error = Error::BadField; return r; }
		int64_t iat = j["iat"].get< int64_t >();
		if ( iat < 0 )                           { r.error = Error::BadField; return r; }

		if ( !j["ttl"].is_number_integer() )     { r.error = Error::BadField; return r; }
		int ttl = j["ttl"].get< int >();
		if ( ttl < 0 )                           { r.error = Error::BadField; return r; }

		if ( ttl > 0 )
		{
			int64_t expiryMs = ( iat + int64_t( ttl ) ) * 1000LL;
			if ( expiryMs < NowUnixMs() )
			{
				r.error = Error::Expired;
				return r;
			}
		}

		r.decoded.version       = v;
		r.decoded.relayHost     = rly;
		r.decoded.relayPort     = uint16_t( p );
		r.decoded.userId        = uid;
		r.decoded.userAuthToken = tok;
		r.decoded.pairId        = pid;
		r.decoded.pairSecret    = sec;
		r.decoded.roleHint      = rl;
		r.decoded.issuedAtMs    = iat * 1000LL;
		r.decoded.ttlSeconds    = ttl;
		r.ok                    = true;
		return r;
	}
	catch ( const std::exception& )
	{
		r.error = Error::BadField;
		return r;
	}
}

std::string GenerateRandomSecret()
{
	uint8_t raw[32];
	NTSTATUS st = BCryptGenRandom(
		nullptr, raw, sizeof( raw ), BCRYPT_USE_SYSTEM_PREFERRED_RNG );
	if ( !BCRYPT_SUCCESS( st ) )
	{
		// Fallback на std::random_device — не crypto-grade, но не nullptr.
		std::random_device rd;
		for ( size_t i = 0; i < sizeof( raw ); ++i )
			raw[i] = static_cast< uint8_t >( rd() & 0xFFu );
	}
	std::vector< uint8_t > v( raw, raw + sizeof( raw ) );
	return Base64UrlEncode( v );
}

}  // namespace pair_code
