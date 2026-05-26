#pragma once

// MatchPairingFsm — координирует обмен lobby_id'ами между двумя orchestrator-парами
// (master + slave) в self-play 5v5.
//
// Workflow:
//   1. DLL каждого бота пишет match_pending_<pid>.json когда GC отдал lobbyId.
//   2. Orchestrator сканит эти файлы, вызывает OnMatchPendingFile(idx, lobby_id).
//   3. Когда у локальной стороны набралось N (==botCount) одинаковых lobby_id —
//      onBroadcast({type:"match_found", lobby_ids:[...], local_majority:X}).
//   4. Получаем peer-сообщение с его 5 lobby_ids → объединяем 10 → если все
//      идентичны → DECISION=ACCEPT(lobbyId), иначе DECISION=CANCEL.
//   5. Tick(now) проверяет timeout WAIT_PEER → CANCEL.
//   6. Caller (orchestrator) подхватывает decision из Tick(), пишет ready_up /
//      cancel_queue команды во все боты, потом дёргает OnDecisionApplied().

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include <json.hpp>

struct PeerMsg
{
	std::string    type;   // "hb" | "match_found" | "decision" | "match_result"
	nlohmann::json body;   // полное тело (после verify HMAC)
};

enum class PairingPhase
{
	IDLE,           // ничего не происходит
	LOCAL_FOUND,    // локальные 5 botов отдали свои lobby_id, broadcast'нули peer'у
	WAIT_PEER,      // ждём peer-сообщение match_found
	DECIDED_ACCEPT, // решили accept, ожидаем OnDecisionApplied
	DECIDED_CANCEL  // решили cancel, ожидаем OnDecisionApplied
};

struct PairingDecision
{
	enum Kind { NONE, ACCEPT, CANCEL };
	Kind        kind     = NONE;
	uint64_t    lobbyId  = 0;
	std::string reason;        // для лога ("peer_timeout" / "lobby_mismatch" / "agreed")
};

class MatchPairingFsm
{
public:
	void Init( int matchSyncTimeoutS, int maxConsecutiveCancels, int botCount );

	// Bot idx (0..N-1) написал match_pending_<pid>.json с этим lobbyId.
	// idempotent: повтор того же idx + lobby не меняет состояние.
	void OnMatchPendingFile( int botIdx, uint64_t lobbyId );

	// Прилетело сообщение от peer (master ↔ slave).
	void OnPeerMessage( const PeerMsg& m );

	// Вызывать каждый MonitorTick. Возвращает decision когда FSM перешёл в
	// DECIDED_*; повторные вызовы возвращают NONE до OnDecisionApplied().
	PairingDecision Tick( uint64_t nowMs );

	// Caller сообщает что он применил decision (записал ready_up/cancel_queue
	// во все боты). FSM сбрасывается к IDLE и готов к следующему циклу.
	void OnDecisionApplied( const PairingDecision& d );

	PairingPhase phase() const;
	int          CancelStreak() const;
	void         ResetCancelStreak();

	// Полный сброс (например, при остановке фермы).
	void Reset();

	// onBroadcast — callback от orchestrator'а, который шлёт msg либо через
	// IPC (master) либо через slave-peer. Передаётся nlohmann::json уже
	// в виде объекта (без HMAC — обёртка делается ниже на TCP layer).
	std::function<void(const nlohmann::json&)> onBroadcast;

	// onSyncStartMsg — passthrough callback для msg types, которые относятся
	// к synchronized START FARM handshake (SyncStartCoordinator). Match-FSM их
	// не обрабатывает (см. OnPeerMessage). Устанавливается извне (orchestrator
	// wires в SyncStartCoordinator::OnPeerMessage).
	std::function<void(const PeerMsg&)> onSyncStartMsg;

	// Snapshot для GUI / лога.
	struct DebugStatus
	{
		PairingPhase phase = PairingPhase::IDLE;
		int          localFilled = 0;        // сколько ботов отдали lobby
		int          localTotal = 0;
		uint64_t     localMajorityLobby = 0; // 0 если ещё не сошлось
		int          peerCount = 0;          // сколько lobby пришло от peer
		uint64_t     waitDeadlineMs = 0;
		int          cancelStreak = 0;
	};
	DebugStatus GetDebug() const;

private:
	mutable std::mutex m_mx;
	PairingPhase m_phase = PairingPhase::IDLE;

	std::map<int, uint64_t> m_localPending;   // botIdx → lobbyId
	std::vector<uint64_t>   m_peerLobbyIds;
	uint64_t                m_waitDeadlineMs = 0;
	uint64_t                m_localMajority = 0;

	int  m_matchSyncTimeoutS    = 15;
	int  m_botCount             = 5;
	int  m_cancelStreak         = 0;
	int  m_maxConsecutiveCancels = 3;

	PairingDecision m_pendingDecision;  // выгружается через Tick()
	bool            m_decisionConsumed = true;

	// Helpers (require m_mx held).
	uint64_t ComputeLocalMajority() const;
	bool     AllLocalAgree() const;
	bool     CombinedAgree( uint64_t lobby ) const;
	void     EmitDecision( PairingDecision::Kind k, uint64_t lobby, const std::string& reason );
};
