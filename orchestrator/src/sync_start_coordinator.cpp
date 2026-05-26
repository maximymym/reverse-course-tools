#include "sync_start_coordinator.h"

#include <cassert>
#include <cstdio>
#include <random>

using nlohmann::json;

int64_t SyncStartCoordinator::NowMs_()
{
	using namespace std::chrono;
	return duration_cast<milliseconds>( steady_clock::now().time_since_epoch() ).count();
}

std::string SyncStartCoordinator::GenerateRequestId_() const
{
	// Lex-comparable id: 13-digit zero-padded ms epoch + "-" + 8 hex chars.
	// 13 digits = ms epoch до ~year 2286, lex compare = numeric compare для
	// одинаковой длины. Random suffix ломает tie если оба orchestrator'a
	// начали в одну ms.
	using namespace std::chrono;
	int64_t ms = duration_cast<milliseconds>( system_clock::now().time_since_epoch() ).count();

	static thread_local std::mt19937_64 rng{ std::random_device{}() };
	uint32_t rnd = (uint32_t)( rng() & 0xFFFFFFFFu );

	char buf[32];
	std::snprintf( buf, sizeof( buf ), "%013lld-%08x", (long long)ms, rnd );
	return std::string( buf );
}

void SyncStartCoordinator::TransitionTo_( SyncStartState s, int64_t nowMs, PendingActions& actions )
{
	// Sanity: invalid self-loops indicate logic error. IDLE→IDLE OK (Reset path).
	if ( s == m_state && s != SyncStartState::IDLE )
	{
		assert( false && "SyncStartCoordinator: redundant self-transition" );
		return;
	}

	m_state          = s;
	m_enteredStateMs = nowMs;

	// Set/clear ackDeadline based on state.
	switch ( s )
	{
		case SyncStartState::WAITING_PEER_ACK:
			m_ackDeadlineMs = nowMs + (int64_t)ackTimeoutMs;
			break;
		case SyncStartState::PEER_REQUESTED:
			m_ackDeadlineMs = nowMs + (int64_t)userResponseTimeoutMs;
			break;
		case SyncStartState::CONFIRMING:
			// Короткий hold — STARTING fire'ится в этом же tick после ack send.
			m_ackDeadlineMs = nowMs + 500;
			break;
		case SyncStartState::STARTING:
			m_ackDeadlineMs = nowMs + (int64_t)startingHoldMs;
			actions.fireStartFarm = true;
			break;
		case SyncStartState::DECLINED:
			m_ackDeadlineMs = nowMs + (int64_t)declinedToastMs;
			break;
		case SyncStartState::TIMEOUT:
			m_ackDeadlineMs = nowMs + (int64_t)timeoutToastMs;
			break;
		case SyncStartState::IDLE:
			m_ackDeadlineMs = 0;
			m_requestId.clear();
			m_peerRole.clear();
			m_configHash.clear();
			// m_myRole / m_lastDeclineReason остаются для snapshot debug'а.
			break;
	}

	actions.fireStateChange = true;
	actions.stateForCb      = s;
}

void SyncStartCoordinator::Flush_( PendingActions& actions )
{
	// Все callbacks за пределами m_mx — иначе deadlock через re-entry
	// (broadcast → relay → peer → ... → OnPeerMessage).
	if ( broadcast )
	{
		for ( auto& m : actions.broadcasts ) broadcast( m );
	}
	if ( actions.fireStartFarm && startFarmFn )
	{
		startFarmFn();
	}
	if ( actions.fireStateChange && onStateChange )
	{
		onStateChange( actions.stateForCb );
	}
}

bool SyncStartCoordinator::Initiate( const std::string& myRole, const std::string& configHash,
	const std::string& myStrategy )
{
	PendingActions actions;
	{
		std::lock_guard<std::mutex> lk( m_mx );
		if ( m_state != SyncStartState::IDLE ) return false;

		m_myRole      = myRole;
		m_configHash  = configHash;
		m_myStrategy  = myStrategy;     // Bug B fix: shipped peer'у
		m_peerStrategy.clear();         // initiator не знает peer strategy
		m_requestId   = GenerateRequestId_();
		m_lastDeclineReason.clear();

		// Build start_request — payload в "body" (ipc_proto::Sign wire contract).
		// Поле "strategy" — peer прочитает и invert'нёт у себя (responder=opposite).
		json b;
		b["type"] = "start_request";
		b["body"] = {
			{ "request_id",  m_requestId },
			{ "role",        myRole },
			{ "config_hash", configHash },
			{ "strategy",    myStrategy },
		};
		actions.broadcasts.push_back( std::move( b ) );

		TransitionTo_( SyncStartState::WAITING_PEER_ACK, NowMs_(), actions );
	}
	Flush_( actions );
	return true;
}

std::string SyncStartCoordinator::GetEffectiveStrategy() const
{
	std::lock_guard<std::mutex> lk( m_mx );
	// Initiator path — мы передавали myStrategy в Initiate.
	if ( !m_myStrategy.empty() )
		return m_myStrategy;
	// Responder path — peer прислал свою strategy, мы играем opposite.
	if ( !m_peerStrategy.empty() )
	{
		if ( m_peerStrategy == "WIN" )  return "LOSE";
		if ( m_peerStrategy == "LOSE" ) return "WIN";
		return m_peerStrategy;  // DEBOOST / unknown → mirror (не наша забота)
	}
	return "";  // decoupled — caller uses fallback
}

void SyncStartCoordinator::UserCancel()
{
	PendingActions actions;
	{
		std::lock_guard<std::mutex> lk( m_mx );
		if ( m_state != SyncStartState::WAITING_PEER_ACK ) return;

		json b;
		b["type"] = "start_cancel";
		b["body"] = {
			{ "request_id", m_requestId },
			{ "reason",     "user_cancel" },
		};
		actions.broadcasts.push_back( std::move( b ) );

		m_lastDeclineReason = "user_cancel";
		TransitionTo_( SyncStartState::IDLE, NowMs_(), actions );
	}
	Flush_( actions );
}

void SyncStartCoordinator::UserAccept()
{
	PendingActions actions;
	{
		std::lock_guard<std::mutex> lk( m_mx );
		if ( m_state != SyncStartState::PEER_REQUESTED ) return;

		json b;
		b["type"] = "start_ack";
		b["body"] = {
			{ "request_id", m_requestId },
			{ "accept",     true },
		};
		actions.broadcasts.push_back( std::move( b ) );

		// Через CONFIRMING сразу в STARTING — handshake завершён, наш ack отправлен,
		// инициатор узнает по start_ack(accept). Без waiting на peer'a — оба
		// бегут в STARTING параллельно (responder сразу здесь, initiator в его
		// OnPeerMessage(start_ack accept)).
		TransitionTo_( SyncStartState::CONFIRMING, NowMs_(), actions );
	}
	Flush_( actions );

	// CONFIRMING → STARTING в этом же логическом шаге. Делаем отдельным под-блоком
	// чтобы actions из первого transition'а flushнулись (broadcast ack уходит до
	// startFarmFn).
	PendingActions actions2;
	{
		std::lock_guard<std::mutex> lk( m_mx );
		if ( m_state != SyncStartState::CONFIRMING ) return;
		TransitionTo_( SyncStartState::STARTING, NowMs_(), actions2 );
	}
	Flush_( actions2 );
}

void SyncStartCoordinator::UserDecline( const std::string& reason )
{
	PendingActions actions;
	{
		std::lock_guard<std::mutex> lk( m_mx );
		if ( m_state != SyncStartState::PEER_REQUESTED ) return;

		json b;
		b["type"] = "start_ack";
		b["body"] = {
			{ "request_id", m_requestId },
			{ "accept",     false },
			{ "reason",     reason },
		};
		actions.broadcasts.push_back( std::move( b ) );

		m_lastDeclineReason = reason.empty() ? std::string( "user_decline" ) : reason;
		TransitionTo_( SyncStartState::DECLINED, NowMs_(), actions );
	}
	Flush_( actions );
}

void SyncStartCoordinator::OnPeerMessage( const std::string& msgType, const json& body )
{
	PendingActions actions;
	{
		std::lock_guard<std::mutex> lk( m_mx );

		// ping/pong — keepalive, не меняем state. Pong-on-ping reply делает
		// orchestrator level (RelayClient telemetry). Здесь просто игнор.
		if ( msgType == "ping" || msgType == "pong" )
		{
			return;
		}

		if ( msgType == "start_request" )
		{
			std::string incomingId   = body.value( "request_id", std::string() );
			std::string incomingRole = body.value( "role",       std::string() );

			if ( incomingId.empty() ) return;  // malformed

			bool acceptIntoPeerRequested = false;

			// Race detection: мы тоже initiator?
			if ( m_state == SyncStartState::WAITING_PEER_ACK )
			{
				// Lex compare: smaller id wins (initiator-retained).
				if ( m_requestId < incomingId )
				{
					// Мы winner — ignore incoming. Peer загнётся по timeout'у
					// или примет наш ack когда мы его пришлём.
					return;
				}
				// Мы loser: cancel свой request СТАРЫМ id, потом примем peer'а.
				json cancel;
				cancel["type"] = "start_cancel";
				cancel["body"] = {
					{ "request_id", m_requestId },
					{ "reason",     "race_lost" },
				};
				actions.broadcasts.push_back( std::move( cancel ) );
				m_lastDeclineReason     = "race_lost";
				acceptIntoPeerRequested = true;
			}
			else if ( m_state == SyncStartState::IDLE )
			{
				acceptIntoPeerRequested = true;
			}
			else
			{
				// Мы заняты (CONFIRMING/STARTING/DECLINED/TIMEOUT) — peer должен
				// был выждать. Отбрасываем decline'ом, state не меняем.
				json ack;
				ack["type"] = "start_ack";
				ack["body"] = {
					{ "request_id", incomingId },
					{ "accept",     false },
					{ "reason",     "busy" },
				};
				actions.broadcasts.push_back( std::move( ack ) );
			}

			if ( acceptIntoPeerRequested )
			{
				m_requestId  = incomingId;
				m_peerRole   = incomingRole;
				m_configHash = body.value( "config_hash", std::string() );
				// Bug B fix: peer strategy → инвертируем в GetEffectiveStrategy.
				m_peerStrategy = body.value( "strategy", std::string() );
				m_myStrategy.clear();  // responder не initiator, своего нет
				TransitionTo_( SyncStartState::PEER_REQUESTED, NowMs_(), actions );
			}
		}
		else if ( msgType == "start_ack" )
		{
			std::string incomingId = body.value( "request_id", std::string() );
			bool        accept     = body.value( "accept",     false );
			std::string reason     = body.value( "reason",     std::string() );

			// Принимаем ack только если мы initiator (WAITING_PEER_ACK) и id matches.
			if ( m_state != SyncStartState::WAITING_PEER_ACK )       return;
			if ( !incomingId.empty() && incomingId != m_requestId )  return;

			if ( accept )
			{
				TransitionTo_( SyncStartState::STARTING, NowMs_(), actions );
			}
			else
			{
				m_lastDeclineReason = reason.empty() ? std::string( "peer_decline" ) : reason;
				TransitionTo_( SyncStartState::DECLINED, NowMs_(), actions );
			}
		}
		else if ( msgType == "start_cancel" )
		{
			std::string incomingId = body.value( "request_id", std::string() );
			std::string reason     = body.value( "reason",     std::string() );

			// Cancel от peer'а: если мы в PEER_REQUESTED с тем же id — IDLE.
			// Если мы WAITING_PEER_ACK — peer mid-flight передумал, тоже IDLE.
			if ( m_state == SyncStartState::PEER_REQUESTED ||
			     m_state == SyncStartState::WAITING_PEER_ACK )
			{
				if ( !incomingId.empty() && !m_requestId.empty() && incomingId != m_requestId )
				{
					// Stale cancel — игнор.
					return;
				}
				m_lastDeclineReason = reason.empty() ? std::string( "peer_cancel" ) : reason;
				TransitionTo_( SyncStartState::IDLE, NowMs_(), actions );
			}
		}
	}

	Flush_( actions );
}

bool SyncStartCoordinator::Tick( int64_t nowMs )
{
	PendingActions actions;
	bool changed = false;
	{
		std::lock_guard<std::mutex> lk( m_mx );

		if ( m_ackDeadlineMs == 0 || nowMs < m_ackDeadlineMs ) return false;

		switch ( m_state )
		{
			case SyncStartState::WAITING_PEER_ACK:
				// Initiator's ack timeout → TIMEOUT.
				m_lastDeclineReason = "ack_timeout";
				TransitionTo_( SyncStartState::TIMEOUT, nowMs, actions );
				changed = true;
				break;

			case SyncStartState::PEER_REQUESTED:
			{
				// User не ответил вовремя → auto-decline + DECLINED.
				json b;
				b["type"]       = "start_ack";
				b["request_id"] = m_requestId;
				b["accept"]     = false;
				b["reason"]     = "no_response";
				actions.broadcasts.push_back( std::move( b ) );

				m_lastDeclineReason = "no_response";
				TransitionTo_( SyncStartState::DECLINED, nowMs, actions );
				changed = true;
				break;
			}

			case SyncStartState::CONFIRMING:
				// Should have transitioned synchronously в UserAccept. Если
				// застряли — fallback в STARTING.
				TransitionTo_( SyncStartState::STARTING, nowMs, actions );
				changed = true;
				break;

			case SyncStartState::STARTING:
				// startingHoldMs elapsed — startFarmFn уже отработал, IDLE.
				TransitionTo_( SyncStartState::IDLE, nowMs, actions );
				changed = true;
				break;

			case SyncStartState::DECLINED:
			case SyncStartState::TIMEOUT:
				// Toast lifetime elapsed.
				TransitionTo_( SyncStartState::IDLE, nowMs, actions );
				changed = true;
				break;

			case SyncStartState::IDLE:
				// Не должно срабатывать — ackDeadlineMs=0 в IDLE.
				m_ackDeadlineMs = 0;
				break;
		}
	}
	Flush_( actions );
	return changed;
}

void SyncStartCoordinator::Reset( const std::string& reason )
{
	PendingActions actions;
	{
		std::lock_guard<std::mutex> lk( m_mx );
		if ( m_state == SyncStartState::IDLE ) return;
		m_lastDeclineReason = reason.empty() ? std::string( "reset" ) : reason;
		TransitionTo_( SyncStartState::IDLE, NowMs_(), actions );
	}
	Flush_( actions );
}

SyncStartSnapshot SyncStartCoordinator::GetSnapshot() const
{
	std::lock_guard<std::mutex> lk( m_mx );
	SyncStartSnapshot s;
	s.state             = m_state;
	s.requestId         = m_requestId;
	s.enteredStateMs    = m_enteredStateMs;
	s.ackDeadlineMs     = m_ackDeadlineMs;
	s.peerRole          = m_peerRole;
	s.lastDeclineReason = m_lastDeclineReason;
	s.myStrategy        = m_myStrategy;
	s.peerStrategy      = m_peerStrategy;
	return s;
}
