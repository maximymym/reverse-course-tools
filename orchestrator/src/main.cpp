#include "orchestrator.h"
#include "gui.h"
#include "scripts_sync.h"
#include "hero_portraits.h"

#include <Windows.h>
#include <string>

int WINAPI WinMain( HINSTANCE, HINSTANCE, LPSTR, int )
{
	// ── EXE directory ──
	char exePath[MAX_PATH];
	GetModuleFileNameA( nullptr, exePath, MAX_PATH );

	std::string exeDir( exePath );
	auto lastSlash = exeDir.find_last_of( "\\/" );
	if ( lastSlash != std::string::npos )
		exeDir = exeDir.substr( 0, lastSlash );

	gui::g_exeDir = exeDir;

	// --autostart: запустить ферму сразу после Init(), без ручного клика по
	// START FARM в GUI. Используется на dotahost для итер-loop'ов (фикс
	// чит-логики на удалёнке без сидения в RustDesk).
	const bool autoStart = strstr( GetCommandLineA(), "--autostart" ) != nullptr;

	// ── Sync Lua scripts installDir\scripts → C:\temp\andromeda\scripts ──
	// DLL читает скрипты из hardcoded C:\temp\andromeda\. После update юзера
	// спрашиваем сохранить ли правки или overwrite (см. scripts_sync.h).
	{
		std::string srcScripts = exeDir + "\\scripts";
		scripts_sync::SyncOnStartup( srcScripts, "C:\\temp\\andromeda\\scripts" );
	}

	// ── Sync game data installDir\data → C:\temp\andromeda\data ──
	// 2026-05-23: CGameDataDB::Init читает items.json/npc_*.json из этого пути.
	// Без этого recipe expansion (Meteor Hammer -> components) фейлит,
	// боты бесконечно спамят PURCHASE_ITEM на recipe items без эффекта.
	// Файлы только read-only data, юзер их не редактирует — простой overwrite.
	{
		std::string srcData = exeDir + "\\data";
		std::string dstData = "C:\\temp\\andromeda\\data";
		if ( GetFileAttributesA( srcData.c_str() ) != INVALID_FILE_ATTRIBUTES )
		{
			CreateDirectoryA( "C:\\temp\\andromeda", nullptr );
			CreateDirectoryA( dstData.c_str(), nullptr );
			char cmd[1024];
			snprintf( cmd, sizeof( cmd ),
				"cmd.exe /c xcopy \"%s\\*\" \"%s\\\" /Y /Q >nul 2>&1",
				srcData.c_str(), dstData.c_str() );
			STARTUPINFOA si{}; si.cb = sizeof( si ); si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
			PROCESS_INFORMATION pi{};
			if ( CreateProcessA( nullptr, cmd, nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
				nullptr, nullptr, &si, &pi ) )
			{
				WaitForSingleObject( pi.hProcess, 30000 );
				CloseHandle( pi.hProcess );
				CloseHandle( pi.hThread );
			}
		}
	}

	// ── Ensure config dir + defaults exist (first run after bootstrap) ──
	std::string configDir = exeDir + "\\config";
	CreateDirectoryA( configDir.c_str(), nullptr );

	// Create default farm.json if missing
	{
		std::string farmPath = configDir + "\\farm.json";
		if ( GetFileAttributesA( farmPath.c_str() ) == INVALID_FILE_ATTRIBUTES )
		{
			FILE* f = fopen( farmPath.c_str(), "w" );
			if ( f )
			{
				fputs( "{\n"
					"    \"heroes\": [\n"
					"        \"npc_dota_hero_skeleton_king\",\n"
					"        \"npc_dota_hero_sniper\",\n"
					"        \"npc_dota_hero_drow_ranger\",\n"
					"        \"npc_dota_hero_viper\",\n"
					"        \"npc_dota_hero_bristleback\"\n"
					"    ],\n"
					"    \"region\": 128,\n"
					"    \"game_mode\": 2,\n"
					"    \"dll_path\": \"Andromeda-Dota2-Base.dll\",\n"
					"    \"handle_exe\": \"handle64.exe\",\n"
					"    \"profiles_dir\": \"C:\\\\BotProfiles\"\n"
					"}\n", f );
				fclose( f );
			}
		}
	}

	// Fallback: check parent dir (dev layout)
	if ( GetFileAttributesA( ( configDir + "\\accounts.json" ).c_str() ) == INVALID_FILE_ATTRIBUTES )
	{
		std::string altDir = exeDir + "\\..\\config";
		if ( GetFileAttributesA( ( altDir + "\\accounts.json" ).c_str() ) != INVALID_FILE_ATTRIBUTES )
			configDir = altDir;
	}

	// ── Init Orchestrator (load config — empty accounts is OK, GUI handles it) ──
	Orchestrator orch;
	if ( GetFileAttributesA( ( configDir + "\\accounts.json" ).c_str() ) != INVALID_FILE_ATTRIBUTES )
		orch.Init( configDir, exeDir );
	else
	{
		// Запускаем kernel proxy redirect сразу — ещё до setup wizard'а, чтобы
		// Steam запущенный из "Open Steam" уже был под NAT'ом. Иначе первый
		// login утечёт реальный IP до того как accounts.json появится.
		orch.EarlyStartProxy( exeDir );
	}

	// --autostart: триггерим ферму ДО gui::Init чтобы не зависело от DX11/UI
	// готовности. StartFarmThread детачится, GUI всё равно поднимется поверх
	// running setup'а.
	{
		char dbg[512];
		const char* cl = GetCommandLineA();
		snprintf( dbg, sizeof( dbg ), "[autostart] flag=%d initialized=%d cmdline=%s",
			(int)autoStart, (int)orch.IsInitialized(), cl ? cl : "<null>" );
		orch.LogPublic( dbg );
	}
	if ( autoStart && orch.IsInitialized() )
		orch.StartFarm();

	// ── Init GUI ──
	if ( !gui::Init( 700, 520 ) )
	{
		MessageBoxA( nullptr, "Failed to initialize DirectX 11.", "Error", MB_OK | MB_ICONERROR );
		return 1;
	}

	// ── Hero portrait cache (lazy PNG → DX11 SRV) ──
	// Bind to the swap-chain device so SRVs share the same context that ImGui
	// is sampling from. Assets dir is shipped next to the EXE.
	hero_portraits::Init( gui::g_pd3dDevice, exeDir + "\\assets\\heroes" );

	// ── Main Loop (GUI handles login screen → dashboard) ──
	gui::Run( orch );

	// ── Cleanup ──
	hero_portraits::Shutdown();  // release SRVs before device is freed
	gui::Shutdown();
	return 0;
}
