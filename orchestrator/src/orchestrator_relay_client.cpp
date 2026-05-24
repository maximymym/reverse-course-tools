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

bool RelayPeer::Start( const std::string& host, uint16_t port,
	const std::string& userId, const std::string& userAuthToken,
	const std::string& pairId, const std::string& role,
	const std::string& secret,
	std::function<void( const PeerMsg& )> onMsg )
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
	const int kHbIntervalMs = 3000;

	while ( m_running.load() )
	{
		// ── Connect ──
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
		int64_t lastHb = ipc_proto::NowMs();

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
						// Auth-related errors: 60s backoff (нужно вмешательство юзера).
						// Прочие коды (max_pairs, quota_exceeded, malformed_hello): 30s.
						if ( code == "auth_failed" || code == "unknown_user"
							|| code == "user_disabled" )
							m_authFailureBackoffMs.store( 60000 );
						else
							m_authFailureBackoffMs.store( 30000 );
						// break из inner loop → outer reconnect-loop увидит
						// m_authFailureBackoffMs и применит его.
						goto disconnect;
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
		if ( !m_running.load() ) break;

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
}

void RelayPeer::Send( const json& msg )
{
	if ( !m_running.load() ) return;
	std::string line = ipc_proto::Sign( msg, m_secret );

	std::lock_guard<std::mutex> lk( m_sendMx );
	if ( m_sock == INVALID_SOCKET ) return;
	send( m_sock, line.data(), (int)line.size(), 0 );
}
