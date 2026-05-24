package main

import (
	"log/slog"
	"os"
	"os/signal"
	"strconv"
	"strings"
	"syscall"
)

func main() {
	level := parseLevel(getenv("RELAY_LOG_LEVEL", "info"))
	log := slog.New(slog.NewJSONHandler(os.Stdout, &slog.HandlerOptions{Level: level}))
	slog.SetDefault(log)

	cfg := Config{
		Bind:                 getenv("RELAY_BIND", ":5050"),
		AdminBind:            getenv("RELAY_ADMIN_BIND", ":5051"),
		AdminToken:           getenv("RELAY_ADMIN_TOKEN", ""),
		UsersFile:            getenv("RELAY_USERS_FILE", "/etc/dota_relay/users.json"),
		UsersReloadIntervalS: getenvInt("RELAY_USERS_RELOAD_INTERVAL_S", 30),
		BufferPerPair:        getenvInt("RELAY_BUFFER_PER_PAIR", 10),
		MaxPairsGlobal:       getenvInt("RELAY_MAX_PAIRS_GLOBAL", 10000),
		PeerTimeoutS:         getenvInt("RELAY_PEER_TIMEOUT_S", 30),
		HelloDeadlineS:       getenvInt("RELAY_HELLO_DEADLINE_S", 5),
		MaxLineBytes:         getenvInt("RELAY_MAX_LINE_BYTES", 65536),
		ConnectRatePerMin:    getenvInt("RELAY_CONNECT_RATELIMIT_PER_MIN", 10),
	}

	log.Info("relay starting",
		"bind", cfg.Bind,
		"admin_bind", cfg.AdminBind,
		"users_file", cfg.UsersFile,
		"buffer_per_pair", cfg.BufferPerPair,
		"max_pairs_global", cfg.MaxPairsGlobal,
		"peer_timeout_s", cfg.PeerTimeoutS,
		"connect_ratelimit_per_min", cfg.ConnectRatePerMin,
		"admin_enabled", cfg.AdminToken != "",
	)

	users := NewUserDB(cfg.UsersFile, log)
	if err := users.Load(); err != nil {
		log.Error("initial users load failed", "err", err.Error())
		os.Exit(1)
	}

	stop := make(chan struct{})
	go users.PollLoop(cfg.UsersReloadIntervalS, stop)

	srv := NewServer(cfg, users, log)
	admin := NewAdminServer(srv, log)

	sigCh := make(chan os.Signal, 1)
	signal.Notify(sigCh, syscall.SIGINT, syscall.SIGTERM, syscall.SIGHUP)
	go func() {
		for s := range sigCh {
			if s == syscall.SIGHUP {
				log.Info("SIGHUP — reloading users")
				if err := users.Load(); err != nil {
					log.Warn("reload failed", "err", err.Error())
				}
				continue
			}
			log.Info("signal received, shutting down", "signal", s.String())
			close(stop)
			srv.Stop()
			admin.Stop()
			return
		}
	}()

	go func() {
		if err := admin.Run(); err != nil {
			log.Error("admin server error", "err", err.Error())
		}
	}()

	if err := srv.Run(); err != nil {
		log.Error("relay fatal", "err", err.Error())
		os.Exit(1)
	}
	log.Info("relay stopped")
}

func parseLevel(s string) slog.Level {
	switch strings.ToLower(s) {
	case "debug":
		return slog.LevelDebug
	case "warn":
		return slog.LevelWarn
	case "error":
		return slog.LevelError
	default:
		return slog.LevelInfo
	}
}

func getenv(key, def string) string {
	if v := os.Getenv(key); v != "" {
		return v
	}
	return def
}

func getenvInt(key string, def int) int {
	if v := os.Getenv(key); v != "" {
		if n, err := strconv.Atoi(v); err == nil {
			return n
		}
	}
	return def
}
