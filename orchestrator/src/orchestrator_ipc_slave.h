#pragma once

// SlavePeer — outbound TCP клиент к master OrchestratorIpc.
// Background thread: connect → recv-loop → on EOF: backoff reconnect.
// Heartbeat каждые 3 сек. Сообщения подписываются тем же HMAC что в master.
//
// Reconnect backoff: 1500 → 3000 → 6000 → 12000 → 12000 (capped).

#include <Windows.h>
#include <winsock2.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include "match_pairing_fsm.h"  // PeerMsg

class SlavePeer
{
public:
	SlavePeer();
	~SlavePeer();

	bool Start( const std::string& host, uint16_t port, const std::string& secret,
		std::function<void( const PeerMsg& )> onMsg );
	void Stop();

	// Синхронно (best-effort) подписать и отправить msg.
	void Send( const nlohmann::json& msg );

	bool        IsConnected() const { return m_connected.load(); }
	int64_t     LastPeerHbMs() const { return m_lastPeerHbMs.load(); }
	std::string LastError() const;

private:
	void RunLoop();

	std::string                     m_host;
	uint16_t                        m_port = 0;
	std::string                     m_secret;
	std::function<void( const PeerMsg& )> m_onMsg;

	std::atomic<bool>               m_running{ false };
	std::atomic<bool>               m_connected{ false };
	std::atomic<int64_t>            m_lastPeerHbMs{ 0 };

	std::thread                     m_thread;

	// m_sock защищён m_sendMx (использует Send, RunLoop меняет когда reconnect'ит).
	SOCKET                          m_sock = INVALID_SOCKET;
	mutable std::mutex              m_sendMx;

	mutable std::mutex              m_errMx;
	std::string                     m_lastError;

	bool                            m_wsaInited = false;
};
