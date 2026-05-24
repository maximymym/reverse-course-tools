# Prompt для следующей сессии — Dota 2 Farm

**Контекст предыдущих сессий** (прочитать ДО начала работы):
- `SESSION_2026-04-18.md` — Phase A (GSI HTTP + schema 2 + party panel)
- `SESSION_2026-04-18_PhaseB_RE.md` — reverse engineering singleton/SOCache
- `SESSION_2026-04-18_PhaseB_DONE.md` — Phase B v1 + party state fixes
- `SESSION_2026-04-18_LUA-PORT.md` — Lua port + F9 hot-reload + ban postmortem
- `SESSION_2026-04-18_ANTIBAN-A1.md` — **последняя сессия** — A.1/A.2/A.3/B.1/B.2 SHIPPED

Текущий актуальный билд `2026.04.18-antiban-a1`:
- DLL `Andromeda-Dota2-Base.dll` md5 `36f8936763880695a32c4d00c2ac4003`
- EXE `DotaFarm.exe` md5 `efd0557b163cf5ee430476033a0c1160` (spoofer + serial launch)
- `HwidSpoofer.exe` md5 `f242adf7c8326be78457c4ea73b66e20` (33 MB, включён в zip)
- `bot_controller.lua` md5 `b70667a750b029b8d2c2d4c96dd3b06d` (jitter+cast_cd+chat)
- `DotaFarm_deploy.zip` md5 `0dbd539c6b61d8e7df37b9ee550f2d97`, 39337947 bytes
- Deployed `eft-deploy:/data/static/dota/DotaFarm.zip`
- Loader URL: `https://v1per.tech/dota/DotaFarm.zip`
- Version: `https://v1per.tech/dota/version.txt` → `2026.04.18-antiban-a1`

---

## КРИТИЧНО — live verification ДО любого MM

Направления A.1/A.2/A.3 + B.1/B.2 уже закодированы, но в live match НЕ
тестировались. Пользователь НЕ должен идти в PvP-MM, пока ниже 3 пункта
не подтверждены в bot-match:

### Verify 1 — Spoofer запускается без сбоев

В `config/farm.json` поставить `"spoofer_enabled": true`.
StartFarm → в `DotaFarm.log` должно быть:
```
#0: spoofing HWID (seed=76561198...._202604)...
#0: HWID spoofed OK
```

Проверить `HwidSpoofer.exe verify --seed "<steamId>_202604"` вручную —
SMBIOS UUID / MAC / MachineGuid совпадают с ожидаемыми.

Если `SPOOF_FAILED` — запустить orchestrator как admin (он уже должен быть
в manifest, но проверить). Проверить что `dist/HwidSpoofer.exe` в zip.

### Verify 2 — Jitter работает

В bot-match 1 бот → смотреть `C:\temp\andromeda\botbrain.log`:
- `action=REACTION_UNSTUCK` через 80-250ms ПОСЛЕ `stuck=1` (а НЕ через 2ms)
- `action=REACTION_CAST_<ability>` через 80-250ms после обнаружения цели
- НЕТ повторных `CAST_vampiric_spirit` чаще 1/секунду на одного target

Если все действия в тот же тик — возможно `RandomInt`/`RandomFloat` не
находятся из `BotController_Think` (проверить `LuaStubs.hpp`, они там на строке 66).

### Verify 3 — Serial launch mode

Поставить `"launch_mode": "sequential"` в `farm.json`.
StartFarm → лог:
```
sequential mode: launching bot #0 only (next run: #1, cooldown=600s)
```
Только 1 Steam активен. После Stop+Start — запускается #1.

---

## Направление A.4 — Win-rate throttling (опц., 20 мин)

После 6 подряд побед — "slow play" (урезать `attack_radius` до 400,
`critical_hp` до 0.40 → бот отходит чаще).

- Читать `gamesPlayed` и `winRate` из `Settings::BotFarm` или
  per-bot status JSON (`DllLauncher` пишет в `status_<pid>.json`).
- В `BotController_Think` смотреть `BotControllerState.win_streak` (если
  есть хук или GSI endgame → incr/reset).
- Этот приоритет требует dll-side state exposure — может быть проще
  пробросить `win_streak` через bindings либо через shared file.

---

## Направление B.3 — Proxy status в GUI (документация + GUI кнопка)

Пользователь покупает IPRoyal Static Residential ISP вручную. В
`orchestrator` добавить section в GUI "Proxy status":
- Кнопка `Check IP` → `curl --socks5 user:pass@ip:port https://api.ipify.org`
- Если публичный IP != proxy IP → красный баннер "ProxyOff — не запускать Steam"

Подключение через **Proxifier Rules**:
- `steam.exe`, `steamwebhelper.exe`, `dota2.exe`, `gameoverlayui.exe` → SOCKS5
- Документировать в `PROXY_GUIDE.md`.

---

## Направление C — F7 remote script refresh

**Идея:** править `bot_controller.lua` у себя, пушить на сервер, удалённый
человек жмёт F7 → DLL тянет zip со скриптами → HotReload без пересборки.

### C.1 — Serverside

Два файла в `/data/static/dota/`:
- `bots_scripts.zip` — архив `scripts/bots/*.lua` (~500 KB)
- `scripts_manifest.json` — `{ "version": <unix_ts>, "md5": "...", "files": [...] }`

Deploy script `tools/dota2/deploy_scripts.sh`:
```bash
cd C:/temp/andromeda/scripts
powershell -Command "Compress-Archive -Path 'bots\\*.lua' -DestinationPath '/c/temp/bots_scripts.zip' -Force"
md5=$(md5sum /c/temp/bots_scripts.zip | awk '{print $1}')
echo "{\"version\":$(date +%s),\"md5\":\"$md5\"}" > /c/temp/scripts_manifest.json
scp /c/temp/bots_scripts.zip /c/temp/scripts_manifest.json eft-deploy:/data/static/dota/
```

### C.2 — F7 в DLL

В `CBotBrain.cpp` или `DllLauncher.cpp`:
- `GetAsyncKeyState(VK_F7) & 0x8000` + debounce (сейчас F9 — локальный reload)
- `FetchManifest()` — WinHTTP GET `/dota/scripts_manifest.json`
- Сравнить `manifest.version` vs persisted `scripts_version.txt`
- Если новее: GET `/dota/bots_scripts.zip`, MD5 verify, unzip через
  `powershell Expand-Archive -Force -Path zip -DestinationPath temp` + atomic swap
- `HotReload()` после замены

### C.3 — Безопасность

- Signature: manifest подписан ECDSA (reuse ключи из STALCUBE —
  `tools/stalcube/keys/`). DLL валидирует перед применением.
- Size cap 5 MB / 500 KB на файл.
- Rate limit: не чаще 1 update в 10с.

---

## Направление E — Phase B v2 (SOCache pull) — отложено

Без изменений. Приоритет низкий пока антибан не стабилен.

## Направление F — Matchmaking GC (CMsgStartFindingMatch) — отложено

Пользователь играет через UI "Play vs Bots", GC путь не нужен для
начального тестирования.

---

## Открытые баги (мониторинг)

- **2.1 попап party invite** — косметика, low priority
- **2.2 скиллы после lvl 2** — проверить в live после A (jitter + cd могут фиксить)
- **2.3 мид-бот АФК** — теоретически fixed через `GetCreepFrontLocation`
- **2.4 POST_GAME → IN_MENU auto-requeue** — без изменений

---

## Что нужно от пользователя

1. **Прокси** (IPRoyal Static Residential ISP) — credentials для Proxifier
2. **Spoofer driver**: проверить есть ли `spoofer.sys` в сборке.
   Путь по умолчанию: `tools/spoofer/driver/spoofer_driver.sys` или
   `C:\temp\spoofer_driver.sys`. Без драйвера spoofer работает без SMBIOS
   (registry + MAC only — слабее).
3. Свежий `botbrain.log` с bot-match после A.1/A.2/A.3 — для verification.
4. **НЕ соваться в live PvP** пока verify 1/2/3 не подтверждены.

---

## Готовая инфраструктура (не переделывать)

- Lua port + F9 hot-reload (`lane_config.lua`, `bot_controller.lua`)
- **A.1** jitter 80-250ms на реакциях
- **A.2** cooldown 0.8-1.5с на same ability × target
- **A.3** random chat 2-3/матч (team chat)
- **B.1** HwidSpoofer в launch pipeline (`hwid_spoof.h/.cpp`)
- **B.2** serial launch mode (один аккаунт за раз, ротация)
- botbrain.log в telemetry (заливается на сервер каждые 60с)
- Phase A GSI listener + status schema 2
- Phase B v1 runtime locator (singleton/SOCache без hardcoded RVA)
- Pause All toggle + shared pause.flag
- Shared ping_data.bin
- Party state reset fixes
- HwidSpoofer.exe в zip (33 MB, автодоставка на таргет)

---

## Ключевые файлы

**Исходники DLL:** `C:/temp/andromeda_src/Andromeda-Dota2-Base/Andromeda/BotFarm/BotBrain/`
- `CBotBrain.cpp/hpp` — state + Think + HotReload + LoadBotScripts
- `LuaStubs.hpp` — global stubs (RandomInt/RandomFloat, lane fallback, chat)
- `LuaUnitProxy.cpp/hpp` — UnitHandle / AbilityHandle / ItemHandle

**Исходники orchestrator:** `tools/dota2/orchestrator/src/`
- `main.cpp`, `orchestrator.cpp` — main loop + spoofer+serial launch
- `hwid_spoof.{h,cpp}` — spoofer wrapper (new)
- `dota_launcher.cpp`, `steam_launcher.cpp` — launch pipeline
- `telemetry.h` — upload (botbrain.log included)
- `gui.cpp` — ImGui GUI (⬅ Proxy status здесь для B.3)

**Исходники spoofer:** `tools/spoofer/orchestrator/`
- `spoofer.py`, `HwidSpoofer.spec`, `dist/HwidSpoofer.exe`
- `driver/spoofer_driver.c` — kernel driver

**Runtime:**
- `C:/temp/andromeda/scripts/bots/` — staging (user правит)
- `C:/Users/aleks/AppData/Local/DotaFarm/` — runtime (DLL + scripts)
- `C:/temp/andromeda/botbrain.log` — per-tick telemetry
- `C:/temp/andromeda/debug.log` — DLL DEV_LOG
- `C:/Users/aleks/AppData/Local/DotaFarm/DotaFarm.log` — orchestrator

**Сервер:**
- `eft-deploy:/data/static/dota/DotaFarm.zip` — loader target
- `eft-deploy:/data/static/dota/version.txt` — current version string
- `eft-deploy:/data/logs/<email>/` — telemetry logs
