#pragma once

#include <string>

namespace bot_dota_dir
{

// Обнаружить Dota install dir по steamExe (parse libraryfolders.vdf +
// appmanifest_570.acf). Возвращает путь к "dota 2 beta" корню, например
// "D:\SteamLibrary\steamapps\common\dota 2 beta". Пустая строка = не найдено.
std::string FindDotaInstallDir( const std::string& steamExe );

// Создаёт BotDota\<idx>\ рядом с dotaInstallDir (тот же том — обязательно для
// hardlink dota2.exe). Hardlink dota2.exe + junction'ы для game/, core/, bin/
// и прочих критичных директорий. Cross-volume: fallback copy на C:\BotDota\N\
// (небольшой — один exe ~15 MB).
//
// Возвращает путь к BotDota\<idx>\dota2.exe, готовый для CreateProcess.
// Пустая строка = setup failed.
std::string EnsureBotDotaDir( int idx, const std::string& dotaInstallDir );

// Корень BotDota\<idx>\ (same-volume с Dota install или на C:\ при fallback).
std::string GetBotDotaDir( int idx, const std::string& dotaInstallDir );

} // namespace bot_dota_dir
