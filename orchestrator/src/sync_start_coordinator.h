#pragma once

// SyncStartCoordinator — отдельный FSM для synchronized START FARM handshake
// между master и slave orchestrator'ом. Match-FSM (MatchPairingFsm) не
// затрагивается: эти msg types (start_request / start_ack / start_cancel /
// ping / pong) MatchPairingFsm пропускает через onSyncStartMsg callback в
// SyncStartCoordinator::OnPeerMessage.
//
// State diagram:
//
//   IDLE ──Initiate()──▶ WAITING_PEER_ACK
//                          │ OnPeerMessage(start_ack accept) ──▶ STARTING ──▶ IDLE (after startFn)
//                          │ OnPeerMessage(start_ack decline) ──▶ DECLINED ──(2s toast)──▶ IDLE
//                          │ Tick() ackDeadlineMs hit         ──▶ TIMEOUT  ──(2s toast)──▶ IDLE
//                          │ UserCancel()                     ──▶ broadcast(start_cancel) + IDLE
//                          │ OnPeerMessage(start_request)     ──▶ tie-break by lexicographic
//                          │                                       request_id; loser sends
//                          │                                       start_cancel(race_lost) и
//                          │                                       transition в PEER_REQUESTED.
//                          │
//   IDLE ──OnPeerMessage(start_request)──▶ PEER_REQUESTED
//                          │ UserAccept()                     ──▶ broadcast(start_ack accept)
//                          │                                       + CONFIRMING ──▶ STARTING
//                          │ UserDecline(reason)              ──▶ broadcast(start_ack decline)
//                          │                                       + DECLINED ──▶ IDLE
//                          │ Tick() userResponseTimeoutMs hit ──▶ start_ack decline (no_response)
//                          │                                       + DECLINED
//
//   STARTING:
//     startFarmFn() invoked exactly once при transition.
//     После 1s в этом state → IDLE (caller actual farm boot выполняется
//     синхронно в startFarmFn от caller'a thread'а, не из coordinator'а).
//
//   Reset(reason):
//     Из любого state → IDLE с m_lastDeclineReason=reason.
//     Используется например при peer disconnect.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include <json.hpp>

enum class SyncStartState
{
	IDLE,
	WAITING_PEER_ACK,   // мы отправили start_request, ждём ack
	PEER_REQUESTED,     // peer отправил start_request, ждём user accept/decline
	CONFIRMING,         // user accepted, ack sent, ждём STARTING confirmation
	STARTING,           // handshake завершён — startFarmFn вызвана
	DECLINED,           // peer declined OR мы declined → toast → return IDLE
	TIMEOUT             // ack timeout / user no-response → toast → return IDLE
};

struct SyncStartSnapshot
{
	SyncStartState state              = SyncStartState::IDLE;
	std::string    requestId;
	int64_t        enteredStateMs     = 0;
	int64_t        ackDeadlineMs      = 0;   // when timeout fires (0 если N/A)
	std::string    peerRole;                 // initiator's role from request
	std::string    lastDeclineReason;
};

class SyncStartCoordinator
{
public:
	// Callbacks (заполнять извне до Initiate()):
	//   broadcast    — отправить json peer'у через RelayPeer/OrchestratorIpc/SlavePeer
	//   startFarmFn  — вызывается ровно один раз при transition в STARTING
	//   onStateChange — для GUI notification (опционально, под mutex'ом не вызывается)
	std::function<void(const nlohmann::json&)> broadcast;
	std::function<void()>                      startFarmFn;
	std::function<void(SyncStartState)>        onStateChange;

	// Configuration:
	int ackTimeoutMs          = 15000;  // initiator: peer ack timeout
	int userResponseTimeoutMs = 30000;  // responder: user accept/decline deadline
	int startingHoldMs        = 1000;   // STARTING → IDLE задержка (после startFarmFn)
	int declinedToastMs       = 2000;   // DECLINED → IDLE задержка (toast lifetime)
	int timeoutToastMs        = 2000;   // TIMEOUT  → IDLE задержка

	// ── User actions ─────────────────────────────────────────────────

	// Initiate: user clicked START FARM. Возвращает false если SyncStart != IDLE.
	// myRole / configHash едут в start_request body для context'а.
	bool Initiate( const std::string& myRole, const std::string& configHash );

	// Initiator's CANCEL button (отменить свой start_request).
	void UserCancel();

	// Responder's ACCEPT button: PEER_REQUESTED → CONFIRMING → ack send → STARTING.
	void UserAccept();

	// Responder's DECLINE button.
	void UserDecline( const std::string& reason );

	// ── Peer message handler ─────────────────────────────────────────

	// Routed из MatchPairingFsm::onSyncStartMsg или напрямую.
	// msgType: "start_request" | "start_ack" | "start_cancel" | "ping" | "pong"
	void OnPeerMessage( const std::string& msgType, const nlohmann::json& body );

	// ── Lifecycle ────────────────────────────────────────────────────

	// Tick from MonitorTick (10Hz OK) — проверяет timeouts + STARTING/DECLINED hold.
	// Возвращает true если state поменялся в этот tick.
	bool Tick( int64_t nowMs );

	// Force reset (peer disconnect / farm stop).
	void Reset( const std::string& reason );

	// GUI snapshot.
	SyncStartSnapshot GetSnapshot() const;

private:
	mutable std::mutex m_mx;
	SyncStartState     m_state              = SyncStartState::IDLE;
	std::string        m_requestId;
	std::string        m_myRole;
	std::string        m_peerRole;
	std::string        m_configHash;
	int64_t            m_enteredStateMs     = 0;
	int64_t            m_ackDeadlineMs      = 0;
	std::string        m_lastDeclineReason;

	// Pending callbacks выполняются ПОСЛЕ release mutex'а (чтобы избежать
	// re-entry в OnPeerMessage / UserAccept из broadcast lambda). Broadcasts
	// — vector чтобы race-loss path мог накопить cancel + state-transition msg.
	struct PendingActions
	{
		bool                          fireStateChange = false;
		SyncStartState                stateForCb      = SyncStartState::IDLE;
		bool                          fireStartFarm   = false;
		std::vector<nlohmann::json>   broadcasts;
	};

	// TransitionTo_ требует m_mx. Заполняет actions для после-release execution.
	void TransitionTo_( SyncStartState s, int64_t nowMs, PendingActions& actions );

	// Helper: now in ms (steady or system; используем steady).
	static int64_t NowMs_();

	// Generate request_id (lex-comparable: zero-padded epoch ms + random hex).
	std::string GenerateRequestId_() const;

	// Execute actions outside m_mx.
	void Flush_( PendingActions& actions );
};
