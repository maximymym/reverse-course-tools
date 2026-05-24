package main

import (
	"bufio"
	"encoding/json"
	"errors"
	"fmt"
	"log/slog"
	"net"
	"os"
	"strings"
	"testing"
	"time"
)

// ---------- helpers ----------

type fakeConn struct {
	written chan []byte
	closed  bool
}

func newFakeConn() *fakeConn { return &fakeConn{written: make(chan []byte, 64)} }

func (f *fakeConn) Write(p []byte) (int, error) {
	cp := make([]byte, len(p))
	copy(cp, p)
	f.written <- cp
	return len(p), nil
}
func (f *fakeConn) Read(p []byte) (int, error)         { return 0, nil }
func (f *fakeConn) Close() error                       { f.closed = true; return nil }
func (f *fakeConn) LocalAddr() net.Addr                { return &net.TCPAddr{} }
func (f *fakeConn) RemoteAddr() net.Addr               { return &net.TCPAddr{} }
func (f *fakeConn) SetDeadline(t time.Time) error      { return nil }
func (f *fakeConn) SetReadDeadline(t time.Time) error  { return nil }
func (f *fakeConn) SetWriteDeadline(t time.Time) error { return nil }

func testLogger() *slog.Logger {
	return slog.New(slog.NewTextHandler(os.Stderr, &slog.HandlerOptions{Level: slog.LevelError}))
}

func newTestUserDB(t *testing.T, users map[string]*User) *UserDB {
	t.Helper()
	dir := t.TempDir()
	path := dir + "/users.json"
	data, _ := json.Marshal(users)
	if err := os.WriteFile(path, data, 0644); err != nil {
		t.Fatal(err)
	}
	db := NewUserDB(path, testLogger())
	if err := db.Load(); err != nil {
		t.Fatal(err)
	}
	return db
}

func collectOutbox(t *testing.T, peer *Peer, want int) []string {
	t.Helper()
	got := make([]string, 0, want)
	deadline := time.After(500 * time.Millisecond)
	for len(got) < want {
		select {
		case line, ok := <-peer.Outbox:
			if !ok {
				return got
			}
			got = append(got, strings.TrimSpace(string(line)))
		case <-deadline:
			return got
		}
	}
	return got
}

// ---------- core routing tests ----------

func TestRegisterRouteMessage(t *testing.T) {
	users := newTestUserDB(t, map[string]*User{
		"alice": {AuthToken: "t", MaxPairs: 5, MaxMsgPerMin: 1000, MaxBytesPerMin: 1 << 20, Enabled: true},
	})
	mgr := NewPairManager(100, 10, users, testLogger())

	mc := newFakeConn()
	pair, master, err := mgr.Register("alice", "p1", "master", mc)
	if err != nil {
		t.Fatalf("register master: %v", err)
	}
	sc := newFakeConn()
	_, slave, err := mgr.Register("alice", "p1", "slave", sc)
	if err != nil {
		t.Fatalf("register slave: %v", err)
	}
	user, st, _ := users.Authenticate("alice", "t")

	msg := []byte(`{"type":"match_found","ts":1,"body":{},"sig":""}` + "\n")
	pair.OnMessage("master", msg, user, st, testLogger())
	got := collectOutbox(t, slave, 1)
	if len(got) != 1 || !strings.Contains(got[0], "match_found") {
		t.Fatalf("slave should receive match_found, got %v", got)
	}

	pair.OnMessage("slave", []byte(`{"type":"ack","ts":2,"body":{},"sig":""}`+"\n"), user, st, testLogger())
	got = collectOutbox(t, master, 1)
	if len(got) != 1 || !strings.Contains(got[0], "ack") {
		t.Fatalf("master should receive ack, got %v", got)
	}
}

func TestHeartbeatNotRelayed(t *testing.T) {
	users := newTestUserDB(t, map[string]*User{
		"alice": {AuthToken: "t", MaxPairs: 5, MaxMsgPerMin: 1000, MaxBytesPerMin: 1 << 20, Enabled: true},
	})
	mgr := NewPairManager(100, 10, users, testLogger())

	mc := newFakeConn()
	pair, _, _ := mgr.Register("alice", "p1", "master", mc)
	sc := newFakeConn()
	_, slave, _ := mgr.Register("alice", "p1", "slave", sc)
	user, st, _ := users.Authenticate("alice", "t")

	hb := []byte(`{"type":"hb","ts":1,"body":{},"sig":""}` + "\n")
	pair.OnMessage("master", hb, user, st, testLogger())

	select {
	case line := <-slave.Outbox:
		t.Fatalf("hb should NOT be relayed, got %q", string(line))
	case <-time.After(100 * time.Millisecond):
	}
	if pair.Master.LastHbMs == 0 {
		t.Fatalf("master LastHbMs should be updated after hb")
	}
}

func TestBufferOnAbsentPeer(t *testing.T) {
	users := newTestUserDB(t, map[string]*User{
		"alice": {AuthToken: "t", MaxPairs: 5, MaxMsgPerMin: 1000, MaxBytesPerMin: 1 << 20, Enabled: true},
	})
	mgr := NewPairManager(100, 10, users, testLogger())
	mc := newFakeConn()
	pair, _, _ := mgr.Register("alice", "p1", "master", mc)
	user, st, _ := users.Authenticate("alice", "t")

	for i := 0; i < 11; i++ {
		body := fmt.Sprintf(`{"type":"data","ts":%d,"body":{"i":%d},"sig":""}`+"\n", i, i)
		pair.OnMessage("master", []byte(body), user, st, testLogger())
	}
	pair.Mx.Lock()
	bufLen := len(pair.Buffer)
	first := pair.Buffer[0]
	pair.Mx.Unlock()
	if bufLen != 10 {
		t.Fatalf("expected buffer cap=10, got %d", bufLen)
	}
	var hdr struct {
		Body struct {
			I int `json:"i"`
		} `json:"body"`
	}
	if err := json.Unmarshal(stripNewline(first), &hdr); err != nil {
		t.Fatal(err)
	}
	if hdr.Body.I != 1 {
		t.Fatalf("expected oldest i=1 (i=0 dropped), got i=%d", hdr.Body.I)
	}

	sc := newFakeConn()
	_, slave, _ := mgr.Register("alice", "p1", "slave", sc)
	got := collectOutbox(t, slave, 10)
	if len(got) != 10 {
		t.Fatalf("expected 10 flushed messages, got %d", len(got))
	}
}

func TestUnregisterRemovesEmptyPair(t *testing.T) {
	users := newTestUserDB(t, map[string]*User{
		"alice": {AuthToken: "t", MaxPairs: 5, MaxMsgPerMin: 1000, MaxBytesPerMin: 1 << 20, Enabled: true},
	})
	mgr := NewPairManager(100, 10, users, testLogger())
	c1 := newFakeConn()
	pair, master, _ := mgr.Register("alice", "p1", "master", c1)
	mgr.Unregister(pair, "master", master)
	if mgr.Stats() != 0 {
		t.Fatalf("empty pair should be removed, stats=%d", mgr.Stats())
	}
	st := users.State("alice")
	st.mu.Lock()
	active := st.pairsActive
	st.mu.Unlock()
	if active != 0 {
		t.Fatalf("pairsActive should be 0, got %d", active)
	}
}

func TestReplaceExistingRole(t *testing.T) {
	users := newTestUserDB(t, map[string]*User{
		"alice": {AuthToken: "t", MaxPairs: 5, MaxMsgPerMin: 1000, MaxBytesPerMin: 1 << 20, Enabled: true},
	})
	mgr := NewPairManager(100, 10, users, testLogger())
	c1 := newFakeConn()
	pair, oldMaster, _ := mgr.Register("alice", "p1", "master", c1)

	c2 := newFakeConn()
	_, newMaster, _ := mgr.Register("alice", "p1", "master", c2)

	if !c1.closed {
		t.Fatal("old master conn should be closed on replacement")
	}
	if pair.Master != newMaster {
		t.Fatal("pair.Master should be the new peer")
	}
	if pair.Master == oldMaster {
		t.Fatal("pair.Master must not be the old peer")
	}
}

func TestMalformedJsonDropped(t *testing.T) {
	users := newTestUserDB(t, map[string]*User{
		"alice": {AuthToken: "t", MaxPairs: 5, MaxMsgPerMin: 1000, MaxBytesPerMin: 1 << 20, Enabled: true},
	})
	mgr := NewPairManager(100, 10, users, testLogger())
	c1 := newFakeConn()
	pair, _, _ := mgr.Register("alice", "p1", "master", c1)
	c2 := newFakeConn()
	_, slave, _ := mgr.Register("alice", "p1", "slave", c2)
	user, st, _ := users.Authenticate("alice", "t")

	pair.OnMessage("master", []byte("not json at all\n"), user, st, testLogger())
	pair.OnMessage("master", []byte(`{"type":"good","ts":1,"body":{},"sig":""}`+"\n"), user, st, testLogger())

	got := collectOutbox(t, slave, 1)
	if len(got) != 1 || !strings.Contains(got[0], "good") {
		t.Fatalf("only good msg should reach slave, got %v", got)
	}
}

func TestReadLineNewlineDelimited(t *testing.T) {
	r := bufio.NewReaderSize(strings.NewReader("hello\nworld\n"), 1024)
	a, err := readLine(r, 1024)
	if err != nil || strings.TrimSpace(string(a)) != "hello" {
		t.Fatalf("first line: %v %q", err, string(a))
	}
	b, err := readLine(r, 1024)
	if err != nil || strings.TrimSpace(string(b)) != "world" {
		t.Fatalf("second line: %v %q", err, string(b))
	}
}

// ---------- multi-tenancy / auth tests ----------

func TestAuthUnknownUser(t *testing.T) {
	db := newTestUserDB(t, map[string]*User{
		"alice": {AuthToken: "t", Enabled: true},
	})
	_, _, err := db.Authenticate("bob", "t")
	if !errors.Is(err, errUnknownUser) {
		t.Fatalf("expected errUnknownUser, got %v", err)
	}
}

func TestAuthDisabledUser(t *testing.T) {
	db := newTestUserDB(t, map[string]*User{
		"alice": {AuthToken: "t", Enabled: false},
	})
	_, _, err := db.Authenticate("alice", "t")
	if !errors.Is(err, errUserDisabled) {
		t.Fatalf("expected errUserDisabled, got %v", err)
	}
}

func TestAuthWrongToken(t *testing.T) {
	db := newTestUserDB(t, map[string]*User{
		"alice": {AuthToken: "right-token", Enabled: true},
	})
	_, _, err := db.Authenticate("alice", "wrong-token")
	if !errors.Is(err, errAuthFailed) {
		t.Fatalf("expected errAuthFailed, got %v", err)
	}
}

func TestAuthOK(t *testing.T) {
	db := newTestUserDB(t, map[string]*User{
		"alice": {AuthToken: "t", Enabled: true},
	})
	u, st, err := db.Authenticate("alice", "t")
	if err != nil || u == nil || st == nil {
		t.Fatalf("expected ok, got u=%v st=%v err=%v", u, st, err)
	}
}

func TestPairScopedByUser(t *testing.T) {
	users := newTestUserDB(t, map[string]*User{
		"alice": {AuthToken: "ta", MaxPairs: 5, MaxMsgPerMin: 1000, MaxBytesPerMin: 1 << 20, Enabled: true},
		"bob":   {AuthToken: "tb", MaxPairs: 5, MaxMsgPerMin: 1000, MaxBytesPerMin: 1 << 20, Enabled: true},
	})
	mgr := NewPairManager(100, 10, users, testLogger())

	ca := newFakeConn()
	pa, ma, err := mgr.Register("alice", "shared-id", "master", ca)
	if err != nil {
		t.Fatal(err)
	}
	cb := newFakeConn()
	pb, mb, err := mgr.Register("bob", "shared-id", "master", cb)
	if err != nil {
		t.Fatal(err)
	}
	if pa == pb {
		t.Fatal("pairs with same pair_id but different user must be DIFFERENT objects")
	}
	if ca.closed {
		t.Fatal("alice's master conn should NOT be closed by bob's register")
	}
	_ = ma
	_ = mb
	if mgr.Stats() != 2 {
		t.Fatalf("expected 2 pairs, got %d", mgr.Stats())
	}
}

func TestMaxPairsPerUser(t *testing.T) {
	users := newTestUserDB(t, map[string]*User{
		"alice": {AuthToken: "t", MaxPairs: 2, MaxMsgPerMin: 1000, MaxBytesPerMin: 1 << 20, Enabled: true},
	})
	mgr := NewPairManager(100, 10, users, testLogger())
	c1 := newFakeConn()
	if _, _, err := mgr.Register("alice", "p1", "master", c1); err != nil {
		t.Fatal(err)
	}
	c2 := newFakeConn()
	if _, _, err := mgr.Register("alice", "p2", "master", c2); err != nil {
		t.Fatal(err)
	}
	c3 := newFakeConn()
	_, _, err := mgr.Register("alice", "p3", "master", c3)
	if !errors.Is(err, errMaxPairs) {
		t.Fatalf("expected errMaxPairs, got %v", err)
	}
}

func TestMaxPairsGlobal(t *testing.T) {
	users := newTestUserDB(t, map[string]*User{
		"alice": {AuthToken: "t", MaxPairs: 1000, MaxMsgPerMin: 1000, MaxBytesPerMin: 1 << 20, Enabled: true},
	})
	mgr := NewPairManager(2, 10, users, testLogger())
	if _, _, err := mgr.Register("alice", "p1", "master", newFakeConn()); err != nil {
		t.Fatal(err)
	}
	if _, _, err := mgr.Register("alice", "p2", "master", newFakeConn()); err != nil {
		t.Fatal(err)
	}
	_, _, err := mgr.Register("alice", "p3", "master", newFakeConn())
	if !errors.Is(err, errMaxPairsGlobal) {
		t.Fatalf("expected errMaxPairsGlobal, got %v", err)
	}
}

func TestQuotaMsgPerMinExhaustion(t *testing.T) {
	users := newTestUserDB(t, map[string]*User{
		"alice": {AuthToken: "t", MaxPairs: 5, MaxMsgPerMin: 5, MaxBytesPerMin: 1 << 20, Enabled: true},
	})
	mgr := NewPairManager(100, 10, users, testLogger())
	c1 := newFakeConn()
	pair, _, _ := mgr.Register("alice", "p1", "master", c1)
	c2 := newFakeConn()
	_, slave, _ := mgr.Register("alice", "p1", "slave", c2)
	user, st, _ := users.Authenticate("alice", "t")

	for i := 0; i < 10; i++ {
		body := fmt.Sprintf(`{"type":"x","ts":%d,"body":{},"sig":""}`+"\n", i)
		pair.OnMessage("master", []byte(body), user, st, testLogger())
	}

	got := collectOutbox(t, slave, 10)
	if len(got) != 5 {
		t.Fatalf("expected 5 routed (cap), got %d", len(got))
	}
	st.mu.Lock()
	qe := st.quotaExceeded
	st.mu.Unlock()
	if qe < 5 {
		t.Fatalf("expected quotaExceeded counter ≥5, got %d", qe)
	}
}

func TestUserDBReload(t *testing.T) {
	dir := t.TempDir()
	path := dir + "/users.json"
	initial := map[string]*User{
		"alice": {AuthToken: "old", Enabled: true},
	}
	data, _ := json.Marshal(initial)
	_ = os.WriteFile(path, data, 0644)

	db := NewUserDB(path, testLogger())
	if err := db.Load(); err != nil {
		t.Fatal(err)
	}
	if _, _, err := db.Authenticate("alice", "old"); err != nil {
		t.Fatalf("auth ok before reload: %v", err)
	}

	updated := map[string]*User{
		"alice": {AuthToken: "new", Enabled: true},
		"bob":   {AuthToken: "bobtok", Enabled: true},
	}
	data, _ = json.Marshal(updated)
	_ = os.WriteFile(path, data, 0644)

	if err := db.Load(); err != nil {
		t.Fatal(err)
	}
	if _, _, err := db.Authenticate("alice", "new"); err != nil {
		t.Fatalf("alice with new token after reload: %v", err)
	}
	if _, _, err := db.Authenticate("alice", "old"); err == nil {
		t.Fatal("old token must be invalidated after reload")
	}
	if _, _, err := db.Authenticate("bob", "bobtok"); err != nil {
		t.Fatalf("bob (newly added) auth: %v", err)
	}
}

// ---------- HTTP admin/metrics tests ----------

func TestMetricsEndpoint(t *testing.T) {
	users := newTestUserDB(t, map[string]*User{
		"alice": {AuthToken: "t", MaxPairs: 5, MaxMsgPerMin: 1000, MaxBytesPerMin: 1 << 20, Enabled: true},
	})
	cfg := Config{
		Bind: ":0", AdminBind: "127.0.0.1:0", BufferPerPair: 10, MaxPairsGlobal: 100,
		PeerTimeoutS: 30, HelloDeadlineS: 5, MaxLineBytes: 65536, ConnectRatePerMin: 10,
	}
	srv := NewServer(cfg, users, testLogger())

	mc := newFakeConn()
	pair, _, _ := srv.Manager().Register("alice", "p1", "master", mc)
	user, st, _ := users.Authenticate("alice", "t")
	pair.OnMessage("master", []byte(`{"type":"x","ts":1,"body":{},"sig":""}`+"\n"), user, st, testLogger())

	admin := NewAdminServer(srv, testLogger())
	rec := httpTestRec()
	req := httpTestReq("GET", "/metrics", "", "")
	admin.metrics(rec, req)
	body := rec.body.String()
	if !strings.Contains(body, "relay_pairs_active{user_id=\"alice\"} 1") {
		t.Fatalf("expected pairs_active in metrics, got: %s", body)
	}
	if !strings.Contains(body, "relay_messages_total") {
		t.Fatalf("expected messages_total in metrics: %s", body)
	}
	if !strings.Contains(body, "relay_uptime_seconds") {
		t.Fatalf("expected uptime metric: %s", body)
	}
}

func TestAdminReloadEndpoint(t *testing.T) {
	dir := t.TempDir()
	path := dir + "/users.json"
	data, _ := json.Marshal(map[string]*User{
		"alice": {AuthToken: "old", Enabled: true},
	})
	_ = os.WriteFile(path, data, 0644)

	db := NewUserDB(path, testLogger())
	_ = db.Load()
	cfg := Config{Bind: ":0", AdminBind: "127.0.0.1:0", AdminToken: "ADM",
		BufferPerPair: 10, MaxPairsGlobal: 100, PeerTimeoutS: 30,
		HelloDeadlineS: 5, MaxLineBytes: 65536}
	srv := NewServer(cfg, db, testLogger())
	admin := NewAdminServer(srv, testLogger())

	updated, _ := json.Marshal(map[string]*User{
		"alice": {AuthToken: "new", Enabled: true},
	})
	_ = os.WriteFile(path, updated, 0644)

	rec := httpTestRec()
	req := httpTestReq("POST", "/admin/users/reload", "", "ADM")
	admin.adminAuth(admin.adminReload)(rec, req)
	if rec.code != 200 {
		t.Fatalf("expected 200, got %d body=%s", rec.code, rec.body.String())
	}
	if _, _, err := db.Authenticate("alice", "new"); err != nil {
		t.Fatalf("after admin reload, new token must work: %v", err)
	}
}

func TestAdminAuthRequired(t *testing.T) {
	users := newTestUserDB(t, map[string]*User{"alice": {AuthToken: "t", Enabled: true}})
	cfg := Config{Bind: ":0", AdminBind: "127.0.0.1:0", AdminToken: "secret",
		BufferPerPair: 10, MaxPairsGlobal: 100}
	srv := NewServer(cfg, users, testLogger())
	admin := NewAdminServer(srv, testLogger())

	rec := httpTestRec()
	req := httpTestReq("POST", "/admin/users/reload", "", "wrong-token")
	admin.adminAuth(admin.adminReload)(rec, req)
	if rec.code != 403 {
		t.Fatalf("expected 403 with wrong token, got %d", rec.code)
	}
}

func TestAdminDisabledWhenNoToken(t *testing.T) {
	users := newTestUserDB(t, map[string]*User{"alice": {AuthToken: "t", Enabled: true}})
	cfg := Config{Bind: ":0", AdminBind: "127.0.0.1:0", AdminToken: "",
		BufferPerPair: 10, MaxPairsGlobal: 100}
	srv := NewServer(cfg, users, testLogger())
	admin := NewAdminServer(srv, testLogger())

	rec := httpTestRec()
	req := httpTestReq("GET", "/admin/stats?user_id=alice", "", "any")
	admin.adminAuth(admin.adminStats)(rec, req)
	if rec.code != 403 {
		t.Fatalf("expected 403 when admin disabled, got %d", rec.code)
	}
}

// ---------- per-IP connect rate-limit ----------

func TestConnectRateLimitPerIP(t *testing.T) {
	users := newTestUserDB(t, map[string]*User{
		"alice": {AuthToken: "t", MaxPairs: 100, MaxMsgPerMin: 10000, MaxBytesPerMin: 1 << 20, Enabled: true},
	})
	cfg := Config{
		Bind: ":0", AdminBind: "127.0.0.1:0",
		BufferPerPair: 10, MaxPairsGlobal: 100,
		PeerTimeoutS: 30, HelloDeadlineS: 5, MaxLineBytes: 65536,
		ConnectRatePerMin: 10, // 10/min burst, refill 10/60s
	}
	srv := NewServer(cfg, users, testLogger())

	ip := "1.2.3.4"
	allowed := 0
	rejected := 0
	for i := 0; i < 12; i++ {
		if srv.ipLimiter(ip).Allow() {
			allowed++
		} else {
			rejected++
		}
	}
	if allowed != 10 {
		t.Fatalf("expected 10 allowed (burst cap), got %d", allowed)
	}
	if rejected != 2 {
		t.Fatalf("expected 2 rejected (11th + 12th), got %d", rejected)
	}
}

// ---------- disable user kicks active conns ----------

func TestDisableUserKicksActive(t *testing.T) {
	users := newTestUserDB(t, map[string]*User{
		"alice": {AuthToken: "t", MaxPairs: 5, MaxMsgPerMin: 1000, MaxBytesPerMin: 1 << 20, Enabled: true},
	})
	cfg := Config{Bind: ":0", AdminBind: "127.0.0.1:0", AdminToken: "ADM",
		BufferPerPair: 10, MaxPairsGlobal: 100}
	srv := NewServer(cfg, users, testLogger())
	users.SetDisabledKick(srv.Manager().KickUser)

	c1 := newFakeConn()
	if _, _, err := srv.Manager().Register("alice", "p1", "master", c1); err != nil {
		t.Fatal(err)
	}
	if err := users.Disable("alice"); err != nil {
		t.Fatal(err)
	}
	// Disable() запускает kick в горутине — ждём.
	deadline := time.Now().Add(500 * time.Millisecond)
	for !c1.closed && time.Now().Before(deadline) {
		time.Sleep(10 * time.Millisecond)
	}
	if !c1.closed {
		t.Fatal("active conn should be closed by Disable()")
	}
	if _, _, err := users.Authenticate("alice", "t"); !errors.Is(err, errUserDisabled) {
		t.Fatalf("expected errUserDisabled, got %v", err)
	}
}
