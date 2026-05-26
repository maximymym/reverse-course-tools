#pragma once

#include <cstdint>
#include <string>

namespace pair_code
{

enum class Error
{
	BadPrefix,
	BadFormat,
	BadVersion,
	CrcFail,
	BadBase64,
	BadJson,
	MissingField,
	BadField,
	Expired,
	SameMachine
};

const char* Describe( Error e );

struct Decoded
{
	int         version       = 1;
	std::string relayHost;
	uint16_t    relayPort     = 0;
	std::string userId;
	std::string userAuthToken;
	std::string pairId;
	std::string pairSecret;
	std::string roleHint;       // "M" or "S"
	int64_t     issuedAtMs     = 0;
	int         ttlSeconds     = 0;
};

struct DecodeResult
{
	bool    ok      = false;
	Decoded decoded;
	Error   error   = Error::BadFormat;
};

std::string Encode(
	const std::string& relayHost,
	uint16_t           relayPort,
	const std::string& userId,
	const std::string& userAuthToken,
	const std::string& pairId,
	const std::string& pairSecret,
	const std::string& roleHint = "S",
	int                ttlSeconds = 0
);

DecodeResult Decode( const std::string& code );

// Generate base64url-encoded 32 random bytes — ready-to-use pair_secret для
// hello-handshake. Cryptographic randomness через BCryptGenRandom, fallback на
// std::random_device при сбое CNG.
std::string GenerateRandomSecret();

}  // namespace pair_code
