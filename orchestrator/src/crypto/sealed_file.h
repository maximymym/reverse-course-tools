#pragma once
// =====================================================================
// SealedFile: per-machine AES-256-GCM file envelope for local persistence
// (license.dat, accounts.json). Key = HKDF_SHA256(hwid_hash, salt, info).
//
// On-disk layout:
//   [0..4)    magic = "DFRM"
//   [4..16)   IV    (12 bytes, random per write)
//   [16..32)  tag   (16 bytes, GCM authentication tag)
//   [32..)    ciphertext
//
// Reads validate magic + decrypt + auth tag. Mismatched HWID -> auth fails
// and UnsealFile returns success=false with empty plaintext (fail loud).
// Callers handle legacy plain-text migration externally (no magic header).
// =====================================================================

#include <Windows.h>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

#include "crypto.h"
#include "aes.h"
#include "../hwid.h"

namespace sealed_file {

inline constexpr char kMagic[4] = { 'D', 'F', 'R', 'M' };
inline constexpr size_t kMagicLen = 4;
inline constexpr size_t kIvLen    = 12;
inline constexpr size_t kTagLen   = 16;
inline constexpr size_t kHeaderLen = kMagicLen + kIvLen + kTagLen; // 32

// HKDF parameters shared by every sealed file in this orchestrator.
inline constexpr char kSalt[] = "dotafarm-local-v1";
inline constexpr char kInfo[] = "local-store";

struct UnsealResult {
    bool                 ok;        // true if header valid AND tag verified
    bool                 hasMagic;  // true if magic header was present (vs legacy plain)
    std::vector<uint8_t> plaintext; // empty on failure
};

// Derive 32-byte file key from current HWID via HKDF-SHA256.
// Returns false only on BCrypt failure (very rare).
inline bool DeriveKey(uint8_t out[32]) {
    std::string hwidHex = hwid::Generate();
    if (hwidHex.empty()) return false;
    return crypto::HKDF_SHA256(
        reinterpret_cast<const uint8_t*>(hwidHex.data()), hwidHex.size(),
        reinterpret_cast<const uint8_t*>(kSalt), sizeof(kSalt) - 1,
        kInfo,
        out, 32);
}

inline bool HasMagic(const std::vector<uint8_t>& raw) {
    return raw.size() >= kMagicLen && memcmp(raw.data(), kMagic, kMagicLen) == 0;
}

// Read entire file into byte vector (raw — caller decides if sealed or legacy).
inline bool ReadFileBytes(const std::string& path, std::vector<uint8_t>& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) return false;
    f.seekg(0, std::ios::end);
    std::streamoff sz = f.tellg();
    if (sz < 0) return false;
    f.seekg(0, std::ios::beg);
    out.resize(static_cast<size_t>(sz));
    if (sz > 0) f.read(reinterpret_cast<char*>(out.data()), sz);
    return f.good() || f.eof();
}

inline bool WriteFileBytes(const std::string& path, const uint8_t* data, size_t len) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f.is_open()) return false;
    if (len > 0) f.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(len));
    return f.good();
}

// Encrypt + write sealed envelope.
inline bool SealFile(const std::string& path, const uint8_t* plaintext, size_t plainLen) {
    uint8_t key[32]{};
    if (!DeriveKey(key)) return false;

    uint8_t iv[kIvLen]{};
    if (!aes::GenerateRandomIV(iv, kIvLen)) {
        SecureZeroMemory(key, sizeof(key));
        return false;
    }

    auto ct = aes::EncryptGCM(key, sizeof(key), iv, kIvLen, plaintext, plainLen);
    SecureZeroMemory(key, sizeof(key));
    if (ct.size() < kTagLen) return false; // EncryptGCM failed

    // ct layout from aes::EncryptGCM = encData || 16-byte tag
    size_t dataLen = ct.size() - kTagLen;
    const uint8_t* encData = ct.data();
    const uint8_t* tag     = ct.data() + dataLen;

    std::vector<uint8_t> out;
    out.reserve(kHeaderLen + dataLen);
    out.insert(out.end(), kMagic, kMagic + kMagicLen);
    out.insert(out.end(), iv, iv + kIvLen);
    out.insert(out.end(), tag, tag + kTagLen);
    out.insert(out.end(), encData, encData + dataLen);

    return WriteFileBytes(path, out.data(), out.size());
}

inline bool SealFile(const std::string& path, const std::string& plaintext) {
    return SealFile(path,
                    reinterpret_cast<const uint8_t*>(plaintext.data()),
                    plaintext.size());
}

inline bool SealFile(const std::string& path, const std::vector<uint8_t>& plaintext) {
    return SealFile(path, plaintext.data(), plaintext.size());
}

// Read + decrypt sealed envelope. If file is legacy (no magic), returned
// result has hasMagic=false, ok=false, plaintext=raw file bytes — the caller
// should treat that as a migration hint.
inline UnsealResult UnsealFile(const std::string& path) {
    UnsealResult r{};
    std::vector<uint8_t> raw;
    if (!ReadFileBytes(path, raw)) return r;

    if (!HasMagic(raw)) {
        // Legacy plain-text path — hand bytes back unchanged for migration.
        r.hasMagic = false;
        r.ok       = false;
        r.plaintext = std::move(raw);
        return r;
    }
    r.hasMagic = true;

    if (raw.size() < kHeaderLen) return r; // truncated

    const uint8_t* iv  = raw.data() + kMagicLen;
    const uint8_t* tag = raw.data() + kMagicLen + kIvLen;
    const uint8_t* enc = raw.data() + kHeaderLen;
    size_t encLen      = raw.size() - kHeaderLen;

    // aes::DecryptGCM expects ciphertext || tag (Go gcm.Seal layout).
    std::vector<uint8_t> ctWithTag;
    ctWithTag.reserve(encLen + kTagLen);
    ctWithTag.insert(ctWithTag.end(), enc, enc + encLen);
    ctWithTag.insert(ctWithTag.end(), tag, tag + kTagLen);

    uint8_t key[32]{};
    if (!DeriveKey(key)) return r;

    auto pt = aes::DecryptGCM(key, sizeof(key), iv, kIvLen,
                              ctWithTag.data(), ctWithTag.size());
    SecureZeroMemory(key, sizeof(key));

    // DecryptGCM returns empty vector on tag mismatch / decrypt failure.
    // Empty plaintext is also a valid encrypted empty payload, but our
    // callers never seal empty strings, so we treat empty as failure.
    if (pt.empty() && encLen > 0) return r;

    r.ok        = true;
    r.plaintext = std::move(pt);
    return r;
}

// Convenience: read sealed file as std::string (for textual payloads like JSON / license key).
// success: tag verified, file had magic header, plaintext valid utf-8 bytes.
inline bool UnsealFileToString(const std::string& path, std::string& out, bool& hadMagic) {
    auto r = UnsealFile(path);
    hadMagic = r.hasMagic;
    if (!r.ok) {
        // For legacy: hand caller the plain bytes so they can migrate.
        if (!r.hasMagic && !r.plaintext.empty()) {
            out.assign(reinterpret_cast<const char*>(r.plaintext.data()),
                       r.plaintext.size());
        } else {
            out.clear();
        }
        return false;
    }
    out.assign(reinterpret_cast<const char*>(r.plaintext.data()),
               r.plaintext.size());
    return true;
}

} // namespace sealed_file
