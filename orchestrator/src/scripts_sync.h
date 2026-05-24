#pragma once

#include <string>

// Синхронизация скриптов installDir → C:\temp\andromeda\scripts\ на старте.
//
// Зачем: DLL читает Lua из hardcoded C:\temp\andromeda\scripts\bots\, а update
// приходит в installDir\scripts\. Без sync'а юзер будет играть на СТАРЫХ
// скриптах после обновления.
//
// Режим: Keep-user-edits с диалогом.
//   - Нет .installed_version → первый запуск, copy + write version (без диалога).
//   - .installed_version == .dist_version → update не нужен.
//   - .installed_version != .dist_version → MessageBox:
//       Yes     = copy + update version (перезапись правок)
//       No      = пропустить, only update version marker (оставить правки)
//       Cancel  = backup + copy + update version
//
// Вызывать из WinMain ДО Orchestrator::Init.
namespace scripts_sync
{

// srcRoot = путь к папке где лежат installDir\scripts\bots\ (обычно g_exeDir + "\\scripts").
// dstRoot = "C:\\temp\\andromeda\\scripts".
// Возвращает true если sync успешен (или не нужен), false при ошибке копирования.
bool SyncOnStartup( const std::string& srcRoot, const std::string& dstRoot );

} // namespace scripts_sync
