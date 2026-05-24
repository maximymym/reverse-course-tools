#pragma once

// OrchestratorIpc — TCP master-listener для парного 5v5 self-play.
// Принимает соединения от SlavePeer, обменивается line-delimited JSON
// сообщениями подписанными HMAC-SHA256 (shared secret). Каждое валидное
// сообщение прокидывается в onMsg callback (parsed PeerMsg).
//
// Wire protocol — одна строка на сообщение:
//   {"type":"match_found","ts":<epoch_ms>,"body":{...},"sig":"<hex hmac>"}\n
// HMAC считается над "<type>|<ts>|<body json (compact)>".
//
// Multi-client: master может принять до 4 slave'ов; broadcast идёт всем.
// (Для классической пары достаточно 1, но overhead невелик.)

#include <Windows.h>
#include <winsock2.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "match_pairing_fsm.h"  // PeerMsg

class OrchestratorIpc
{
public:
	OrchestratorIpc();
	~OrchestratorIpc();

	// bindIp == "127.0.0.1" → INADDR_LOOPBACK; иначе INADDR_ANY (cross-host).
	// secret — shared HMAC ключ (≥16 ASCII символов рекомендуется).
	bool Start( uint16_t port, const std::string& bindIp, const std::string& secret,
		std::function<void( const PeerMsg& )> onMsg );
	void Stop();

	// Подписать и разослать всем connected client'ам. Если ни одного нет — no-op.
	void Broadcast( const nlohmann::json& msg );

	bool    IsConnected() const;            // ≥1 alive client
	int     ClientCount() const;
	int64_t LastPeerHbMs() const;           // epoch ms последнего hb от любого client
	std::string LastError() const;
	uint16_t Port() const { return m_port; }

private:
	void AcceptLoop();
	void ClientLoop( SOCKET c );
	void SetError( const std::string& e );

	SOCKET                          m_listenSock = INVALID_SOCKET;
	uint16_t                        m_port       = 0;
	std::vector<SOCKET>             m_clients;
	mutable std::mutex              m_clientsMx;
	std::vector<std::thread>        m_clientThreads;
	std::thread                     m_acceptThread;
	std::atomic<bool>               m_running{ false };
	std::string                     m_secret;
	std::function<void( const PeerMsg& )> m_onMsg;
	std::atomic<int64_t>            m_lastPeerHbMs{ 0 };
	mutable std::mutex              m_errMx;
	std::string                     m_lastError;
	bool                            m_wsaInited = false;
};

// ── Helpers (shared с slave) ────────────────────────────────────────────
namespace ipc_proto
{

// HMAC-SHA256, hex lowercase. Inline impl (нет внешних deps).
std::string HmacSha256Hex( const std::string& key, const std::string& data );

// Собрать wire-line: serialize msg с type/ts/body/sig полями.
// msg должен быть object с "type" и "body" (либо без body — тогда {} ).
// Если "ts" отсутствует — выставится текущим epoch_ms.
std::string Sign( const nlohmann::json& msg, const std::string& secret );

// Распарсить wire-line. Возвращает true если HMAC valid и timestamp delta < 10s.
// Заполняет out с type/body. nowMs — текущее system time для проверки skew.
bool Verify( const std::string& line, const std::string& secret, int64_t nowMs, PeerMsg& out );

int64_t NowMs();

} // namespace ipc_proto
