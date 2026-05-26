#include "match_pairing_fsm.h"

#include <algorithm>
#include <unordered_map>

using nlohmann::json;

void MatchPairingFsm::Init( int matchSyncTimeoutS, int maxConsecutiveCancels, int botCount )
{
	std::lock_guard<std::mutex> lk( m_mx );
	m_matchSyncTimeoutS    = matchSyncTimeoutS > 0 ? matchSyncTimeoutS : 15;
	m_maxConsecutiveCancels = maxConsecutiveCancels > 0 ? maxConsecutiveCancels : 3;
	m_botCount             = botCount > 0 ? botCount : 5;
	m_phase                = PairingPhase::IDLE;
	m_localPending.clear();
	m_peerLobbyIds.clear();
	m_waitDeadlineMs       = 0;
	m_localMajority        = 0;
	m_pendingDecision      = PairingDecision{};
	m_decisionConsumed     = true;
}

void MatchPairingFsm::Reset()
{
	std::lock_guard<std::mutex> lk( m_mx );
	m_phase = PairingPhase::IDLE;
	m_localPending.clear();
	m_peerLobbyIds.clear();
	m_waitDeadlineMs   = 0;
	m_localMajority    = 0;
	m_pendingDecision  = PairingDecision{};
	m_decisionConsumed = true;
}

PairingPhase MatchPairingFsm::phase() const
{
	std::lock_guard<std::mutex> lk( m_mx );
	return m_phase;
}

int MatchPairingFsm::CancelStreak() const
{
	std::lock_guard<std::mutex> lk( m_mx );
	return m_cancelStreak;
}

void MatchPairingFsm::ResetCancelStreak()
{
	std::lock_guard<std::mutex> lk( m_mx );
	m_cancelStreak = 0;
}

uint64_t MatchPairingFsm::ComputeLocalMajority() const
{
	if ( m_localPending.empty() ) return 0;
	std::unordered_map<uint64_t, int> counts;
	for ( auto& [k, v] : m_localPending ) counts[v]++;
	uint64_t best = 0;
	int bestCnt = 0;
	for ( auto& [v, c] : counts )
	{
		if ( c > bestCnt ) { bestCnt = c; best = v; }
	}
	return best;
}

bool MatchPairingFsm::AllLocalAgree() const
{
	if ( (int)m_localPending.size() < m_botCount ) return false;
	if ( m_localPending.empty() ) return false;
	uint64_t first = m_localPending.begin()->second;
	for ( auto& [k, v] : m_localPending )
		if ( v != first ) return false;
	return true;
}

bool MatchPairingFsm::CombinedAgree( uint64_t lobby ) const
{
	if ( lobby == 0 ) return false;
	for ( auto& [k, v] : m_localPending ) if ( v != lobby ) return false;
	for ( auto v : m_peerLobbyIds )       if ( v != lobby ) return false;
	return true;
}

void MatchPairingFsm::EmitDecision( PairingDecision::Kind k, uint64_t lobby, const std::string& reason )
{
	m_pendingDecision.kind    = k;
	m_pendingDecision.lobbyId = lobby;
	m_pendingDecision.reason  = reason;
	m_decisionConsumed        = false;

	if ( k == PairingDecision::ACCEPT )
	{
		m_phase = PairingPhase::DECIDED_ACCEPT;
		json m;
		m["type"]     = "decision";
		m["action"]   = "accept";
		m["lobby_id"] = lobby;
		m["reason"]   = reason;
		if ( onBroadcast ) onBroadcast( m );
	}
	else if ( k == PairingDecision::CANCEL )
	{
		m_phase = PairingPhase::DECIDED_CANCEL;
		json m;
		m["type"]   = "decision";
		m["action"] = "cancel";
		m["reason"] = reason;
		if ( onBroadcast ) onBroadcast( m );
	}
}

void MatchPairingFsm::OnMatchPendingFile( int botIdx, uint64_t lobbyId )
{
	if ( botIdx < 0 || lobbyId == 0 ) return;

	std::lock_guard<std::mutex> lk( m_mx );

	// idempotent повтор — игнор.
	auto it = m_localPending.find( botIdx );
	if ( it != m_localPending.end() && it->second == lobbyId ) return;

	// Изменили решение во время WAIT_PEER — обновляем (rare race).
	m_localPending[botIdx] = lobbyId;

	// Triggers только из IDLE / LOCAL_FOUND. WAIT_PEER ждёт peer'а, не реагируем
	// на повторные file events чтобы не спамить broadcast'ы.
	if ( m_phase != PairingPhase::IDLE && m_phase != PairingPhase::LOCAL_FOUND )
		return;

	if ( (int)m_localPending.size() >= m_botCount )
	{
		m_localMajority = ComputeLocalMajority();
		bool allAgree   = AllLocalAgree();

		// Broadcast peer'у наш lobby_ids set.
		json m;
		m["type"]            = "match_found";
		m["local_majority"]  = m_localMajority;
		m["all_agree"]       = allAgree;
		json arr = json::array();
		for ( auto& [k, v] : m_localPending ) arr.push_back( v );
		m["lobby_ids"]       = arr;
		if ( onBroadcast ) onBroadcast( m );

		m_phase            = PairingPhase::WAIT_PEER;
		m_waitDeadlineMs   = 0;  // выставится в Tick на nowMs+timeout (см. ниже)

		// Если peer уже прислал свои lobby ранее — попытаемся сразу скомбить.
		if ( !m_peerLobbyIds.empty() )
		{
			if ( allAgree && CombinedAgree( m_localMajority ) )
				EmitDecision( PairingDecision::ACCEPT, m_localMajority, "agreed" );
			else
				EmitDecision( PairingDecision::CANCEL, 0, "lobby_mismatch" );
		}
	}
}

void MatchPairingFsm::OnPeerMessage( const PeerMsg& m )
{
	// Sync-start handshake messages route в SyncStartCoordinator (отдельный FSM).
	// Match-FSM не должна их видеть — иначе reset cancelStreak / ложные phase
	// transitions. Callback не под mutex — SyncStartCoordinator имеет свой.
	if ( m.type == "start_request" || m.type == "start_ack" ||
	     m.type == "start_cancel"  || m.type == "ping"      || m.type == "pong" )
	{
		if ( onSyncStartMsg ) onSyncStartMsg( m );
		return;
	}

	std::lock_guard<std::mutex> lk( m_mx );

	if ( m.type == "match_found" )
	{
		m_peerLobbyIds.clear();
		if ( m.body.contains( "lobby_ids" ) && m.body["lobby_ids"].is_array() )
		{
			for ( auto& v : m.body["lobby_ids"] )
			{
				if ( v.is_number_unsigned() ) m_peerLobbyIds.push_back( v.get<uint64_t>() );
				else if ( v.is_number() )      m_peerLobbyIds.push_back( (uint64_t)v.get<int64_t>() );
			}
		}

		// Если мы уже WAIT_PEER — пробуем закрыть decision сейчас.
		if ( m_phase == PairingPhase::WAIT_PEER )
		{
			bool allAgree = AllLocalAgree();
			if ( allAgree && CombinedAgree( m_localMajority ) )
				EmitDecision( PairingDecision::ACCEPT, m_localMajority, "agreed" );
			else
				EmitDecision( PairingDecision::CANCEL, 0, "lobby_mismatch" );
		}
		// Если ещё IDLE/LOCAL_FOUND — peer успел раньше; ждём пока локальные
		// 5 заполнятся, тогда OnMatchPendingFile выше сравнит.
	}
	else if ( m.type == "decision" )
	{
		// Master/slave обмениваются "decision" чтобы оба применили согласованно.
		// Но поскольку обе стороны независимо выводят одно и то же решение
		// (lobby_ids идентичны → ACCEPT, иначе CANCEL), peer-decision —
		// resilience signal. Если peer прислал accept а мы ещё в WAIT_PEER —
		// принимаем его авторитет.
		std::string action = m.body.value( "action", std::string() );
		if ( m_phase == PairingPhase::WAIT_PEER && action == "accept" )
		{
			uint64_t lid = m.body.value( "lobby_id", (uint64_t)0 );
			if ( lid == 0 ) lid = m_localMajority;
			EmitDecision( PairingDecision::ACCEPT, lid, "peer_decision" );
		}
		else if ( m_phase == PairingPhase::WAIT_PEER && action == "cancel" )
		{
			EmitDecision( PairingDecision::CANCEL, 0, "peer_decision" );
		}
	}
	// "hb" / "match_result" / прочее — обработка в orchestrator'е, FSM не трогаем.
}

PairingDecision MatchPairingFsm::Tick( uint64_t nowMs )
{
	std::lock_guard<std::mutex> lk( m_mx );

	// Установить deadline при первом Tick после WAIT_PEER (раньше у нас не было now).
	if ( m_phase == PairingPhase::WAIT_PEER && m_waitDeadlineMs == 0 )
		m_waitDeadlineMs = nowMs + (uint64_t)m_matchSyncTimeoutS * 1000ULL;

	if ( m_phase == PairingPhase::WAIT_PEER && nowMs > m_waitDeadlineMs && m_waitDeadlineMs != 0 )
	{
		EmitDecision( PairingDecision::CANCEL, 0, "peer_timeout" );
	}

	if ( !m_decisionConsumed )
	{
		PairingDecision d = m_pendingDecision;
		// Не сбрасываем m_decisionConsumed здесь — caller делает OnDecisionApplied,
		// иначе один decision дёрнется два раза. Но Tick должен вернуть d ровно
		// один раз. Помечаем consumed=true сразу.
		m_decisionConsumed = true;
		return d;
	}
	return PairingDecision{};
}

void MatchPairingFsm::OnDecisionApplied( const PairingDecision& d )
{
	std::lock_guard<std::mutex> lk( m_mx );
	if ( d.kind == PairingDecision::ACCEPT )
	{
		m_cancelStreak = 0;
	}
	else if ( d.kind == PairingDecision::CANCEL )
	{
		m_cancelStreak++;
	}
	// Сбрасываем к IDLE — следующий цикл начнётся заново когда боты снова
	// напишут match_pending файлы.
	m_phase = PairingPhase::IDLE;
	m_localPending.clear();
	m_peerLobbyIds.clear();
	m_waitDeadlineMs   = 0;
	m_localMajority    = 0;
	m_pendingDecision  = PairingDecision{};
	m_decisionConsumed = true;
}

MatchPairingFsm::DebugStatus MatchPairingFsm::GetDebug() const
{
	std::lock_guard<std::mutex> lk( m_mx );
	DebugStatus s;
	s.phase              = m_phase;
	s.localFilled        = (int)m_localPending.size();
	s.localTotal         = m_botCount;
	s.localMajorityLobby = m_localMajority ? m_localMajority : ComputeLocalMajority();
	s.peerCount          = (int)m_peerLobbyIds.size();
	s.waitDeadlineMs     = m_waitDeadlineMs;
	s.cancelStreak       = m_cancelStreak;
	return s;
}
