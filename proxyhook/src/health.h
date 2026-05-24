#pragma once

#include <atomic>
#include <cstdint>
#include <string>

namespace proxyhook
{

// Счётчики SOCKS5 handshake'ов — инкрементируются из hooks_tcp.
// Читаются из health thread, который пишет их в JSON.
struct HealthCounters
{
    std::atomic<uint64_t> socks5Ok   { 0 };
    std::atomic<uint64_t> socks5Fail { 0 };
    // UDP тоже, если включён
    std::atomic<uint64_t> udpOk      { 0 };
    std::atomic<uint64_t> udpFail    { 0 };
};

// Получить глобальный счётчик (singleton).
HealthCounters& GetHealthCounters();

// Запустить health-thread. Пишет `C:\temp\andromeda\proxyhook_health_<PID>.json`
// каждые N секунд. Выполняет:
//   - SOCKS5 HTTP probe к api.ipify.org → exit IP
//   - Self-check hooked APIs (GetVolumeInformationW, RegQueryValueExW для MachineGuid,
//     GetAdaptersInfo, GetSystemFirmwareTable) → сравнение с expected fake values
// Thread detach'ится; живёт до DLL_PROCESS_DETACH.
void StartHealthThread();

} // namespace proxyhook
