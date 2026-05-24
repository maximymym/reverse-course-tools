#pragma once
// =====================================================================
// AES-256-GCM encrypt/decrypt via BCrypt API (Windows).
// Vendored from deploy/stub/src/aes.h. Tag is appended after ciphertext
// (matches Go gcm.Seal layout).
// =====================================================================

#include <Windows.h>
#include <bcrypt.h>
#include <cstdint>
#include <cstring>
#include <vector>

#pragma comment(lib, "bcrypt.lib")

namespace aes {

// Decrypt AES-256-GCM. ciphertext layout = encData || 16-byte tag.
inline std::vector<uint8_t> DecryptGCM(const uint8_t* key, size_t keyLen,
                                        const uint8_t* iv, size_t ivLen,
                                        const uint8_t* ciphertext, size_t cipherLen) {
    std::vector<uint8_t> result;

    if (keyLen != 32 || ivLen != 12 || cipherLen <= 16)
        return result;

    size_t dataLen = cipherLen - 16;
    const uint8_t* tag = ciphertext + dataLen;

    BCRYPT_ALG_HANDLE hAlg = nullptr;
    BCRYPT_KEY_HANDLE hKey = nullptr;

    NTSTATUS st = BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_AES_ALGORITHM, nullptr, 0);
    if (!BCRYPT_SUCCESS(st)) return result;

    st = BCryptSetProperty(hAlg, BCRYPT_CHAINING_MODE,
                           (PUCHAR)BCRYPT_CHAIN_MODE_GCM,
                           sizeof(BCRYPT_CHAIN_MODE_GCM), 0);
    if (!BCRYPT_SUCCESS(st)) {
        BCryptCloseAlgorithmProvider(hAlg, 0);
        return result;
    }

    st = BCryptGenerateSymmetricKey(hAlg, &hKey, nullptr, 0,
                                     const_cast<PUCHAR>(key), static_cast<ULONG>(keyLen), 0);
    if (!BCRYPT_SUCCESS(st)) {
        BCryptCloseAlgorithmProvider(hAlg, 0);
        return result;
    }

    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO authInfo;
    BCRYPT_INIT_AUTH_MODE_INFO(authInfo);
    authInfo.pbNonce = const_cast<PUCHAR>(iv);
    authInfo.cbNonce = static_cast<ULONG>(ivLen);
    authInfo.pbTag = const_cast<PUCHAR>(tag);
    authInfo.cbTag = 16;

    ULONG cbResult = 0;
    result.resize(dataLen);

    st = BCryptDecrypt(hKey, const_cast<PUCHAR>(ciphertext), static_cast<ULONG>(dataLen),
                       &authInfo, nullptr, 0,
                       result.data(), static_cast<ULONG>(dataLen), &cbResult, 0);

    if (!BCRYPT_SUCCESS(st)) {
        result.clear();
    } else {
        result.resize(cbResult);
    }

    if (hKey) BCryptDestroyKey(hKey);
    if (hAlg) BCryptCloseAlgorithmProvider(hAlg, 0);

    return result;
}

inline bool GenerateRandomIV(uint8_t* iv, size_t len) {
    return BCRYPT_SUCCESS(BCryptGenRandom(nullptr, iv, static_cast<ULONG>(len),
                                           BCRYPT_USE_SYSTEM_PREFERRED_RNG));
}

// Encrypt AES-256-GCM. Result layout = ciphertext || 16-byte tag.
inline std::vector<uint8_t> EncryptGCM(const uint8_t* key, size_t keyLen,
                                        const uint8_t* iv, size_t ivLen,
                                        const uint8_t* plaintext, size_t plainLen) {
    std::vector<uint8_t> result;

    if (keyLen != 32 || ivLen != 12)
        return result;

    BCRYPT_ALG_HANDLE hAlg = nullptr;
    BCRYPT_KEY_HANDLE hKey = nullptr;

    NTSTATUS st = BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_AES_ALGORITHM, nullptr, 0);
    if (!BCRYPT_SUCCESS(st)) return result;

    st = BCryptSetProperty(hAlg, BCRYPT_CHAINING_MODE,
                           (PUCHAR)BCRYPT_CHAIN_MODE_GCM,
                           sizeof(BCRYPT_CHAIN_MODE_GCM), 0);
    if (!BCRYPT_SUCCESS(st)) {
        BCryptCloseAlgorithmProvider(hAlg, 0);
        return result;
    }

    st = BCryptGenerateSymmetricKey(hAlg, &hKey, nullptr, 0,
                                     const_cast<PUCHAR>(key), static_cast<ULONG>(keyLen), 0);
    if (!BCRYPT_SUCCESS(st)) {
        BCryptCloseAlgorithmProvider(hAlg, 0);
        return result;
    }

    uint8_t tag[16]{};

    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO authInfo;
    BCRYPT_INIT_AUTH_MODE_INFO(authInfo);
    authInfo.pbNonce = const_cast<PUCHAR>(iv);
    authInfo.cbNonce = static_cast<ULONG>(ivLen);
    authInfo.pbTag = tag;
    authInfo.cbTag = 16;

    ULONG cbResult = 0;
    // Plaintext can be empty (plainLen==0); pass nullptr buffers safely.
    result.resize(plainLen);

    st = BCryptEncrypt(hKey,
                       plainLen ? const_cast<PUCHAR>(plaintext) : nullptr,
                       static_cast<ULONG>(plainLen),
                       &authInfo, nullptr, 0,
                       plainLen ? result.data() : nullptr,
                       static_cast<ULONG>(plainLen), &cbResult, 0);

    if (!BCRYPT_SUCCESS(st)) {
        result.clear();
    } else {
        result.resize(cbResult);
        result.insert(result.end(), tag, tag + 16);
    }

    if (hKey) BCryptDestroyKey(hKey);
    if (hAlg) BCryptCloseAlgorithmProvider(hAlg, 0);

    return result;
}

inline std::vector<uint8_t> DecryptGCMHex(const char* keyHex, const char* ivHex,
                                            const uint8_t* ciphertext, size_t cipherLen) {
    auto hexByte = [](char hi, char lo) -> uint8_t {
        auto nib = [](char c) -> uint8_t {
            if (c >= '0' && c <= '9') return static_cast<uint8_t>(c - '0');
            if (c >= 'a' && c <= 'f') return static_cast<uint8_t>(c - 'a' + 10);
            if (c >= 'A' && c <= 'F') return static_cast<uint8_t>(c - 'A' + 10);
            return 0;
        };
        return static_cast<uint8_t>((nib(hi) << 4) | nib(lo));
    };

    if (strlen(keyHex) != 64 || strlen(ivHex) != 24)
        return {};

    uint8_t key[32], iv[12];
    for (int i = 0; i < 32; i++) key[i] = hexByte(keyHex[i*2], keyHex[i*2+1]);
    for (int i = 0; i < 12; i++) iv[i] = hexByte(ivHex[i*2], ivHex[i*2+1]);

    auto result = DecryptGCM(key, 32, iv, 12, ciphertext, cipherLen);

    SecureZeroMemory(key, sizeof(key));
    SecureZeroMemory(iv, sizeof(iv));

    return result;
}

} // namespace aes
