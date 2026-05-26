#include "orchestrator_relay_client.h"
#include "orchestrator_ipc.h"  // ipc_proto::Sign / Verify / NowMs

#include <ws2tcpip.h>
#include <chrono>

#pragma comment(lib, "ws2_32.lib")

using nlohmann::json;

RelayPeer::RelayPeer() = default;

RelayPeer::~RelayPeer()
{
	Stop();
}

std::string RelayPeer::LastError() const
{
	std::lock_guard<std::mutex> lk( m_errMx );
	return m_lastError;
}

std::string RelayPeer::LastRelayErrorCode() const
{
	std::lock_guard<std::mutex> lk( m_errMx );
	return m_lastRelayErrorCode;
}

void RelayPeer::SetState( State s )
{
	// State transitions: fire callback под m_errMx чтобы избежать race'ов
	// с GUI'ем который параллельно может звать GetSnapshot.
	State prev = m_state.exchange( s );
	if ( prev == s ) return;
	std::function<void( State )> cb;
	{
		std::lock_guard<std::mutex> lk( m_errMx );
		cb = m_onState;
	}
	if ( cb ) cb( s );
}

void RelayPeer::RequestReconnect()
{
	if ( !m_running.load() ) return;
	m_forceReconnect.store( true );
	std::lock_guard<std::mutex> lk( m_sendMx );
	if ( m_sock != INVALID_SOCKET )
	{
		closesocket( m_sock );
		m_sock = INVALID_SOCKET;
	}
}

RelayPeer::Snapshot RelayPeer::GetSnapshot() const
{
	Snapshot s{};
	s.state           = m_state.load();
	s.connected       = m_connected.load();
	s.lastActivityMs  = m_lastActivityMs.load();
	s.lastRttMs       = m_lastRttMs.load();
	s.msgSent         = m_msgSent.load();
	s.msgRecv         = m_msgRecv.load();
	{
		std::lock_guard<std::mutex> lk( m_errMx );
		s.lastError          = m_lastError;
		s.lastRelayErrorCode = m_lastRelayErrorCode;
	}
	return s;
}

bool RelayPeer::Start( const std::string& host, uint16_t port,
	const std::string& userId, const std::string& userAuthToken,
	const std::string& pairId, const std::string& role,
	const std::string& secret,
	std::function<void( const PeerMsg& )> onMsg,
	std::function<void( State )> onState )
{
	if ( m_running.exchange( true ) )
		return true;

	m_host          = host;
	m_port          = port;
	m_userId        = userId;
	m_userAuthToken = userAuthToken;
	m_pairId        = pairId;
	m_role          = role;
	m_secret        = secret;
	m_onMsg         = std::move( onMsg );
	{
		// onState хранится под m_errMx — тот же lock используется в SetState
		// при чтении, чтобы избежать race'а с Stop'ом.
		std::lock_guard<std::mutex> lk( m_errMx );
		m_onState = std::move( onState );
	}

	WSADATA wsa{};
	if ( WSAStartup( MAKEWORD( 2, 2 ), &wsa ) != 0 )
	{
		std::lock_guard<std::mutex> lk( m_errMx );
		m_lastError = "WSAStartup failed";
		m_running = false;
		return false;
	}
	m_wsaInited = true;

	m_thread = std::thread( &RelayPeer::RunLoop, this );
	return true;
}

void RelayPeer::Stop()
{
	if ( !m_running.exchange( false ) )
		return;

	{
		std::lock_guard<std::mutex> lk( m_sendMx );
		if ( m_sock != INVALID_SOCKET )
		{
			closesocket( m_sock );
			m_sock = INVALID_SOCKET;
		}
	}
	if ( m_thread.joinable() ) m_thread.join();

	{
		std::lock_guard<std::mutex> lk( m_errMx );
		m_onState = nullptr;
	}

	if ( m_wsaInited )
	{
		WSACleanup();
		m_wsaInited = false;
	}
}

bool RelayPeer::SendHello( SOCKET s )
{
	// Hello — первое сообщение после connect. Multi-tenant credentials:
	// relay проверяет user_id+auth_token по своей user db, при auth fail
	// шлёт {"type":"error","body":{"code":"auth_failed",...}} и закрывает
	// соединение. HMAC подпись здесь относится к master ↔ slave secret,
	// relay её не верифицирует.
	json hello;
	hello["type"] = "hello";
	hello["body"] = json::object();
	hello["body"]["user_id"]    = m_userId;
	hello["body"]["auth_token"] = m_userAuthToken;
	hello["body"]["pair_id"]    = m_pairId;
	hello["body"]["role"]       = m_role;

	std::string line = ipc_proto::Sign( hello, m_secret );
	int n = send( s, line.data(), (int)line.size(), 0 );
	return n == (int)line.size();
}

void RelayPeer::RunLoop()
{
	int backoffMs = 1500;
	const int kHbIntervalMs   = 3000;
	const int kPingIntervalMs = 5000;

	while ( m_running.load() )
	{
		// ── Connect ──
		SetState( State::Connecting );
		SOCKET s = socket( AF_INET, SOCK_STREAM, IPPROTO_TCP );
		if ( s == INVALID_SOCKET )
		{
			Sleep( backoffMs );
			continue;
		}

		sockaddr_in addr{};
		addr.sin_family = AF_INET;
		addr.sin_port   = htons( m_port );

		if ( inet_pton( AF_INET, m_host.c_str(), &addr.sin_addr ) != 1 )
		{
			addrinfo hints{}, *res = nullptr;
			hints.ai_family   = AF_INET;
			hints.ai_socktype = SOCK_STREAM;
			if ( getaddrinfo( m_host.c_str(), nullptr, &hints, &res ) != 0 || !res )
			{
				closesocket( s );
				{
					std::lock_guard<std::mutex> lk( m_errMx );
					m_lastError = "DNS resolve failed: " + m_host;
				}
				Sleep( backoffMs );
				backoffMs = ( backoffMs * 2 < 12000 ) ? backoffMs * 2 : 12000;
				continue;
			}
			addr.sin_addr = ( (sockaddr_in*)res->ai_addr )->sin_addr;
			freeaddrinfo( res );
		}

		if ( connect( s, (sockaddr*)&addr, sizeof( addr ) ) == SOCKET_ERROR )
		{
			int err = WSAGetLastError();
			closesocket( s );
			{
				std::lock_guard<std::mutex> lk( m_errMx );
				char buf[160];
				snprintf( buf, sizeof( buf ), "relay connect() failed (WSA=%d) %s:%u",
					err, m_host.c_str(), (unsigned)m_port );
				m_lastError = buf;
			}
			if ( !m_running.load() ) break;
			Sleep( backoffMs );
			backoffMs = ( backoffMs * 2 < 12000 ) ? backoffMs * 2 : 12000;
			continue;
		}

		// ── Hello first ──
		// Если hello не уйдёт целиком — relay скорее всего не заведёт pair и
		// дропнет нас при первом heartbeat'е. Лучше сразу re-connect.
		if ( !SendHello( s ) )
		{
			closesocket( s );
			{
				std::lock_guard<std::mutex> lk( m_errMx );
				m_lastError = "relay hello send failed";
			}
			Sleep( backoffMs );
			backoffMs = ( backoffMs * 2 < 12000 ) ? backoffMs * 2 : 12000;
			continue;
		}

		// Connected.
		{
			std::lock_guard<std::mutex> lk( m_sendMx );
			m_sock = s;
		}
		m_connected.store( true );
		SetState( State::Connected );
		backoffMs = 1500;
		{
			std::lock_guard<std::mutex> lk( m_errMx );
			m_lastError = "connected";
			m_lastRelayErrorCode.clear();
		}

		// ── recv loop с per-iteration timeout (через select) для heartbeat ──
		std::string acc;
		acc.reserve( 4096 );
		char tmp[2048];
		int64_t lastHb       = ipc_proto::NowMs();
		int64_t lastPingSent = 0;

		while ( m_running.load() )
		{
			int64_t now = ipc_proto::NowMs();
			if ( now - lastHb >= kHbIntervalMs )
			{
				json hb;
				hb["type"] = "hb";
				hb["body"] = json::object();
				Send( hb );
				lastHb = now;
			}
			// 5-second ping cadence для RTT measurement. Идём независимо от hb,
			// т.к. relay не echo'ит hb'ы обратно.
			if ( now - lastPingSent >= kPingIntervalMs )
			{
				json ping;
				ping["type"] = "ping";
				ping["body"] = json::object();
				ping["body"]["ts"] = now;
				Send( ping );
				lastPingSent = now;
				m_lastPingSentMs.store( now );
			}

			fd_set rset;
			FD_ZERO( &rset );
			FD_SET( s, &rset );
			timeval tv{}; tv.tv_sec = 1; tv.tv_usec = 0;
			int sel = select( 0, &rset, nullptr, nullptr, &tv );
			if ( sel == SOCKET_ERROR ) break;
			if ( sel == 0 ) continue;

			int n = recv( s, tmp, sizeof( tmp ), 0 );
			if ( n <= 0 ) break;
			acc.append( tmp, n );

			size_t pos;
			while ( ( pos = acc.find( '\n' ) ) != std::string::npos )
			{
				std::string line = acc.substr( 0, pos );
				acc.erase( 0, pos + 1 );
				if ( line.empty() ) continue;
				if ( !line.empty() && line.back() == '\r' ) line.pop_back();

				PeerMsg pm;
				if ( ipc_proto::Verify( line, m_secret, ipc_proto::NowMs(), pm ) )
				{
					m_lastActivityMs.store( ipc_proto::NowMs() );
					m_msgRecv.fetch_add( 1 );

					// Relay error message — выставляем adaptive backoff и
					// дисконнектим (relay так и так закроет соединение после).
					// Не передаём в FSM — это не peer-сообщение.
					if ( pm.type == "error" )
					{
						std::string code = pm.body.value( "code", std::string{} );
						std::string text = pm.body.value( "message", std::string{} );
						{
							std::lock_guard<std::mutex> lk( m_errMx );
							m_lastRelayErrorCode = code;
							m_lastError = "relay error code=" + code + " message=" + text;
						}
						bool isAuth = ( code == "auth_failed" || code == "unknown_user"
							|| code == "user_disabled" );
						// Auth-related errors: 60s backoff (нужно вмешательство юзера).
						// Прочие коды (max_pairs, quota_exceeded, malformed_hello): 30s.
						m_authFailureBackoffMs.store( isAuth ? 60000 : 30000 );
						if ( isAuth ) SetState( State::AuthFailed );
						// break из inner loop → outer reconnect-loop увидит
						// m_authFailureBackoffMs и применит его.
						goto disconnect;
					}

					// Ping/pong: pong от нашего ping'а → измеряем RTT.
					// Ping от relay (или peer'а через relay) → отвечаем pong с echo_ts.
					if ( pm.type == "pong" )
					{
						int64_t echoTs = pm.body.value( "echo_ts", (int64_t)0 );
						if ( echoTs > 0 )
						{
							int64_t rtt = ipc_proto::NowMs() - echoTs;
							if ( rtt >= 0 && rtt < 60000 )
								m_lastRttMs.store( rtt );
						}
						continue;
					}
					if ( pm.type == "ping" )
					{
						int64_t ts = pm.body.value( "ts", (int64_t)0 );
						json pong;
						pong["type"] = "pong";
						pong["body"] = json::object();
						pong["body"]["echo_ts"] = ts;
						Send( pong );
						continue;
					}

					// Hello-ack от relay (если он будет такие слать) — игнор.
					// Relay тоже может пересылать peer'ский hb — фильтруем.
					if ( pm.type != "hb" && pm.type != "hello" && m_onMsg )
						m_onMsg( pm );
				}
			}
			if ( acc.size() > 256 * 1024 ) break;
		}

	disconnect:
		// Disconnected.
		{
			std::lock_guard<std::mutex> lk( m_sendMx );
			if ( m_sock != INVALID_SOCKET )
			{
				closesocket( m_sock );
				m_sock = INVALID_SOCKET;
			}
		}
		m_connected.store( false );
		// State::AuthFailed sticky — не downgrade'им до Disconnected, пусть
		// GUI видит причину пока не пройдёт backoff и не случится reconnect.
		if ( m_state.load() != State::AuthFailed )
			SetState( State::Disconnected );
		if ( !m_running.load() ) break;

		// Force-reconnect: пользователь нажал Reconnect — bypass всех backoff'ов.
		if ( m_forceReconnect.exchange( false ) )
		{
			// Сбрасываем и authFailureBackoff, и regular backoff.
			m_authFailureBackoffMs.store( 0 );
			backoffMs = 1500;
			continue;
		}

		// Adaptive backoff: relay сказал что-то типа auth_failed —
		// одноразово используем длинный sleep вместо обычного exp backoff.
		int forced = m_authFailureBackoffMs.exchange( 0 );
		if ( forced > 0 )
		{
			Sleep( forced );
			backoffMs = 1500;  // после длинного sleep'а возвращаемся к исходному
		}
		else
		{
			Sleep( backoffMs );
			backoffMs = ( backoffMs * 2 < 12000 ) ? backoffMs * 2 : 12000;
		}
	}
	SetState( State::Disconnected );
}

void RelayPeer::Send( const json& msg )
{
	if ( !m_running.load() ) return;
	std::string line = ipc_proto::Sign( msg, m_secret );

	std::lock_guard<std::mutex> lk( m_sendMx );
	if ( m_sock == INVALID_SOCKET )
	{
		// Surface dropped-socket sends в lastError, throttled чтобы не спамить
		// (Send зовётся per heartbeat + per pairing/sync-start message). Без
		// этого TOCTOU между connected-check и Send'ом виден только как
		// 15-second "TIMEOUT" в GUI без root-cause в логе.
		const int64_t nowMs = (int64_t)ipc_proto::NowMs();
		const int64_t lastLog = m_lastDeadSendLogMs.load();
		if ( nowMs - lastLog > 5000 )
		{
			m_lastDeadSendLogMs.store( nowMs );
			std::lock_guard<std::mutex> elk( m_errMx );
			m_lastError = "relay send dropped: socket already closed";
		}
		return;
	}
	int n = send( m_sock, line.data(), (int)line.size(), 0 );
	if ( n == (int)line.size() )
		m_msgSent.fetch_add( 1 );
}
