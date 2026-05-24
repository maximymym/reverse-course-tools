#pragma once

// MemReclaim — system-wide memory reclamation API (memreduct backend port).
//
// Заменяет старый per-process EmptyWorkingSet trim (TrimWorkingSetTree из
// DotaLauncher), который вызывал визуальный stutter в Dota'е из-за принудительной
// выгрузки активных текстур/шейдеров. Здесь используются исключительно
// system-wide вызовы NtSetSystemInformation(SystemMemoryListInformation, ...) —
// они чистят standby/modified/free PFN lists и комбинируют идентичные страницы
// БЕЗ touch'а активных working sets процессов.
//
// Все syscall'ы резолвятся runtime через GetProcAddress(ntdll) — нет
// dependency на ntdll.lib и явный fallback на старых Windows.
//
// License note: реимплементация с нуля по публичным winternl/ntddk константам
// (Geoff Chappell + MSDN). memreduct (GPL-3.0) НЕ используется как код.

#include <Windows.h>
#include <cstdint>
#include <string>
#include <vector>

namespace MemReclaim
{
	// ── UI threshold constants (single source of truth для GUI цветов) ──
	// Memory load %% при котором bar меняет цвет:
	//   < kPressureWarn  → green  (всё ок)
	//   ≥ kPressureWarn  → amber  (заметная нагрузка)
	//   ≥ kPressureCrit  → red    (вот-вот OOM)
	constexpr unsigned kPressureWarn = 60;
	constexpr unsigned kPressureCrit = 85;

	// ── Privilege bootstrap ──
	// Включает SeProfileSingleProcessPrivilege (id=13) + SeIncreaseQuotaPrivilege
	// (id=5) через RtlAdjustPrivilege. Обязательно для PurgeStandby/Combine
	// (SystemFileCacheInformationEx и SystemCombinePhysicalMemoryInformation
	// тоже требуют эти привилегии).
	// Зовётся однократно при Orchestrator::Init() после load конфига.
	// Возврат: true если ОБЕ привилегии успешно включены. false → system-wide
	// reclaim работать не будет (но per-process EmptyWorkingSet всё ещё ок).
	bool EnablePrivileges();

	// ── Reclamation primitives (mapping на memreduct GUI checkbox'ы) ──
	// Каждый возвращает true если NtSetSystemInformation вернул STATUS_SUCCESS.
	// Failure не throw — просто false, caller'ы могут залогать.

	// "Working set" checkbox в memreduct: MemoryEmptyWorkingSets (2).
	// ПРЕДУПРЕЖДЕНИЕ: эквивалент per-process EmptyWorkingSet на ВСЕ процессы
	// системы. Вызывает тот же stutter в Dota'е. По умолчанию OFF в нашем GUI.
	bool EmptyAllWorkingSets();

	// "Modified page list" checkbox: MemoryFlushModifiedList (3).
	// Сбрасывает dirty PFN на диск (готовит их к standby). Создаёт disk pressure
	// на 50-500ms — может ощущаться как mini-freeze. По умолчанию OFF.
	bool FlushModified();

	// "Standby list" checkbox: MemoryPurgeStandbyList (4).
	// Главный workhorse — отбирает все standby pages (file cache которым никто
	// прямо сейчас не пользуется) обратно в free pool. ПОВТОРНЫЕ чтения с диска
	// будут медленнее, но live working sets не страдают.
	bool PurgeStandby();

	// "Priority 0 standby" checkbox: MemoryPurgeLowPriorityStandbyList (5).
	// Безопаснее — только cache с priority 0 (наименее ценный). По умолчанию ON.
	bool PurgeLowPriorityStandby();

	// "Combine memory lists" checkbox: SystemCombinePhysicalMemoryInformation
	// (Win10+). Дедупликация идентичных PFN — даёт reuse между N инстансами
	// dota2.exe (одинаковый client.dll/engine2.dll/textures). По умолчанию ON.
	// На Win7/8 возвращает false (функция отсутствует).
	bool CombineMemoryLists();

	// ── System memory snapshot (для GUI panel + log дельт) ──
	struct MemoryStats
	{
		uint64_t totalPhysMB    = 0;
		uint64_t availPhysMB    = 0;
		uint64_t commitUsedMB   = 0;   // current commit charge (system)
		uint64_t commitLimitMB  = 0;   // total commit limit (RAM + pagefile)
		uint64_t standbyMB      = 0;   // SystemMemoryListInformation.StandbyPageCount × pageSize
		uint64_t modifiedMB     = 0;   // ModifiedPageCount
		uint64_t freeMB         = 0;   // FreePageCount
		unsigned memoryLoadPct  = 0;   // GlobalMemoryStatusEx.dwMemoryLoad (0-100)
		bool     valid          = false;
	};
	MemoryStats QuerySystemMemory();

	// ── Per-process snapshot (для GUI "BOTS X.X GB" + точечный TRIM BOTS) ──
	struct ProcInfo
	{
		DWORD       pid     = 0;
		std::string name;        // image name (без пути)
		uint64_t    wsBytes  = 0;
		uint64_t    pmBytes  = 0; // private bytes
		uint64_t    handles  = 0;
		uint64_t    threads  = 0;
	};

	// Снимок ВСЕХ процессов system-wide (top-N не делаем — caller фильтрует
	// по pid/name). Может занимать 5-15ms на машине с 200+ процессами,
	// поэтому caller'ы (GUI) кешируют результат на 2 секунды.
	std::vector<ProcInfo> EnumProcessesRam();

	// Удобный фильтр: суммировать WS только для указанных PID'ов.
	// Используется в GUI чтобы показать "BOTS X.X GB" = sum(WS) по dota+steam
	// PID'ам активных ботов. Сам делает EnumProcessesRam() — caller'у не нужно
	// (но если у него уже есть свежий snapshot — можно скормить через
	// SumWsBytesIn(snapshot, pids) overload).
	uint64_t SumWsBytesFor( const std::vector<DWORD>& pids );
	uint64_t SumWsBytesIn( const std::vector<ProcInfo>& snapshot,
		const std::vector<DWORD>& pids );
}
