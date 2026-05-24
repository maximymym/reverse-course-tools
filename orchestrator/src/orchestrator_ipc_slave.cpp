#include "orchestrator_ipc_slave.h"
#include "orchestrator_ipc.h"  // ipc_proto::Sign / Verify / NowMs

#include <ws2tcpip.h>
#include <chrono>

#pragma comment(lib, "ws2_32.lib")

using nlohmann::json;

SlavePeer::SlavePeer() = default;

SlavePeer::~SlavePeer()
{
	Stop();
}

std::string SlavePeer::LastError() const
{
	std::lock_guard<std::mutex> lk( m_errMx );
	return m_lastError;
}

bool SlavePeer::Start( const std::string& host, uint16_t port, const std::string& secret,
	std::function<void( const PeerMsg& )> onMsg )
{
	if ( m_running.exchange( true ) )
		return true;

	m_host   = host;
	m_port   = port;
	m_secret = secret;
	m_onMsg  = std::move( onMsg );

	WSADATA wsa{};
	if ( WSAStartup( MAKEWORD( 2, 2 ), &wsa ) != 0 )
	{
		std::lock_guard<std::mutex> lk( m_errMx );
		m_lastError = "WSAStartup failed";
		m_running = false;
		return false;
	}
	m_wsaInited = true;

	m_thread = std::thread( &SlavePeer::RunLoop, this );
	return true;
}

void SlavePeer::Stop()
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

void SlavePeer::RunLoop()
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
		// Resolve host (IPv4)
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
				char buf[128];
				snprintf( buf, sizeof( buf ), "connect() failed (WSA=%d)", err );
				m_lastError = buf;
			}
			if ( !m_running.load() ) break;
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
		}

		// recv loop с per-iteration timeout (через select), чтобы успевать слать hb.
		std::string acc;
		acc.reserve( 4096 );
		char tmp[2048];
		int64_t lastHb = ipc_proto::NowMs();

		while ( m_running.load() )
		{
			// Send heartbeat если время.
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
			if ( sel == 0 ) continue;  // timeout — re-check hb

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
					m_lastPeerHbMs.store( ipc_proto::NowMs() );
					if ( pm.type != "hb" && m_onMsg )
						m_onMsg( pm );
				}
			}
			if ( acc.size() > 256 * 1024 ) break;
		}

		// Disconnected — закрыть и пере-connect.
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
		Sleep( backoffMs );
		backoffMs = ( backoffMs * 2 < 12000 ) ? backoffMs * 2 : 12000;
	}
}

void SlavePeer::Send( const json& msg )
{
	if ( !m_running.load() ) return;
	std::string line = ipc_proto::Sign( msg, m_secret );

	std::lock_guard<std::mutex> lk( m_sendMx );
	if ( m_sock == INVALID_SOCKET ) return;
	send( m_sock, line.data(), (int)line.size(), 0 );
}
