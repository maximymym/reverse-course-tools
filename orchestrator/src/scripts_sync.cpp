#include "scripts_sync.h"

#include <Windows.h>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <ctime>

namespace fs = std::filesystem;

namespace scripts_sync
{

// Valve-legacy файлы/директории, которые ЛОМАЮТ наш Lua namespace при match
// start (conflict в GetBotNames / bot_generic.lua и т.п.). Даже если
// remove_all полностью отработал — оставляем явный sweep как backstop, чтобы
// старые install'ы (где директория частично заблокирована антивирусом, Dota
// держит хендл и т.п.) гарантированно очистились.
//
// 2026-05-04 incident: при upgrade с 2026.05.04.rebuild → minify-portable юзер
// получил мерж: новый item_build.lua + старые ability_item_usage_generic.lua
// (230 KB Valve generic) + FretBots.lua + Buff/ + --mode_item_generic.lua
// → ItemPurchaseThink overrides конфликтовали, герои перестали покупать,
// RAM x2 от двойной логики. Добавлены в blacklist чтобы SweepValveLegacy их
// гарантированно сносил даже если RemoveAllChecked не отработал.
static const char* kValveLegacyNames[] = {
	"bot_generic.lua",
	"hero_selection.lua",
	"team_desires.lua",
	"ability_usage_generic.lua",
	"ability_item_usage_generic.lua",
	"item_purchase_generic.lua",
	"ward_manager_generic.lua",
	"mode_defend_tower_top_generic.lua",
	"mode_defend_tower_mid_generic.lua",
	"mode_defend_tower_bot_generic.lua",
	"mode_farm_generic.lua",
	"mode_laning_generic.lua",
	"mode_push_tower_top_generic.lua",
	"mode_push_tower_mid_generic.lua",
	"mode_push_tower_bot_generic.lua",
	"mode_roam_generic.lua",
	"mode_retreat_generic.lua",
	"mode_roshan_generic.lua",
	"mode_rune_generic.lua",
	"mode_ward_generic.lua",
	"mode_gank_generic.lua",
	"mode_defend_ally_generic.lua",
	"mode_assemble_generic.lua",
	"--mode_item_generic.lua",   // двойной дефис не матчится mode_*_generic glob
	"FretBots.lua",              // Steam Workshop FretBots mod (стороннее)
	"BotLib",
	"Buff",                      // FretBots companion
	"Customize",
	"FretBots",
	"FunLib",
	"ts_libs",
	"Install-to-vscript",
};

// Явно подчищаем конкретные Valve-legacy записи + любые mode_*_generic.lua.
// Вызываем ПОСЛЕ remove_all — если тот частично отработал (locked file), этот
// sweep добьёт оставшиеся артефакты.
static void SweepValveLegacy( const std::string& dstBots )
{
	std::error_code ec;
	for ( const char* name : kValveLegacyNames )
	{
		fs::path p = fs::path( dstBots ) / name;
		if ( fs::exists( p, ec ) )
			fs::remove_all( p, ec );
	}
	// Glob mode_*_generic.lua на случай если Valve добавит новый режим —
	// наш хардкод list не покроет, а префикс стабилен.
	if ( fs::exists( dstBots, ec ) )
	{
		for ( auto& entry : fs::directory_iterator( dstBots, ec ) )
		{
			if ( ec ) break;
			if ( !entry.is_regular_file() ) continue;
			std::string fname = entry.path().filename().string();
			if ( fname.rfind( "mode_", 0 ) == 0 &&
			     fname.find( "_generic.lua" ) != std::string::npos )
			{
				std::error_code rec;
				fs::remove( entry.path(), rec );
			}
		}
	}
}

// Возвращает true если remove_all отработал чисто. Если нет — показывает
// warning и возвращает false (caller решает что делать).
static bool RemoveAllChecked( const std::string& dstBots )
{
	std::error_code ec;
	fs::remove_all( dstBots, ec );
	if ( ec )
	{
		char msg[1024];
		snprintf( msg, sizeof( msg ),
			"Could not fully wipe old bot scripts at:\n  %s\n\n"
			"Error: %s\n\n"
			"This usually means Dota 2 or Steam is running and holding a file open.\n"
			"Close Dota and Steam, then click OK to continue.\n"
			"(Update will proceed but old Valve legacy files may remain — possibly "
			"causing crashes at match start.)",
			dstBots.c_str(), ec.message().c_str() );
		MessageBoxA( nullptr, msg, "DotaFarm — scripts wipe warning",
			MB_OK | MB_ICONWARNING );
		return false;
	}
	return true;
}

static std::string ReadVersionFile( const std::string& path )
{
	std::ifstream f( path );
	if ( !f.is_open() ) return "";
	std::string line;
	std::getline( f, line );
	// strip CR/LF/whitespace
	while ( !line.empty() && ( line.back() == '\r' || line.back() == '\n' || line.back() == ' ' || line.back() == '\t' ) )
		line.pop_back();
	return line;
}

static bool WriteVersionFile( const std::string& path, const std::string& ver )
{
	std::ofstream f( path );
	if ( !f.is_open() ) return false;
	f << ver;
	return true;
}

// Рекурсивно копирует srcDir в dstDir, перезаписывая existing. Возвращает
// количество скопированных файлов, или -1 при ошибке.
static int CopyRecursive( const std::string& srcDir, const std::string& dstDir )
{
	std::error_code ec;
	if ( !fs::exists( srcDir, ec ) ) return -1;

	fs::create_directories( dstDir, ec );
	if ( ec ) return -1;

	int count = 0;
	auto opts = fs::copy_options::recursive |
	            fs::copy_options::overwrite_existing |
	            fs::copy_options::copy_symlinks;
	try
	{
		for ( auto& entry : fs::recursive_directory_iterator( srcDir ) )
		{
			auto rel = fs::relative( entry.path(), srcDir, ec );
			if ( ec ) continue;
			auto dst = fs::path( dstDir ) / rel;
			if ( entry.is_directory() )
			{
				fs::create_directories( dst, ec );
			}
			else if ( entry.is_regular_file() )
			{
				fs::create_directories( dst.parent_path(), ec );
				fs::copy_file( entry.path(), dst, fs::copy_options::overwrite_existing, ec );
				if ( !ec ) count++;
			}
		}
	}
	catch ( ... )
	{
		return -1;
	}
	return count;
}

// Архивирует dstDir в zip рядом с dstRoot (через powershell Compress-Archive).
// Возвращает путь к созданному zip'у или пустую строку при ошибке.
static std::string BackupDir( const std::string& dstRoot, const std::string& installedVer )
{
	// Timestamp для уникальности (разные backup'ы не затирают друг друга).
	time_t t = time( nullptr );
	struct tm tm;
	localtime_s( &tm, &t );
	char stamp[32];
	snprintf( stamp, sizeof( stamp ), "%04d%02d%02d-%02d%02d%02d",
		tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
		tm.tm_hour, tm.tm_min, tm.tm_sec );

	std::string verTag = installedVer.empty() ? "unknown" : installedVer;
	// Заменить опасные символы
	for ( char& c : verTag )
		if ( c == '/' || c == '\\' || c == ':' || c == ' ' ) c = '-';

	std::string zipPath = dstRoot + "_backup_" + verTag + "_" + stamp + ".zip";

	char cmd[2048];
	snprintf( cmd, sizeof( cmd ),
		"powershell.exe -NoProfile -NonInteractive -Command "
		"\"Compress-Archive -Path '%s\\*' -DestinationPath '%s' -Force\"",
		dstRoot.c_str(), zipPath.c_str() );

	STARTUPINFOA si{}; si.cb = sizeof( si ); si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
	PROCESS_INFORMATION pi{};
	if ( !CreateProcessA( nullptr, cmd, nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
		nullptr, nullptr, &si, &pi ) )
		return "";
	WaitForSingleObject( pi.hProcess, 60000 );
	DWORD ec = 1;
	GetExitCodeProcess( pi.hProcess, &ec );
	CloseHandle( pi.hProcess );
	CloseHandle( pi.hThread );
	if ( ec != 0 ) return "";
	return zipPath;
}

bool SyncOnStartup( const std::string& srcRoot, const std::string& dstRoot )
{
	// srcRoot = installDir\scripts  (где распаковался zip)
	// dstRoot = C:\temp\andromeda\scripts  (где DLL читает скрипты)

	std::string srcBots    = srcRoot + "\\bots";
	std::string dstBots    = dstRoot + "\\bots";
	std::string srcVerFile = srcBots + "\\.dist_version";
	std::string dstVerFile = dstBots + "\\.installed_version";

	// Если в пакете нет scripts/bots — нечего синхронизировать.
	if ( GetFileAttributesA( srcBots.c_str() ) == INVALID_FILE_ATTRIBUTES )
		return true;

	std::string distVer      = ReadVersionFile( srcVerFile );
	std::string installedVer = ReadVersionFile( dstVerFile );
	bool haveDst = ( GetFileAttributesA( dstBots.c_str() ) != INVALID_FILE_ATTRIBUTES );

	// Вариант 1: первый запуск (нет andromeda\scripts\bots или нет .installed_version).
	// Copy без диалога — юзер ещё не мог ничего отредактировать.
	// ВАЖНО: remove_all ДО copy, иначе Valve legacy файлы (mode_*_generic.lua,
	// bot_generic.lua, hero_selection.lua) от предыдущих установок остаются в
	// dstBots и ломают наш Lua namespace → crash при match start в loading screen.
	if ( !haveDst || installedVer.empty() )
	{
		if ( haveDst )
			RemoveAllChecked( dstBots );  // clean slate (warning на failure)
		CreateDirectoryA( dstRoot.c_str(), nullptr );
		SweepValveLegacy( dstBots );
		int n = CopyRecursive( srcBots, dstBots );
		if ( n < 0 )
		{
			MessageBoxA( nullptr, "First-time scripts copy failed.\n"
				"Check permissions on C:\\temp\\andromeda\\.",
				"DotaFarm — sync error", MB_OK | MB_ICONERROR );
			return false;
		}
		SweepValveLegacy( dstBots );  // пост-copy sweep на случай legacy внутри src
		WriteVersionFile( dstVerFile, distVer );
		return true;
	}

	// Вариант 2: версии совпадают — update не нужен. НО: всё равно гоним
	// SweepValveLegacy на случай если предыдущая инсталляция оставила orphan
	// файлы которые теперь в blacklist (см. 2026-05-04 incident выше). Sweep
	// idempotent + быстрый (один проход по dstBots).
	if ( !distVer.empty() && distVer == installedVer )
	{
		SweepValveLegacy( dstBots );
		return true;
	}

	// Вариант 3: версии различаются → спрашиваем юзера.
	char msg[1024];
	snprintf( msg, sizeof( msg ),
		"Found a newer version of bot scripts.\n\n"
		"  Your installed version : %s\n"
		"  New version from update: %s\n\n"
		"YES     — Overwrite your edits with the new scripts\n"
		"NO       — Keep your edits (new fixes/features will be missing)\n"
		"CANCEL  — Backup your edits to a zip, then apply new scripts\n\n"
		"Scripts live in:\n  %s",
		installedVer.empty() ? "(unknown)" : installedVer.c_str(),
		distVer.empty()      ? "(unknown)" : distVer.c_str(),
		dstBots.c_str() );

	int choice = MessageBoxA( nullptr, msg,
		"DotaFarm — Bot scripts update",
		MB_YESNOCANCEL | MB_ICONQUESTION | MB_DEFBUTTON3 );

	if ( choice == IDYES )
	{
		// Overwrite без backup'а. remove_all ДО copy — иначе Valve legacy файлы
		// остаются и ломают Lua namespace при match start.
		RemoveAllChecked( dstBots );
		SweepValveLegacy( dstBots );
		int n = CopyRecursive( srcBots, dstBots );
		if ( n < 0 )
		{
			MessageBoxA( nullptr, "Copy failed.", "DotaFarm — sync error", MB_OK | MB_ICONERROR );
			return false;
		}
		SweepValveLegacy( dstBots );
		WriteVersionFile( dstVerFile, distVer );
		return true;
	}
	else if ( choice == IDCANCEL )
	{
		// Backup → затем clean + copy.
		std::string zip = BackupDir( dstBots, installedVer );
		if ( zip.empty() )
		{
			int r = MessageBoxA( nullptr,
				"Backup failed. Overwrite anyway?\n"
				"(YES = overwrite without backup, NO = abort)",
				"DotaFarm — backup error", MB_YESNO | MB_ICONWARNING );
			if ( r != IDYES ) return true;  // user aborted, keep edits
		}
		RemoveAllChecked( dstBots );
		SweepValveLegacy( dstBots );
		int n = CopyRecursive( srcBots, dstBots );
		if ( n < 0 )
		{
			MessageBoxA( nullptr, "Copy failed after backup.",
				"DotaFarm — sync error", MB_OK | MB_ICONERROR );
			return false;
		}
		SweepValveLegacy( dstBots );
		WriteVersionFile( dstVerFile, distVer );

		if ( !zip.empty() )
		{
			char info[512];
			snprintf( info, sizeof( info ),
				"Your edits were backed up to:\n%s\n\n"
				"New scripts are now active.",
				zip.c_str() );
			MessageBoxA( nullptr, info,
				"DotaFarm — backup saved", MB_OK | MB_ICONINFORMATION );
		}
		return true;
	}
	else
	{
		// IDNO — keep edits, но обновляем installed_version чтобы не спрашивать
		// каждый запуск. Юзер может вручную снять запись если захочет update позже.
		WriteVersionFile( dstVerFile, distVer );
		return true;
	}
}

} // namespace scripts_sync
