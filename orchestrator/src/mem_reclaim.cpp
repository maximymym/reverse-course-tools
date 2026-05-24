#include "mem_reclaim.h"

#include <Psapi.h>
#include <TlHelp32.h>
#include <algorithm>
#include <unordered_set>

#pragma comment( lib, "psapi.lib" )

namespace
{
	// ── Local ntdll typedefs (winternl.h не публикует эти константы) ──
	using NTSTATUS = LONG;
	constexpr NTSTATUS STATUS_SUCCESS = 0;

	// SYSTEM_INFORMATION_CLASS значения — стабильны со времён Win Vista.
	// Источник: Geoff Chappell ntdll/api/sysinfo, MSDN winternl.
	constexpr ULONG SystemMemoryListInformation              = 0x50;  // 80
	constexpr ULONG SystemFileCacheInformationEx             = 0x51;  // 81
	constexpr ULONG SystemCombinePhysicalMemoryInformation   = 0x82;  // 130

	// SYSTEM_MEMORY_LIST_COMMAND — аргумент set-операции.
	enum SYSTEM_MEMORY_LIST_COMMAND : ULONG
	{
		MemoryCaptureAccessedBits             = 0,
		MemoryCaptureAndResetAccessedBits     = 1,
		MemoryEmptyWorkingSets                = 2,
		MemoryFlushModifiedList               = 3,
		MemoryPurgeStandbyList                = 4,
		MemoryPurgeLowPriorityStandbyList     = 5,
		MemoryCommandMax                      = 6,
	};

	// SYSTEM_MEMORY_LIST_INFORMATION (query-вариант same class).
	// Поля до Win10 1809 не менялись. На x64 — PFN counts = ULONG_PTR (8 байт).
	struct SYSTEM_MEMORY_LIST_INFORMATION
	{
		ULONG_PTR ZeroPageCount;
		ULONG_PTR FreePageCount;
		ULONG_PTR ModifiedPageCount;
		ULONG_PTR ModifiedNoWritePageCount;
		ULONG_PTR BadPageCount;
		ULONG_PTR PageCountByPriority[8];      // standby list, priority 0-7
		ULONG_PTR RepurposedPagesByPriority[8];
		ULONG_PTR ModifiedPageCountPageFile;
	};

	// MEMORY_COMBINE_INFORMATION_EX — argument for combine syscall.
	// Поля обнулены, ядро само делает работу.
	struct MEMORY_COMBINE_INFORMATION_EX
	{
		HANDLE    Handle;
		ULONG_PTR PagesCombined;
		ULONG     Flags;
	};

	// SYSTEM_FILECACHE_INFORMATION — для системного file cache trim.
	struct SYSTEM_FILECACHE_INFORMATION
	{
		ULONG_PTR CurrentSize;
		ULONG_PTR PeakSize;
		ULONG     PageFaultCount;
		ULONG_PTR MinimumWorkingSet;
		ULONG_PTR MaximumWorkingSet;
		ULONG_PTR CurrentSizeIncludingTransitionInPages;
		ULONG_PTR PeakSizeIncludingTransitionInPages;
		ULONG     TransitionRePurposeCount;
		ULONG     Flags;
	};

	// ── ntdll function pointer cache (lazy GetProcAddress) ──
	using PFN_NtSetSystemInformation   = NTSTATUS (NTAPI*)( ULONG, PVOID, ULONG );
	using PFN_NtQuerySystemInformation = NTSTATUS (NTAPI*)( ULONG, PVOID, ULONG, PULONG );
	using PFN_RtlAdjustPrivilege       = NTSTATUS (NTAPI*)( ULONG, BOOLEAN, BOOLEAN, PBOOLEAN );

	struct NtFns
	{
		PFN_NtSetSystemInformation   set    = nullptr;
		PFN_NtQuerySystemInformation query  = nullptr;
		PFN_RtlAdjustPrivilege       adjust = nullptr;
		bool resolved = false;
	};

	NtFns& GetNt()
	{
		static NtFns fns;
		if ( !fns.resolved )
		{
			HMODULE h = GetModuleHandleW( L"ntdll.dll" );
			if ( h )
			{
				fns.set    = (PFN_NtSetSystemInformation)
					GetProcAddress( h, "NtSetSystemInformation" );
				fns.query  = (PFN_NtQuerySystemInformation)
					GetProcAddress( h, "NtQuerySystemInformation" );
				fns.adjust = (PFN_RtlAdjustPrivilege)
					GetProcAddress( h, "RtlAdjustPrivilege" );
			}
			fns.resolved = true;
		}
		return fns;
	}

	bool SetMemListCommand( SYSTEM_MEMORY_LIST_COMMAND cmd )
	{
		auto& nt = GetNt();
		if ( !nt.set ) return false;
		SYSTEM_MEMORY_LIST_COMMAND c = cmd;
		NTSTATUS s = nt.set( SystemMemoryListInformation, &c, sizeof( c ) );
		return s == STATUS_SUCCESS;
	}

	// Page size кешируется (SystemInfo не меняется в runtime).
	uint64_t PageSize()
	{
		static uint64_t s = 0;
		if ( !s )
		{
			SYSTEM_INFO si{};
			GetSystemInfo( &si );
			s = si.dwPageSize ? si.dwPageSize : 4096;
		}
		return s;
	}
}

namespace MemReclaim
{

bool EnablePrivileges()
{
	auto& nt = GetNt();
	if ( !nt.adjust ) return false;

	constexpr ULONG SE_INCREASE_QUOTA_PRIVILEGE       = 5;
	constexpr ULONG SE_PROF_SINGLE_PROCESS_PRIVILEGE  = 13;

	BOOLEAN prev = FALSE;
	NTSTATUS s1 = nt.adjust( SE_PROF_SINGLE_PROCESS_PRIVILEGE, TRUE, FALSE, &prev );
	NTSTATUS s2 = nt.adjust( SE_INCREASE_QUOTA_PRIVILEGE,      TRUE, FALSE, &prev );
	// STATUS_NOT_ALL_ASSIGNED = 0x106 — частичный success; считаем за неудачу
	// чтобы caller'у было видно что система ограничена.
	return s1 == STATUS_SUCCESS && s2 == STATUS_SUCCESS;
}

bool EmptyAllWorkingSets()           { return SetMemListCommand( MemoryEmptyWorkingSets ); }
bool FlushModified()                 { return SetMemListCommand( MemoryFlushModifiedList ); }
bool PurgeStandby()                  { return SetMemListCommand( MemoryPurgeStandbyList ); }
bool PurgeLowPriorityStandby()       { return SetMemListCommand( MemoryPurgeLowPriorityStandbyList ); }

bool CombineMemoryLists()
{
	auto& nt = GetNt();
	if ( !nt.set ) return false;
	MEMORY_COMBINE_INFORMATION_EX info{};   // все поля 0 — ядро сам комбинирует
	NTSTATUS s = nt.set( SystemCombinePhysicalMemoryInformation,
		&info, sizeof( info ) );
	return s == STATUS_SUCCESS;
}

MemoryStats QuerySystemMemory()
{
	MemoryStats out{};

	MEMORYSTATUSEX mse{};
	mse.dwLength = sizeof( mse );
	if ( GlobalMemoryStatusEx( &mse ) )
	{
		out.totalPhysMB   = mse.ullTotalPhys      / ( 1024ULL * 1024ULL );
		out.availPhysMB   = mse.ullAvailPhys      / ( 1024ULL * 1024ULL );
		out.commitLimitMB = mse.ullTotalPageFile  / ( 1024ULL * 1024ULL );
		uint64_t pageFileFree = mse.ullAvailPageFile / ( 1024ULL * 1024ULL );
		out.commitUsedMB  = ( out.commitLimitMB > pageFileFree )
			? out.commitLimitMB - pageFileFree : 0;
		out.memoryLoadPct = mse.dwMemoryLoad;
		out.valid = true;
	}

	auto& nt = GetNt();
	if ( nt.query )
	{
		SYSTEM_MEMORY_LIST_INFORMATION mli{};
		ULONG ret = 0;
		NTSTATUS s = nt.query( SystemMemoryListInformation,
			&mli, sizeof( mli ), &ret );
		if ( s == STATUS_SUCCESS )
		{
			uint64_t ps = PageSize();
			uint64_t standbyPages = 0;
			for ( int i = 0; i < 8; i++ )
				standbyPages += (uint64_t)mli.PageCountByPriority[i];
			out.standbyMB  = ( standbyPages * ps )                  / ( 1024ULL * 1024ULL );
			out.modifiedMB = ( (uint64_t)mli.ModifiedPageCount * ps ) / ( 1024ULL * 1024ULL );
			out.freeMB     = ( (uint64_t)mli.FreePageCount     * ps ) / ( 1024ULL * 1024ULL );
		}
	}

	return out;
}

std::vector<ProcInfo> EnumProcessesRam()
{
	std::vector<ProcInfo> out;

	DWORD pids[2048];
	DWORD bytesReturned = 0;
	if ( !K32EnumProcesses( pids, sizeof( pids ), &bytesReturned ) )
		return out;
	const DWORD count = bytesReturned / sizeof( DWORD );
	out.reserve( count );

	for ( DWORD i = 0; i < count; i++ )
	{
		DWORD pid = pids[i];
		if ( pid == 0 ) continue;

		HANDLE h = OpenProcess(
			PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ,
			FALSE, pid );
		if ( !h ) continue;

		ProcInfo p;
		p.pid = pid;

		PROCESS_MEMORY_COUNTERS_EX pmc{};
		pmc.cb = sizeof( pmc );
		if ( K32GetProcessMemoryInfo( h, (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof( pmc ) ) )
		{
			p.wsBytes = pmc.WorkingSetSize;
			p.pmBytes = pmc.PrivateUsage;
		}

		DWORD handleCount = 0;
		if ( GetProcessHandleCount( h, &handleCount ) )
			p.handles = handleCount;

		// Image name (cheap path: K32GetModuleBaseNameA из base module).
		char name[MAX_PATH] = { 0 };
		if ( K32GetModuleBaseNameA( h, nullptr, name, MAX_PATH ) )
			p.name = name;

		CloseHandle( h );

		out.push_back( std::move( p ) );
	}

	// Threads через snapshot (one pass, дёшево). Тут только thread count
	// нужен, поэтому не открываем каждый thread handle.
	HANDLE snap = CreateToolhelp32Snapshot( TH32CS_SNAPTHREAD, 0 );
	if ( snap != INVALID_HANDLE_VALUE )
	{
		THREADENTRY32 te{};
		te.dwSize = sizeof( te );
		if ( Thread32First( snap, &te ) )
		{
			do
			{
				for ( auto& p : out )
				{
					if ( p.pid == te.th32OwnerProcessID )
					{
						p.threads++;
						break;
					}
				}
			}
			while ( Thread32Next( snap, &te ) );
		}
		CloseHandle( snap );
	}

	return out;
}

uint64_t SumWsBytesIn( const std::vector<ProcInfo>& snapshot,
	const std::vector<DWORD>& pids )
{
	if ( pids.empty() || snapshot.empty() ) return 0;
	std::unordered_set<DWORD> set( pids.begin(), pids.end() );
	uint64_t total = 0;
	for ( const auto& p : snapshot )
		if ( set.count( p.pid ) )
			total += p.wsBytes;
	return total;
}

uint64_t SumWsBytesFor( const std::vector<DWORD>& pids )
{
	auto snap = EnumProcessesRam();
	return SumWsBytesIn( snap, pids );
}

} // namespace MemReclaim
