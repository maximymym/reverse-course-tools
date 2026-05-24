#pragma once

// =====================================================================
// gsi_install — установка gamestate_integration_andromeda.cfg в Dota.
// Один cfg на всю инсталляцию доты, все 5 ботов шлют на один и тот же URL.
// Различение ботов — по player.steamid в JSON.
// =====================================================================

#include <string>

namespace gsi_install
{

// Найти install path Dota 2 (содержит подпапку game/dota/cfg/...).
// Стратегия: 1) запущенный dota2.exe, 2) libraryfolders.vdf, 3) common paths.
// Возвращает "" если не найдено.
std::string FindDotaInstall( const std::string& steamExe );

// Записать cfg-файл. Если файл уже существует — перезаписывает.
// outPath — куда положили (для лога), outError — текст ошибки.
bool InstallGsiConfig(
	const std::string& dotaInstallPath,
	unsigned short     port,
	const std::string& token,
	std::string*       outPath = nullptr,
	std::string*       outError = nullptr );

} // namespace gsi_install
