#include "injector.h"
#include "manual_mapper.h"
#include <json.hpp>
#include <fstream>
#include <cstdio>
#include <objbase.h>
#pragma comment(lib, "ole32.lib")

using json = nlohmann::json;

bool Injector::Init()
{
	HANDLE hToken = nullptr;
	if ( !OpenProcessToken( GetCurrentProcess(), TOKEN_ALL_ACCESS, &hToken ) )
		return false;

	LUID luid;
	if ( !LookupPrivilegeValueA( nullptr, "SeDebugPrivilege", &luid ) )
	{
		CloseHandle( hToken );
		return false;
	}

	TOKEN_PRIVILEGES tp{};
	tp.PrivilegeCount = 1;
	tp.Privileges[0].Luid = luid;
	tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

	BOOL ok = AdjustTokenPrivileges( hToken, FALSE, &tp, sizeof( tp ), nullptr, nullptr );
	CloseHandle( hToken );

	m_bInitialized = ( ok && GetLastError() == ERROR_SUCCESS );
	return m_bInitialized;
}

bool Injector::InjectLoadLibrary( DWORD pid, const std::string& dllPath )
{
	HANDLE hProcess = OpenProcess( PROCESS_ALL_ACCESS, FALSE, pid );
	if ( !hProcess )
	{
		manual_mapper::LogFn( "[ll] OpenProcess(%lu) failed: 0x%lX (process likely dead)\n",
			pid, GetLastError() );
		return false;
	}
	DWORD exitCode = 0;
	if ( GetExitCodeProcess( hProcess, &exitCode ) && exitCode != STILL_ACTIVE )
	{
		manual_mapper::LogFn( "[ll] pid=%lu already exited (code=%lu) before inject\n",
			pid, exitCode );
		CloseHandle( hProcess );
		return false;
	}

	// Allocate memory in target for DLL path
	size_t pathLen = dllPath.size() + 1;
	void* pRemote = VirtualAllocEx( hProcess, nullptr, pathLen, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE );
	if ( !pRemote )
	{
		CloseHandle( hProcess );
		return false;
	}

	// Write DLL path
	if ( !WriteProcessMemory( hProcess, pRemote, dllPath.c_str(), pathLen, nullptr ) )
	{
		VirtualFreeEx( hProcess, pRemote, 0, MEM_RELEASE );
		CloseHandle( hProcess );
		return false;
	}

	// Get LoadLibraryA address
	HMODULE hKernel = GetModuleHandleA( "kernel32.dll" );
	auto pLoadLib = (LPTHREAD_START_ROUTINE)GetProcAddress( hKernel, "LoadLibraryA" );

	// Create remote thread
	HANDLE hThread = CreateRemoteThread( hProcess, nullptr, 0, pLoadLib, pRemote, 0, nullptr );
	if ( !hThread )
	{
		VirtualFreeEx( hProcess, pRemote, 0, MEM_RELEASE );
		CloseHandle( hProcess );
		return false;
	}

	DWORD wr = WaitForSingleObject( hThread, 10000 );

	// Check if DLL was loaded. Если thread не завершился в отведённое время —
	// LoadLibraryA ещё в работе (редко, но бывает при DllMain, который сам
	// LoadLibrary подцепляет). В этом случае exitCode будет STILL_ACTIVE и мы
	// репортим fail — orchestrator должен считать inject неудачным.
	DWORD threadExit = 0;
	GetExitCodeThread( hThread, &threadExit );

	CloseHandle( hThread );
	VirtualFreeEx( hProcess, pRemote, 0, MEM_RELEASE );
	CloseHandle( hProcess );

	if ( wr != WAIT_OBJECT_0 )
	{
		manual_mapper::LogFn( "[ll] WaitForSingleObject pid=%lu wr=0x%lX threadExit=0x%lX (timeout or proc died)\n",
			pid, wr, threadExit );
		return false;
	}
	if ( threadExit == 0 )
	{
		manual_mapper::LogFn( "[ll] LoadLibraryA returned NULL in pid=%lu (DLL failed to load — check DllMain)\n", pid );
		return false;
	}
	manual_mapper::LogFn( "[ll] LoadLibrary OK pid=%lu hModule=0x%lX\n", pid, threadExit );
	return true;
}

bool Injector::InjectManualMap( DWORD pid, const uint8_t* dllBuf, size_t dllSize )
{
	if ( !dllBuf || dllSize < 0x200 ) return false;
	return manual_mapper::ManualMap( pid, dllBuf, dllSize );
}

bool Injector::InjectViaTempFile( DWORD pid, const uint8_t* dllBuf, size_t dllSize,
	const char* tag )
{
	if ( !dllBuf || dllSize < 0x200 ) return false;

	manual_mapper::LogFn( "[tf] InjectViaTempFile pid=%lu size=%zuB tag=%s\n",
		pid, dllSize, ( tag && *tag ) ? tag : "x" );

	// Bail early if the target is already dead — writing a temp file just to
	// fail on OpenProcess is wasted I/O.
	HANDLE hAlive = OpenProcess( PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid );
	if ( !hAlive )
	{
		manual_mapper::LogFn( "[tf] OpenProcess(%lu) failed: 0x%lX — process dead, abort\n",
			pid, GetLastError() );
		return false;
	}
	DWORD ec = 0;
	if ( GetExitCodeProcess( hAlive, &ec ) && ec != STILL_ACTIVE )
	{
		manual_mapper::LogFn( "[tf] pid=%lu already exited (code=%lu), abort\n", pid, ec );
		CloseHandle( hAlive );
		return false;
	}
	CloseHandle( hAlive );

	// Build random temp path: %TEMP%\df_<tag>_<GUID>.dll
	char tempDir[MAX_PATH] = { 0 };
	if ( !GetTempPathA( MAX_PATH, tempDir ) ) return false;
	GUID g{};
	if ( CoCreateGuid( &g ) != S_OK ) return false;
	char guidStr[40] = { 0 };
	wsprintfA( guidStr, "%08lX%04X%04X", g.Data1, g.Data2, g.Data3 );
	char tempPath[MAX_PATH] = { 0 };
	wsprintfA( tempPath, "%sdf_%s_%s.dll", tempDir,
		( tag && *tag ) ? tag : "x", guidStr );

	// Write decrypted buffer to disk
	HANDLE hFile = CreateFileA( tempPath, GENERIC_WRITE, 0, nullptr,
		CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr );
	if ( hFile == INVALID_HANDLE_VALUE ) return false;
	DWORD written = 0;
	BOOL wok = WriteFile( hFile, dllBuf, (DWORD)dllSize, &written, nullptr );
	FlushFileBuffers( hFile );
	CloseHandle( hFile );
	if ( !wok || written != dllSize )
	{
		DeleteFileA( tempPath );
		return false;
	}

	// LoadLibrary inject
	bool ok = InjectLoadLibrary( pid, tempPath );

	// Secure-delete: overwrite contents with zeros, then unlink. LoadLibrary
	// keeps the file mapped (image section), so DeleteFileA may fail until the
	// target unloads the DLL — schedule MoveFileEx delayed delete in that case.
	HANDLE hOv = CreateFileA( tempPath, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
		nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr );
	if ( hOv != INVALID_HANDLE_VALUE )
	{
		std::vector<uint8_t> zeros( 64 * 1024, 0 );
		size_t remaining = dllSize;
		while ( remaining > 0 )
		{
			DWORD chunk = (DWORD)( remaining > zeros.size() ? zeros.size() : remaining );
			DWORD w = 0;
			if ( !WriteFile( hOv, zeros.data(), chunk, &w, nullptr ) || w != chunk )
				break;
			remaining -= chunk;
		}
		FlushFileBuffers( hOv );
		CloseHandle( hOv );
	}
	if ( !DeleteFileA( tempPath ) )
	{
		// File still mapped by target process — schedule delete on reboot.
		MoveFileExA( tempPath, nullptr, MOVEFILE_DELAY_UNTIL_REBOOT );
	}

	return ok;
}

bool Injector::WriteInstanceConfig( DWORD pid, int instanceId, const char* role,
	const std::vector<std::string>& heroes,
	const uint64_t* partyMembers, int partyCount,
	uint32_t region, uint32_t gameMode,
	uint64_t reconnectLobbyId )
{
	CreateDirectoryA( "C:\\temp", nullptr );
	CreateDirectoryA( "C:\\temp\\andromeda", nullptr );

	char path[MAX_PATH];
	snprintf( path, sizeof( path ), "C:\\temp\\andromeda\\instance_%lu.json", pid );

	json j;
	j["instance_id"] = instanceId;
	j["role"] = role;
	j["region"] = region;
	j["game_mode"] = gameMode;

	// B4: при recovery с известным lobby — DLL должна reconnect, а НЕ инициировать
	// matchmaking. auto_queue выключаем чтобы избежать race "ищем новый match
	// пока пытаемся reconnect". DLL-side читает эти поля; если их нет — старое
	// поведение (matchmaking init через auto_queue=true).
	if ( reconnectLobbyId != 0 )
	{
		j["auto_queue"]          = false;
		j["reconnect_to_match"]  = true;
		j["lobby_id"]            = reconnectLobbyId;
	}
	else
	{
		j["auto_queue"] = true;
	}

	// Hero pool — DLL cycles through these during pick phase
	json heroArr = json::array();
	for ( auto& h : heroes )
		heroArr.push_back( h );
	j["heroes"] = heroArr;

	json members = json::array();
	for ( int i = 0; i < partyCount; i++ )
		members.push_back( partyMembers[i] );
	j["party_members"] = members;

	// Atomic write
	char tmpPath[MAX_PATH];
	snprintf( tmpPath, sizeof( tmpPath ), "%s.tmp", path );

	std::ofstream f( tmpPath );
	if ( !f.is_open() )
		return false;

	f << j.dump( 2 );
	f.close();

	return MoveFileExA( tmpPath, path, MOVEFILE_REPLACE_EXISTING ) != 0;
}

bool Injector::WriteProxyConfig( DWORD pid, const std::string& proxy,
	const std::string& hwidSeed )
{
	CreateDirectoryA( "C:\\temp", nullptr );
	CreateDirectoryA( "C:\\temp\\andromeda", nullptr );

	char path[MAX_PATH];
	snprintf( path, sizeof( path ), "C:\\temp\\andromeda\\proxy_%lu.json", pid );

	json j;
	j["proxy"]      = proxy;
	// UDP hook нестабилен — ломает Steam (login/bootstrap не проходит).
	// Для MVP отключаем. Steam login + matchmaking работают через TCP proxy.
	// Dota voice/P2P UDP уходит напрямую (не через proxy) — это acceptable.
	j["enable_udp"] = false;
	j["bypass_hosts"] = json::array( { "127.", "localhost", "steamloopback.host" } );

	if ( !hwidSeed.empty() )
	{
		json h;
		h["enabled"] = true;
		h["seed"]    = hwidSeed;
		j["hwid_spoof"] = h;
	}

	char tmpPath[MAX_PATH];
	snprintf( tmpPath, sizeof( tmpPath ), "%s.tmp", path );

	std::ofstream f( tmpPath );
	if ( !f.is_open() )
		return false;

	f << j.dump( 2 );
	f.close();

	return MoveFileExA( tmpPath, path, MOVEFILE_REPLACE_EXISTING ) != 0;
}
