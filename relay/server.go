package main

import (
	"bufio"
	"encoding/json"
	"errors"
	"io"
	"log/slog"
	"net"
	"sync"
	"sync/atomic"
	"time"

	"golang.org/x/time/rate"
)

type Config struct {
	Bind                 string
	AdminBind            string
	AdminToken           string
	UsersFile            string
	UsersReloadIntervalS int
	BufferPerPair        int
	MaxPairsGlobal       int
	PeerTimeoutS         int
	HelloDeadlineS       int
	MaxLineBytes         int
	ConnectRatePerMin    int
}

type Server struct {
	cfg     Config
	mgr     *PairManager
	users   *UserDB
	log     *slog.Logger
	ln      net.Listener
	stopped atomic.Bool
	startMs int64

	ipMu       sync.Mutex
	ipLimiters map[string]*rate.Limiter

	connRejectAuth     atomic.Uint64
	connRejectMaxPairs atomic.Uint64
	connRejectRate     atomic.Uint64
	connRejectHello    atomic.Uint64
	connTotal          atomic.Uint64
}

func NewServer(cfg Config, users *UserDB, log *slog.Logger) *Server {
	mgr := NewPairManager(cfg.MaxPairsGlobal, cfg.BufferPerPair, users, log)
	s := &Server{
		cfg:        cfg,
		mgr:        mgr,
		users:      users,
		log:        log,
		ipLimiters: make(map[string]*rate.Limiter),
		startMs:    time.Now().UnixMilli(),
	}
	users.SetDisabledKick(mgr.KickUser)
	return s
}

func (s *Server) Manager() *PairManager { return s.mgr }
func (s *Server) Users() *UserDB        { return s.users }
func (s *Server) StartMs() int64        { return s.startMs }

func (s *Server) Counters() map[string]uint64 {
	return map[string]uint64{
		"auth_failed":  s.connRejectAuth.Load(),
		"max_pairs":    s.connRejectMaxPairs.Load(),
		"rate_limit":   s.connRejectRate.Load(),
		"bad_hello":    s.connRejectHello.Load(),
		"total_accept": s.connTotal.Load(),
	}
}

func (s *Server) Run() error {
	ln, err := net.Listen("tcp", s.cfg.Bind)
	if err != nil {
		return err
	}
	s.ln = ln
	s.log.Info("relay listening", "bind", s.cfg.Bind)

	go s.reaperLoop()

	for {
		c, err := ln.Accept()
		if err != nil {
			if s.stopped.Load() {
				return nil
			}
			s.log.Warn("accept error", "err", err.Error())
			continue
		}
		s.connTotal.Add(1)
		go s.handleConn(c)
	}
}

func (s *Server) Stop() {
	s.stopped.Store(true)
	if s.ln != nil {
		_ = s.ln.Close()
	}
}

type helloMsg struct {
	Type string `json:"type"`
	Body struct {
		UserID    string `json:"user_id"`
		AuthToken string `json:"auth_token"`
		PairID    string `json:"pair_id"`
		Role      string `json:"role"`
	} `json:"body"`
}

type errorBody struct {
	Code    string `json:"code"`
	Message string `json:"message"`
}

type errorMsg struct {
	Type string    `json:"type"`
	Ts   int64     `json:"ts"`
	Body errorBody `json:"body"`
}

func (s *Server) sendError(c net.Conn, code, msg string) {
	em := errorMsg{
		Type: "error",
		Ts:   time.Now().UnixMilli(),
		Body: errorBody{Code: code, Message: msg},
	}
	b, _ := json.Marshal(em)
	_ = c.SetWriteDeadline(time.Now().Add(2 * time.Second))
	_, _ = c.Write(append(b, '\n'))
}

func (s *Server) ipLimiter(ip string) *rate.Limiter {
	s.ipMu.Lock()
	defer s.ipMu.Unlock()
	l, ok := s.ipLimiters[ip]
	if !ok {
		perSec := float64(s.cfg.ConnectRatePerMin) / 60.0
		l = rate.NewLimiter(rate.Limit(perSec), s.cfg.ConnectRatePerMin)
		s.ipLimiters[ip] = l
	}
	return l
}

func clientIP(c net.Conn) string {
	addr := c.RemoteAddr().String()
	if h, _, err := net.SplitHostPort(addr); err == nil {
		return h
	}
	return addr
}

func (s *Server) handleConn(c net.Conn) {
	remote := c.RemoteAddr().String()
	ip := clientIP(c)

	if s.cfg.ConnectRatePerMin > 0 {
		if !s.ipLimiter(ip).Allow() {
			s.connRejectRate.Add(1)
			s.log.Info("rate limit reject", "remote", remote)
			_ = c.Close()
			return
		}
	}

	defer func() { _ = c.Close() }()

	_ = c.SetReadDeadline(time.Now().Add(time.Duration(s.cfg.HelloDeadlineS) * time.Second))

	r := bufio.NewReaderSize(c, s.cfg.MaxLineBytes)
	line, err := readLine(r, s.cfg.MaxLineBytes)
	if err != nil {
		s.connRejectHello.Add(1)
		s.log.Info("hello read error", "remote", remote, "err", err.Error())
		return
	}

	var hello helloMsg
	if err := json.Unmarshal(line, &hello); err != nil {
		s.connRejectHello.Add(1)
		s.sendError(c, "malformed_hello", "invalid JSON")
		s.log.Info("hello parse error", "remote", remote, "err", err.Error())
		return
	}
	if hello.Type != "hello" || hello.Body.UserID == "" || hello.Body.AuthToken == "" ||
		hello.Body.PairID == "" {
		s.connRejectHello.Add(1)
		s.sendError(c, "malformed_hello", "missing required fields")
		s.log.Info("hello invalid fields", "remote", remote, "type", hello.Type)
		return
	}
	role := hello.Body.Role
	if role != "master" && role != "slave" {
		s.connRejectHello.Add(1)
		s.sendError(c, "malformed_hello", "bad role")
		return
	}

	user, state, authErr := s.users.Authenticate(hello.Body.UserID, hello.Body.AuthToken)
	if authErr != nil {
		s.connRejectAuth.Add(1)
		code := errCode(authErr)
		s.sendError(c, code, authErr.Error())
		s.log.Info("auth reject", "remote", remote, "user_id", hello.Body.UserID, "code", code)
		return
	}

	pair, peer, regErr := s.mgr.Register(hello.Body.UserID, hello.Body.PairID, role, c)
	if regErr != nil {
		s.connRejectMaxPairs.Add(1)
		s.sendError(c, errCodePair(regErr), regErr.Error())
		s.log.Info("register reject", "remote", remote, "user_id", hello.Body.UserID,
			"pair_id", hello.Body.PairID, "err", regErr.Error())
		return
	}

	_ = c.SetReadDeadline(time.Time{})
	s.log.Info("connect", "remote", remote, "user_id", hello.Body.UserID,
		"pair_id", hello.Body.PairID, "role", role)

	go s.outboxLoop(peer, pair, state)

	defer func() {
		dur := time.Since(time.UnixMilli(peer.connectedMs))
		s.mgr.Unregister(pair, role, peer)
		s.log.Info("disconnect", "remote", remote, "user_id", hello.Body.UserID,
			"pair_id", hello.Body.PairID, "role", role, "duration_s", int64(dur.Seconds()),
			"messages", peer.messages, "bytes", peer.bytes)
	}()

	for {
		line, err := readLine(r, s.cfg.MaxLineBytes)
		if err != nil {
			if !errors.Is(err, io.EOF) {
				s.log.Debug("read error", "remote", remote, "err", err.Error())
			}
			return
		}
		pair.OnMessage(role, line, user, state, s.log)
		if state != nil {
			state.mu.Lock()
			state.totalMsgIn++
			state.totalBytesIn += uint64(len(line))
			state.mu.Unlock()
		}
	}
}

func (s *Server) outboxLoop(peer *Peer, pair *Pair, state *UserState) {
	for line := range peer.Outbox {
		_ = peer.Conn.SetWriteDeadline(time.Now().Add(10 * time.Second))
		n, err := peer.Conn.Write(line)
		if err != nil {
			s.log.Debug("write error", "user_id", pair.UserID, "pair_id", pair.PairID,
				"role", peer.Role, "err", err.Error())
			_ = peer.Conn.Close()
			return
		}
		if n == 0 || line[len(line)-1] != '\n' {
			if _, err := peer.Conn.Write([]byte{'\n'}); err != nil {
				return
			}
		}
		if state != nil {
			state.mu.Lock()
			state.totalMsgOut++
			state.totalBytesOut += uint64(len(line))
			state.mu.Unlock()
		}
	}
}

func (s *Server) reaperLoop() {
	t := time.NewTicker(5 * time.Second)
	defer t.Stop()
	for range t.C {
		if s.stopped.Load() {
			return
		}
		cutoff := time.Now().UnixMilli() - int64(s.cfg.PeerTimeoutS)*1000
		s.mgr.ReapStale(cutoff)
	}
}

func readLine(r *bufio.Reader, maxBytes int) ([]byte, error) {
	var buf []byte
	for {
		chunk, isPrefix, err := r.ReadLine()
		if err != nil {
			return nil, err
		}
		if buf == nil && !isPrefix {
			out := make([]byte, len(chunk)+1)
			copy(out, chunk)
			out[len(chunk)] = '\n'
			return out, nil
		}
		buf = append(buf, chunk...)
		if len(buf) > maxBytes {
			return nil, errors.New("line too long")
		}
		if !isPrefix {
			buf = append(buf, '\n')
			return buf, nil
		}
	}
}
