#include "orchestrator_ipc.h"

#include <ws2tcpip.h>

#include <chrono>
#include <cstdint>
#include <cstring>

#pragma comment(lib, "ws2_32.lib")

using nlohmann::json;

// ═══════════════════════════════════════════════════════════════════════════
// SHA-256 — inline pure-C implementation (ref: FIPS 180-4).
// ═══════════════════════════════════════════════════════════════════════════
namespace
{

struct Sha256Ctx
{
	uint8_t  buf[64];
	uint64_t bitLen = 0;
	uint32_t state[8];
	uint32_t bufLen = 0;
};

static const uint32_t kSha256K[64] = {
	0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,
	0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,
	0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
	0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,
	0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,
	0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
	0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,
	0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u
};

static inline uint32_t Rotr( uint32_t x, uint32_t n ) { return ( x >> n ) | ( x << ( 32 - n ) ); }

static void Sha256Init( Sha256Ctx& c )
{
	c.state[0] = 0x6a09e667u; c.state[1] = 0xbb67ae85u;
	c.state[2] = 0x3c6ef372u; c.state[3] = 0xa54ff53au;
	c.state[4] = 0x510e527fu; c.state[5] = 0x9b05688cu;
	c.state[6] = 0x1f83d9abu; c.state[7] = 0x5be0cd19u;
	c.bitLen = 0; c.bufLen = 0;
}

static void Sha256Block( Sha256Ctx& c, const uint8_t blk[64] )
{
	uint32_t w[64];
	for ( int i = 0; i < 16; i++ )
		w[i] = ( (uint32_t)blk[i*4] << 24 ) | ( (uint32_t)blk[i*4+1] << 16 ) |
			   ( (uint32_t)blk[i*4+2] << 8 ) |  (uint32_t)blk[i*4+3];
	for ( int i = 16; i < 64; i++ )
	{
		uint32_t s0 = Rotr( w[i-15], 7 ) ^ Rotr( w[i-15], 18 ) ^ ( w[i-15] >> 3 );
		uint32_t s1 = Rotr( w[i-2], 17 ) ^ Rotr( w[i-2], 19 ) ^ ( w[i-2] >> 10 );
		w[i] = w[i-16] + s0 + w[i-7] + s1;
	}
	uint32_t a=c.state[0], b=c.state[1], cc=c.state[2], d=c.state[3];
	uint32_t e=c.state[4], f=c.state[5], g=c.state[6], h=c.state[7];
	for ( int i = 0; i < 64; i++ )
	{
		uint32_t S1 = Rotr( e, 6 ) ^ Rotr( e, 11 ) ^ Rotr( e, 25 );
		uint32_t ch = ( e & f ) ^ ( ( ~e ) & g );
		uint32_t t1 = h + S1 + ch + kSha256K[i] + w[i];
		uint32_t S0 = Rotr( a, 2 ) ^ Rotr( a, 13 ) ^ Rotr( a, 22 );
		uint32_t mj = ( a & b ) ^ ( a & cc ) ^ ( b & cc );
		uint32_t t2 = S0 + mj;
		h = g; g = f; f = e; e = d + t1;
		d = cc; cc = b; b = a; a = t1 + t2;
	}
	c.state[0]+=a; c.state[1]+=b; c.state[2]+=cc; c.state[3]+=d;
	c.state[4]+=e; c.state[5]+=f; c.state[6]+=g; c.state[7]+=h;
}

static void Sha256Update( Sha256Ctx& c, const uint8_t* data, size_t len )
{
	c.bitLen += (uint64_t)len * 8ULL;
	while ( len > 0 )
	{
		uint32_t take = 64 - c.bufLen;
		if ( take > len ) take = (uint32_t)len;
		memcpy( c.buf + c.bufLen, data, take );
		c.bufLen += take;
		data += take;
		len -= take;
		if ( c.bufLen == 64 )
		{
			Sha256Block( c, c.buf );
			c.bufLen = 0;
		}
	}
}

static void Sha256Final( Sha256Ctx& c, uint8_t out[32] )
{
	c.buf[c.bufLen++] = 0x80;
	if ( c.bufLen > 56 )
	{
		while ( c.bufLen < 64 ) c.buf[c.bufLen++] = 0;
		Sha256Block( c, c.buf );
		c.bufLen = 0;
	}
	while ( c.bufLen < 56 ) c.buf[c.bufLen++] = 0;
	uint64_t bl = c.bitLen;
	for ( int i = 7; i >= 0; i-- ) c.buf[c.bufLen++] = (uint8_t)( bl >> ( i * 8 ) );
	Sha256Block( c, c.buf );
	for ( int i = 0; i < 8; i++ )
	{
		out[i*4]   = (uint8_t)( c.state[i] >> 24 );
		out[i*4+1] = (uint8_t)( c.state[i] >> 16 );
		out[i*4+2] = (uint8_t)( c.state[i] >> 8 );
		out[i*4+3] = (uint8_t)( c.state[i] );
	}
}

static void Sha256Bytes( const uint8_t* data, size_t len, uint8_t out[32] )
{
	Sha256Ctx c; Sha256Init( c );
	Sha256Update( c, data, len );
	Sha256Final( c, out );
}

static std::string ToHexLower( const uint8_t* p, size_t n )
{
	static const char hexd[] = "0123456789abcdef";
	std::string s; s.resize( n * 2 );
	for ( size_t i = 0; i < n; i++ )
	{
		s[i*2]   = hexd[( p[i] >> 4 ) & 0xF];
		s[i*2+1] = hexd[p[i] & 0xF];
	}
	return s;
}

} // namespace

// ═══════════════════════════════════════════════════════════════════════════
// ipc_proto: HMAC + sign + verify
// ═══════════════════════════════════════════════════════════════════════════
namespace ipc_proto
{

int64_t NowMs()
{
	using namespace std::chrono;
	return duration_cast<milliseconds>( system_clock::now().time_since_epoch() ).count();
}

std::string HmacSha256Hex( const std::string& key, const std::string& data )
{
	// HMAC-SHA256 per RFC 2104.
	uint8_t k0[64];
	if ( key.size() > 64 )
	{
		uint8_t kh[32]; Sha256Bytes( (const uint8_t*)key.data(), key.size(), kh );
		memcpy( k0, kh, 32 );
		memset( k0 + 32, 0, 32 );
	}
	else
	{
		memcpy( k0, key.data(), key.size() );
		memset( k0 + key.size(), 0, 64 - key.size() );
	}
	uint8_t ikey[64], okey[64];
	for ( int i = 0; i < 64; i++ )
	{
		ikey[i] = k0[i] ^ 0x36;
		okey[i] = k0[i] ^ 0x5c;
	}

	Sha256Ctx ic; Sha256Init( ic );
	Sha256Update( ic, ikey, 64 );
	Sha256Update( ic, (const uint8_t*)data.data(), data.size() );
	uint8_t inner[32]; Sha256Final( ic, inner );

	Sha256Ctx oc; Sha256Init( oc );
	Sha256Update( oc, okey, 64 );
	Sha256Update( oc, inner, 32 );
	uint8_t out[32]; Sha256Final( oc, out );

	return ToHexLower( out, 32 );
}

std::string Sign( const json& msgIn, const std::string& secret )
{
	json msg = msgIn;
	if ( !msg.contains( "type" ) ) msg["type"] = "unknown";
	if ( !msg.contains( "body" ) ) msg["body"] = json::object();
	if ( !msg.contains( "ts" ) )   msg["ts"]   = NowMs();

	std::string type = msg["type"].get<std::string>();
	int64_t     ts   = msg["ts"].is_number() ? msg["ts"].get<int64_t>() : NowMs();
	std::string body = msg["body"].dump();

	// Сериализуем canonical: type|ts|body
	std::string toSign = type + "|" + std::to_string( ts ) + "|" + body;
	msg["sig"] = HmacSha256Hex( secret, toSign );
	// line-delimited: один компактный JSON + \n
	return msg.dump() + "\n";
}

bool Verify( const std::string& line, const std::string& secret, int64_t nowMs, PeerMsg& out )
{
	if ( line.empty() ) return false;
	try
	{
		json m = json::parse( line );
		if ( !m.contains( "sig" ) || !m["sig"].is_string() ) return false;
		if ( !m.contains( "type" ) || !m["type"].is_string() ) return false;
		if ( !m.contains( "ts" )   || !m["ts"].is_number() )   return false;

		std::string sig  = m["sig"].get<std::string>();
		std::string type = m["type"].get<std::string>();
		int64_t     ts   = m["ts"].get<int64_t>();
		json        body = m.value( "body", json::object() );

		// skew ≤10s в любую сторону
		int64_t delta = nowMs > ts ? nowMs - ts : ts - nowMs;
		if ( delta > 10000 ) return false;

		std::string toSign = type + "|" + std::to_string( ts ) + "|" + body.dump();
		std::string expect = HmacSha256Hex( secret, toSign );

		// constant-time compare
		if ( expect.size() != sig.size() ) return false;
		uint8_t diff = 0;
		for ( size_t i = 0; i < expect.size(); i++ )
			diff |= (uint8_t)( expect[i] ^ sig[i] );
		if ( diff != 0 ) return false;

		out.type = type;
		out.body = body;
		return true;
	}
	catch ( ... )
	{
		return false;
	}
}

} // namespace ipc_proto

// ═══════════════════════════════════════════════════════════════════════════
// OrchestratorIpc
// ═══════════════════════════════════════════════════════════════════════════

OrchestratorIpc::OrchestratorIpc() = default;

OrchestratorIpc::~OrchestratorIpc()
{
	Stop();
}

void OrchestratorIpc::SetError( const std::string& e )
{
	std::lock_guard<std::mutex> lk( m_errMx );
	m_lastError = e;
}

std::string OrchestratorIpc::LastError() const
{
	std::lock_guard<std::mutex> lk( m_errMx );
	return m_lastError;
}

bool OrchestratorIpc::Start( uint16_t port, const std::string& bindIp, const std::string& secret,
	std::function<void( const PeerMsg& )> onMsg )
{
	if ( m_running.exchange( true ) )
		return true;

	m_secret = secret;
	m_onMsg  = std::move( onMsg );
	m_port   = port;

	WSADATA wsa{};
	if ( WSAStartup( MAKEWORD( 2, 2 ), &wsa ) != 0 )
	{
		SetError( "WSAStartup failed" );
		m_running = false;
		return false;
	}
	m_wsaInited = true;

	SOCKET s = socket( AF_INET, SOCK_STREAM, IPPROTO_TCP );
	if ( s == INVALID_SOCKET )
	{
		SetError( "socket() failed" );
		WSACleanup(); m_wsaInited = false;
		m_running = false;
		return false;
	}

	BOOL reuse = TRUE;
	setsockopt( s, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof( reuse ) );

	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_port   = htons( port );
	if ( bindIp == "127.0.0.1" || bindIp == "localhost" || bindIp.empty() )
		addr.sin_addr.s_addr = htonl( INADDR_LOOPBACK );
	else if ( bindIp == "0.0.0.0" )
		addr.sin_addr.s_addr = htonl( INADDR_ANY );
	else
	{
		// Конкретный IP — принимаем только loopback, иначе INADDR_ANY (любой
		// интерфейс), чтобы slave с разной машины смог подключиться.
		addr.sin_addr.s_addr = htonl( INADDR_ANY );
	}

	if ( bind( s, (sockaddr*)&addr, sizeof( addr ) ) == SOCKET_ERROR )
	{
		int err = WSAGetLastError();
		char buf[128];
		snprintf( buf, sizeof( buf ), "bind() failed (port %u, WSA=%d)", (unsigned)port, err );
		SetError( buf );
		closesocket( s );
		WSACleanup(); m_wsaInited = false;
		m_running = false;
		return false;
	}
	if ( listen( s, 4 ) == SOCKET_ERROR )
	{
		SetError( "listen() failed" );
		closesocket( s );
		WSACleanup(); m_wsaInited = false;
		m_running = false;
		return false;
	}

	m_listenSock   = s;
	m_acceptThread = std::thread( &OrchestratorIpc::AcceptLoop, this );
	return true;
}

void OrchestratorIpc::Stop()
{
	if ( !m_running.exchange( false ) )
		return;

	if ( m_listenSock != INVALID_SOCKET )
	{
		closesocket( m_listenSock );
		m_listenSock = INVALID_SOCKET;
	}
	if ( m_acceptThread.joinable() )
		m_acceptThread.join();

	{
		std::lock_guard<std::mutex> lk( m_clientsMx );
		for ( SOCKET c : m_clients )
			if ( c != INVALID_SOCKET ) closesocket( c );
		m_clients.clear();
	}
	for ( auto& t : m_clientThreads )
		if ( t.joinable() ) t.join();
	m_clientThreads.clear();

	if ( m_wsaInited )
	{
		WSACleanup();
		m_wsaInited = false;
	}
	m_port = 0;
}

void OrchestratorIpc::AcceptLoop()
{
	while ( m_running.load() )
	{
		sockaddr_in cli{};
		int cliLen = sizeof( cli );
		SOCKET c = accept( m_listenSock, (sockaddr*)&cli, &cliLen );
		if ( c == INVALID_SOCKET )
		{
			if ( !m_running.load() ) break;
			Sleep( 50 );
			continue;
		}

		{
			std::lock_guard<std::mutex> lk( m_clientsMx );
			m_clients.push_back( c );
		}
		m_clientThreads.emplace_back( &OrchestratorIpc::ClientLoop, this, c );
	}
}

void OrchestratorIpc::ClientLoop( SOCKET c )
{
	std::string acc;
	acc.reserve( 4096 );
	char tmp[2048];

	while ( m_running.load() )
	{
		int n = recv( c, tmp, sizeof( tmp ), 0 );
		if ( n <= 0 ) break;
		acc.append( tmp, n );

		// Делим по '\n'
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
				if ( pm.type == "hb" )
				{
					m_lastPeerHbMs.store( ipc_proto::NowMs() );
				}
				else
				{
					m_lastPeerHbMs.store( ipc_proto::NowMs() ); // любой valid msg = живой
					if ( m_onMsg ) m_onMsg( pm );
				}
			}
			// invalid — silently drop (HMAC fail или skew > 10s).
		}
		if ( acc.size() > 256 * 1024 )
		{
			// pathological — drop
			break;
		}
	}

	closesocket( c );
	{
		std::lock_guard<std::mutex> lk( m_clientsMx );
		for ( auto it = m_clients.begin(); it != m_clients.end(); ++it )
		{
			if ( *it == c ) { m_clients.erase( it ); break; }
		}
	}
}

void OrchestratorIpc::Broadcast( const json& msg )
{
	if ( !m_running.load() ) return;
	std::string line = ipc_proto::Sign( msg, m_secret );

	std::lock_guard<std::mutex> lk( m_clientsMx );
	for ( SOCKET c : m_clients )
	{
		if ( c == INVALID_SOCKET ) continue;
		// Best-effort — send может вернуть < line.size(); в локальных условиях
		// (loopback или LAN, line ≤2KB) такого почти не бывает. Если случится —
		// клиент следующим recv'ом увидит мусор, drop'нет и пере-подключится.
		send( c, line.data(), (int)line.size(), 0 );
	}
}

bool OrchestratorIpc::IsConnected() const
{
	std::lock_guard<std::mutex> lk( m_clientsMx );
	return !m_clients.empty();
}

int OrchestratorIpc::ClientCount() const
{
	std::lock_guard<std::mutex> lk( m_clientsMx );
	return (int)m_clients.size();
}

int64_t OrchestratorIpc::LastPeerHbMs() const
{
	return m_lastPeerHbMs.load();
}
