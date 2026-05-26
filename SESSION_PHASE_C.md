# SESSION_PHASE_C — Deploy dota_relay на eft-deploy

**Date:** 2026-05-26
**Branch:** `feature/two-stand-coordination`
**Scope:** Phase C плана — TCP-relay для multi-tenant pairing master↔slave.

## Что сделано

Поднят `dota_relay` Docker контейнер на eft-deploy (144.31.85.217) рядом с существующими сервисами (eft_server, eft_caddy, eft_postgres, eft_redis).

### Endpoint

| Endpoint | Адрес | Назначение |
|---|---|---|
| TCP pairing relay | `144.31.85.217:5050` / `v1per.tech:5050` | master+slave orchestrator коннектятся сюда |
| HTTP admin | `127.0.0.1:5051` (loopback) | `/admin/*` (X-Admin-Token), `/health`, `/metrics` |

### Users

`users.json` (на сервере `/opt/eft/dota_relay/users.json`, chmod 600):
- `stand_a` — для 217.60.245.131 (dotahost), max_pairs=5
- `stand_b` — для 217.60.245.152 (dotahost2), max_pairs=5

Limits per stand: 1200 msg/min, 2 MB/min — с запасом для match coordination overhead.

### Secrets

Все секреты живут в `tools/dota2/.secrets_two_stand.txt` (gitignored):
- `RELAY_ADMIN_TOKEN` — `/admin/*` endpoints + ssh tunnel curl
- `stand_a` / `stand_b` auth_tokens — для hello-handshake к relay
- `pair_secret` — HMAC-SHA256 для подписи сообщений master↔slave (relay этот ключ НЕ знает, валидируется только endpoints)

Token rotation policy: каждые 90 дней через `openssl rand -hex 16/32`.

## Deploy шаги (commit'нуто)

Изменения в коде:
- `tools/dota2/relay/Dockerfile`: `golang:1.22-alpine` → `golang:1.25-alpine` (go.mod требует 1.25)
- `tools/dota2/.gitignore`: добавлены правила `relay/users.json`, `.secrets_*.txt`

Локальная подготовка (НЕ committed):
- `tools/dota2/relay/users.json` — с реальными auth_tokens
- `tools/dota2/relay/.env` — `RELAY_ADMIN_TOKEN`
- `tools/dota2/.secrets_two_stand.txt` — workbook со всеми токенами

Команды deploy (выполнены 2026-05-26):
```bash
# Tar archive (exclude relay.exe Windows binary)
cd tools/dota2/relay
tar --exclude='*.exe' -czf - . | ssh eft-deploy 'cd /opt/eft/dota_relay && tar -xzf -'
ssh eft-deploy 'cd /opt/eft/dota_relay && chmod 600 .env users.json'

# Build + run
ssh eft-deploy 'cd /opt/eft/dota_relay && docker compose build relay && docker compose up -d'

# Firewall
ssh eft-deploy 'ufw allow 5050/tcp'
```

## Verification

```bash
# Health endpoint (loopback only, через ssh tunnel)
$ ssh eft-deploy 'curl -s http://127.0.0.1:5051/health'
{"pairs_active":0,"status":"ok","uptime_s":16,"users_loaded":2}

# Prometheus metrics
$ ssh eft-deploy 'curl -s http://127.0.0.1:5051/metrics | head'
# HELP relay_pairs_active Active paired connections per user
# TYPE relay_pairs_active gauge
relay_messages_total{user_id="stand_a",direction="in"} 0

# External TCP reachability (с aleks PC)
PowerShell> (New-Object Net.Sockets.TcpClient).Connect('144.31.85.217', 5050)
# → True (connection OK)
```

Container запущен, healthy. `users_loaded=2` подтверждает что наши tokens parsed.

## Gotchas

1. **Dockerfile go version** — `go.mod` ranchлёкается на `go 1.25.0`, default `golang:1.22-alpine` не работает. Fix → `golang:1.25-alpine`. Можно было бы downgrade go.mod, но 1.25 уже LTS-ish, без проблем.
2. **Tar exclude .exe** — `relay.exe` в репо для локального dev (Go binary 8.6MB), не нужен на Linux runtime. Использовали `tar --exclude='*.exe'`.
3. **`.env` chmod 600** — после tar extract default 644, контейнер всё равно прочёл бы, но secrets best practice = 600.
4. **`docker-compose.yml` version warning** — `version: "3.8"` устарел в Compose v5. Не fatal, оставил для совместимости с локалом.

## Что НЕ сделано

- `smoke_test.py` (тест handshake двумя fake клиентами) — отложен, требует relay.exe или python clients. Live verify придёт через реальный orchestrator в Phase D.
- TLS — relay принимает plain TCP, payload integrity-protected HMAC'ом. Если будет multi-tenant сценарий — повесить nginx reverse-proxy с TLS на `5443 → 127.0.0.1:5050` (см. README Security section).

## Next: Phase B → D

Phase B — verify wiring в orchestrator (где `MatchPairingFsm::Init` зовётся, как маппится `farm.json::pairing`). Должно быть уже всё готово, надо просто прочитать `Orchestrator::Init` и убедиться.

Phase D — заполнить `pairing` секцию в `farm.json` на обоих стендах с реальными creds. Шаблон в `.secrets_two_stand.txt`.
