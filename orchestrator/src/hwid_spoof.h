#pragma once

#include <string>
#include <cstdint>

// HWID spoofer wrapper — вызывает dist/HwidSpoofer.exe перед запуском Steam.
//
// Seed format: "<steamId>_<YYYYMM>". Monthly rotation — один и тот же
// аккаунт в разных месяцах получает разный HWID, но в рамках месяца стабилен
// (иначе Steam Guard триггерится на каждом запуске).
//
// Orchestrator запущен с requireAdministrator, поэтому прямой CreateProcess
// хватает — UAC prompt не нужен.

namespace hwid_spoof
{

// Собрать seed для аккаунта: "<steamId>_<YYYYMM>"
std::string MakeSeed( uint64_t steamId );

// Собрать machine-wide seed для FullPC спуфа: "machine_<MACHINENAME>_<YYYYMM>".
// Засоленность через ComputerName гарантирует что у разных машин одного владельца
// (например на работе и дома) HWID будет разный — иначе оба ПК схлопнутся в один
// fingerprint когда юзер сам себя залогинит на обоих.
std::string MakeMachineSeed();

// Запустить HwidSpoofer.exe spoof --seed <seed> --yes (non-interactive).
// timeoutMs — сколько ждём процесс (default 45s, SMBIOS driver + MAC возни).
// Возвращает true если exit code 0.
bool RunSpoof( const std::string& spooferExe, const std::string& seed,
              int timeoutMs = 45000 );

// Verify текущий HWID против seed. Возвращает true если HWID совпадает.
bool VerifySpoof( const std::string& spooferExe, const std::string& seed,
                 int timeoutMs = 15000 );

} // namespace hwid_spoof
