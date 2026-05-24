#pragma once
// =====================================================================
// Crypto utilities: SHA256, HMAC-SHA256, HKDF-SHA256, ECDSA P-256 verify
// Vendored from deploy/stub/src/crypto.h — standalone, BCrypt-based.
// =====================================================================

#include <Windows.h>
#include <bcrypt.h>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <string>
#include <vector>

#pragma comment(lib, "bcrypt.lib")

namespace crypto {

// === Hex string to bytes ===
inline bool HexToBytes(const char* hex, uint8_t* out, size_t outLen) {
    size_t hexLen = strlen(hex);
    if (hexLen != outLen * 2) return false;
    for (size_t i = 0; i < outLen; i++) {
        char hi = hex[i * 2], lo = hex[i * 2 + 1];
        auto nibble = [](char c) -> uint8_t {
            if (c >= '0' && c <= '9') return static_cast<uint8_t>(c - '0');
            if (c >= 'a' && c <= 'f') return static_cast<uint8_t>(c - 'a' + 10);
            if (c >= 'A' && c <= 'F') return static_cast<uint8_t>(c - 'A' + 10);
            return 0;
        };
        out[i] = static_cast<uint8_t>((nibble(hi) << 4) | nibble(lo));
    }
    return true;
}

// === SHA256 of raw data (BCrypt) ===
inline bool SHA256(const void* data, size_t len, uint8_t out[32]) {
    BCRYPT_ALG_HANDLE hAlg = nullptr;
    BCRYPT_HASH_HANDLE hHash = nullptr;
    bool ok = false;

    if (BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM, nullptr, 0)) &&
        BCRYPT_SUCCESS(BCryptCreateHash(hAlg, &hHash, nullptr, 0, nullptr, 0, 0))) {
        BCryptHashData(hHash, static_cast<PUCHAR>(const_cast<void*>(data)),
                       static_cast<ULONG>(len), 0);
        ok = BCRYPT_SUCCESS(BCryptFinishHash(hHash, out, 32, 0));
    }

    if (hHash) BCryptDestroyHash(hHash);
    if (hAlg) BCryptCloseAlgorithmProvider(hAlg, 0);
    return ok;
}

// === SHA256 -> hex string ===
inline std::string SHA256Hex(const void* data, size_t len) {
    uint8_t hash[32]{};
    if (!SHA256(data, len, hash)) return "";
    char hex[65]{};
    for (int i = 0; i < 32; i++) snprintf(hex + i * 2, 3, "%02x", hash[i]);
    return hex;
}

// === ECDSA P-256 signature verification (BCrypt) ===
inline bool VerifyECDSA(const uint8_t* pubkey64, const uint8_t* hash32, const uint8_t* sig64) {
    BCRYPT_ALG_HANDLE hAlg = nullptr;
    BCRYPT_KEY_HANDLE hKey = nullptr;
    bool ok = false;

    struct {
        BCRYPT_ECCKEY_BLOB header;
        uint8_t xy[64];
    } blob{};
    blob.header.dwMagic = BCRYPT_ECDSA_PUBLIC_P256_MAGIC;
    blob.header.cbKey = 32;
    memcpy(blob.xy, pubkey64, 64);

    if (BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_ECDSA_P256_ALGORITHM, nullptr, 0)) &&
        BCRYPT_SUCCESS(BCryptImportKeyPair(hAlg, nullptr, BCRYPT_ECCPUBLIC_BLOB,
                                            &hKey, reinterpret_cast<PUCHAR>(&blob),
                                            sizeof(blob), 0))) {
        NTSTATUS st = BCryptVerifySignature(hKey, nullptr,
                                             const_cast<PUCHAR>(hash32), 32,
                                             const_cast<PUCHAR>(sig64), 64, 0);
        ok = BCRYPT_SUCCESS(st);
    }

    if (hKey) BCryptDestroyKey(hKey);
    if (hAlg) BCryptCloseAlgorithmProvider(hAlg, 0);

    return ok;
}

// === HMAC-SHA256 (BCrypt) ===
inline bool HMAC_SHA256(const uint8_t* key, size_t keyLen,
                        const uint8_t* data, size_t dataLen,
                        uint8_t out[32]) {
    BCRYPT_ALG_HANDLE hAlg = nullptr;
    BCRYPT_HASH_HANDLE hHash = nullptr;
    bool ok = false;

    if (BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM,
                                                    nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG)) &&
        BCRYPT_SUCCESS(BCryptCreateHash(hAlg, &hHash, nullptr, 0,
                                         const_cast<PUCHAR>(key), static_cast<ULONG>(keyLen), 0))) {
        BCryptHashData(hHash, const_cast<PUCHAR>(data), static_cast<ULONG>(dataLen), 0);
        ok = BCRYPT_SUCCESS(BCryptFinishHash(hHash, out, 32, 0));
    }

    if (hHash) BCryptDestroyHash(hHash);
    if (hAlg) BCryptCloseAlgorithmProvider(hAlg, 0);
    return ok;
}

// === HKDF-SHA256 (Extract + Expand, RFC 5869) ===
// outLen must be <= 32 (single expand block).
inline bool HKDF_SHA256(const uint8_t* ikm, size_t ikmLen,
                         const uint8_t* salt, size_t saltLen,
                         const char* info,
                         uint8_t* out, size_t outLen) {
    if (outLen > 32) return false;

    uint8_t prk[32]{};
    if (!HMAC_SHA256(salt, saltLen, ikm, ikmLen, prk))
        return false;

    size_t infoLen = info ? strlen(info) : 0;
    std::vector<uint8_t> expandData(infoLen + 1);
    if (infoLen > 0) memcpy(expandData.data(), info, infoLen);
    expandData[infoLen] = 0x01;

    uint8_t t1[32]{};
    if (!HMAC_SHA256(prk, 32, expandData.data(), expandData.size(), t1)) {
        SecureZeroMemory(prk, 32);
        return false;
    }

    memcpy(out, t1, outLen);
    SecureZeroMemory(prk, 32);
    SecureZeroMemory(t1, 32);
    return true;
}

} // namespace crypto
