#include "role_rotation.h"

#include <Windows.h>
#include <fstream>
#include <sstream>
#include <chrono>

#include <json.hpp>

using json = nlohmann::json;

static int64_t NowMsRR()
{
	using namespace std::chrono;
	return duration_cast<milliseconds>( system_clock::now().time_since_epoch() ).count();
}

bool RoleRotation::Load( const std::string& path )
{
	std::ifstream f( path );
	if ( !f.is_open() ) return false;

	try
	{
		json j;
		f >> j;

		if ( j.contains( "strategy" ) && j["strategy"].is_string() )
			m_data.strategy = j["strategy"].get<std::string>();
		if ( j.contains( "last_match_id" ) && j["last_match_id"].is_number() )
			m_data.lastMatchId = j["last_match_id"].get<uint64_t>();
		if ( j.contains( "we_won" ) && j["we_won"].is_boolean() )
			m_data.weWon = j["we_won"].get<bool>();
		if ( j.contains( "ts" ) && j["ts"].is_number() )
			m_data.ts = j["ts"].get<int64_t>();

		m_data.history.clear();
		if ( j.contains( "history" ) && j["history"].is_array() )
		{
			for ( auto& h : j["history"] )
			{
				MatchRecord r;
				if ( h.contains( "match_id" ) )  r.matchId  = h["match_id"].get<uint64_t>();
				if ( h.contains( "strategy" ) )  r.strategy = h["strategy"].get<std::string>();
				if ( h.contains( "we_won" ) )    r.weWon    = h["we_won"].get<bool>();
				if ( h.contains( "ts" ) )        r.ts       = h["ts"].get<int64_t>();
				m_data.history.push_back( r );
			}
		}
		return true;
	}
	catch ( ... )
	{
		return false;
	}
}

bool RoleRotation::SaveAtomic( const std::string& path )
{
	try
	{
		json j;
		j["strategy"]      = m_data.strategy;
		j["last_match_id"] = m_data.lastMatchId;
		j["we_won"]        = m_data.weWon;
		j["ts"]            = m_data.ts;

		json hist = json::array();
		for ( auto& r : m_data.history )
		{
			json h;
			h["match_id"] = r.matchId;
			h["strategy"] = r.strategy;
			h["we_won"]   = r.weWon;
			h["ts"]       = r.ts;
			hist.push_back( h );
		}
		j["history"] = hist;

		std::string tmp = path + ".tmp";
		{
			std::ofstream f( tmp, std::ios::binary | std::ios::trunc );
			if ( !f.is_open() ) return false;
			f << j.dump( 2 );
			f.flush();
			if ( !f ) return false;
		}

		// Гарантируем существование родителя.
		size_t lastSlash = path.find_last_of( "\\/" );
		if ( lastSlash != std::string::npos )
		{
			std::string parent = path.substr( 0, lastSlash );
			CreateDirectoryA( parent.c_str(), nullptr );
		}

		if ( !MoveFileExA( tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING ) )
		{
			DeleteFileA( tmp.c_str() );
			return false;
		}
		return true;
	}
	catch ( ... )
	{
		return false;
	}
}

std::string RoleRotation::NextStrategy() const
{
	const std::string& last = m_data.strategy;
	if ( last == "WIN" )  return "LOSE";
	if ( last == "LOSE" ) return "WIN";
	// DEBOOST или cold start — возвращаемся в rotation с WIN.
	return "WIN";
}

void RoleRotation::RecordMatch( uint64_t matchId, const std::string& strategy, bool weWon )
{
	MatchRecord r;
	r.matchId  = matchId;
	r.strategy = strategy;
	r.weWon    = weWon;
	r.ts       = NowMsRR();

	m_data.history.push_back( r );
	while ( m_data.history.size() > 5 )
		m_data.history.pop_front();

	m_data.strategy    = strategy;
	m_data.lastMatchId = matchId;
	m_data.weWon       = weWon;
	m_data.ts          = r.ts;
}

void RoleRotation::UpsertMatch( uint64_t matchId, const std::string& strategy, bool weWon )
{
	// Master broadcast'нул match_result с тем же matchId что slave уже записал
	// локально из POST_GAME — перезаписать поверх (мастер = source of truth).
	if ( !m_data.history.empty() && m_data.history.back().matchId == matchId )
	{
		MatchRecord& back = m_data.history.back();
		back.strategy = strategy;
		back.weWon    = weWon;
		back.ts       = NowMsRR();
		m_data.strategy    = strategy;
		m_data.lastMatchId = matchId;
		m_data.weWon       = weWon;
		m_data.ts          = back.ts;
		return;
	}
	// Иначе — новая запись (peer обогнал нас, у нас POST_GAME ещё не сработал).
	RecordMatch( matchId, strategy, weWon );
}

void RoleRotation::Reset()
{
	m_data = LastRole{};
}
