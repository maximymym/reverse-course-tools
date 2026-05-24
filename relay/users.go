package main

import (
	"crypto/subtle"
	"encoding/json"
	"errors"
	"fmt"
	"log/slog"
	"os"
	"sync"
	"sync/atomic"
	"time"
)

// User — запись из users.json.
type User struct {
	AuthToken      string `json:"auth_token"`
	MaxPairs       int    `json:"max_pairs"`
	MaxMsgPerMin   int    `json:"max_msg_per_min"`
	MaxBytesPerMin int    `json:"max_bytes_per_min"`
	Enabled        bool   `json:"enabled"`
	Created        string `json:"created"`
	Notes          string `json:"notes"`
}

// UserState — runtime per-user счётчики.
type UserState struct {
	mu             sync.Mutex
	msgWindow      slidingWindow
	bytesWindow    slidingWindow
	totalMsgIn     uint64
	totalMsgOut    uint64
	totalBytesIn   uint64
	totalBytesOut  uint64
	quotaExceeded  uint64
	pairsActive    int
}

type UserDB struct {
	mu          sync.RWMutex
	users       map[string]*User
	states      map[string]*UserState
	path        string
	lastMtime   time.Time
	loadCount   atomic.Uint64
	loadErrors  atomic.Uint64
	disabledKick func(userID string)
	log         *slog.Logger
}

func NewUserDB(path string, log *slog.Logger) *UserDB {
	return &UserDB{
		users:  make(map[string]*User),
		states: make(map[string]*UserState),
		path:   path,
		log:    log,
	}
}

func (db *UserDB) SetDisabledKick(fn func(userID string)) {
	db.mu.Lock()
	defer db.mu.Unlock()
	db.disabledKick = fn
}

// Load reads users.json from disk and replaces in-memory map.
// Existing UserState (counters) preserved across reloads.
func (db *UserDB) Load() error {
	st, err := os.Stat(db.path)
	if err != nil {
		db.loadErrors.Add(1)
		return fmt.Errorf("stat users file: %w", err)
	}
	data, err := os.ReadFile(db.path)
	if err != nil {
		db.loadErrors.Add(1)
		return fmt.Errorf("read users file: %w", err)
	}
	var raw map[string]*User
	if err := json.Unmarshal(data, &raw); err != nil {
		db.loadErrors.Add(1)
		return fmt.Errorf("parse users file: %w", err)
	}

	db.mu.Lock()
	prev := db.users
	db.users = raw
	db.lastMtime = st.ModTime()

	// Сохраняем существующие states; для новых users создаём.
	for uid := range raw {
		if _, ok := db.states[uid]; !ok {
			db.states[uid] = &UserState{}
		}
	}
	// Найти ставшие disabled для kick.
	var newlyDisabled []string
	for uid, u := range raw {
		if pu, ok := prev[uid]; ok && pu.Enabled && !u.Enabled {
			newlyDisabled = append(newlyDisabled, uid)
		}
	}
	kickFn := db.disabledKick
	db.mu.Unlock()

	db.loadCount.Add(1)
	db.log.Info("users loaded", "path", db.path, "count", len(raw), "mtime", st.ModTime().Format(time.RFC3339))

	if kickFn != nil {
		for _, uid := range newlyDisabled {
			kickFn(uid)
		}
	}
	return nil
}

// PollLoop — фоновая горутина: при изменении mtime — reload.
func (db *UserDB) PollLoop(intervalSec int, stop <-chan struct{}) {
	if intervalSec <= 0 {
		intervalSec = 30
	}
	t := time.NewTicker(time.Duration(intervalSec) * time.Second)
	defer t.Stop()
	for {
		select {
		case <-stop:
			return
		case <-t.C:
			st, err := os.Stat(db.path)
			if err != nil {
				continue
			}
			db.mu.RLock()
			same := st.ModTime().Equal(db.lastMtime)
			db.mu.RUnlock()
			if !same {
				if err := db.Load(); err != nil {
					db.log.Warn("users reload failed", "err", err.Error())
				}
			}
		}
	}
}

// Authenticate проверяет user_id+auth_token. Returns (user, state, error).
func (db *UserDB) Authenticate(userID, token string) (*User, *UserState, error) {
	db.mu.RLock()
	defer db.mu.RUnlock()

	u, ok := db.users[userID]
	if !ok {
		return nil, nil, errUnknownUser
	}
	if !u.Enabled {
		return nil, nil, errUserDisabled
	}
	if subtle.ConstantTimeCompare([]byte(u.AuthToken), []byte(token)) != 1 {
		return nil, nil, errAuthFailed
	}
	return u, db.states[userID], nil
}

func (db *UserDB) Disable(userID string) error {
	db.mu.Lock()
	defer db.mu.Unlock()
	u, ok := db.users[userID]
	if !ok {
		return errUnknownUser
	}
	u.Enabled = false
	if db.disabledKick != nil {
		go db.disabledKick(userID)
	}
	return nil
}

func (db *UserDB) Snapshot() map[string]User {
	db.mu.RLock()
	defer db.mu.RUnlock()
	out := make(map[string]User, len(db.users))
	for k, v := range db.users {
		out[k] = *v
	}
	return out
}

func (db *UserDB) State(userID string) *UserState {
	db.mu.RLock()
	defer db.mu.RUnlock()
	return db.states[userID]
}

func (db *UserDB) AllStates() map[string]*UserState {
	db.mu.RLock()
	defer db.mu.RUnlock()
	out := make(map[string]*UserState, len(db.states))
	for k, v := range db.states {
		out[k] = v
	}
	return out
}

func (db *UserDB) Stats() (loads, errs uint64) {
	return db.loadCount.Load(), db.loadErrors.Load()
}

var (
	errUnknownUser  = errors.New("unknown user")
	errUserDisabled = errors.New("user disabled")
	errAuthFailed   = errors.New("auth failed")
)

// errCode мапит ошибку на стабильный код для error message.
func errCode(err error) string {
	switch {
	case errors.Is(err, errUnknownUser):
		return "unknown_user"
	case errors.Is(err, errUserDisabled):
		return "user_disabled"
	case errors.Is(err, errAuthFailed):
		return "auth_failed"
	default:
		return "auth_failed"
	}
}
