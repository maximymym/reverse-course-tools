#include "bot_dota_dir.h"

#include <Windows.h>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <vector>

namespace bot_dota_dir
{

// Helper: same-volume check (identical to steam_launcher.cpp helper but local)
static bool SameVolume( const char* pathA, const char* pathB )
{
	char volA[MAX_PATH] = {};
	char volB[MAX_PATH] = {};
	if ( !GetVolumePathNameA( pathA, volA, MAX_PATH ) ) return false;
	if ( !GetVolumePathNameA( pathB, volB, MAX_PATH ) ) return false;
	return _stricmp( volA, volB ) == 0;
}

static bool HardLinkOrCopy( const char* dst, const char* src )
{
	if ( GetFileAttributesA( dst ) != INVALID_FILE_ATTRIBUTES )
		return true;

	if ( SameVolume( dst, src ) )
	{
		if ( CreateHardLinkA( dst, src, nullptr ) )
			return true;
	}
	return CopyFileA( src, dst, TRUE ) != FALSE;
}

static bool CreateJunction( const char* linkDir, const char* targetDir )
{
	char cmd[1024];
	snprintf( cmd, sizeof( cmd ), "cmd.exe /c mklink /J \"%s\" \"%s\"", linkDir, targetDir );

	STARTUPINFOA si{};
	si.cb = sizeof( si );
	si.dwFlags = STARTF_USESHOWWINDOW;
	si.wShowWindow = SW_HIDE;
	PROCESS_INFORMATION pi{};
	if ( !CreateProcessA( nullptr, cmd, nullptr, nullptr,
		FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi ) )
		return false;

	WaitForSingleObject( pi.hProcess, 10000 );
	DWORD exitCode = 1;
	GetExitCodeProcess( pi.hProcess, &exitCode );
	CloseHandle( pi.hProcess );
	CloseHandle( pi.hThread );
	return exitCode == 0;
}

// Parse libraryfolders.vdf — возвращает список корней SteamLibrary.
// Формат VDF:
//   "libraryfolders"
//   {
//     "0" { "path" "C:\\Steam" "apps" { "570" "123456" } }
//     "1" { "path" "D:\\SteamLibrary" ... }
//   }
static std::vector<std::string> ParseLibraryFolders( const std::string& steamDir )
{
	std::vector<std::string> roots;
	std::string path = steamDir + "\\steamapps\\libraryfolders.vdf";
	std::ifstream f( path );
	if ( !f.is_open() )
	{
		// fallback old location
		path = steamDir + "\\config\\libraryfolders.vdf";
		f.open( path );
		if ( !f.is_open() ) return roots;
	}

	std::string line;
	while ( std::getline( f, line ) )
	{
		// "path"		"D:\\SteamLibrary"
		size_t keyStart = line.find( "\"path\"" );
		if ( keyStart == std::string::npos ) continue;
		size_t valStart = line.find( '"', keyStart + 6 );
		if ( valStart == std::string::npos ) continue;
		size_t valEnd = line.find( '"', valStart + 1 );
		if ( valEnd == std::string::npos ) continue;

		std::string rootPath = line.substr( valStart + 1, valEnd - valStart - 1 );
		// VDF escapes: double-backslash represents single backslash.
		std::string unescaped;
		for ( size_t i = 0; i < rootPath.size(); ++i )
		{
			if ( rootPath[i] == '\\' && i + 1 < rootPath.size() && rootPath[i + 1] == '\\' )
			{
				unescaped.push_back( '\\' );
				++i;
			}
			else
			{
				unescaped.push_back( rootPath[i] );
			}
		}
		roots.push_back( unescaped );
	}
	return roots;
}

std::string FindDotaInstallDir( const std::string& steamExe )
{
	// steamExe = "C:\Program Files (x86)\Steam\steam.exe" или "C:\BotSteam\0\steam.exe".
	// Нам нужен REAL steam install path — BotSteam dir содержит junction steamapps
	// который ведёт в real Steam, но libraryfolders.vdf там может отсутствовать.
	std::string steamDir = steamExe;
	auto sl = steamDir.find_last_of( "\\/" );
	if ( sl != std::string::npos ) steamDir = steamDir.substr( 0, sl );

	// Collect candidate library roots
	std::vector<std::string> roots = ParseLibraryFolders( steamDir );
	roots.insert( roots.begin(), steamDir ); // main Steam dir — первый кандидат

	for ( const auto& root : roots )
	{
		std::string candidate = root + "\\steamapps\\common\\dota 2 beta";
		// Проверяем наличие dota2.exe внутри
		std::string exe = candidate + "\\game\\bin\\win64\\dota2.exe";
		if ( GetFileAttributesA( exe.c_str() ) != INVALID_FILE_ATTRIBUTES )
			return candidate;
	}

	// Last-resort common paths
	const char* fallback[] = {
		"D:\\SteamLibrary\\steamapps\\common\\dota 2 beta",
		"C:\\Program Files (x86)\\Steam\\steamapps\\common\\dota 2 beta",
		"E:\\SteamLibrary\\steamapps\\common\\dota 2 beta",
	};
	for ( auto c : fallback )
	{
		std::string exe = std::string( c ) + "\\game\\bin\\win64\\dota2.exe";
		if ( GetFileAttributesA( exe.c_str() ) != INVALID_FILE_ATTRIBUTES )
			return c;
	}

	return {};
}

std::string GetBotDotaDir( int idx, const std::string& dotaInstallDir )
{
	// Выбираем volume: тот же что у dota install (для hardlink efficiency).
	// Если detection fail — C:\BotDota\N\.
	char volume[MAX_PATH] = {};
	if ( !dotaInstallDir.empty() &&
		GetVolumePathNameA( dotaInstallDir.c_str(), volume, MAX_PATH ) )
	{
		char out[MAX_PATH];
		// volume уже имеет trailing '\' → "D:\\"
		snprintf( out, sizeof( out ), "%sBotDota\\%d", volume, idx );
		return out;
	}
	char out[MAX_PATH];
	snprintf( out, sizeof( out ), "C:\\BotDota\\%d", idx );
	return out;
}

std::string EnsureBotDotaDir( int idx, const std::string& dotaInstallDir )
{
	if ( dotaInstallDir.empty() )
		return {};

	std::string botDir = GetBotDotaDir( idx, dotaInstallDir );
	std::string botExe = botDir + "\\dota2.exe";

	// Already set up? Check exe exists и не reparse point.
	{
		DWORD attrs = GetFileAttributesA( botExe.c_str() );
		if ( attrs != INVALID_FILE_ATTRIBUTES )
		{
			if ( !( attrs & FILE_ATTRIBUTE_REPARSE_POINT ) )
				return botExe; // already real file / hardlink
			// Stale reparse point — rebuild
			DeleteFileA( botExe.c_str() );
		}
	}

	// Create base
	// BotDota корень на диске (volume)
	{
		char volume[MAX_PATH] = {};
		if ( GetVolumePathNameA( botDir.c_str(), volume, MAX_PATH ) )
		{
			std::string botRoot = std::string( volume ) + "BotDota";
			CreateDirectoryA( botRoot.c_str(), nullptr );
		}
	}
	CreateDirectoryA( botDir.c_str(), nullptr );

	// Mirror оригинальную Dota структуру (dota 2 beta/game/bin/win64/dota2.exe)
	// внутри botDir. Hardlink сам dota2.exe + dll'и из game/bin/win64, junction
	// для остальных директорий (game/dota, game/core, top-level core/bin если
	// есть). dota2.exe ищет свои assets по relative paths от своего location,
	// поэтому mirror layout обязателен — нельзя просто hardlink dota2.exe в
	// корень botDir. Process path становится botDir/game/bin/win64/dota2.exe,
	// что матчит sing-box regex BotDota/N/.*

	std::string srcDota2 = dotaInstallDir + "\\game\\bin\\win64\\dota2.exe";
	if ( GetFileAttributesA( srcDota2.c_str() ) == INVALID_FILE_ATTRIBUTES )
		return {};

	// Mirror directory structure до game/bin/win64 (real dirs, not junctions).
	std::string botGame = botDir + "\\game";
	std::string botGameBin = botGame + "\\bin";
	std::string botGameBinWin64 = botGameBin + "\\win64";
	CreateDirectoryA( botGame.c_str(), nullptr );
	CreateDirectoryA( botGameBin.c_str(), nullptr );
	CreateDirectoryA( botGameBinWin64.c_str(), nullptr );

	// Hardlink dota2.exe в нашу структуру
	std::string dstDota2 = botGameBinWin64 + "\\dota2.exe";
	// Clean stale
	{
		DWORD a = GetFileAttributesA( dstDota2.c_str() );
		if ( a != INVALID_FILE_ATTRIBUTES && ( a & FILE_ATTRIBUTE_REPARSE_POINT ) )
			DeleteFileA( dstDota2.c_str() );
	}
	if ( !HardLinkOrCopy( dstDota2.c_str(), srcDota2.c_str() ) )
		return {};

	// Hardlink все .dll и прочие файлы из game\bin\win64\ (кроме dota2.exe уже
	// обработанного). Эти dll'и (steamclient64.dll, tier0.dll и т.п.)
	// загружаются dota2 через LoadLibrary — они должны лежать рядом.
	{
		std::string search = dotaInstallDir + "\\game\\bin\\win64\\*";
		WIN32_FIND_DATAA fd;
		HANDLE hFind = FindFirstFileA( search.c_str(), &fd );
		if ( hFind != INVALID_HANDLE_VALUE )
		{
			do
			{
				if ( fd.cFileName[0] == '.' ) continue;
				if ( fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY ) continue;
				if ( _stricmp( fd.cFileName, "dota2.exe" ) == 0 ) continue;

				std::string src = dotaInstallDir + "\\game\\bin\\win64\\" + fd.cFileName;
				std::string dst = botGameBinWin64 + "\\" + fd.cFileName;
				// Clean stale reparse
				DWORD a = GetFileAttributesA( dst.c_str() );
				if ( a != INVALID_FILE_ATTRIBUTES && ( a & FILE_ATTRIBUTE_REPARSE_POINT ) )
					DeleteFileA( dst.c_str() );
				HardLinkOrCopy( dst.c_str(), src.c_str() );
			}
			while ( FindNextFileA( hFind, &fd ) );
			FindClose( hFind );
		}
	}

	// Junction для всех других top-level директорий в game\ (dota, core, hammer,
	// platform, etc.). Junction = одна команда на директорию, все содержимое
	// доступно прозрачно.
	{
		std::string search = dotaInstallDir + "\\game\\*";
		WIN32_FIND_DATAA fd;
		HANDLE hFind = FindFirstFileA( search.c_str(), &fd );
		if ( hFind != INVALID_HANDLE_VALUE )
		{
			do
			{
				if ( fd.cFileName[0] == '.' ) continue;
				if ( !( fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY ) ) continue;
				if ( _stricmp( fd.cFileName, "bin" ) == 0 ) continue; // уже сделали

				std::string src = dotaInstallDir + "\\game\\" + fd.cFileName;
				std::string dst = botGame + "\\" + fd.cFileName;
				if ( GetFileAttributesA( dst.c_str() ) != INVALID_FILE_ATTRIBUTES )
					continue;
				CreateJunction( dst.c_str(), src.c_str() );
			}
			while ( FindNextFileA( hFind, &fd ) );
			FindClose( hFind );
		}
	}

	// Junction на top-level game\ items которые являются файлами (gameinfo.gi и т.п.)
	// — эти файлы читает dota2 через fullpath от exe location. Делаем их hardlink.
	// Actually gameinfo.gi лежит в game\dota\gameinfo.gi — это уже под junction'ом.
	// На всякий случай hardlink top-level files из game\:
	{
		std::string search = dotaInstallDir + "\\game\\*";
		WIN32_FIND_DATAA fd;
		HANDLE hFind = FindFirstFileA( search.c_str(), &fd );
		if ( hFind != INVALID_HANDLE_VALUE )
		{
			do
			{
				if ( fd.cFileName[0] == '.' ) continue;
				if ( fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY ) continue;

				std::string src = dotaInstallDir + "\\game\\" + fd.cFileName;
				std::string dst = botGame + "\\" + fd.cFileName;
				HardLinkOrCopy( dst.c_str(), src.c_str() );
			}
			while ( FindNextFileA( hFind, &fd ) );
			FindClose( hFind );
		}
	}

	// Top-level directories в dotaInstallDir (core, bin, — обычно только game есть
	// на Source 2, но на всякий случай).
	{
		std::string search = dotaInstallDir + "\\*";
		WIN32_FIND_DATAA fd;
		HANDLE hFind = FindFirstFileA( search.c_str(), &fd );
		if ( hFind != INVALID_HANDLE_VALUE )
		{
			do
			{
				if ( fd.cFileName[0] == '.' ) continue;
				if ( !( fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY ) ) continue;
				if ( _stricmp( fd.cFileName, "game" ) == 0 ) continue; // уже сделали

				std::string src = dotaInstallDir + "\\" + fd.cFileName;
				std::string dst = botDir + "\\" + fd.cFileName;
				if ( GetFileAttributesA( dst.c_str() ) != INVALID_FILE_ATTRIBUTES )
					continue;
				CreateJunction( dst.c_str(), src.c_str() );
			}
			while ( FindNextFileA( hFind, &fd ) );
			FindClose( hFind );
		}
	}

	// Top-level files в dotaInstallDir (README, и т.п.) — hardlink
	{
		std::string search = dotaInstallDir + "\\*";
		WIN32_FIND_DATAA fd;
		HANDLE hFind = FindFirstFileA( search.c_str(), &fd );
		if ( hFind != INVALID_HANDLE_VALUE )
		{
			do
			{
				if ( fd.cFileName[0] == '.' ) continue;
				if ( fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY ) continue;

				std::string src = dotaInstallDir + "\\" + fd.cFileName;
				std::string dst = botDir + "\\" + fd.cFileName;
				HardLinkOrCopy( dst.c_str(), src.c_str() );
			}
			while ( FindNextFileA( hFind, &fd ) );
			FindClose( hFind );
		}
	}

	// Возвращаем реальный путь к dota2.exe в нашей mirror-structure
	return dstDota2;
}

} // namespace bot_dota_dir
