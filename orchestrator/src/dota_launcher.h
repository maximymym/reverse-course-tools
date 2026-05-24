#pragma once

#include "config.h"
#include <Windows.h>
#include <TlHelp32.h>
#include <vector>
#include <string>

class DotaLauncher
{
public:
	// Kill dota_singleton_mutex — tries NtQuerySystemInformation first, handle64.exe as fallback
	bool KillDotaMutex( DWORD dotaPid, const std::string& handleExe );

	// Kill mutex via handle64.exe (Sysinternals)
	bool KillMutexViaHandle64( DWORD dotaPid, const std::string& handleExe );

	// Kill mutex via NtQuerySystemInformation + DuplicateHandle (no external tools)
	bool KillMutexNative( DWORD dotaPid );

	// Wait for dota2.exe to be fully ready (client.dll loaded)
	bool WaitForDotaReady( DWORD pid, int timeoutMs = 60000 );

	static std::vector<DWORD> FindDotaPids();
	static std::vector<DWORD> FindSteamPids();
	static std::vector<DWORD> FindProcessByName( const char* name );
	static bool IsProcessAlive( DWORD pid );
	static bool IsModuleLoaded( DWORD pid, const char* moduleName );

	// Force OS to evict the target process' resident pages (working set → 0).
	// Pages remain in commit/pagefile; they get faulted back in on touch.
	// На dota2.exe эффективно: 20-30k handles тянут много pages которые ни
	// рендер ни AI бот фактически не трогают между MonitorTick'ами. Возврат
	// reclaim'ом через minor page fault — стоит дёшево относительно общего
	// тика бота (10-20ms на инстанс при 4GB WS).
	static bool TrimWorkingSet( DWORD pid );

	// Найти все дочерние PID (recursive) для parentPid. Для bot.steamPid это
	// 5-7 steamwebhelper.exe + gameoverlayui.exe — главный источник
	// RAM-инфляции (~600 MB WS на webhelper, 7×5 = 4 GB всех инстансов).
	// Возвращает PIDs ВСЕХ потомков, не включая сам parentPid.
	// Реализован через BFS по Process32First/Next snapshot (один snapshot
	// на запрос, не повторяем для каждой глубины — дёшево).
	static std::vector<DWORD> FindChildPidsRecursive( DWORD parentPid );

	// Trim parent + всё его дерево потомков. Возвращает количество успешно
	// затримленных процессов (включая parent). 0 = не нашли или access denied.
	static int TrimWorkingSetTree( DWORD parentPid );

	// Запустить ТОЛЬКО dota2.exe для уже работающего Steam (recovery path).
	// Steam НЕ перезапускается (auth_pending risk при new login session).
	// Перед запуском убивает stale dota_singleton_mutex на ВСЕХ живых dota PID
	// (если в процессе recovery какие-то остались зомби).
	// excludeExistingPids: список уже известных Dota PID до launch — функция
	// вернёт первый НОВЫЙ PID появившийся в течение timeoutMs.
	// 0 = не появилась.
	DWORD LaunchDotaOnly( int instanceIdx, const FarmConfig& cfg, DWORD steamPid,
		const std::vector<DWORD>& excludeExistingPids, int timeoutMs = 120000 );
};

// Применить CPU affinity к pid (dota2.exe) согласно глобальной настройке
// FarmConfig.coresPerInstance. instanceIdx используется для шифта mask'и,
// чтобы 5 ботов не дрались за одни и те же ядра. coresPerInstance == 0 —
// no-op (без лимита). Возвращает true если SetProcessAffinityMask успешно
// применился (или no-op). Идемпотентна — безопасно звать несколько раз.
bool ApplyDotaCpuAffinity( DWORD pid, int instanceIdx, int coresPerInstance );
