package main

import (
	"encoding/json"
	"errors"
	"log/slog"
	"net"
	"sync"
	"time"
)

const outboxCap = 256

type Peer struct {
	Conn        net.Conn
	Role        string
	UserID      string
	LastHbMs    int64
	Outbox      chan []byte
	closed      bool
	connectedMs int64
	messages    uint64
	bytes       uint64
}

type Pair struct {
	Key       string // "<user_id>:<pair_id>"
	UserID    string
	PairID    string
	Master    *Peer
	Slave     *Peer
	Buffer    [][]byte
	bufferCap int
	Mx        sync.Mutex
}

type PairManager struct {
	mu             sync.Mutex
	pairs          map[string]*Pair
	maxPairsGlobal int
	bufferCap      int
	users          *UserDB
	log            *slog.Logger
}

func NewPairManager(maxPairsGlobal, bufferCap int, users *UserDB, log *slog.Logger) *PairManager {
	return &PairManager{
		pairs:          make(map[string]*Pair),
		maxPairsGlobal: maxPairsGlobal,
		bufferCap:      bufferCap,
		users:          users,
		log:            log,
	}
}

// PairKey scoped: <user_id>:<pair_id>
func PairKey(userID, pairID string) string {
	return userID + ":" + pairID
}

func (m *PairManager) Register(userID, pairID, role string, conn net.Conn) (*Pair, *Peer, error) {
	m.mu.Lock()
	defer m.mu.Unlock()

	key := PairKey(userID, pairID)
	pair, ok := m.pairs[key]
	if !ok {
		users := m.users.Snapshot()
		uconf, exists := users[userID]
		if !exists {
			return nil, nil, errUnknownUser
		}
		state := m.users.State(userID)
		if state != nil {
			state.mu.Lock()
			activeUserPairs := state.pairsActive
			state.mu.Unlock()
			if uconf.MaxPairs > 0 && activeUserPairs >= uconf.MaxPairs {
				return nil, nil, errMaxPairs
			}
		}
		if m.maxPairsGlobal > 0 && len(m.pairs) >= m.maxPairsGlobal {
			return nil, nil, errMaxPairsGlobal
		}
		pair = &Pair{
			Key:       key,
			UserID:    userID,
			PairID:    pairID,
			bufferCap: m.bufferCap,
		}
		m.pairs[key] = pair
		if state != nil {
			state.mu.Lock()
			state.pairsActive++
			state.mu.Unlock()
		}
	}

	pair.Mx.Lock()
	defer pair.Mx.Unlock()

	now := time.Now().UnixMilli()
	peer := &Peer{
		Conn:        conn,
		Role:        role,
		UserID:      userID,
		LastHbMs:    now,
		Outbox:      make(chan []byte, outboxCap),
		connectedMs: now,
	}

	switch role {
	case "master":
		if pair.Master != nil {
			closePeer(pair.Master)
			m.log.Info("peer replaced", "user_id", userID, "pair_id", pairID, "role", "master")
		}
		pair.Master = peer
	case "slave":
		if pair.Slave != nil {
			closePeer(pair.Slave)
			m.log.Info("peer replaced", "user_id", userID, "pair_id", pairID, "role", "slave")
		}
		pair.Slave = peer
	default:
		return nil, nil, errors.New("invalid role")
	}

	if pair.Master != nil && pair.Slave != nil {
		m.log.Info("pair complete", "user_id", userID, "pair_id", pairID)
	}

	if len(pair.Buffer) > 0 {
		flushed := 0
		for _, line := range pair.Buffer {
			select {
			case peer.Outbox <- line:
				flushed++
			default:
			}
		}
		pair.Buffer = pair.Buffer[:0]
		if flushed > 0 {
			m.log.Info("buffer flushed", "user_id", userID, "pair_id", pairID, "role", role, "messages", flushed)
		}
	}

	return pair, peer, nil
}

func (m *PairManager) Unregister(pair *Pair, role string, peer *Peer) {
	pair.Mx.Lock()
	switch role {
	case "master":
		if pair.Master == peer {
			pair.Master = nil
		}
	case "slave":
		if pair.Slave == peer {
			pair.Slave = nil
		}
	}
	closePeer(peer)
	empty := pair.Master == nil && pair.Slave == nil
	pair.Mx.Unlock()

	if empty {
		m.mu.Lock()
		if cur, ok := m.pairs[pair.Key]; ok && cur == pair {
			delete(m.pairs, pair.Key)
			if state := m.users.State(pair.UserID); state != nil {
				state.mu.Lock()
				if state.pairsActive > 0 {
					state.pairsActive--
				}
				state.mu.Unlock()
			}
			m.log.Info("pair removed", "user_id", pair.UserID, "pair_id", pair.PairID)
		}
		m.mu.Unlock()
	}
}

func (m *PairManager) ReapStale(cutoffMs int64) {
	m.mu.Lock()
	pairs := make([]*Pair, 0, len(m.pairs))
	for _, p := range m.pairs {
		pairs = append(pairs, p)
	}
	m.mu.Unlock()

	for _, p := range pairs {
		p.Mx.Lock()
		if p.Master != nil && p.Master.LastHbMs < cutoffMs {
			m.log.Info("reap stale", "user_id", p.UserID, "pair_id", p.PairID, "role", "master")
			closePeer(p.Master)
			p.Master = nil
		}
		if p.Slave != nil && p.Slave.LastHbMs < cutoffMs {
			m.log.Info("reap stale", "user_id", p.UserID, "pair_id", p.PairID, "role", "slave")
			closePeer(p.Slave)
			p.Slave = nil
		}
		p.Mx.Unlock()
	}
}

func (m *PairManager) KickUser(userID string) {
	m.mu.Lock()
	pairs := make([]*Pair, 0)
	for _, p := range m.pairs {
		if p.UserID == userID {
			pairs = append(pairs, p)
		}
	}
	m.mu.Unlock()
	for _, p := range pairs {
		p.Mx.Lock()
		if p.Master != nil {
			closePeer(p.Master)
			p.Master = nil
		}
		if p.Slave != nil {
			closePeer(p.Slave)
			p.Slave = nil
		}
		p.Mx.Unlock()
	}
	m.log.Info("user kicked", "user_id", userID, "pairs", len(pairs))
}

func (m *PairManager) Stats() (totalPairs int) {
	m.mu.Lock()
	defer m.mu.Unlock()
	return len(m.pairs)
}

func (m *PairManager) PairsByUser() map[string][]string {
	m.mu.Lock()
	defer m.mu.Unlock()
	out := make(map[string][]string)
	for _, p := range m.pairs {
		out[p.UserID] = append(out[p.UserID], p.PairID)
	}
	return out
}

type msgHeader struct {
	Type string `json:"type"`
}

func (p *Pair) OnMessage(fromRole string, line []byte, user *User, state *UserState, log *slog.Logger) bool {
	p.Mx.Lock()
	defer p.Mx.Unlock()

	var h msgHeader
	if err := json.Unmarshal(stripNewline(line), &h); err != nil {
		return false
	}

	now := time.Now().UnixMilli()

	if h.Type == "hb" {
		switch fromRole {
		case "master":
			if p.Master != nil {
				p.Master.LastHbMs = now
			}
		case "slave":
			if p.Slave != nil {
				p.Slave.LastHbMs = now
			}
		}
		return true
	}

	if state != nil && user != nil {
		msgSum := state.msgWindow.Add(1)
		byteSum := state.bytesWindow.Add(int64(len(line)))
		if user.MaxMsgPerMin > 0 && msgSum > int64(user.MaxMsgPerMin) {
			state.mu.Lock()
			state.quotaExceeded++
			state.mu.Unlock()
			log.Warn("quota exceeded", "user_id", p.UserID, "reason", "msg_per_min",
				"limit", user.MaxMsgPerMin, "actual", msgSum)
			return false
		}
		if user.MaxBytesPerMin > 0 && byteSum > int64(user.MaxBytesPerMin) {
			state.mu.Lock()
			state.quotaExceeded++
			state.mu.Unlock()
			log.Warn("quota exceeded", "user_id", p.UserID, "reason", "bytes_per_min",
				"limit", user.MaxBytesPerMin, "actual", byteSum)
			return false
		}
	}

	switch fromRole {
	case "master":
		if p.Master != nil {
			p.Master.LastHbMs = now
			p.Master.messages++
			p.Master.bytes += uint64(len(line))
		}
	case "slave":
		if p.Slave != nil {
			p.Slave.LastHbMs = now
			p.Slave.messages++
			p.Slave.bytes += uint64(len(line))
		}
	}

	var dest *Peer
	if fromRole == "master" {
		dest = p.Slave
	} else {
		dest = p.Master
	}

	if dest == nil {
		if len(p.Buffer) >= p.bufferCap {
			p.Buffer = p.Buffer[1:]
			log.Warn("buffer full", "user_id", p.UserID, "pair_id", p.PairID)
		}
		p.Buffer = append(p.Buffer, append([]byte(nil), line...))
		log.Debug("buffered for absent peer", "user_id", p.UserID, "pair_id", p.PairID,
			"from", fromRole, "type", h.Type, "buf", len(p.Buffer))
		return true
	}

	select {
	case dest.Outbox <- line:
	default:
		log.Warn("outbox full", "user_id", p.UserID, "pair_id", p.PairID, "role", dest.Role)
	}
	return true
}

func closePeer(peer *Peer) {
	if peer == nil || peer.closed {
		return
	}
	peer.closed = true
	close(peer.Outbox)
	_ = peer.Conn.Close()
}

func stripNewline(b []byte) []byte {
	if len(b) > 0 && b[len(b)-1] == '\n' {
		return b[:len(b)-1]
	}
	return b
}

var (
	errMaxPairs       = errors.New("max pairs per user reached")
	errMaxPairsGlobal = errors.New("max global pairs reached")
)

func errCodePair(err error) string {
	switch {
	case errors.Is(err, errMaxPairs), errors.Is(err, errMaxPairsGlobal):
		return "max_pairs"
	case errors.Is(err, errUnknownUser):
		return "unknown_user"
	default:
		return "auth_failed"
	}
}
