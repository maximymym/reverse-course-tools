#include "hooks.h"

#include <MinHook.h>
#include <cstring>
#include <cstdio>
#include <fstream>
#include <string>
#include <json.hpp>

using json = nlohmann::json;

namespace proxyhook
{

// ── Originals ──
using fn_CreateProcessA_t = BOOL (WINAPI*)(
    LPCSTR, LPSTR, LPSECURITY_ATTRIBUTES, LPSECURITY_ATTRIBUTES,
    BOOL, DWORD, LPVOID, LPCSTR, LPSTARTUPINFOA, LPPROCESS_INFORMATION );

using fn_CreateProcessW_t = BOOL (WINAPI*)(
    LPCWSTR, LPWSTR, LPSECURITY_ATTRIBUTES, LPSECURITY_ATTRIBUTES,
    BOOL, DWORD, LPVOID, LPCWSTR, LPSTARTUPINFOW, LPPROCESS_INFORMATION );

static fn_CreateProcessA_t o_CreateProcessA = nullptr;
static fn_CreateProcessW_t o_CreateProcessW = nullptr;

// ── Helpers ──

// Сообщить orchestrator'у что spawn'нулся новый child PID — orchestrator'а
// kernel-redirect engine добавит его в watchlist быстрее чем 500ms polling.
// Pipe named "\\.\pipe\DotaFarmChildPid". Если pipe закрыт (orchestrator не
// слушает либо kernel redirect выключен) — silently игнорируем; polling
// thread orchestrator'а всё равно подберёт PID.
static void NotifyChildPid( DWORD parentPid, DWORD childPid )
{
    HANDLE hPipe = CreateFileA( "\\\\.\\pipe\\DotaFarmChildPid",
        GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr );
    if ( hPipe == INVALID_HANDLE_VALUE )
        return;
    char msg[64];
    int n = snprintf( msg, sizeof( msg ), "%lu %lu\n", parentPid, childPid );
    DWORD written = 0;
    WriteFile( hPipe, msg, (DWORD)n, &written, nullptr );
    CloseHandle( hPipe );
}

// Копируем proxy_<ppid>.json → proxy_<cpid>.json чтобы child нашёл конфиг.
static bool CopyProxyConfig( DWORD childPid )
{
    std::string src = GetConfigPathForPid( GetCurrentProcessId() );
    std::string dst = GetConfigPathForPid( childPid );

    // CopyFile не через heap — простой путь
    if ( !CopyFileA( src.c_str(), dst.c_str(), FALSE ) )
    {
        DbgLog( "CopyProxyConfig: CopyFile %s -> %s failed (err=%lu)",
            src.c_str(), dst.c_str(), GetLastError() );
        return false;
    }
    return true;
}

// Inject ProxyHook.dll в child process (CreateRemoteThread + LoadLibraryA).
// Thread handle блочащийся на WaitForSingleObject до 10s.
static bool InjectSelfIntoChild( HANDLE hProcess )
{
    const std::string& dllPath = g_state().selfDllPath;
    if ( dllPath.empty() )
    {
        DbgLog( "InjectSelfIntoChild: selfDllPath empty" );
        return false;
    }

    size_t sz = dllPath.size() + 1;
    void* pRemote = VirtualAllocEx( hProcess, nullptr, sz,
        MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE );
    if ( !pRemote )
    {
        DbgLog( "InjectSelfIntoChild: VirtualAllocEx failed (err=%lu)", GetLastError() );
        return false;
    }

    if ( !WriteProcessMemory( hProcess, pRemote, dllPath.c_str(), sz, nullptr ) )
    {
        DbgLog( "InjectSelfIntoChild: WriteProcessMemory failed (err=%lu)", GetLastError() );
        VirtualFreeEx( hProcess, pRemote, 0, MEM_RELEASE );
        return false;
    }

    HMODULE hK32 = GetModuleHandleA( "kernel32.dll" );
    auto pLL = (LPTHREAD_START_ROUTINE)GetProcAddress( hK32, "LoadLibraryA" );
    HANDLE hThread = CreateRemoteThread( hProcess, nullptr, 0, pLL, pRemote, 0, nullptr );
    if ( !hThread )
    {
        DbgLog( "InjectSelfIntoChild: CreateRemoteThread failed (err=%lu)", GetLastError() );
        VirtualFreeEx( hProcess, pRemote, 0, MEM_RELEASE );
        return false;
    }

    WaitForSingleObject( hThread, 10000 );

    DWORD exitCode = 0;
    GetExitCodeThread( hThread, &exitCode );
    CloseHandle( hThread );
    VirtualFreeEx( hProcess, pRemote, 0, MEM_RELEASE );

    if ( exitCode == 0 )
    {
        DbgLog( "InjectSelfIntoChild: LoadLibrary returned 0 (failed)" );
        return false;
    }
    return true;
}

// ── Hook ──

static BOOL WINAPI h_CreateProcessA( LPCSTR app, LPSTR cmdLine,
    LPSECURITY_ATTRIBUTES psa, LPSECURITY_ATTRIBUTES tsa,
    BOOL inherit, DWORD flags, LPVOID env, LPCSTR cwd,
    LPSTARTUPINFOA si, LPPROCESS_INFORMATION pi )
{
    bool forceSuspended = ( ( flags & CREATE_SUSPENDED ) == 0 );
    DWORD newFlags = flags | CREATE_SUSPENDED;

    BOOL ok = o_CreateProcessA( app, cmdLine, psa, tsa, inherit, newFlags, env, cwd, si, pi );
    if ( !ok )
        return FALSE;

    DbgLog( "CreateProcessA: spawned PID %lu (%s)", pi->dwProcessId,
        app ? app : ( cmdLine ? cmdLine : "(null)" ) );

    NotifyChildPid( GetCurrentProcessId(), pi->dwProcessId );

    // Copy proxy config → child
    if ( !CopyProxyConfig( pi->dwProcessId ) )
    {
        DbgLog( "CreateProcessA: CopyProxyConfig failed for PID %lu — skipping inject",
            pi->dwProcessId );
        // Продолжаем — может быть child без WS2_32 доступа, тогда inject всё равно ломанёт
    }
    else
    {
        if ( !InjectSelfIntoChild( pi->hProcess ) )
            DbgLog( "CreateProcessA: inject failed into PID %lu", pi->dwProcessId );
    }

    if ( forceSuspended )
        ResumeThread( pi->hThread );

    return TRUE;
}

static BOOL WINAPI h_CreateProcessW( LPCWSTR app, LPWSTR cmdLine,
    LPSECURITY_ATTRIBUTES psa, LPSECURITY_ATTRIBUTES tsa,
    BOOL inherit, DWORD flags, LPVOID env, LPCWSTR cwd,
    LPSTARTUPINFOW si, LPPROCESS_INFORMATION pi )
{
    bool forceSuspended = ( ( flags & CREATE_SUSPENDED ) == 0 );
    DWORD newFlags = flags | CREATE_SUSPENDED;

    BOOL ok = o_CreateProcessW( app, cmdLine, psa, tsa, inherit, newFlags, env, cwd, si, pi );
    if ( !ok )
        return FALSE;

    char n[512];
    int nn = 0;
    if ( app )
        nn = WideCharToMultiByte( CP_UTF8, 0, app, -1, n, sizeof( n ), nullptr, nullptr );
    else if ( cmdLine )
        nn = WideCharToMultiByte( CP_UTF8, 0, cmdLine, -1, n, sizeof( n ), nullptr, nullptr );
    if ( nn <= 0 ) strcpy( n, "(null)" );

    DbgLog( "CreateProcessW: spawned PID %lu (%s)", pi->dwProcessId, n );

    NotifyChildPid( GetCurrentProcessId(), pi->dwProcessId );

    if ( CopyProxyConfig( pi->dwProcessId ) )
    {
        if ( !InjectSelfIntoChild( pi->hProcess ) )
            DbgLog( "CreateProcessW: inject failed into PID %lu", pi->dwProcessId );
    }

    if ( forceSuspended )
        ResumeThread( pi->hThread );

    return TRUE;
}

// ── Install ──

bool InstallProcessHooks()
{
    HMODULE hK32 = GetModuleHandleA( "kernel32.dll" );
    if ( !hK32 )
        return false;

    void* tA = GetProcAddress( hK32, "CreateProcessA" );
    void* tW = GetProcAddress( hK32, "CreateProcessW" );
    if ( !tA || !tW )
        return false;

    MH_STATUS s1 = MH_CreateHook( tA, (LPVOID)h_CreateProcessA, (LPVOID*)&o_CreateProcessA );
    MH_STATUS s2 = MH_CreateHook( tW, (LPVOID)h_CreateProcessW, (LPVOID*)&o_CreateProcessW );
    if ( s1 != MH_OK || s2 != MH_OK )
    {
        DbgLog( "InstallProcessHooks: create %d/%d", s1, s2 );
        return false;
    }
    s1 = MH_EnableHook( tA );
    s2 = MH_EnableHook( tW );
    if ( s1 != MH_OK || s2 != MH_OK )
    {
        DbgLog( "InstallProcessHooks: enable %d/%d", s1, s2 );
        return false;
    }
    DbgLog( "InstallProcessHooks: OK" );
    return true;
}

} // namespace proxyhook
