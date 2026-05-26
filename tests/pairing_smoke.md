# Pairing UX v2 — 2-instance loopback smoke checklist

**Branch:** `feature/two-stand-coordination`
**Build under test:** `tools/dota2/orchestrator/build/Release/DotaFarm.exe`
**Expected MD5:** `177b59ac16d2386f130d36aced942ac1`
**Plan reference:** `C:\Users\aleks\.claude\plans\vivid-knitting-mountain.md` — Verification → Local loopback smoke

Goal: prove the new Pairing UX v2 stack (Pair Code Generate/Paste, расширенный Pairing Panel, Sync Start modal, gated START FARM, Reconnect/Disconnect, STALE LINK detection) работает end-to-end на одной машине через 2 orchestrator instances + local Docker relay, БЕЗ regressions относительно legacy uxV2=false path.

Smoke выполняется **coordinator'ом вручную через RDP/локальную сессию**. Headless run невозможен — DotaFarm.exe требует DX11 окно и interactive GUI.

---

## Section 1 — Prerequisites

### Build
- [ ] `feature/two-stand-coordination` checked out (commits 5bad17d → 3d2b0f1 inclusive)
- [ ] `tools/dota2/orchestrator/build/Release/DotaFarm.exe` exists
- [ ] MD5 совпадает с `177b59ac16d2386f130d36aced942ac1`:
  ```powershell
  Get-FileHash -Algorithm MD5 .\tools\dota2\orchestrator\build\Release\DotaFarm.exe
  ```

### Relay
Two options — выбрать одну:

**Option A — local Docker relay (recommended for isolated smoke):**
- [ ] Docker Desktop запущен
- [ ] Контейнер не запущен заранее (мы поднимаем его шагом setup):
  ```powershell
  docker ps --filter name=dota_relay
  ```

**Option B — already-deployed eft-deploy relay:**
- [ ] `144.31.85.217:5050` reachable from this machine:
  ```powershell
  Test-NetConnection 144.31.85.217 -Port 5050
  ```
- [ ] Ваш `user_id` / `user_auth_token` whitelisted в `tools/dota2/relay/users.json` на сервере

### Profile dirs
Smoke использует **two independent copies of DotaFarm.exe**, каждая в своём dir с собственным `config\farm.json`. Это потому что orchestrator резолвит config относительно exe (`<exeDir>\config\farm.json` — см. `main.cpp:64`), `--profile-dir` flag не существует.

- [ ] Будут созданы `C:\temp\smoke\master\` и `C:\temp\smoke\slave\` (Section 2 step)

### Bot accounts
- [ ] Для smoke достаточно **2-3 ботов на сторону** (не все 5). Они нужны только для проверки что START FARM запускает launch process; full match не требуется.
- [ ] `accounts.json` с этими ботами скопирован в каждый профиль (см. setup script)

---

## Section 2 — Setup script (PowerShell)

Выполнить **один раз** перед первым прогоном. Идемпотентно — повторный запуск перезатирает профили чистым baseline'ом.

```powershell
# === pairing_smoke setup ===
$ErrorActionPreference = "Stop"

$RepoRoot   = "C:\Users\aleks\OneDrive\Документы\реверс курс"
$SrcExe     = Join-Path $RepoRoot "tools\dota2\orchestrator\build\Release\DotaFarm.exe"
$SrcRelDir  = Join-Path $RepoRoot "tools\dota2\orchestrator\build\Release"   # assets, sing-box, WinDivert, scripts, wintun
$SrcCfgDir  = Join-Path $RepoRoot "tools\dota2\config"
$AccountsSrc= Join-Path $RepoRoot "tools\dota2\config\accounts.json"          # ВАЖНО: реальные боты, скопировать вручную если шаблон пуст

$Master = "C:\temp\smoke\master"
$Slave  = "C:\temp\smoke\slave"

# 1. Чистим и создаём dirs
foreach ($d in @($Master, $Slave)) {
    if (Test-Path $d) { Remove-Item $d -Recurse -Force }
    New-Item -ItemType Directory -Path $d | Out-Null
    New-Item -ItemType Directory -Path (Join-Path $d "config") | Out-Null
}

# 2. Копируем EXE + sibling runtime files (sing-box, WinDivert*, wintun, assets/, scripts/)
foreach ($d in @($Master, $Slave)) {
    Copy-Item (Join-Path $SrcRelDir "*") $d -Recurse -Force
}

# 3. Базовый farm.json в каждом профиле — copy template, потом разница ниже
foreach ($d in @($Master, $Slave)) {
    Copy-Item (Join-Path $SrcCfgDir "farm.json") (Join-Path $d "config\farm.json") -Force
    if (Test-Path $AccountsSrc) {
        Copy-Item $AccountsSrc (Join-Path $d "config\accounts.json") -Force
    } else {
        Write-Warning "accounts.json не найден в template — создай вручную перед smoke"
    }
}

# 4. Patch master farm.json: pairing.uxV2=true, pairing.enabled=true, role=master,
#    distinct user_id, baseline relay/pair fields
$masterFarm = Join-Path $Master "config\farm.json"
$jm = Get-Content $masterFarm -Raw | ConvertFrom-Json
if (-not $jm.pairing.PSObject.Properties['uxV2']) {
    $jm.pairing | Add-Member -NotePropertyName 'uxV2' -NotePropertyValue $true
} else { $jm.pairing.uxV2 = $true }
$jm.pairing.enabled         = $true
$jm.pairing.transport       = "relay"
$jm.pairing.role            = "master"
$jm.pairing.user_id         = "smoke_master"
$jm.pairing.user_auth_token = "REPLACE_WITH_SMOKE_MASTER_TOKEN"
$jm.pairing.pair_id         = "smoke-loopback-2026"
$jm.pairing.pair_secret     = "smoke-secret-32chars-XXXXXXXXXXX"   # 32 chars, любой ASCII
$jm.pairing.relay_host      = "127.0.0.1"     # Option A. Для Option B: "144.31.85.217"
$jm.pairing.relay_port      = 5050
$jm | ConvertTo-Json -Depth 20 | Set-Content $masterFarm -Encoding UTF8

# 5. Patch slave farm.json: same pair_id/pair_secret/relay_host, distinct user_id,
#    pairing.enabled=false (slave получит creds через Paste Pair Code на шаге 6 чек-листа)
$slaveFarm = Join-Path $Slave "config\farm.json"
$js = Get-Content $slaveFarm -Raw | ConvertFrom-Json
if (-not $js.pairing.PSObject.Properties['uxV2']) {
    $js.pairing | Add-Member -NotePropertyName 'uxV2' -NotePropertyValue $true
} else { $js.pairing.uxV2 = $true }
$js.pairing.enabled  = $false                # будет включён через Paste flow
$js.pairing.role     = "slave"
$js.pairing.user_id  = "smoke_slave"
$js.pairing.user_auth_token = "REPLACE_WITH_SMOKE_SLAVE_TOKEN"
$js | ConvertTo-Json -Depth 20 | Set-Content $slaveFarm -Encoding UTF8

# 6. Relay startup (Option A only)
$startRelay = $true
if ($startRelay) {
    Push-Location (Join-Path $RepoRoot "tools\dota2\relay")
    docker compose up -d
    Pop-Location
    Start-Sleep -Seconds 2
    docker ps --filter name=dota_relay --format "{{.Status}}"
}

Write-Host ""
Write-Host "Master profile: $Master"
Write-Host "Slave  profile: $Slave"
Write-Host "Relay:         http://127.0.0.1:5050  (admin :5051)"
Write-Host ""
Write-Host "TODO before running smoke:"
Write-Host "  1) Edit user_auth_token в обоих farm.json — реальные tokens из relay/users.json"
Write-Host "  2) Verify accounts.json в каждом профиле содержит >=2 рабочих ботов"
Write-Host "  3) Launch master: cd $Master ; .\DotaFarm.exe"
Write-Host "  4) Launch slave:  cd $Slave  ; .\DotaFarm.exe"
```

**Users seed для local relay.** На стороне `tools/dota2/relay/users.json` (Option A) добавить:
```json
{
  "smoke_master": { "token": "REPLACE_WITH_SMOKE_MASTER_TOKEN", "quota": 100 },
  "smoke_slave":  { "token": "REPLACE_WITH_SMOKE_SLAVE_TOKEN",  "quota": 100 }
}
```
После правки — `docker compose restart relay` (или подождать `RELAY_USERS_RELOAD_INTERVAL_S=30s`).

---

## Section 3 — Manual checklist (14 steps)

Каждый шаг: **Action** → **Expected** → отметить ☐ Pass / ☒ Fail. Failures фиксировать в Section 6 с repro steps.

> Throughout: master = `C:\temp\smoke\master\DotaFarm.exe`, slave = `C:\temp\smoke\slave\DotaFarm.exe`. Открыты в двух разных окнах, два разных process'а.

### 1. Launch master orchestrator
- **Action:** `cd C:\temp\smoke\master ; .\DotaFarm.exe`
- **Expected:** GUI поднялся, в Pairing panel виден badge (либо `CONNECTING`, либо `PAIR READY` если creds валидны, либо `DISCONNECTED` если relay не reachable). НЕТ краша на старте.
- **Pass:** ☐

### 2. Verify uxV2 panel visible
- **Action:** Visual check Pairing panel (под секцией heroes/region)
- **Expected:** Расширенная панель видна (live counters: hb age, RTT, clients, msgSent, msgRecv). Кнопки `GENERATE PAIR CODE`, `PASTE PAIR CODE`, `RECONNECT`, `DISCONNECT` присутствуют.
- **Pass:** ☐

### 3. Master reaches PAIR READY (waiting peer)
- **Action:** Wait ≤10s после launch
- **Expected:** Badge `PAIR READY` (или `WAITING PEER` если backend различает). hb age < 3s, RTT < 100ms loopback. msgSent counter ползёт (hello + ping). msgRecv ползёт если relay отвечает.
- **Pass:** ☐
- **If fail:** проверить `docker logs dota_relay --tail 50` — auth ошибки видны там.

### 4. Generate Pair Code on master
- **Action:** Click `GENERATE PAIR CODE` в Pairing panel
- **Expected:** Modal "Generate Pair Code" открылся. Внутри отображается код формата `DOTAFARM-PAIR-1.<base64url payload>.<crc8 hex>`. Кнопка `COPY` копирует в clipboard. Длина кода 80-200 символов.
- **Pass:** ☐
- **If fail:** ошибка `BadField`/`pair_secret too short` обычно означает что в master farm.json `pair_secret` < 32 chars.

### 5. Launch slave orchestrator
- **Action:** Открыть второе окно: `cd C:\temp\smoke\slave ; .\DotaFarm.exe`
- **Expected:** GUI поднялся. Pairing panel badge = `NOT CONFIGURED` (потому что `pairing.enabled=false` в slave farm.json после setup).
- **Pass:** ☐

### 6. Paste Pair Code on slave
- **Action:** На slave: click `PASTE PAIR CODE` → вставить код из step 4 в текстовое поле → проверить decoded preview (host, user_id, pair_id, role=master в коде) → click `Apply & Connect`
- **Expected:** Modal закрылся, slave farm.json атомарно перезаписан (SavePairingConfigAtomic), backend reinit'нулся (Orchestrator::ReinitPairing). Badge переходит `CONNECTING` → `PAIR READY` within 5s.
- **Pass:** ☐
- **Note:** если slave role в коде = `master`, slave должен автоматически переключиться на `slave` (config.cpp::ApplyPairCode инвертирует).

### 7. Both orchestrators at PAIR READY
- **Action:** Сравнить обе панели
- **Expected:** Оба показывают `PAIR READY`. hb age < 3s в обоих. RTT < 100ms. clients = 1/1 (есть peer). msgSent > 0, msgRecv > 0 в обоих.
- **Pass:** ☐

### 8. START FARM gated until PAIR READY (UX check)
- **Action:** Visual check на master: кнопка `START FARM` enabled? Label содержит "(sync)"?
- **Expected:** Кнопка enabled (потому что мы на PAIR READY). Label `START FARM (sync)` (или подобный — см. gui.cpp:2055-2059). Если PAIR READY ещё не достигнут — кнопка disabled с tooltip.
- **Pass:** ☐
- **Negative re-check:** временно остановите relay (`docker stop dota_relay`), убедитесь что button disable'ится через 12s (STALE LINK). Потом `docker start dota_relay`, дождитесь PAIR READY и продолжайте.

### 9. Master triggers Sync Start
- **Action:** На master click `START FARM`
- **Expected:** На master state переходит `STARTING` (visible в Pairing panel или general status). На slave **modal** "Peer Wants To Start Farming" появляется в течение 1s с countdown 30s и кнопками `ACCEPT & START` / `DECLINE`.
- **Pass:** ☐

### 10. Slave ACCEPT — both LAUNCHING
- **Action:** На slave click `ACCEPT & START` (в течение 30s до timeout)
- **Expected:** Modal закрылся. Оба orchestrator одновременно переходят в STARTING → LAUNCHING (bot launch process кидается — Steam запускается, bots привязываются). Pairing panel state = `RUNNING` или `LAUNCHING` в обоих. msgSent/Recv counters растут.
- **Pass:** ☐
- **Note:** Полный match не нужен для smoke. Если боты успели вылететь в Steam-окно — pass. Можно закрыть Steam окна и сразу останавливать ботов (stop_farm command где-нибудь в menu).

### 11. STALE LINK detection on relay outage
- **Action:** `docker stop dota_relay` (Option A) или `ssh eft-deploy "cd /opt/eft && docker stop dota_relay"` (Option B)
- **Expected:** В течение 12s оба badges переходят в `STALE LINK` (warn) → `DISCONNECTED` (error). hb age растёт > 30s. Кнопка `START FARM` disable'ится (gated).
- **Pass:** ☐

### 12. Auto-reconnect after relay restart
- **Action:** `docker start dota_relay` (Option A) или восстановить на eft-deploy
- **Expected:** В течение 15s (exponential backoff 1.5s→12s) оба badges → `CONNECTING` → `PAIR READY`. RTT восстанавливается. Никаких manual действий не требуется.
- **Pass:** ☐
- **If fail:** check логи orchestrator (рядом с exe `DotaFarm.log`) на auth failures — если 30-60s long backoff из-за auth — это intended (см. plan risk table).

### 13. DISCONNECT button → NOT CONFIGURED, peer sees STALE
- **Action:** На master click `DISCONNECT`
- **Expected:** Master badge → `NOT CONFIGURED` (pairing.enabled flipped to false, config saved). На slave badge через 12s → `STALE LINK` (peer ушёл).
- **Pass:** ☐

### 14. RECONNECT restores from saved code
- **Action:** На master click `RECONNECT` (либо снова `PASTE PAIR CODE` если поток требует) — должен использовать saved pair_secret/pair_id из farm.json
- **Expected:** Master badge → `CONNECTING` → `PAIR READY` within 5s БЕЗ необходимости снова вставлять код. Slave badge возвращается → `PAIR READY` (peer вернулся). msgSent/Recv обнуляются для новой сессии или продолжают расти — не критично.
- **Pass:** ☐

### Edge cases (sub-steps, не считать отдельными в pass count)

**14a. Broken Pair Code rejected**
- **Action:** На fresh slave (или после DISCONNECT) `PASTE PAIR CODE` → вставить код с typo в середине payload (поменять один char) → Apply
- **Expected:** Visible error: `CRC mismatch` / `BadFormat` / `BadBase64`. Backend НЕ reinit'нулся, farm.json НЕ изменился (можно проверить mtime).
- **Pass:** ☐

**14b. Simultaneous START FARM tie-break**
- **Action:** Оба orchestrator на PAIR READY → одновременно нажать `START FARM` (в пределах 1-2 сек). Coordinator должен попросить ассистента или вторую руку.
- **Expected:** Sync Start FSM делает lexicographic tie-break по request_id UUID. Один initiator → STARTING. Второй получает `start_declined` (или поглощается как duplicate), его modal либо не открывается, либо закрывается с toast "peer already started".
- **Pass:** ☐
- **If fail:** проверить sync_start_coordinator.cpp tie-break logic — это High risk row в plan risk table.

**14c. DECLINE → toast on initiator**
- **Action:** Master click `START FARM` → на slave modal `DECLINE`
- **Expected:** Slave modal закрылся. Master state возвращается в `PAIR READY`. Toast/status line у master показывает "Peer declined start".
- **Pass:** ☐

---

## Section 4 — Pass criteria

Smoke pass tiers:

- ☐ **≥ 10 / 14** core steps PASS → **smoke OK**, ready for 2-stand deploy (Section 5)
- ☐ **14 / 14** core steps + все 3 edge cases PASS → **production ready**, тэгать и deploy

Если < 10/14 — НЕ deploy'ить. Фиксировать failures в Section 6, передавать в T12 (cpp-reviewer) / T13 (silent-failure-hunter) или back-port в feature branch для повторной итерации.

---

## Section 5 — Production deploy checklist

Выполнять **после** smoke pass tier ≥ 10/14 (предпочтительно 14/14).

### 5.1 Enable uxV2 on .131 and .152
Используем `tools/dota2/scripts/patch_farm_pairing.ps1` — пока он НЕ trogает `uxV2`, нужно либо расширить скрипт, либо ручной jq pass на каждом стенде:

```powershell
# Расширение скрипта (one-line patch в patch_farm_pairing.ps1):
# Добавить после `$j.pairing.relay_port = 5050`:
#   if (-not $j.pairing.PSObject.Properties['uxV2']) {
#       $j.pairing | Add-Member -NotePropertyName 'uxV2' -NotePropertyValue $true
#   } else { $j.pairing.uxV2 = $true }

# Применить на .131 (master):
ssh dotahost "powershell -Command `"Invoke-Expression (Get-Content C:\temp\patch_farm_pairing.ps1 -Raw)`"" `
    -StandId stand_a -Role master -RelayHost 144.31.85.217

# Применить на .152 (slave): аналогично
```

Ручной альтернативный путь (через PowerShell ConvertFrom-Json, БЕЗ скрипта):
```powershell
$f = "C:\Users\Administrator\AppData\Local\DotaFarm\config\farm.json"
$j = Get-Content $f -Raw | ConvertFrom-Json
if (-not $j.pairing.PSObject.Properties['uxV2']) {
    $j.pairing | Add-Member -NotePropertyName 'uxV2' -NotePropertyValue $true
}
$j.pairing.uxV2 = $true
$j | ConvertTo-Json -Depth 20 | Set-Content $f -Encoding UTF8
```

После правки: рестарт orchestrator на стенде.

### 5.2 48h soak
- [ ] Ферма работает на обоих стендах 48h без regressions
- [ ] `DotaFarm.log` на обоих не показывает new crashes / repeating warnings
- [ ] Periodic check pairing panel: `PAIR READY` стабилен, hb age < 5s sustainably
- [ ] Lobby matches: ≥ 1 successful pair → ACCEPT → in-game за 48h

### 5.3 Deploy
- [ ] `bash tools/dota2/deploy.sh 2026.05.26-pairing-ux-v2` (зальёт DotaFarm.zip на v1per.tech)
- [ ] Verify download: `curl -I https://v1per.tech/dota/DotaFarm.zip` returns 200 + Content-Length expected
- [ ] Smoke download + unzip на test machine, проверить что версия включает `uxV2` в farm.json template

### 5.4 UPDATE_LOG.txt
- [ ] Обновить `tools/dota2/UPDATE_LOG.txt` user-facing changelog (русский, < 12 строк, без имён файлов / классов / функций).
- [ ] Заголовок: `DotaFarm — апдейт 2026-05-26`
- [ ] Контент: что юзер заметит (новая Pairing panel? Sync Start кнопка? Lower-friction связь между стендами?). НЕ упоминать `uxV2`, `Sync Start FSM`, `pair_code` — это технические термины.
- [ ] Link: `Скачать: https://v1per.tech/dota/DotaFarm.zip`

### 5.5 Tag + PR
- [ ] `git tag deploy/2026-05-26-pairing-ux-v2`
- [ ] `git push origin feature/two-stand-coordination --tags`
- [ ] Create PR на github.com/maximymym/reverse-course-tools: `feature/two-stand-coordination` → `main`
- [ ] PR description: ссылки на план (`vivid-knitting-mountain.md`), summary всех Wave 1-4 changes, smoke results (✓ N/14)

---

## Section 6 — Failures log

> Заполняется coordinator'ом во время smoke. Каждый failed step — отдельный блок.

### Failure #N — Step <X>
- **Step:** <copy step title>
- **Expected:** <copy expected>
- **Observed:** <что произошло реально>
- **Repro steps:**
  1. ...
  2. ...
- **Suspected cause:** <FSM bug? config write race? auth? GUI state divergence? — см. plan risk table>
- **Logs:** `<exeDir>\DotaFarm.log` lines / `docker logs dota_relay` lines
- **Action item:** <bugfix branch name / re-open Task #N>

---

## Notes / Gotchas

- **Кириллица в exe path:** `C:\temp\smoke\` намеренно ASCII-only — multiple Windows APIs (CE injectDLL, некоторые WinHTTP edge cases) ломаются на Cyrillic paths. Не запускать DotaFarm.exe из `OneDrive\Документы\...`.
- **`docker compose` vs `docker-compose`:** в зависимости от Docker Desktop версии. Скрипт использует современный `docker compose` (без дефиса).
- **`users.json` reload:** локальный relay перечитывает users каждые 30s (`RELAY_USERS_RELOAD_INTERVAL_S=30`). Если нет терпения — `docker compose restart relay`.
- **Two exe instances + Single Steam:** smoke gated на STARTING/LAUNCHING (step 10) НЕ требует чтобы оба процесса успешно подняли Steam. Один Steam может быть busy / занят — главное что pairing handshake прошёл и both orchestrators agreed to start. Bot launch fail допустим в smoke (не critical path для pairing UX v2 verification).
- **Если local accounts.json пуст / шаблонный:** orchestrator поднимется в setup wizard mode. Pairing panel всё равно должна работать (там `pairing.enabled` checked independently of accounts presence). Step 10 (launch bots) будет fail — это OK для smoke; passing 9/14 core ≠ smoke fail, но фиксировать в failures section.
