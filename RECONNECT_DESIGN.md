# Авто-реконнект при крашe dota2.exe в катке

**Цель:** orchestrator замечает падение `dota2.exe` одного из 5 ботов в катке за <30 с, перезапускает Dota в том же sandbox, реинжектит Andromeda DLL, возвращается в активный матч через GC reconnect, **не валит остальную пати**.

## 1. Detection (signal fusion)

Три независимых сигнала, объединяются в один FSM (см. ниже). Использовать только один — false-positive heaven (idle alt-tab, loading screen, GC ping spike). Сочетание ≥2 — high confidence.

| Сигнал | Источник | Threshold | False-positive риски |
|---|---|---|---|
| **S1: process gone** | `DotaLauncher::IsProcessAlive(dotaPid)` | мгновенно | пользователь убил dota2 руками — **valid** crash trigger |
| **S2: heartbeat freeze** | `bot.heartbeatAgeMs` (status_<pid>.json) | `>15s` SUSPECTED, `>40s` CONFIRMED | loading screen ~30 с (Dota main thread занят), patch download, paused match. **Замаскировать** через `bot.gameStateId` — игнорить freeze пока `state==LOADING_SCREEN/HERO_SELECTION` или `bot.paused==true` |
| **S3: dump file watch** | `FindFirstChangeNotificationA("C:\\BotSteam\\<i>\\dumps")` filter `crash_dota2.exe_*.dmp` | мгновенно | dump для `assert_*` / `gameoverlayui64.exe` НЕ считать (бот 1 имеет 19 assert dump'ов — это soft-asserts, dota2 жива) |

**Decision rule:** `CONFIRMED_DEAD = S1 OR (S3 AND heartbeat_age>5s) OR (S2>40s AND not in LOADING/PAUSED)`. S1 — главный, остальные подстраховка если CreateToolhelp лагает в момент tick'а (~2с polling в `MonitorTick`).

## 2. Recovery FSM

Per-bot state machine, поверх существующего `BotInstance`. Новые поля: `RecoveryState recovery`, `int crashCount`, `time_point lastCrashAt`, `uint64_t lastKnownLobbyId`, `int reconnectAttempts`.

```
HEALTHY
  │ S2>15s ИЛИ S3 (dump appeared)
  ▼
SUSPECTED_DEAD  ── (S2 recover) ──► HEALTHY
  │ S1 ИЛИ S3+S2>5s ИЛИ S2>40s
  ▼
CONFIRMED_DEAD  → snapshot lastKnownLobbyId / wasInMatch=(state==GAME_IN_PROGRESS) / crashCount++
  │ guard: crashCount<max_per_match, cooldown elapsed
  │ если 3+ за 5 мин → DEAD (kill instance, не restart)
  ▼
RELAUNCHING      → m_proxy.RemoveRootPid(steam) → kill stale dota PID если жив зомби → DotaLauncher::KillDotaMutex(остальные доты) → SteamLauncher::LaunchDota(idx, cfg, m_bots[idx].steamPid) (Steam ЖИВ → не трогаем!)
  │ wait dota2.exe PID (≤120s, иначе FAILED)
  ▼
INJECTING        → Injector::WriteProxyConfig (proxy+hwidSeed: ТОТ ЖЕ seed что и при оригинале — иначе HWID меняется в matchе → instant VAC flag) → InjectLoadLibrary(ProxyHook.dll) → wait client.dll → WriteInstanceConfig (тот же idx/role/hero pool/party) → InjectLoadLibrary(Andromeda)
  │ wait status_<newpid>.json появился (≤30s) И heartbeat_ms свежий
  ▼
RECONNECTING     → wait gc_ready=true (status.connection.gc_ready) → если wasInMatch И lastKnownLobbyId≠0: ждём появления RECONNECT prompt → автоклик через DLL command (см. §5)
  │ heartbeat_ms растёт И (gameStateId==GAME_IN_PROGRESS ИЛИ wasInMatch=false)
  ▼
IN_GAME → reset reconnectAttempts, reset crashCount после 5 мин stable
```

Timeouts: SUSPECTED→CONFIRMED ≤25s; CONFIRMED→RELAUNCHING immediate; RELAUNCHING ≤120s; INJECTING ≤60s; RECONNECTING ≤90s. Любой timeout → FAILED → подсчитать в `crashCount`, retry с backoff.

## 3. Реализация — файлы

**Новый модуль** `crash_watchdog.h/.cpp`:
- `CrashWatchdog::Tick(BotInstance& bot, FarmConfig& cfg)` — оценка 3 сигналов, переход FSM.
- `DumpWatcher` thread-per-bot: `FindFirstChangeNotificationA("C:\\BotSteam\\<i>\\dumps", FALSE, FILE_NOTIFY_CHANGE_FILE_NAME)` → атомарный `flag` в `BotInstance::recovery.dumpPending`.
- `RecoverInstance(int idx)` — выполняет состояния RELAUNCHING/INJECTING/RECONNECTING на детачнутом thread (как `StartFarmThread` — main loop не блокируется).

**Расширить:**
- `orchestrator.cpp` `MonitorTick()` строки 886-898 — вместо `if(!IsProcessAlive)` → `m_watchdog.Tick(bot, m_config)`. Текущий `RestartInstance` (строки 1026-1037) **переписать** под FSM (он сейчас kills mutex и спавнит Steam — игнорирует существующий steamPid и не реинжектит DLL).
- `orchestrator.h` — добавить `CrashWatchdog m_watchdog;` поле, в `BotInstance` — `RecoveryState recovery{};` струкутру.
- `dota_launcher.h` — раскрыть `LaunchDotaOnly(steamPid, idx, cfg)` (отдельный путь от полного `StartFarmThread` — Steam уже жив, нам нужен только spawn dota через тот же `BotSteam\<i>\steam.exe -applaunch 570 ...`).
- `injector.cpp` — `WriteProxyConfig` уже принимает hwidSeed; кэшировать seed в `BotInstance::recovery.hwidSeed` после первого инжекта чтобы при relaunch использовать тот же.

## 4. Конфиг (новые поля `farm.json`)

```json
"crash_recovery": {
    "enabled": true,
    "heartbeat_suspect_s": 15,
    "heartbeat_confirm_s": 40,
    "dump_watch_enabled": true,
    "max_reconnects_per_match": 3,
    "max_crashes_per_window": 3,
    "crash_window_min": 5,
    "reconnect_cooldown_s": 10,
    "loading_state_grace_s": 90,
    "respoof_hwid_on_relaunch": false
}
```

`respoof_hwid_on_relaunch=false` — ВАЖНО: при reconnect mid-match HWID должен совпадать с pre-crash, иначе Valve flag (см. NOT TO DO).

## 5. Auto-click RECONNECT prompt — выбор

Варианты от худшего к лучшему:

1. **Win32 mouse_event на координаты** — отбросить. Координаты зависят от resolution/UI scale, FindWindow найдёт окно но не кнопку.
2. **OpenCV image-match** — overkill, +30 МБ зависимости, false-positive на splash screens.
3. **GSI HUD button** — GSI **не отдаёт** UI state, только gamestate. Не подходит.
4. **Steam GC packet** (CMsgClientToGCSpectateUserOrLastPlayed/Reconnect) — требует reverse доп. proto, рискованно.
5. **Andromeda DLL command file** — **БЕРЁМ ЭТО.** DLL уже читает `C:\temp\andromeda\instance_<pid>.json` для конфига. Расширяем: orchestrator пишет `C:\temp\andromeda\command_<pid>.json` `{cmd:"reconnect_to_match", lobby_id:123}`, DLL в SM tick видит файл, дёргает `IDOTAGameRules->ReconnectToLobby()` ИЛИ если в menu — нажимает кнопку через panorama JS hook (`$.DispatchEvent("DOTAReconnectToMatch")`). DLL уже в процессе → доступ к panorama без UI clicks. Самый надёжный, no race condition с window focus.

## 6. Edge cases

- **Crash в loading screen** — `gameStateId==INIT/HERO_SELECTION` → grace period 90s, не считать heartbeat freeze за crash. После timeout — full relaunch цикл.
- **Party leader crashed** — после восстановления leader должен переотправить invite. После INJECTING переинжектить с `role="leader"`, DLL увидит pendingInviteId=0 → создаст новую party. Members в SM state получат invite как обычно (`BuildPartyMembers` уже handles это в инъект-конфиге).
- **3+ crashes за 5 мин** → instance в `DEAD` state, orchestrator больше не пытается. UI показывает "Crash loop — manual restart needed". Защита от Valve `auth_pending` block после серии crashed connections.
- **Sandbox locks** — после crash Dota могла оставить `dota_singleton_mutex` (особенно при assert dumps). Перед `LaunchDota` → `KillDotaMutex` на ВСЕХ живых dota PID (как делает `StartFarmThread` строки 401-410).
- **Sequential crashes одного бота на одной катке** — `max_reconnects_per_match=3` с reset когда `match.lobby_id` сменился.
- **Steam клиент тоже умер** (видим `assert_steam.exe_*.dmp` в bot1) — escalate до full per-bot relaunch (Steam+Dota), используя `RestartInstance` оригинальную логику но per-bot не global.

## 7. NOT TO DO

- **НЕ перезапускать Steam** при crashed dota если steamPid жив. Steam relaunch = новый login session = `auth_pending` ban risk + sing-box re-route delay.
- **НЕ переспуфить HWID** при reconnect (`respoof_hwid_on_relaunch=false` default). Mid-match HWID change — anti-cheat detect signature. Spoof только при full StartFarm cycle.
- **НЕ реконнектиться если match закончился** (`gameStateId==POST_GAME` либо `lobby_id` поменялся в GSI пока ждали). Просто вернуться в IN_MENU.
- **НЕ инжектить Andromeda до client.dll loaded** — `WaitForDotaReady` обязательно перед InjectLoadLibrary (иначе DLL крашит от missing engine symbols).
- **НЕ удалять dump файлы** до telemetry::UploadDump (orchestrator.cpp строки 853-879). Crash dumps — единственный диагностический artifact для root-cause работ team-lead'а (Tasks #1, #2).
- **НЕ агрессивный polling** dumps/ через FindFirstChangeNotification — один handle per bot (5 шт), не per-tick scan (избегаем `IsProcessAlive` lag-induced false negative — оставить S1 как primary).
