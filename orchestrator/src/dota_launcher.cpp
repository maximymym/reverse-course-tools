#include "dota_launcher.h"
#include "steam_launcher.h"
#include <cstdio>
#include <cstring>
#include <winternl.h>
#include <algorithm>

#pragma comment(lib, "ntdll.lib")

// ── NtQuerySystemInformation types ──

#define SystemHandleInformation 16

typedef struct _SYSTEM_HANDLE_ENTRY {
	ULONG  ProcessId;
	BYTE   ObjectTypeNumber;
	BYTE   Flags;
	USHORT Handle;
	PVOID  Object;
	ACCESS_MASK GrantedAccess;
} SYSTEM_HANDLE_ENTRY;

typedef struct _SYSTEM_HANDLE_INFORMATION {
	ULONG HandleCount;
	SYSTEM_HANDLE_ENTRY Handles[1];
} SYSTEM_HANDLE_INFORMATION;

typedef NTSTATUS( NTAPI* pNtQuerySystemInformation )(
	ULONG, PVOID, ULONG, PULONG );

typedef NTSTATUS( NTAPI* pNtQueryObject )(
	HANDLE, ULONG, PVOID, ULONG, PULONG );

#define ObjectNameInformation 1

// ── Kill mutex: try native first, then handle64.exe ──

bool DotaLauncher::KillDotaMutex( DWORD dotaPid, const std::string& handleExe )
{
	// Try native approach first (faster, no external dependency)
	if ( KillMutexNative( dotaPid ) )
		return true;

	// Fallback to handle64.exe
	return KillMutexViaHandle64( dotaPid, handleExe );
}

// ── Native mutex kill via NtQuerySystemInformation ──

bool DotaLauncher::KillMutexNative( DWORD dotaPid )
{
	HANDLE hProcess = OpenProcess( PROCESS_DUP_HANDLE, FALSE, dotaPid );
	if ( !hProcess )
		return false;

	auto NtQuerySysInfo = (pNtQuerySystemInformation)GetProcAddress(
		GetModuleHandleA( "ntdll.dll" ), "NtQuerySystemInformation" );
	auto NtQueryObj = (pNtQueryObject)GetProcAddress(
		GetModuleHandleA( "ntdll.dll" ), "NtQueryObject" );

	if ( !NtQuerySysInfo || !NtQueryObj )
	{
		CloseHandle( hProcess );
		return false;
	}

	// Query all system handles (start with 4MB, grow if needed)
	ULONG bufSize = 4 * 1024 * 1024;
	BYTE* buf = nullptr;
	NTSTATUS status;

	for ( int attempt = 0; attempt < 5; attempt++ )
	{
		buf = (BYTE*)malloc( bufSize );
		if ( !buf ) { CloseHandle( hProcess ); return false; }

		status = NtQuerySysInfo( SystemHandleInformation, buf, bufSize, nullptr );
		if ( status == 0 ) // STATUS_SUCCESS
			break;

		free( buf );
		buf = nullptr;
		bufSize *= 2;
	}

	if ( !buf )
	{
		CloseHandle( hProcess );
		return false;
	}

	auto* info = (SYSTEM_HANDLE_INFORMATION*)buf;
	bool killed = false;

	for ( ULONG i = 0; i < info->HandleCount && !killed; i++ )
	{
		auto& entry = info->Handles[i];
		if ( entry.ProcessId != dotaPid )
			continue;

		// Duplicate handle to our process to query its name
		HANDLE hDup = nullptr;
		if ( !DuplicateHandle( hProcess, (HANDLE)(ULONG_PTR)entry.Handle,
			GetCurrentProcess(), &hDup, 0, FALSE, DUPLICATE_SAME_ACCESS ) )
			continue;

		// Query object name (with timeout protection — skip if it hangs)
		BYTE nameBuf[1024]{};
		ULONG retLen = 0;
		status = NtQueryObj( hDup, ObjectNameInformation, nameBuf, sizeof( nameBuf ), &retLen );

		if ( status == 0 )
		{
			auto* nameInfo = (UNICODE_STRING*)nameBuf;
			if ( nameInfo->Buffer && nameInfo->Length > 0 )
			{
				// Check if this is dota_singleton_mutex
				std::wstring name( nameInfo->Buffer, nameInfo->Length / sizeof( WCHAR ) );
				if ( name.find( L"dota_singleton_mutex" ) != std::wstring::npos )
				{
					// Close the handle in the target process
					HANDLE hClose = nullptr;
					DuplicateHandle( hProcess, (HANDLE)(ULONG_PTR)entry.Handle,
						GetCurrentProcess(), &hClose, 0, FALSE, DUPLICATE_CLOSE_SOURCE );
					if ( hClose )
						CloseHandle( hClose );
					killed = true;
				}
			}
		}

		CloseHandle( hDup );
	}

	free( buf );
	CloseHandle( hProcess );
	return killed;
}

// ── handle64.exe fallback ──

bool DotaLauncher::KillMutexViaHandle64( DWORD dotaPid, const std::string& handleExe )
{
	if ( handleExe.empty() || GetFileAttributesA( handleExe.c_str() ) == INVALID_FILE_ATTRIBUTES )
		return false;

	char cmd[512];
	snprintf( cmd, sizeof( cmd ), "\"%s\" -accepteula -a -p %lu", handleExe.c_str(), dotaPid );

	FILE* pipe = _popen( cmd, "r" );
	if ( !pipe )
		return false;

	char line[1024];
	std::string handleHex;

	while ( fgets( line, sizeof( line ), pipe ) )
	{
		if ( strstr( line, "dota_singleton_mutex" ) )
		{
			// Parse: "   1A4: Mutant  \Sessions\1\BaseNamedObjects\dota_singleton_mutex"
			char* p = line;
			while ( *p == ' ' || *p == '\t' ) p++;
			char* colon = strchr( p, ':' );
			if ( colon )
			{
				*colon = '\0';
				handleHex = p;
			}
			break;
		}
	}
	_pclose( pipe );

	if ( handleHex.empty() )
		return true; // not found = already gone

	snprintf( cmd, sizeof( cmd ), "\"%s\" -accepteula -c %s -p %lu -y",
		handleExe.c_str(), handleHex.c_str(), dotaPid );

	pipe = _popen( cmd, "r" );
	if ( !pipe )
		return false;

	bool ok = false;
	while ( fgets( line, sizeof( line ), pipe ) )
	{
		if ( strstr( line, "Handle closed" ) )
		{
			ok = true;
			break;
		}
	}
	_pclose( pipe );
	return ok;
}

// ── Wait for ready ──

bool DotaLauncher::WaitForDotaReady( DWORD pid, int timeoutMs )
{
	DWORD start = GetTickCount();
	while ( (int)( GetTickCount() - start ) < timeoutMs )
	{
		if ( !IsProcessAlive( pid ) )
			return false;
		if ( IsModuleLoaded( pid, "client.dll" ) )
			return true;
		Sleep( 1000 );
	}
	return false;
}

// ── Process utilities ──

std::vector<DWORD> DotaLauncher::FindDotaPids()   { return FindProcessByName( "dota2.exe" ); }
std::vector<DWORD> DotaLauncher::FindSteamPids()   { return FindProcessByName( "steam.exe" ); }

std::vector<DWORD> DotaLauncher::FindProcessByName( const char* name )
{
	std::vector<DWORD> result;
	HANDLE snap = CreateToolhelp32Snapshot( TH32CS_SNAPPROCESS, 0 );
	if ( snap == INVALID_HANDLE_VALUE ) return result;

	PROCESSENTRY32 pe{};
	pe.dwSize = sizeof( pe );
	if ( Process32First( snap, &pe ) )
	{
		do
		{
			if ( _stricmp( pe.szExeFile, name ) == 0 )
				result.push_back( pe.th32ProcessID );
		}
		while ( Process32Next( snap, &pe ) );
	}
	CloseHandle( snap );
	return result;
}

bool DotaLauncher::IsProcessAlive( DWORD pid )
{
	HANDLE h = OpenProcess( PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid );
	if ( !h ) return false;
	DWORD code = 0;
	GetExitCodeProcess( h, &code );
	CloseHandle( h );
	return ( code == STILL_ACTIVE );
}

// ── Working set trim ──
//
// SetProcessWorkingSetSizeEx(hProc, -1, -1, 0) — стандартный путь, эквивалент
// EmptyWorkingSet. PROCESS_SET_QUOTA — официальное требование MSDN (хотя на
// Win10+ часто достаточно PROCESS_QUERY_LIMITED_INFORMATION + PROCESS_SET_QUOTA).
// Возврат: true если syscall удался; "удался" ≠ "WS уменьшился" — система
// решает что выгружать, могут остаться locked pages.
bool DotaLauncher::TrimWorkingSet( DWORD pid )
{
	if ( pid == 0 ) return false;
	HANDLE h = OpenProcess( PROCESS_SET_QUOTA | PROCESS_QUERY_LIMITED_INFORMATION,
		FALSE, pid );
	if ( !h ) return false;
	BOOL ok = SetProcessWorkingSetSizeEx( h, (SIZE_T)-1, (SIZE_T)-1, 0 );
	CloseHandle( h );
	return ok != 0;
}

// ── Process tree walk ──
//
// Один TH32CS_SNAPPROCESS, потом BFS: уровень 0 = parent, собираем childs
// (где th32ParentProcessID == один из текущего уровня), итерируем пока есть
// новые. visited set против циклов из-за PID reuse.
// Для Steam-фермы реальная глубина ≤2 (steam → webhelper → Chromium subs).
std::vector<DWORD> DotaLauncher::FindChildPidsRecursive( DWORD parentPid )
{
	std::vector<DWORD> out;
	if ( parentPid == 0 ) return out;

	HANDLE snap = CreateToolhelp32Snapshot( TH32CS_SNAPPROCESS, 0 );
	if ( snap == INVALID_HANDLE_VALUE ) return out;

	std::vector<std::pair<DWORD, DWORD>> all; // (pid, ppid)
	PROCESSENTRY32 pe{};
	pe.dwSize = sizeof( pe );
	if ( Process32First( snap, &pe ) )
	{
		do
		{
			all.emplace_back( pe.th32ProcessID, pe.th32ParentProcessID );
		}
		while ( Process32Next( snap, &pe ) );
	}
	CloseHandle( snap );

	std::vector<DWORD> frontier{ parentPid };
	std::vector<bool> visited;
	visited.resize( 1 << 17, false ); // PIDs ≤131071 — реальный Windows limit
	if ( parentPid < visited.size() )
		visited[parentPid] = true;

	while ( !frontier.empty() )
	{
		std::vector<DWORD> next;
		for ( DWORD cur : frontier )
		{
			for ( auto& pr : all )
			{
				if ( pr.second != cur || pr.first == cur )
					continue;
				if ( pr.first >= visited.size() || visited[pr.first] )
					continue;
				visited[pr.first] = true;
				next.push_back( pr.first );
				out.push_back( pr.first );
			}
		}
		frontier.swap( next );
	}
	return out;
}

int DotaLauncher::TrimWorkingSetTree( DWORD parentPid )
{
	if ( parentPid == 0 ) return 0;
	int trimmed = 0;
	if ( TrimWorkingSet( parentPid ) )
		trimmed++;
	auto children = FindChildPidsRecursive( parentPid );
	for ( DWORD child : children )
	{
		if ( TrimWorkingSet( child ) )
			trimmed++;
	}
	return trimmed;
}

bool DotaLauncher::IsModuleLoaded( DWORD pid, const char* moduleName )
{
	HANDLE snap = CreateToolhelp32Snapshot( TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid );
	if ( snap == INVALID_HANDLE_VALUE ) return false;

	MODULEENTRY32 me{};
	me.dwSize = sizeof( me );
	bool found = false;
	if ( Module32First( snap, &me ) )
	{
		do
		{
			if ( _stricmp( me.szModule, moduleName ) == 0 )
			{ found = true; break; }
		}
		while ( Module32Next( snap, &me ) );
	}
	CloseHandle( snap );
	return found;
}

// ── Recovery: запустить ТОЛЬКО Dota для живого Steam ──

bool ApplyDotaCpuAffinity( DWORD pid, int instanceIdx, int coresPerInstance )
{
	if ( coresPerInstance <= 0 ) return true;  // no-op = unlimited
	if ( !pid ) return false;

	// Сколько ядер реально доступно процессу (учитываем system affinity).
	HANDLE hProc = OpenProcess(
		PROCESS_QUERY_INFORMATION | PROCESS_SET_INFORMATION,
		FALSE, pid );
	if ( !hProc ) return false;

	DWORD_PTR procMask = 0, sysMask = 0;
	if ( !GetProcessAffinityMask( hProc, &procMask, &sysMask ) || sysMask == 0 )
	{
		CloseHandle( hProc );
		return false;
	}

	// Считаем общее количество доступных системе ядер.
	int totalCores = 0;
	for ( int b = 0; b < (int)( sizeof( DWORD_PTR ) * 8 ); b++ )
		if ( sysMask & ( (DWORD_PTR)1 << b ) ) totalCores++;
	if ( totalCores <= 0 ) { CloseHandle( hProc ); return false; }

	int N = coresPerInstance;
	if ( N > totalCores ) N = totalCores;
	if ( N <= 0 ) { CloseHandle( hProc ); return false; }

	// Шифт mask'и: instance 0 → биты [0..N-1], instance 1 → [N..2N-1], и т.д.
	// При overflow за totalCores — циклимся, чтобы все боты получили N ядер.
	int startBit = ( instanceIdx * N ) % totalCores;
	DWORD_PTR newMask = 0;
	for ( int k = 0; k < N; k++ )
	{
		int bit = ( startBit + k ) % totalCores;
		newMask |= ( (DWORD_PTR)1 << bit );
	}

	// Маска должна быть подмножеством sysMask.
	newMask &= sysMask;
	if ( newMask == 0 ) { CloseHandle( hProc ); return false; }

	BOOL ok = SetProcessAffinityMask( hProc, newMask );
	CloseHandle( hProc );
	return ok != FALSE;
}

DWORD DotaLauncher::LaunchDotaOnly( int instanceIdx, const FarmConfig& cfg, DWORD steamPid,
	const std::vector<DWORD>& excludeExistingPids, int timeoutMs )
{
	// 1. Kill stale mutex'ы на всех живых dota PID (zombie singleton block).
	auto staleDotas = FindDotaPids();
	for ( DWORD pid : staleDotas )
		KillDotaMutex( pid, cfg.handleExe );

	// 2. Spawn dota2.exe из BotDota\<idx>\ через SteamLauncher::LaunchDota
	// (использует тот же VAC parent-process spoof = steam.exe).
	SteamLauncher launcher;
	DWORD spawnedPid = launcher.LaunchDota( instanceIdx, cfg, steamPid );

	// 3. Attribute PID к THIS recovery thread'у. CR-FIX 2026-05-26:
	// раньше код возвращал "первый PID не из excludeExistingPids" — при
	// concurrent recovery threads оба видели одну новую dota и оба её
	// claim'или → один из двух recovery остаётся без процесса → fake-DEAD.
	// Сейчас приоритет:
	//   (a) spawnedPid если IsProcessAlive (наш собственный CreateProcess —
	//       100% наш) — но только если он не в excludeExistingPids,
	//       иначе это zombie/reuse.
	//   (b) если spawnedPid 0 или мёртв — fallback на FindDotaPids ∖ exclude
	//       (старый поведение, для случая когда Steam IPC сам перезапускает
	//       dota через single-instance check).
	// Дополнительная защита: m_recoveryMx в orchestrator.cpp serializes
	// thread'ы, поэтому race по сути устранён, но Fix (a)+(b) держим как
	// belt+suspenders на случай если кто-то снимет mutex в будущем.
	DWORD start = GetTickCount();
	while ( (int)( GetTickCount() - start ) < timeoutMs )
	{
		// (a) Trust spawnedPid если он живой и не в exclude list
		if ( spawnedPid != 0 && IsProcessAlive( spawnedPid ) &&
			std::find( excludeExistingPids.begin(), excludeExistingPids.end(),
				spawnedPid ) == excludeExistingPids.end() )
		{
			return spawnedPid;
		}

		// (b) Fallback: FindDotaPids ∖ exclude. Под mutex'ом эта ветка обычно
		// сразу возвращает нашу dota (Steam single-instance не релевантен
		// в recovery scenario потому что мы убили старую dota перед spawn'ом).
		auto all = FindDotaPids();
		for ( DWORD pid : all )
		{
			if ( std::find( excludeExistingPids.begin(), excludeExistingPids.end(), pid )
				!= excludeExistingPids.end() )
				continue;
			return pid;
		}
		Sleep( 1000 );
	}
	return 0;
}
