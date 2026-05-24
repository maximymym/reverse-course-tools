#pragma once

// =====================================================================
// GSI server — слушает HTTP POST от Dota 2 GameStateIntegration.
// Один thread, blocking accept, по соединению на запрос (короткие, ~10 KB).
// Все боты на машине шлют на один порт; различение по player.steamid.
// =====================================================================

#include <cstdint>
#include <string>
#include <unordered_map>
#include <mutex>
#include <atomic>
#include <thread>
#include <chrono>

struct GsiSnapshot
{
	// Когда получили последний пакет (steady_clock ms).
	int64_t lastUpdateMs = 0;

	// provider
	uint64_t    providerSteamId = 0; // обычно равен player.steamid в обычной игре
	int64_t     providerTimestamp = 0;

	// map
	uint64_t    matchId = 0;
	std::string gameState;           // "DOTA_GAMERULES_STATE_GAME_IN_PROGRESS" и т.п.
	bool        paused = false;
	int         radiantScore = 0;
	int         direScore = 0;
	float       gameTime = 0.f;
	float       clockTime = 0.f;

	// player (локальный)
	std::string playerName;
	int         team = 0;            // 2=radiant, 3=dire, 1=spectator
	int         kills = 0, deaths = 0, assists = 0;
	int         gold = 0;
	int         lastHits = 0, denies = 0;
	int         gpm = 0, xpm = 0;
	int         netWorth = 0;

	// hero
	std::string heroName;            // "npc_dota_hero_skeleton_king"
	int         heroLevel = 0;
	int         hp = 0, maxHp = 0;
	int         mana = 0, maxMana = 0;
	bool        alive = false;
	float       posX = 0, posY = 0;
	bool        hasBuyback = false;
	int         buybackCost = 0;

	// raw json (для дебага / последующих расширений)
	std::string rawJson;
};

class GsiServer
{
public:
	bool Start( unsigned short port = 3477 );
	void Stop();
	bool IsRunning() const { return m_running.load(); }

	// Получить snapshot по steamID (полная копия под локом).
	// Возвращает true если есть актуальные данные (любой возраст).
	bool GetSnapshot( uint64_t steamId, GsiSnapshot& out ) const;

	// Возраст последнего пакета в миллисекундах. -1 если steamId не виден ни разу.
	int64_t GetAgeMs( uint64_t steamId ) const;

	// Сколько уникальных steamID когда-либо слали данные.
	size_t GetSeenSteamIdCount() const;

	// Общее количество принятых HTTP запросов (для health-monitor).
	uint64_t GetTotalRequests() const { return m_totalRequests.load(); }

	// Последняя ошибка (для лога).
	std::string GetLastError() const;

	// Порт, на котором реально слушаем (0 если не запущен).
	unsigned short GetPort() const { return m_port; }

private:
	void ServerLoop();
	void HandleConnection( uintptr_t clientSock );

	// Распарсить HTTP POST body как GSI JSON и обновить m_snapshots.
	void IngestJson( const std::string& body );

	std::atomic<bool>           m_running{ false };
	std::atomic<bool>           m_stopRequested{ false };
	std::thread                 m_thread;
	uintptr_t                   m_listenSock = ~uintptr_t( 0 );
	unsigned short              m_port = 0;

	std::atomic<uint64_t>       m_totalRequests{ 0 };

	mutable std::mutex          m_mutex;
	std::unordered_map<uint64_t, GsiSnapshot> m_snapshots;
	std::string                 m_lastError;
};

GsiServer& GetGsiServer();
