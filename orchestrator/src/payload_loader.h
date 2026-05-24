#pragma once
// =====================================================================
// payload_loader: embedded RT_RCDATA -> AES-256-GCM decrypt -> bytes.
//
// On-disk (== embedded) layout, identical for every artifact:
//   [0..4)    magic (4 ASCII bytes — distinguishes artifact kind)
//   [4..16)   IV (12 bytes — unique per encrypted resource)
//   [16..)    ciphertext || 16-byte GCM tag  (Go gcm.Seal layout — matches
//             crypto/aes.h::DecryptGCM directly)
//
// Magic vocabulary (must stay in sync with scripts/encrypt_resource.py):
//   "DFDL"  — DotaFarm DLL          (Andromeda-Dota2-Base.dll, ProxyHook.dll)
//   "DFSP"  — DotaFarm Spoofer      (spoofer.sys, kdu.exe)
//
// Key derivation per artifact (different (salt, info) pair per kind so a
// leak of one HKDF chain doesn't trivially expose the others):
//   key = HKDF_SHA256(IKM = kEmbedSecret (32B baked at build time),
//                     salt, info, len=32)
//
// The same kEmbedSecret is read by scripts/encrypt_resource.py at build
// time. Secret lives in crypto/embed_secret.h.
// =====================================================================

#include <Windows.h>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "crypto/crypto.h"
#include "crypto/aes.h"
#include "crypto/embed_secret.h"

namespace payload_loader {

inline constexpr size_t kMagicLen = 4;
inline constexpr size_t kIvLen    = 12;
inline constexpr size_t kHeaderLen = kMagicLen + kIvLen; // 16

// Artifact-kind magics.
inline constexpr char kMagicDll     [4] = { 'D', 'F', 'D', 'L' };
inline constexpr char kMagicSpoofer [4] = { 'D', 'F', 'S', 'P' };

// HKDF parameter table — index matches kMagic* above. Salt/info must be
// kept verbatim in sync with encrypt_resource.py invocations from
// CMakeLists.txt.
struct HkdfParams {
	const char* salt;
	const char* info;
};

// Map RESOURCE NAME -> HKDF params. We keep this here (not the magic) so
// callers can ask for an artifact without knowing whether it's a DLL or a
// driver. Adding a new resource = add one row here + one .rc entry + one
// CMake encrypt call with matching --magic/--salt/--info.
inline HkdfParams ParamsForResource( const char* resName )
{
	// DLL group (DFDL)
	if ( strcmp( resName, "ANDROMEDA_DLL" ) == 0 || strcmp( resName, "PROXYHOOK_DLL" ) == 0 )
		return { "dotafarm-dll-v1", "dll-embed" };
	// Spoofer group (DFSP) — kernel driver
	if ( strcmp( resName, "SPOOFER_SYS" ) == 0 )
		return { "dotafarm-sys-v1", "sys-embed" };
	// Spoofer group (DFSP) — usermode exploit binary
	if ( strcmp( resName, "KDU_EXE" ) == 0 )
		return { "dotafarm-exe-v1", "exe-embed" };
	// Unknown -> degrade to DLL params (safer than blank, still HKDF-isolated
	// from the named groups).
	return { "dotafarm-dll-v1", "dll-embed" };
}

// Magic expected for a given resource name. Used to validate header bytes
// before attempting decrypt (catches resource/HKDF mismatches loudly).
inline const char* ExpectedMagicForResource( const char* resName )
{
	if ( strcmp( resName, "SPOOFER_SYS" ) == 0 ) return kMagicSpoofer;
	if ( strcmp( resName, "KDU_EXE"     ) == 0 ) return kMagicSpoofer;
	return kMagicDll; // ANDROMEDA_DLL, PROXYHOOK_DLL, default
}

// Locate an RT_RCDATA blob by name. Returns a pointer + size, no copy.
inline bool FetchResource( const char* resName, const uint8_t*& outData, size_t& outSize )
{
	HMODULE hSelf = GetModuleHandleA( nullptr );
	HRSRC hRes = FindResourceA( hSelf, resName, RT_RCDATA );
	if ( !hRes ) return false;
	HGLOBAL hMem = LoadResource( hSelf, hRes );
	if ( !hMem ) return false;
	DWORD sz = SizeofResource( hSelf, hRes );
	if ( sz < kHeaderLen + 16 ) return false; // need magic+iv+tag at minimum
	void* p = LockResource( hMem );
	if ( !p ) return false;
	outData = static_cast<const uint8_t*>( p );
	outSize = sz;
	return true;
}

// Core universal helper. Fetches the RT_RCDATA blob, validates header
// magic, derives the HKDF key from (resource-specific) salt/info, decrypts
// AES-256-GCM, optionally validates plaintext begins with "MZ" (PE).
// Returns false (and out.clear()) on any failure.
inline bool LoadEmbeddedResource( const char* resName,
                                  bool validatePE,
                                  std::vector<uint8_t>& out )
{
	out.clear();

	const uint8_t* blob = nullptr;
	size_t blobSize = 0;
	if ( !FetchResource( resName, blob, blobSize ) )
		return false;

	const char* expectedMagic = ExpectedMagicForResource( resName );
	if ( memcmp( blob, expectedMagic, kMagicLen ) != 0 )
		return false;

	const uint8_t* iv = blob + kMagicLen;
	const uint8_t* ct = blob + kHeaderLen;
	size_t ctLen = blobSize - kHeaderLen;

	HkdfParams hp = ParamsForResource( resName );
	uint8_t key[32]{};
	bool kok = crypto::HKDF_SHA256(
		embed_secret::kEmbedSecret, sizeof( embed_secret::kEmbedSecret ),
		reinterpret_cast<const uint8_t*>( hp.salt ), strlen( hp.salt ),
		hp.info,
		key, 32 );
	if ( !kok ) return false;

	auto plain = aes::DecryptGCM( key, 32, iv, kIvLen, ct, ctLen );
	SecureZeroMemory( key, sizeof( key ) );

	if ( plain.empty() ) return false;

	if ( validatePE )
	{
		if ( plain.size() < 2 || plain[0] != 'M' || plain[1] != 'Z' )
		{
			SecureZeroMemory( plain.data(), plain.size() );
			return false;
		}
	}

	out = std::move( plain );
	return true;
}

// Memory-only fetch for DLLs that will be manually-mapped without ever
// touching disk. PE-validated.
inline bool LoadEmbeddedDll( const char* resName, std::vector<uint8_t>& out )
{
	return LoadEmbeddedResource( resName, /*validatePE=*/true, out );
}

// Disk-write variant — decrypt an embedded resource and write the plaintext
// to outPath. Used by hwid_spoof for binaries that have to live on disk
// (spoofer.sys / kdu.exe — kernel-driver load takes a file path, not memory;
// kdu.exe is a separate process exec). Validates PE header because both
// artifact families we currently embed are PE (DLL / SYS / EXE).
inline bool ExtractEmbeddedPayload( const char* resourceName, const std::string& outPath )
{
	std::vector<uint8_t> buf;
	if ( !LoadEmbeddedResource( resourceName, /*validatePE=*/true, buf ) ) return false;
	if ( buf.empty() ) return false;

	FILE* f = fopen( outPath.c_str(), "wb" );
	if ( !f )
	{
		SecureZeroMemory( buf.data(), buf.size() );
		return false;
	}
	size_t w = fwrite( buf.data(), 1, buf.size(), f );
	fclose( f );
	SecureZeroMemory( buf.data(), buf.size() );
	return w == buf.size();
}

} // namespace payload_loader
