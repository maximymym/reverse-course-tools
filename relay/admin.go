package main

import (
	"context"
	"encoding/json"
	"fmt"
	"log/slog"
	"net/http"
	"strings"
	"sync"
	"time"
)

type AdminServer struct {
	srv        *Server
	adminToken string
	httpSrv    *http.Server
	log        *slog.Logger
	mu         sync.Mutex
}

func NewAdminServer(srv *Server, log *slog.Logger) *AdminServer {
	a := &AdminServer{srv: srv, adminToken: srv.cfg.AdminToken, log: log}

	mux := http.NewServeMux()
	mux.HandleFunc("/health", a.health)
	mux.HandleFunc("/metrics", a.metrics)
	mux.HandleFunc("/admin/stats", a.adminAuth(a.adminStats))
	mux.HandleFunc("/admin/users/reload", a.adminAuth(a.adminReload))
	mux.HandleFunc("/admin/users/", a.adminAuth(a.adminUserOps))

	a.httpSrv = &http.Server{
		Addr:              srv.cfg.AdminBind,
		Handler:           mux,
		ReadHeaderTimeout: 5 * time.Second,
	}
	return a
}

func (a *AdminServer) Run() error {
	a.log.Info("admin listening", "bind", a.srv.cfg.AdminBind)
	err := a.httpSrv.ListenAndServe()
	if err == http.ErrServerClosed {
		return nil
	}
	return err
}

func (a *AdminServer) Stop() {
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	_ = a.httpSrv.Shutdown(ctx)
}

func (a *AdminServer) adminAuth(h http.HandlerFunc) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		if a.adminToken == "" {
			http.Error(w, "admin disabled", http.StatusForbidden)
			return
		}
		got := r.Header.Get("X-Admin-Token")
		if got != a.adminToken {
			http.Error(w, "forbidden", http.StatusForbidden)
			return
		}
		h(w, r)
	}
}

func (a *AdminServer) health(w http.ResponseWriter, r *http.Request) {
	uptime := (time.Now().UnixMilli() - a.srv.StartMs()) / 1000
	users := a.srv.Users().Snapshot()
	pairs := a.srv.Manager().Stats()
	w.Header().Set("Content-Type", "application/json")
	_ = json.NewEncoder(w).Encode(map[string]interface{}{
		"status":       "ok",
		"uptime_s":     uptime,
		"users_loaded": len(users),
		"pairs_active": pairs,
	})
}

func (a *AdminServer) metrics(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Content-Type", "text/plain; version=0.0.4")
	uptime := (time.Now().UnixMilli() - a.srv.StartMs()) / 1000

	pairsByUser := a.srv.Manager().PairsByUser()
	states := a.srv.Users().AllStates()

	fmt.Fprintln(w, "# HELP relay_pairs_active Active paired connections per user")
	fmt.Fprintln(w, "# TYPE relay_pairs_active gauge")
	for uid, pairs := range pairsByUser {
		fmt.Fprintf(w, "relay_pairs_active{user_id=%q} %d\n", uid, len(pairs))
	}

	fmt.Fprintln(w, "# HELP relay_messages_total Total messages routed per user/direction")
	fmt.Fprintln(w, "# TYPE relay_messages_total counter")
	fmt.Fprintln(w, "# HELP relay_bytes_total Total bytes routed per user/direction")
	fmt.Fprintln(w, "# TYPE relay_bytes_total counter")
	fmt.Fprintln(w, "# HELP relay_quota_exceeded_total Quota-rejected messages per user")
	fmt.Fprintln(w, "# TYPE relay_quota_exceeded_total counter")
	for uid, st := range states {
		st.mu.Lock()
		mIn := st.totalMsgIn
		mOut := st.totalMsgOut
		bIn := st.totalBytesIn
		bOut := st.totalBytesOut
		qe := st.quotaExceeded
		st.mu.Unlock()
		fmt.Fprintf(w, "relay_messages_total{user_id=%q,direction=\"in\"} %d\n", uid, mIn)
		fmt.Fprintf(w, "relay_messages_total{user_id=%q,direction=\"out\"} %d\n", uid, mOut)
		fmt.Fprintf(w, "relay_bytes_total{user_id=%q,direction=\"in\"} %d\n", uid, bIn)
		fmt.Fprintf(w, "relay_bytes_total{user_id=%q,direction=\"out\"} %d\n", uid, bOut)
		fmt.Fprintf(w, "relay_quota_exceeded_total{user_id=%q,reason=\"any\"} %d\n", uid, qe)
	}

	fmt.Fprintln(w, "# HELP relay_connections_rejected_total Connection rejects by reason")
	fmt.Fprintln(w, "# TYPE relay_connections_rejected_total counter")
	for reason, n := range a.srv.Counters() {
		fmt.Fprintf(w, "relay_connections_rejected_total{reason=%q} %d\n", reason, n)
	}

	fmt.Fprintln(w, "# HELP relay_uptime_seconds Process uptime")
	fmt.Fprintln(w, "# TYPE relay_uptime_seconds gauge")
	fmt.Fprintf(w, "relay_uptime_seconds %d\n", uptime)
}

func (a *AdminServer) adminStats(w http.ResponseWriter, r *http.Request) {
	uid := r.URL.Query().Get("user_id")
	if uid == "" {
		http.Error(w, "user_id required", http.StatusBadRequest)
		return
	}
	users := a.srv.Users().Snapshot()
	u, ok := users[uid]
	if !ok {
		http.Error(w, "unknown user", http.StatusNotFound)
		return
	}
	state := a.srv.Users().State(uid)
	pairs := a.srv.Manager().PairsByUser()[uid]
	out := map[string]interface{}{
		"user_id":             uid,
		"enabled":             u.Enabled,
		"max_pairs":           u.MaxPairs,
		"max_msg_per_min":     u.MaxMsgPerMin,
		"max_bytes_per_min":   u.MaxBytesPerMin,
		"pairs_active":        pairs,
		"pairs_active_count":  len(pairs),
	}
	if state != nil {
		state.mu.Lock()
		out["totals"] = map[string]uint64{
			"msg_in":         state.totalMsgIn,
			"msg_out":        state.totalMsgOut,
			"bytes_in":       state.totalBytesIn,
			"bytes_out":      state.totalBytesOut,
			"quota_exceeded": state.quotaExceeded,
		}
		state.mu.Unlock()
		out["current_msg_per_min"] = state.msgWindow.Sum()
		out["current_bytes_per_min"] = state.bytesWindow.Sum()
	}
	w.Header().Set("Content-Type", "application/json")
	_ = json.NewEncoder(w).Encode(out)
}

func (a *AdminServer) adminReload(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "POST only", http.StatusMethodNotAllowed)
		return
	}
	if err := a.srv.Users().Load(); err != nil {
		http.Error(w, err.Error(), http.StatusInternalServerError)
		return
	}
	w.Header().Set("Content-Type", "application/json")
	_ = json.NewEncoder(w).Encode(map[string]string{"status": "reloaded"})
}

func (a *AdminServer) adminUserOps(w http.ResponseWriter, r *http.Request) {
	// /admin/users/<uid>/disable
	parts := strings.Split(strings.TrimPrefix(r.URL.Path, "/admin/users/"), "/")
	if len(parts) < 2 {
		http.Error(w, "bad path", http.StatusBadRequest)
		return
	}
	uid := parts[0]
	op := parts[1]
	switch op {
	case "disable":
		if r.Method != http.MethodPost {
			http.Error(w, "POST only", http.StatusMethodNotAllowed)
			return
		}
		if err := a.srv.Users().Disable(uid); err != nil {
			http.Error(w, err.Error(), http.StatusNotFound)
			return
		}
		a.srv.Manager().KickUser(uid)
		w.Header().Set("Content-Type", "application/json")
		_ = json.NewEncoder(w).Encode(map[string]string{"status": "disabled"})
	default:
		http.Error(w, "unknown op", http.StatusBadRequest)
	}
}
