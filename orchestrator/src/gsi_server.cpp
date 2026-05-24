#include "gsi_server.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <Windows.h>

#include <json.hpp>

#include <vector>
#include <cstdio>
#include <cstring>

#pragma comment(lib, "ws2_32.lib")

using json = nlohmann::json;

static GsiServer g_GsiServer;
GsiServer& GetGsiServer() { return g_GsiServer; }

static int64_t NowMs()
{
	using namespace std::chrono;
	return duration_cast<milliseconds>( steady_clock::now().time_since_epoch() ).count();
}

bool GsiServer::Start( unsigned short port )
{
	if ( m_running.exchange( true ) )
		return true;

	WSADATA wsa{};
	if ( WSAStartup( MAKEWORD( 2, 2 ), &wsa ) != 0 )
	{
		m_running = false;
		std::lock_guard<std::mutex> lk( m_mutex );
		m_lastError = "WSAStartup failed";
		return false;
	}

	SOCKET sock = socket( AF_INET, SOCK_STREAM, IPPROTO_TCP );
	if ( sock == INVALID_SOCKET )
	{
		WSACleanup();
		m_running = false;
		std::lock_guard<std::mutex> lk( m_mutex );
		m_lastError = "socket() failed";
		return false;
	}

	BOOL reuse = TRUE;
	setsockopt( sock, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof( reuse ) );

	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl( INADDR_LOOPBACK ); // localhost-only — не светим наружу
	addr.sin_port = htons( port );

	if ( bind( sock, (sockaddr*)&addr, sizeof( addr ) ) == SOCKET_ERROR )
	{
		int err = WSAGetLastError();
		closesocket( sock );
		WSACleanup();
		m_running = false;
		char buf[128];
		snprintf( buf, sizeof( buf ), "bind() failed (port %u, WSA=%d)", (unsigned)port, err );
		std::lock_guard<std::mutex> lk( m_mutex );
		m_lastError = buf;
		return false;
	}

	if ( listen( sock, 16 ) == SOCKET_ERROR )
	{
		closesocket( sock );
		WSACleanup();
		m_running = false;
		std::lock_guard<std::mutex> lk( m_mutex );
		m_lastError = "listen() failed";
		return false;
	}

	m_listenSock = (uintptr_t)sock;
	m_port = port;
	m_stopRequested = false;
	m_thread = std::thread( &GsiServer::ServerLoop, this );
	return true;
}

void GsiServer::Stop()
{
	if ( !m_running.exchange( false ) )
		return;

	m_stopRequested = true;
	if ( m_listenSock != ~uintptr_t( 0 ) )
	{
		closesocket( (SOCKET)m_listenSock );
		m_listenSock = ~uintptr_t( 0 );
	}
	if ( m_thread.joinable() )
		m_thread.join();
	WSACleanup();
	m_port = 0;
}

void GsiServer::ServerLoop()
{
	while ( !m_stopRequested.load() )
	{
		sockaddr_in cli{};
		int cliLen = sizeof( cli );
		SOCKET client = accept( (SOCKET)m_listenSock, (sockaddr*)&cli, &cliLen );
		if ( client == INVALID_SOCKET )
		{
			if ( m_stopRequested.load() )
				break;
			Sleep( 50 );
			continue;
		}

		// Каждое соединение — новый detached thread. Дота шлёт ~10 пакетов/сек,
		// но по разным TCP сессиям (KeepAlive не используется), так что
		// держим хендлер коротким и не забываем закрыть socket.
		std::thread( [this, client]() { HandleConnection( (uintptr_t)client ); } ).detach();
	}
}

void GsiServer::HandleConnection( uintptr_t clientSock )
{
	SOCKET sock = (SOCKET)clientSock;

	// Read до конца заголовков, потом по Content-Length — body.
	// GSI пакеты обычно ~5–15 KB.
	std::string buf;
	buf.reserve( 16384 );

	char tmp[4096];
	int contentLength = -1;
	size_t headerEnd = std::string::npos;

	// Phase 1: тянем пока не увидим \r\n\r\n
	while ( true )
	{
		int n = recv( sock, tmp, sizeof( tmp ), 0 );
		if ( n <= 0 )
		{
			closesocket( sock );
			return;
		}
		buf.append( tmp, n );
		headerEnd = buf.find( "\r\n\r\n" );
		if ( headerEnd != std::string::npos )
			break;
		if ( buf.size() > 64 * 1024 )
		{
			// слишком длинный header — закрываемся
			closesocket( sock );
			return;
		}
	}

	// Извлечь Content-Length (case-insensitive поиск)
	{
		std::string headers = buf.substr( 0, headerEnd );
		const char* needle = "content-length:";
		std::string lc;
		lc.resize( headers.size() );
		for ( size_t i = 0; i < headers.size(); i++ )
			lc[i] = (char)tolower( (unsigned char)headers[i] );
		size_t p = lc.find( needle );
		if ( p != std::string::npos )
		{
			p += strlen( needle );
			while ( p < lc.size() && ( lc[p] == ' ' || lc[p] == '\t' ) ) p++;
			contentLength = atoi( lc.c_str() + p );
		}
	}

	if ( contentLength <= 0 || contentLength > 4 * 1024 * 1024 )
	{
		// без body или подозрительно большой — отвечаем 400 и выходим
		const char* resp = "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\n\r\n";
		send( sock, resp, (int)strlen( resp ), 0 );
		closesocket( sock );
		return;
	}

	// Phase 2: дочитать body до Content-Length
	size_t haveBody = buf.size() - ( headerEnd + 4 );
	while ( (int)haveBody < contentLength )
	{
		int n = recv( sock, tmp, sizeof( tmp ), 0 );
		if ( n <= 0 )
			break;
		buf.append( tmp, n );
		haveBody += n;
	}

	std::string body = buf.substr( headerEnd + 4, contentLength );

	// Ответ — GSI клиенту хватит пустого 200, тогда он не ретраит.
	const char* resp = "HTTP/1.1 200 OK\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
	send( sock, resp, (int)strlen( resp ), 0 );
	closesocket( sock );

	m_totalRequests.fetch_add( 1, std::memory_order_relaxed );

	IngestJson( body );
}

// ── helpers для безопасного чтения JSON ──
static int64_t ji( const json& j, const char* k, int64_t def = 0 )
{
	auto it = j.find( k );
	if ( it == j.end() || !it->is_number() ) return def;
	return it->get<int64_t>();
}

static uint64_t jui64( const json& j, const char* k, uint64_t def = 0 )
{
	auto it = j.find( k );
	if ( it == j.end() ) return def;
	if ( it->is_number() ) return it->get<uint64_t>();
	if ( it->is_string() )
	{
		try { return std::stoull( it->get<std::string>() ); } catch ( ... ) { return def; }
	}
	return def;
}

static std::string js( const json& j, const char* k, const std::string& def = {} )
{
	auto it = j.find( k );
	if ( it == j.end() || !it->is_string() ) return def;
	return it->get<std::string>();
}

static bool jb( const json& j, const char* k, bool def = false )
{
	auto it = j.find( k );
	if ( it == j.end() || !it->is_boolean() ) return def;
	return it->get<bool>();
}

static double jd( const json& j, const char* k, double def = 0.0 )
{
	auto it = j.find( k );
	if ( it == j.end() || !it->is_number() ) return def;
	return it->get<double>();
}

void GsiServer::IngestJson( const std::string& body )
{
	json j;
	try
	{
		j = json::parse( body );
	}
	catch ( ... )
	{
		return;
	}

	GsiSnapshot snap;
	snap.lastUpdateMs = NowMs();
	snap.rawJson = body;

	// provider — содержит steamid конкретного клиента, который шлёт
	if ( j.contains( "provider" ) && j["provider"].is_object() )
	{
		auto& p = j["provider"];
		snap.providerSteamId = jui64( p, "steamid" );
		snap.providerTimestamp = ji( p, "timestamp" );
	}

	// map
	if ( j.contains( "map" ) && j["map"].is_object() )
	{
		auto& m = j["map"];
		snap.matchId = jui64( m, "matchid" );
		snap.gameState = js( m, "game_state" );
		snap.paused = jb( m, "paused" );
		snap.radiantScore = (int)ji( m, "radiant_score" );
		snap.direScore = (int)ji( m, "dire_score" );
		snap.gameTime = (float)jd( m, "game_time" );
		snap.clockTime = (float)jd( m, "clock_time" );
	}

	// player — в обычной игре это объект с локальным игроком
	const json* playerObj = nullptr;
	if ( j.contains( "player" ) && j["player"].is_object() )
		playerObj = &j["player"];

	uint64_t playerSteamId = 0;
	if ( playerObj )
	{
		playerSteamId = jui64( *playerObj, "steamid" );
		snap.playerName = js( *playerObj, "name" );
		snap.team = (int)ji( *playerObj, "team_name_id", ji( *playerObj, "team", 0 ) );
		snap.kills = (int)ji( *playerObj, "kills" );
		snap.deaths = (int)ji( *playerObj, "deaths" );
		snap.assists = (int)ji( *playerObj, "assists" );
		snap.gold = (int)ji( *playerObj, "gold" );
		snap.lastHits = (int)ji( *playerObj, "last_hits" );
		snap.denies = (int)ji( *playerObj, "denies" );
		snap.gpm = (int)ji( *playerObj, "gpm" );
		snap.xpm = (int)ji( *playerObj, "xpm" );
		snap.netWorth = (int)ji( *playerObj, "net_worth" );

		// team_name строкой
		auto tn = js( *playerObj, "team_name" );
		if ( !tn.empty() )
		{
			if ( tn == "radiant" )    snap.team = 2;
			else if ( tn == "dire" )  snap.team = 3;
		}
	}

	// hero
	if ( j.contains( "hero" ) && j["hero"].is_object() )
	{
		auto& h = j["hero"];
		snap.heroName = js( h, "name" );
		snap.heroLevel = (int)ji( h, "level" );
		snap.hp = (int)ji( h, "health" );
		snap.maxHp = (int)ji( h, "max_health" );
		snap.mana = (int)ji( h, "mana" );
		snap.maxMana = (int)ji( h, "max_mana" );
		snap.alive = jb( h, "alive" );
		snap.posX = (float)jd( h, "xpos" );
		snap.posY = (float)jd( h, "ypos" );
		snap.hasBuyback = jb( h, "has_buyback" );
		snap.buybackCost = (int)ji( h, "buyback_cost" );
	}

	// Ключ для маппинга — в порядке предпочтения:
	//   1) player.steamid     (наиболее надёжный — это реально кто шлёт)
	//   2) provider.steamid   (если player отсутствует)
	uint64_t key = playerSteamId ? playerSteamId : snap.providerSteamId;
	if ( key == 0 )
		return;

	std::lock_guard<std::mutex> lk( m_mutex );
	m_snapshots[key] = std::move( snap );
}

bool GsiServer::GetSnapshot( uint64_t steamId, GsiSnapshot& out ) const
{
	std::lock_guard<std::mutex> lk( m_mutex );
	auto it = m_snapshots.find( steamId );
	if ( it == m_snapshots.end() )
		return false;
	out = it->second;
	return true;
}

int64_t GsiServer::GetAgeMs( uint64_t steamId ) const
{
	std::lock_guard<std::mutex> lk( m_mutex );
	auto it = m_snapshots.find( steamId );
	if ( it == m_snapshots.end() )
		return -1;
	return NowMs() - it->second.lastUpdateMs;
}

size_t GsiServer::GetSeenSteamIdCount() const
{
	std::lock_guard<std::mutex> lk( m_mutex );
	return m_snapshots.size();
}

std::string GsiServer::GetLastError() const
{
	std::lock_guard<std::mutex> lk( m_mutex );
	return m_lastError;
}
