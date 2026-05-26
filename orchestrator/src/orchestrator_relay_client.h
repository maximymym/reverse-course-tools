#pragma once

// RelayPeer — outbound TCP клиент к relay-сервису на VPS (NAT-traversal).
// Концептуально похож на SlavePeer (background thread, recv-loop, exp backoff,
// 3s heartbeat), но имеет ТРИ ключевых отличия:
//
//   1. Hello-handshake. Сразу после connect клиент шлёт hello message с
//      multi-tenant credentials:
//        {"type":"hello","body":{
//          "user_id":     "<выдан admin'ом>",
//          "auth_token":  "<32+ char>",
//          "pair_id":     "<имя пары в namespace user_id>",
//          "role":        "master"|"slave"
//        }}
//      Relay проверяет user_id+auth_token по своей user db. Если auth fail —
//      шлёт {"type":"error","body":{"code":"auth_failed",...}} и закрывает
//      соединение. Pair_id scoped в namespace user_id (две разные пары могут
//      иметь одинаковый pair_id если они у разных users).
//      HMAC (pairSecret) — отдельный механизм для подписи сообщений между
//      master и slave; relay его НЕ знает и НЕ верифицирует.
//
//   2. Пара ролей master/slave симметрична — оба клиента используют RelayPeer.
//      В direct-режиме master поднимает listener (OrchestratorIpc), а slave
//      коннектится (SlavePeer). В relay-режиме оба коннектятся к relay'ю.
//
//   3. Adaptive backoff на error от relay'я. Если relay вернул "auth_failed" /
//      "unknown_user" / "user_disabled" — backoff поднимается до 60s (нет
//      смысла спамить reconnect — нужно вмешательство юзера). Прочие коды
//      (max_pairs / quota_exceeded / malformed_hello) — 30s.
//
// Heartbeat (`{"type":"hb"}`) шлётся каждые 3s. Relay их НЕ пересылает второму
// клиенту (он трэкает их сам для liveness своего состояния).
//
// HMAC (sign/verify) — обязателен (relay не trusted), переиспользуется
// ipc_proto::Sign / ipc_proto::Verify из orchestrator_ipc.

#include <Windows.h>
#include <winsock2.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include "match_pairing_fsm.h"  // PeerMsg

class RelayPeer
{
public:
	enum class State
	{
		Disconnected,
		Connecting,
		Connected,
		AuthFailed   // sticky после auth_failed/unknown_user/user_disabled
	};

	struct Snapshot
	{
		State       state;
		bool        connected;
		int64_t     lastActivityMs;
		int64_t     lastRttMs;       // -1 если ещё не было pong
		uint64_t    msgSent;
		uint64_t    msgRecv;
		std::string lastError;
		std::string lastRelayErrorCode;
	};

	RelayPeer();
	~RelayPeer();

	// host/port — relay endpoint.
	// userId/userAuthToken — multi-tenant credentials выданные admin'ом relay'я.
	// pairId — имя пары в namespace user_id.
	// role — "master" | "slave".
	// secret — общий HMAC ключ для master ↔ slave (relay его не видит).
	// onState — optional callback на изменения State (GUI), может быть nullptr.
	bool Start( const std::string& host, uint16_t port,
		const std::string& userId, const std::string& userAuthToken,
		const std::string& pairId, const std::string& role,
		const std::string& secret,
		std::function<void( const PeerMsg& )> onMsg,
		std::function<void( State )> onState = nullptr );
	void Stop();

	// Подписать и отправить msg. Best-effort: если socket closed — silent no-op
	// (recv loop увидит EOF на следующем select и пере-connect'ится).
	void Send( const nlohmann::json& msg );

	bool        IsConnected() const { return m_connected.load(); }
	// Last activity from relay pipe (любое валидное сообщение, включая ack от
	// peer'а). В отличие от SlavePeer::LastPeerHbMs которое означает "peer
	// живой", здесь это "relay-pipe живой" — peer может временно отвалиться,
	// relay продолжит heartbeat-tracking сам.
	int64_t     LastPeerActivityMs() const { return m_lastActivityMs.load(); }
	std::string LastError() const;

	// Последний код ошибки полученный от relay'я ("" если ошибок не было либо
	// ошибка не auth-related). Используется GUI'ем для индикации auth_failed.
	std::string LastRelayErrorCode() const;

	// Принудительный reconnect: bypass backoff (одноразово). Закрывает текущий
	// сокет (recv loop увидит ошибку → ре-коннект без backoff'а).
	void RequestReconnect();

	// Атомарный snapshot всех telemetry-полей. GUI вызывает 5-10Hz.
	Snapshot GetSnapshot() const;

private:
	void RunLoop();
	bool SendHello( SOCKET s );
	void SetState( State s );

	std::string                     m_host;
	uint16_t                        m_port = 0;
	std::string                     m_userId;
	std::string                     m_userAuthToken;
	std::string                     m_pairId;
	std::string                     m_role;
	std::string                     m_secret;
	std::function<void( const PeerMsg& )> m_onMsg;
	std::function<void( State )>          m_onState;

	std::atomic<bool>               m_running{ false };
	std::atomic<bool>               m_connected{ false };
	std::atomic<int64_t>            m_lastActivityMs{ 0 };

	// Telemetry counters (incremented в Send / recv loop).
	std::atomic<uint64_t>           m_msgSent{ 0 };
	std::atomic<uint64_t>           m_msgRecv{ 0 };

	// RTT: m_lastRttMs = -1 пока нет pong'а. m_lastPingSentMs — timestamp
	// последнего нашего ping'а (для idle-cadence в RunLoop).
	std::atomic<int64_t>            m_lastRttMs{ -1 };
	std::atomic<int64_t>            m_lastPingSentMs{ 0 };

	// State + force-reconnect flag.
	std::atomic<State>              m_state{ State::Disconnected };
	std::atomic<bool>               m_forceReconnect{ false };

	std::thread                     m_thread;

	// m_sock защищён m_sendMx (Send() читает; RunLoop меняет на reconnect).
	SOCKET                          m_sock = INVALID_SOCKET;
	mutable std::mutex              m_sendMx;

	mutable std::mutex              m_errMx;
	std::string                     m_lastError;
	std::string                     m_lastRelayErrorCode;  // "auth_failed" | "" | ...

	// Adaptive backoff: устанавливается когда relay вернул error message.
	// При следующей итерации reconnect-loop'а это значение используется как
	// sleep вместо обычного exp backoff. После использования сбрасывается.
	std::atomic<int>                m_authFailureBackoffMs{ 0 };

	bool                            m_wsaInited = false;
};
