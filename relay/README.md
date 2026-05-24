# dota_relay (multi-tenant)

TCP relay для парного self-play Dota 2. Один сервис обслуживает несколько
независимых пользователей (коммерческий) — каждый со своими `pair_id`-ами,
квотами и токеном. Master+slave orchestrator'ы коннектятся к VPS, делают
hello-handshake, и сервер прокидывает между ними raw line-delimited JSON.

## Wire-протокол

Каждое сообщение — одна строка JSON, терминатор `\n`:

```
{"type":"<msg_type>","ts":<unix_ms>,"body":{...},"sig":"<hex_hmac>"}
```

Первое сообщение от каждого клиента ОБЯЗАНО быть `hello`:

```json
{"type":"hello","ts":1714820000123,
 "body":{
   "user_id":"alice",
   "auth_token":"alice-secret-32-chars-here-aaaaaa",
   "pair_id":"alice-prod-1",
   "role":"master"
 },"sig":"..."}
```

- **`auth_token`** — relay-level аутентификация (защищает от чужого user_id).
- **`sig`** — HMAC integrity message-to-message между endpoints. Relay не
  верифицирует sig — secret только у endpoints.
- **Pair key scoped** — `<user_id>:<pair_id>`. Два разных юзера могут иметь
  одинаковый `pair_id` без конфликта.
- **`type:"hb"`** обновляет `LastHbMs` peer'а, **не релеится**.

## Error message (relay → endpoint)

Перед close relay шлёт:

```json
{"type":"error","ts":<unix_ms>,"body":{"code":"<code>","message":"<text>"}}
```

Коды: `auth_failed`, `unknown_user`, `user_disabled`, `max_pairs`,
`malformed_hello`. Endpoint логирует и применяет backoff перед reconnect'ом.

Quota-overrun **не закрывает conn** — message dropped + counter
`relay_quota_exceeded_total{user_id}` инкрементится. Soft-fail чтобы не
терять активную пару из-за burst'а.

## Логика relay

- TCP listen `:5050`.
- Per-IP token bucket (burst=N, refill=N/60s) — anti-DoS на handshake.
- Hello deadline 5s, `MaxLineBytes` 65536.
- Replacement: повторный hello с тем же `(user_id,pair_id,role)` закрывает
  старое соединение.
- Per-pair buffer для absent peer (drop oldest при `BUFFER_PER_PAIR`).
- Reaper-горутина каждые 5s закрывает peer'ов без hb старше `PEER_TIMEOUT_S`.
- Pair удаляется когда оба peer'а отключены.
- HTTP admin/metrics на `:5051` (отдельный port — биндить только loopback на хосте).

## Конфигурация (env vars)

| Переменная | Default | Описание |
|---|---|---|
| `RELAY_BIND` | `:5050` | TCP listener |
| `RELAY_ADMIN_BIND` | `:5051` | HTTP admin/metrics |
| `RELAY_ADMIN_TOKEN` | _(empty)_ | пустое = `/admin/*` disabled (для dev) |
| `RELAY_USERS_FILE` | `/etc/dota_relay/users.json` | путь к user DB |
| `RELAY_USERS_RELOAD_INTERVAL_S` | `30` | mtime poll interval |
| `RELAY_BUFFER_PER_PAIR` | `10` | message buffer для absent peer |
| `RELAY_MAX_PAIRS_GLOBAL` | `10000` | глобальный лимит пар |
| `RELAY_PEER_TIMEOUT_S` | `30` | reaper drops peers без hb |
| `RELAY_HELLO_DEADLINE_S` | `5` | таймаут на hello |
| `RELAY_MAX_LINE_BYTES` | `65536` | максимальная строка JSON |
| `RELAY_CONNECT_RATELIMIT_PER_MIN` | `10` | per-IP connect rate (0 = off) |
| `RELAY_LOG_LEVEL` | `info` | debug, info, warn, error |

## User management

Файл `users.json` (см. `users.example.json`). Структура:

```json
{
  "<user_id>": {
    "auth_token":      "<32+ char secret>",
    "max_pairs":       5,
    "max_msg_per_min": 600,
    "max_bytes_per_min": 1048576,
    "enabled":         true,
    "created":         "2026-05-04T10:00:00Z",
    "notes":           "free tier"
  }
}
```

### Генерация токена

```bash
openssl rand -hex 16   # → 32 hex символа
```

### Reload

Три способа:
- **Auto:** mtime poll каждые `RELAY_USERS_RELOAD_INTERVAL_S` (по умолчанию 30s).
- **SIGHUP:** `kill -HUP <pid>` — мгновенный reload.
- **HTTP:** `POST /admin/users/reload` (требует `X-Admin-Token`).

UserState (счётчики) сохраняется при reload — счётчики не сбрасываются.
Если `enabled` меняется на `false` — все активные conn пользователя
закрываются (kick).

## Monitoring

### Prometheus scrape

```yaml
scrape_configs:
  - job_name: 'dota_relay'
    static_configs:
      - targets: ['127.0.0.1:5051']  # loopback на eft-deploy
    metrics_path: /metrics
```

### Метрики

```
relay_pairs_active{user_id}                              gauge
relay_messages_total{user_id,direction}                  counter
relay_bytes_total{user_id,direction}                     counter
relay_quota_exceeded_total{user_id,reason}               counter
relay_connections_rejected_total{reason}                 counter
relay_uptime_seconds                                     gauge
```

`reason` для rejected: `auth_failed`, `max_pairs`, `rate_limit`,
`bad_hello`, `total_accept`.

## Admin operations

Все требуют header `X-Admin-Token: $RELAY_ADMIN_TOKEN`. Если
`RELAY_ADMIN_TOKEN` не задан — endpoints возвращают 403 (disabled mode).

```bash
# Per-user детально
curl -H "X-Admin-Token: $TOKEN" \
  http://127.0.0.1:5051/admin/stats?user_id=alice

# Reload users.json без рестарта
curl -X POST -H "X-Admin-Token: $TOKEN" \
  http://127.0.0.1:5051/admin/users/reload

# Disable user (закрывает активные conn)
curl -X POST -H "X-Admin-Token: $TOKEN" \
  http://127.0.0.1:5051/admin/users/alice/disable
```

`GET /health` и `GET /metrics` — без auth.

## Локальная сборка и тесты

```bash
cd tools/dota2/relay
go build .              # → relay (или relay.exe)
go test ./...           # 20 unit-тестов (auth, quota, multi-tenant, admin/metrics)
python smoke_test.py    # end-to-end TCP + HTTP smoke (нужен relay.exe рядом)
```

## Production deploy (на VPS eft-deploy / 109.120.152.45)

Standalone compose рядом с существующими сервисами в `/opt/eft/`:

```bash
# с локальной машины
scp -r tools/dota2/relay eft-deploy:/opt/eft/dota_relay

# на VPS
ssh eft-deploy
cd /opt/eft/dota_relay

# подготовить users.json (КОПИРУЙ из примера и впиши настоящие токены)
cp users.example.json users.json
nano users.json   # записать реальные auth_token (openssl rand -hex 16)

# создать .env с RELAY_ADMIN_TOKEN
echo "RELAY_ADMIN_TOKEN=$(openssl rand -hex 32)" > .env

docker compose up -d --build
docker logs -f dota_relay      # ждать "relay listening bind=:5050"

# открыть порт TCP в firewall (admin port :5051 НЕ открывать наружу)
ufw allow 5050/tcp
```

### Graceful shutdown

При SIGTERM relay закрывает listener и admin server, активные пары
отключаются естественным way через EOF. Heartbeat у endpoints должен
переподключаться с backoff'ом. Без специального drain — relay рассчитан
на быстрый рестарт (≤2s downtime), endpoints автоматически переподключатся.

## Security

| Что | Что защищает | Что НЕ защищает |
|---|---|---|
| `auth_token` (relay-level) | от чужого user_id, DoS чужих слотов | от MITM в transit |
| `sig` (HMAC, у endpoints) | integrity message body | от подмены user_id (relay уже отделил) |
| Per-IP rate limit | от spam handshake / SYN flood одного IP | от distributed botnet |
| `max_pairs` per user | от exhaustion одного user'а | от exhaustion разных users (используй `MAX_PAIRS_GLOBAL`) |

**TLS** не нужен по умолчанию: payload компактный, integrity-protected
HMAC'ом, content не sensitive (matchmaking metadata). Если паранойишь —
повесь nginx reverse-proxy с TLS termination на `:5443 → 127.0.0.1:5050`,
relay TCP бинд оставь на loopback.

## Логирование

Структурированный JSON через `log/slog`. Пример event:

```json
{"time":"2026-05-04T12:00:00Z","level":"INFO","msg":"connect",
 "user_id":"alice","pair_id":"p1","role":"master","remote":"82.155.x.x:34521"}
{"time":"2026-05-04T12:00:01Z","level":"INFO","msg":"pair complete",
 "user_id":"alice","pair_id":"p1"}
{"time":"2026-05-04T12:00:30Z","level":"WARN","msg":"quota exceeded",
 "user_id":"alice","reason":"msg_per_min","limit":600,"actual":612}
{"time":"2026-05-04T12:01:00Z","level":"INFO","msg":"disconnect",
 "user_id":"alice","pair_id":"p1","role":"master","duration_s":60,
 "messages":543,"bytes":12345}
```

Подходит для shipping в Loki/Elasticsearch.
