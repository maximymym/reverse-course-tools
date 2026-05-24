#pragma once

// RoleRotation — strict alternating WIN ↔ LOSE между матчами.
// Источник правды: C:\temp\andromeda\last_role.json.
// Атомарная запись через MoveFileExA (cross-process safe).

#include <cstdint>
#include <deque>
#include <string>

struct MatchRecord
{
	uint64_t    matchId = 0;
	std::string strategy;   // "WIN" | "LOSE" | "DEBOOST"
	bool        weWon = false;
	int64_t     ts = 0;     // epoch ms
};

struct LastRole
{
	std::string strategy;          // последний сыгранный strategy
	uint64_t    lastMatchId = 0;
	bool        weWon = false;
	int64_t     ts = 0;
	std::deque<MatchRecord> history;  // последние 5 матчей (новые в конце)
};

class RoleRotation
{
public:
	// Загрузить состояние из JSON-файла. Не существует / parse fail → cold start.
	bool Load( const std::string& path );

	// Атомарная запись через MoveFileExA.
	bool SaveAtomic( const std::string& path );

	// Strict alternation:
	//   last == "WIN"     → "LOSE"
	//   last == "LOSE"    → "WIN"
	//   last == "DEBOOST" → "WIN"   (DEBOOST вне rotation, после него начинаем с WIN)
	//   пусто (cold)      → "WIN"
	std::string NextStrategy() const;

	// Записать результат прошедшего матча (вызывается из orchestrator после
	// detection POST_GAME). После этого NextStrategy() инвертируется.
	void RecordMatch( uint64_t matchId, const std::string& strategy, bool weWon );

	// Like RecordMatch, но если последняя запись истории имеет тот же matchId —
	// перезаписывает её (single source of truth: мастерский match_result от
	// peer'а перезатирает локальный verdict slave'а, чтобы обе стороны имели
	// идентичную rotation). Idempotent.
	void UpsertMatch( uint64_t matchId, const std::string& strategy, bool weWon );

	// Last strategy (last entry в history либо m_data.strategy если history пусто).
	const std::string& LastStrategy() const { return m_data.strategy; }

	const std::deque<MatchRecord>& History() const { return m_data.history; }
	const LastRole& Data() const { return m_data; }

	// Сбросить в cold-start (для GUI "Reset rotation").
	void Reset();

private:
	LastRole m_data;
};
