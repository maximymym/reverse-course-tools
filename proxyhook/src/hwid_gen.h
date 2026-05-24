#pragma once

#include <cstdint>
#include <string>
#include <array>

namespace proxyhook
{

// HwidFakeValues — полный набор идентификаторов, выведенных из seed.
// Детерминирован: same seed → same values.
struct HwidFakeValues
{
    // MAC address (6 bytes). bit 0x02 первого байта = locally-administered.
    std::array<uint8_t, 6>  mac{};
    // Formatted "XX-XX-XX-XX-XX-XX" uppercase hex.
    std::string             macStr;

    // Volume serial — DWORD. Typically shown as "XXXX-XXXX" в vol command.
    uint32_t                volumeSerial = 0;

    // Disk serial (ATA IDENTIFY). 20 ASCII hex chars, uppercase.
    std::string             diskSerial;

    // MachineGuid — UUID "{xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx}" (lowercase, with braces).
    // Так выдаёт HKLM\SOFTWARE\Microsoft\Cryptography\MachineGuid (без фигурных скобок),
    // а HwProfileGuid — С фигурными скобками. Храним БЕЗ braces, добавляем при необходимости.
    std::string             machineGuid;       // "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx"
    std::string             hwProfileGuid;     // "{xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx}"

    // SMBIOS System UUID (16 bytes raw + formatted without braces).
    std::array<uint8_t, 16> smbiosUuid{};
    std::string             smbiosUuidStr;     // "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx"

    // System Serial (SMBIOS Type 1 / BIOS string). 10 chars uppercase alphanumeric.
    std::string             systemSerial;

    // BIOS vendor/version — fake but plausible.
    std::string             biosVendor;        // "American Megatrends Inc." (fixed)
    std::string             biosVersion;       // "F.<seed-derived>" 8 chars

    // ProductId (Windows) — formatted "XXXXX-XXX-XXXXXXX-XXXXX".
    std::string             productId;
};

// Derive fake values from seed string. Pure function, no side effects.
// Returns true on success. False only if BCrypt unavailable (very rare).
bool DeriveHwid( const std::string& seed, HwidFakeValues& out );

// Компактная утилита для тестов/диагностики — SHA-256(seed) → 32 bytes.
bool Sha256( const void* data, size_t size, uint8_t out[32] );

} // namespace proxyhook
