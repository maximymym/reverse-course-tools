#pragma once

// StrategyWriter — атомарная запись C:\temp\andromeda\strategy.json.
//
// Формат: { "<pid>": "WIN|LOSE|DEBOOST", ... }
//
// Single-machine 5v5 self-play: master и slave orchestrator'ы пишут в один файл,
// каждый держит записи только для СВОИХ pids. Чтобы они не затирали друг друга,
// Write() делает RMW под LockFileEx(EXCLUSIVE) — читает текущий JSON, сохраняет
// чужие записи, заменяет/добавляет свои и пишет обратно атомарно (`*.tmp` →
// MoveFileExA). Lua-боты читают файл lock-free; благодаря MoveFileExA reader
// никогда не видит partial JSON.
//
// Backward-compat: дублируется plain-text strategy.txt с одним словом — это
// глобальная стратегия (берётся с любой записи в JSON, либо из последнего
// Write() call'а). Поллер в Lua сначала пробует strategy.json (per-pid match);
// если файла нет либо нет записи под наш pid — fallback на strategy.txt.

#include <string>
#include <vector>
#include <windows.h>

namespace StrategyWriter
{

// Атомарно записать strategy для перечисленных pids в strategy.json и обновить
// strategy.txt (back-compat). pids — pids ботов ЭТОГО orchestrator'а; чужие
// записи в файле сохраняются. strategy: ровно один из "WIN" | "LOSE" | "DEBOOST".
// Возвращает true при успехе обоих файлов (json + txt).
bool Write( const std::vector<DWORD>& pids, const std::string& strategy );

// Backward-compat overload: пишет ТОЛЬКО strategy.txt (старое поведение,
// без per-pid дедуп). Используется когда orchestrator не передаёт список pids.
bool Write( const std::string& strategy );

// Прочитать strategy.txt (если есть). Используется для GUI status.
std::string Read();

} // namespace StrategyWriter
