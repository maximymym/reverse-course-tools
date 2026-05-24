#include "hwid_spoof.h"

#include <windows.h>
#include <bcrypt.h>
#include <ctime>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#pragma comment( lib, "bcrypt.lib" )

#include "payload_loader.h"

namespace hwid_spoof
{

std::string MakeSeed( uint64_t steamId )
{
	time_t now = time( nullptr );
	tm tm_local{};
	localtime_s( &tm_local, &now );
	int year = tm_local.tm_year + 1900;
	int month = tm_local.tm_mon + 1;

	char buf[64];
	snprintf( buf, sizeof( buf ), "%llu_%d%02d",
		(unsigned long long)steamId, year, month );
	return buf;
}

std::string MakeMachineSeed()
{
	time_t now = time( nullptr );
	tm tm_local{};
	localtime_s( &tm_local, &now );
	int year = tm_local.tm_year + 1900;
	int month = tm_local.tm_mon + 1;

	char host[MAX_COMPUTERNAME_LENGTH + 1] = "host";
	DWORD hostLen = sizeof( host );
	GetComputerNameA( host, &hostLen );

	char buf[96];
	snprintf( buf, sizeof( buf ), "machine_%s_%d%02d", host, year, month );
	return buf;
}

// 16-hex GUID для разделения параллельных вызовов и затруднения forensics.
static std::string MakeRandomTag()
{
	unsigned char rnd[8] = { 0 };
	BCRYPT_ALG_HANDLE alg = nullptr;
	if ( BCRYPT_SUCCESS( BCryptOpenAlgorithmProvider( &alg, BCRYPT_RNG_ALGORITHM, nullptr, 0 ) ) )
	{
		BCryptGenRandom( alg, rnd, sizeof( rnd ), 0 );
		BCryptCloseAlgorithmProvider( alg, 0 );
	}
	else
	{
		// Fallback — не криптостойкий, но для имени temp-папки сойдёт.
		uint64_t t = (uint64_t)GetTickCount64() ^ ( (uint64_t)GetCurrentProcessId() << 32 );
		memcpy( rnd, &t, sizeof( rnd ) );
	}
	char hex[17] = { 0 };
	for ( int i = 0; i < 8; ++i )
		snprintf( hex + i * 2, 3, "%02x", rnd[i] );
	return hex;
}

// Создаём %TEMP%\df_<GUID>\, возвращаем полный путь без trailing slash.
// Empty string при ошибке.
static std::string MakeTempStagingDir()
{
	char tempBase[MAX_PATH] = { 0 };
	DWORD n = GetTempPathA( MAX_PATH, tempBase );
	if ( n == 0 || n >= MAX_PATH )
		return {};

	std::string dir = std::string( tempBase ) + "df_" + MakeRandomTag();
	if ( !CreateDirectoryA( dir.c_str(), nullptr ) )
	{
		DWORD err = GetLastError();
		if ( err != ERROR_ALREADY_EXISTS )
			return {};
	}
	return dir;
}

// Безопасное удаление файла: затереть содержимое нулями, потом DeleteFile.
// Кит, если файл размер > 64 МБ, делаем только delete (overwrite не успеет).
static void SecureDeleteFile( const std::string& path )
{
	HANDLE h = CreateFileA( path.c_str(), GENERIC_WRITE,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr );
	if ( h != INVALID_HANDLE_VALUE )
	{
		LARGE_INTEGER sz{};
		if ( GetFileSizeEx( h, &sz ) && sz.QuadPart > 0 && sz.QuadPart <= ( 64LL << 20 ) )
		{
			std::vector<char> zero( 64 * 1024, 0 );
			LARGE_INTEGER zero_offset{};
			SetFilePointerEx( h, zero_offset, nullptr, FILE_BEGIN );
			LONGLONG remaining = sz.QuadPart;
			DWORD written = 0;
			while ( remaining > 0 )
			{
				DWORD chunk = ( remaining > (LONGLONG)zero.size() ) ? (DWORD)zero.size() : (DWORD)remaining;
				if ( !WriteFile( h, zero.data(), chunk, &written, nullptr ) ) break;
				remaining -= written;
			}
			FlushFileBuffers( h );
		}
		CloseHandle( h );
	}
	DeleteFileA( path.c_str() );
}

// Извлечь spoofer.sys + kdu.exe из embedded resources в staging.
// Возвращает true если оба распакованы.
static bool StageEmbeddedSpoofPayload( const std::string& stagingDir,
                                       std::string& outSpooferSys,
                                       std::string& outKduExe )
{
	outSpooferSys = stagingDir + "\\spoofer.sys";
	outKduExe    = stagingDir + "\\kdu.exe";

	// Resource names согласованы со Stream B (resources.rc). Если Stream B
	// ещё не приземлил resource bake — оба вызова вернут false.
	const char* RES_SPOOFER_SYS = "SPOOFER_SYS";
	const char* RES_KDU_EXE     = "KDU_EXE";

	bool okSys = payload_loader::ExtractEmbeddedPayload( RES_SPOOFER_SYS, outSpooferSys );
	bool okKdu = payload_loader::ExtractEmbeddedPayload( RES_KDU_EXE,    outKduExe );

	if ( !okSys || !okKdu )
	{
		// Cleanup partials
		if ( okSys ) SecureDeleteFile( outSpooferSys );
		if ( okKdu ) SecureDeleteFile( outKduExe );
		return false;
	}
	return true;
}

// In-process spoof: распаковываем kdu+spoofer в TEMP, запускаем kdu, ждём,
// удаляем файлы, RemoveDirectory.
// Command line: kdu.exe spoof --seed <seed> --no-mac -y --driver <sys> --kdu <kdu>
// `--no-mac` критично на VDS (см. MEMORY.md karos_vds_ban_bypass_solved).
static bool RunInProcessSpoof( const std::string& seed, int timeoutMs, bool verifyOnly )
{
	std::string staging = MakeTempStagingDir();
	if ( staging.empty() )
		return false;

	std::string spooferSys, kduExe;
	if ( !StageEmbeddedSpoofPayload( staging, spooferSys, kduExe ) )
	{
		RemoveDirectoryA( staging.c_str() );
		return false;
	}

	// Собираем cmdline. kdu сам валидирует seed; verify mode = другой
	// подкоманда. Если потом окажется что kdu не умеет verify — fallback
	// на дополнительный спуф с тем же seed (idempotent при правильном KDU).
	const char* sub = verifyOnly ? "verify" : "spoof";
	char cmd[1024];
	snprintf( cmd, sizeof( cmd ),
		"\"%s\" %s --seed \"%s\" --no-mac -y --driver \"%s\" --kdu \"%s\"",
		kduExe.c_str(), sub, seed.c_str(),
		spooferSys.c_str(), kduExe.c_str() );

	STARTUPINFOA si{};
	si.cb = sizeof( si );
	si.dwFlags = STARTF_USESHOWWINDOW;
	si.wShowWindow = SW_HIDE;
	PROCESS_INFORMATION pi{};

	std::string cmdCopy = cmd;
	BOOL launched = CreateProcessA(
		nullptr,
		const_cast<char*>( cmdCopy.c_str() ),
		nullptr, nullptr, FALSE,
		CREATE_NO_WINDOW,
		nullptr, staging.c_str(),
		&si, &pi );

	bool result = false;
	if ( launched )
	{
		DWORD waitRes = WaitForSingleObject( pi.hProcess, timeoutMs );
		DWORD exitCode = (DWORD)-1;
		if ( waitRes == WAIT_TIMEOUT )
		{
			TerminateProcess( pi.hProcess, 1 );
			WaitForSingleObject( pi.hProcess, 2000 );
		}
		else
		{
			GetExitCodeProcess( pi.hProcess, &exitCode );
		}
		CloseHandle( pi.hProcess );
		CloseHandle( pi.hThread );
		result = ( waitRes == WAIT_OBJECT_0 && exitCode == 0 );
	}

	SecureDeleteFile( spooferSys );
	SecureDeleteFile( kduExe );
	RemoveDirectoryA( staging.c_str() );

	// Zero out cmdline buffer на случай если cmdCopy остался в RTL stash.
	SecureZeroMemory( const_cast<char*>( cmdCopy.data() ), cmdCopy.size() );

	return result;
}

#ifdef LEGACY_SPOOFER_PATH
// Старый flow через dist/HwidSpoofer.exe оставлен под флагом для отката,
// если Stream B resource bake не успел. По умолчанию НЕ компилируется.
static bool RunSpooferCmdLegacy( const std::string& spooferExe,
                                  const std::string& subcommand,
                                  const std::string& seed,
                                  int timeoutMs )
{
	if ( spooferExe.empty() ) return false;
	if ( GetFileAttributesA( spooferExe.c_str() ) == INVALID_FILE_ATTRIBUTES )
		return false;

	std::string cmd = "\"" + spooferExe + "\" " + subcommand +
		" --seed \"" + seed + "\"";
	if ( subcommand == "spoof" )
		cmd += " --yes";

	STARTUPINFOA si{};
	si.cb = sizeof( si );
	si.dwFlags = STARTF_USESHOWWINDOW;
	si.wShowWindow = SW_HIDE;
	PROCESS_INFORMATION pi{};

	std::string cmdCopy = cmd;
	BOOL ok = CreateProcessA(
		nullptr,
		const_cast<char*>( cmdCopy.c_str() ),
		nullptr, nullptr, FALSE,
		CREATE_NO_WINDOW,
		nullptr, nullptr,
		&si, &pi );

	if ( !ok ) return false;

	DWORD waitRes = WaitForSingleObject( pi.hProcess, timeoutMs );
	DWORD exitCode = (DWORD)-1;
	if ( waitRes == WAIT_TIMEOUT )
	{
		TerminateProcess( pi.hProcess, 1 );
		WaitForSingleObject( pi.hProcess, 2000 );
	}
	else
	{
		GetExitCodeProcess( pi.hProcess, &exitCode );
	}
	CloseHandle( pi.hProcess );
	CloseHandle( pi.hThread );
	return ( waitRes == WAIT_OBJECT_0 && exitCode == 0 );
}
#endif // LEGACY_SPOOFER_PATH

bool RunSpoof( const std::string& spooferExe, const std::string& seed, int timeoutMs )
{
#ifdef LEGACY_SPOOFER_PATH
	if ( !spooferExe.empty() )
		return RunSpooferCmdLegacy( spooferExe, "spoof", seed, timeoutMs );
#else
	(void)spooferExe; // unused в new flow — kdu+sys приходят из embedded
#endif
	return RunInProcessSpoof( seed, timeoutMs, /*verifyOnly=*/false );
}

bool VerifySpoof( const std::string& spooferExe, const std::string& seed, int timeoutMs )
{
#ifdef LEGACY_SPOOFER_PATH
	if ( !spooferExe.empty() )
		return RunSpooferCmdLegacy( spooferExe, "verify", seed, timeoutMs );
#else
	(void)spooferExe;
#endif
	return RunInProcessSpoof( seed, timeoutMs, /*verifyOnly=*/true );
}

} // namespace hwid_spoof
