#pragma once

#include "config.h"
#include <Windows.h>
#include <string>

class SteamLauncher
{
public:
	// Launch Steam ONLY (без Dota). Dota запускается отдельно через LaunchDota()
	// после того как Steam client готов (IPC up). Это обязательно для tun2socks:
	// dota2.exe из BotDota\N\ даёт уникальный path, sing-box матчит per-account.
	DWORD LaunchInstance( int instanceIdx, const FarmConfig& cfg );

	// Launch dota2.exe из BotDota\<idx>\. Steam должен быть уже запущен (steamPid).
	// CreateProcess делается с PROC_THREAD_ATTRIBUTE_PARENT_PROCESS = steamPid, чтобы
	// VAC увидел dota2.exe как child of steam.exe (иначе anti-cheat ругается
	// "не может подтвердить надёжность устройства" и блокирует matchmaking).
	// Возвращает PID dota2. При steamPid=0 fallback на обычный spawn (без spoof).
	DWORD LaunchDota( int instanceIdx, const FarmConfig& cfg, DWORD steamPid = 0 );

	// Set registry AutoLoginUser for the given account
	static bool SetAutoLogin( int instanceIdx, const FarmConfig& cfg );

	// Build environment block with redirected AppData for isolation
	static std::string BuildEnvironmentBlock( int instanceIdx, const std::string& profilesDir );

	// Вариант с proxy — добавляет HTTP_PROXY/HTTPS_PROXY/ALL_PROXY в env.
	static std::string BuildEnvironmentBlock( int instanceIdx, const std::string& profilesDir,
		const std::string& proxy );

	// Get per-bot Steam exe path (checks C:\BotSteam\N\steam.exe, falls back to main)
	static std::pair<std::string, std::string> GetSteamExeFor( int instanceIdx, const FarmConfig& cfg );

	// Kill all steam.exe processes
	static void KillAllSteam();

	// Kill steam.exe (и связанные дочерние steamwebhelper.exe из того же tree)
	// для конкретной BotSteam инстанции. steamPid — ranking id корневого
	// steam.exe. Если 0 — fallback: убить все steam*.exe чьи image path лежат
	// в C:\BotSteam\<idx>\. После TerminateProcess ждём WaitForSingleObject
	// до 5s чтобы Windows успела освободить файловые хендлы (иначе следующий
	// LaunchInstance может упереться в "process is busy").
	static void KillSteamForInstance( int idx, DWORD steamPid );

	// Auto-detect real Steam install path (registry + common paths)
	static std::string DetectSteamExe();

	// Create per-bot Steam dir (C:\BotSteam\{idx}) с hardlink'ами на steam.exe
	// и junctions на директории. Hardlinks дают уникальный process path per-bot
	// для sing-box process_path_regex matching, но НЕ требуют admin (в отличие
	// от symlinks). Real config/ dir → per-bot loginusers.vdf.
	static bool EnsureBotSteamDir( int idx, const std::string& realSteamDir );

	// Build Dota launch args: `-steam -master_ipc_name_override steamN -novid ...`
	// Exposed для GUI "Launch Dota manually" debugging.
	static std::string BuildDotaLaunchArgs( int idx, const FarmConfig& cfg );

private:
	std::string BuildLaunchArgs( int idx, const FarmConfig& cfg );
};
