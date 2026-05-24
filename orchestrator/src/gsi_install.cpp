#include "gsi_install.h"
#include "dota_launcher.h"

#include <Windows.h>
#include <Psapi.h>

#include <fstream>
#include <iterator>
#include <cstdio>

namespace
{

// Снять путь до dota2.exe из запущенного процесса (если есть),
// отрезать "\game\bin\win64\dota2.exe" → корень установки.
std::string DotaPathFromRunningProcess()
{
	auto pids = DotaLauncher::FindDotaPids();
	for ( DWORD pid : pids )
	{
		HANDLE h = OpenProcess( PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid );
		if ( !h ) continue;

		char path[MAX_PATH] = {};
		DWORD sz = MAX_PATH;
		BOOL ok = QueryFullProcessImageNameA( h, 0, path, &sz );
		CloseHandle( h );
		if ( !ok || !path[0] ) continue;

		// path: ...\game\bin\win64\dota2.exe → strip 4 уровня
		std::string p( path );
		for ( int i = 0; i < 4; i++ )
		{
			auto pos = p.find_last_of( "\\/" );
			if ( pos == std::string::npos ) return {};
			p.resize( pos );
		}
		return p;
	}
	return {};
}

// Распарсить libraryfolders.vdf — найти library, в котором есть app 570.
std::string DotaPathFromLibraryFolders( const std::string& steamRoot )
{
	std::string vdf = steamRoot + "\\steamapps\\libraryfolders.vdf";
	std::ifstream f( vdf );
	if ( !f.is_open() ) return {};

	std::string content( ( std::istreambuf_iterator<char>( f ) ),
		std::istreambuf_iterator<char>() );

	// Грубо ищем "570" (id Доты), от него вверх — ближайшее "path".
	auto appPos = content.find( "\"570\"" );
	if ( appPos == std::string::npos ) return {};

	auto pathPos = content.rfind( "\"path\"", appPos );
	if ( pathPos == std::string::npos ) return {};

	auto q1 = content.find( '"', pathPos + 6 );
	if ( q1 == std::string::npos ) return {};
	auto q2 = content.find( '"', q1 + 1 );
	if ( q2 == std::string::npos ) return {};

	// VDF escape: "\\" → "\"
	std::string raw = content.substr( q1 + 1, q2 - q1 - 1 );
	std::string clean;
	clean.reserve( raw.size() );
	for ( size_t i = 0; i < raw.size(); i++ )
	{
		if ( raw[i] == '\\' && i + 1 < raw.size() && raw[i + 1] == '\\' )
		{
			clean.push_back( '\\' );
			i++;
		}
		else clean.push_back( raw[i] );
	}
	return clean + "\\steamapps\\common\\dota 2 beta";
}

bool DirExists( const std::string& p )
{
	DWORD a = GetFileAttributesA( p.c_str() );
	return a != INVALID_FILE_ATTRIBUTES && ( a & FILE_ATTRIBUTE_DIRECTORY );
}

} // anon

namespace gsi_install
{

std::string FindDotaInstall( const std::string& steamExe )
{
	// 1) Запущенный dota2.exe — самый надёжный источник
	{
		auto p = DotaPathFromRunningProcess();
		if ( !p.empty() && DirExists( p + "\\game\\dota" ) )
			return p;
	}

	// 2) libraryfolders.vdf от Steam
	if ( !steamExe.empty() )
	{
		std::string steamDir = steamExe;
		auto sl = steamDir.find_last_of( "\\/" );
		if ( sl != std::string::npos ) steamDir.resize( sl );

		auto p = DotaPathFromLibraryFolders( steamDir );
		if ( !p.empty() && DirExists( p + "\\game\\dota" ) )
			return p;
	}

	// 3) Жёсткие общие пути
	const char* tries[] = {
		"C:\\Program Files (x86)\\Steam\\steamapps\\common\\dota 2 beta",
		"D:\\SteamLibrary\\steamapps\\common\\dota 2 beta",
		"E:\\SteamLibrary\\steamapps\\common\\dota 2 beta",
		"F:\\SteamLibrary\\steamapps\\common\\dota 2 beta",
		"D:\\Steam\\steamapps\\common\\dota 2 beta",
	};
	for ( auto t : tries )
	{
		if ( DirExists( std::string( t ) + "\\game\\dota" ) )
			return t;
	}
	return {};
}

bool InstallGsiConfig(
	const std::string& dotaInstallPath,
	unsigned short     port,
	const std::string& token,
	std::string*       outPath,
	std::string*       outError )
{
	if ( dotaInstallPath.empty() )
	{
		if ( outError ) *outError = "dota install path is empty";
		return false;
	}

	std::string cfgDir = dotaInstallPath + "\\game\\dota\\cfg\\gamestate_integration";

	// Создаём вложенно: на случай если родительские отсутствуют
	{
		std::string p1 = dotaInstallPath + "\\game";
		std::string p2 = p1 + "\\dota";
		std::string p3 = p2 + "\\cfg";
		CreateDirectoryA( p1.c_str(), nullptr );
		CreateDirectoryA( p2.c_str(), nullptr );
		CreateDirectoryA( p3.c_str(), nullptr );
		CreateDirectoryA( cfgDir.c_str(), nullptr );
	}

	if ( !DirExists( cfgDir ) )
	{
		if ( outError ) *outError = "failed to create cfg dir: " + cfgDir;
		return false;
	}

	std::string cfgPath = cfgDir + "\\gamestate_integration_andromeda.cfg";

	// VDF-формат. throttle 0.1s, heartbeat 5s.
	// Шлём только то что реально нужно (events/wearables — лишние мегабайты).
	char body[2048];
	snprintf( body, sizeof( body ),
		"\"Andromeda Dota Farm\"\n"
		"{\n"
		"    \"uri\"        \"http://127.0.0.1:%u/\"\n"
		"    \"timeout\"    \"5.0\"\n"
		"    \"buffer\"     \"0.1\"\n"
		"    \"throttle\"   \"0.1\"\n"
		"    \"heartbeat\"  \"5.0\"\n"
		"    \"data\"\n"
		"    {\n"
		"        \"provider\"      \"1\"\n"
		"        \"map\"           \"1\"\n"
		"        \"player\"        \"1\"\n"
		"        \"hero\"          \"1\"\n"
		"        \"abilities\"     \"1\"\n"
		"        \"items\"         \"1\"\n"
		"        \"draft\"         \"1\"\n"
		"        \"wearables\"     \"0\"\n"
		"        \"buildings\"     \"1\"\n"
		"        \"events\"        \"0\"\n"
		"    }\n"
		"    \"auth\"\n"
		"    {\n"
		"        \"token\"     \"%s\"\n"
		"    }\n"
		"}\n",
		(unsigned)port, token.c_str() );

	FILE* f = nullptr;
	fopen_s( &f, cfgPath.c_str(), "wb" );
	if ( !f )
	{
		if ( outError ) *outError = "fopen failed: " + cfgPath;
		return false;
	}
	fputs( body, f );
	fclose( f );

	if ( outPath ) *outPath = cfgPath;
	return true;
}

} // namespace gsi_install
