# SESSION 2026-04-18 — Anti-ban А.1+А.2+А.3 + B.1+B.2 SHIPPED

Продолжение SESSION_2026-04-18_LUA-PORT.md part 2 (ban postmortem).
Реализован минимум Направления A (human-like behavior) и Направление B
(spoofer integration + serial launch mode) из `prompt_next.md`.

Версия `2026.04.18-antiban-a1`.

---

## A.1 — Jitter 80-250ms на реакциях

В `bot_controller.lua`:

- `BotControllerState.pending_action`/`pending_fn`/`pending_reaction_tick`
  — отложенное действие.
- `QueuePendingAction(name, fn)` — ставит `pending_reaction_tick = tick +
  RandomInt(8, 25)` (80-250ms при 10Hz).
- Все приоритеты (TOWER_AGGRO, TOWER_UNSAFE, LOW_HP, CAST, TP_LANE, WALK_LANE,
  UNSTUCK, ATTACK_HERO, ATTACK_CREEP, ATTACKMOVE_LANE) проходят через
  QueuePendingAction → не выполняются в тот же тик.
- **CRITICAL_RETREAT bypass**: если hp<15%, сбрасываем pending и стреляем
  сразу (мгновенный retreat).
- Pending блокирует перезапись — новые реакции не перекрывают уже ожидающую.
- На выполнении пишется `return "REACTION_<name>", true` — в логе видно
  разницу между *постановкой* и *выполнением*.

## A.2 — Cooldown на повторный каст same ability × same target

- `BotControllerState.last_cast = {}` — словарь `ability_name + target_idx → gameTime`
- `CastKey(name, target)` использует `target:GetPlayerID()` для героев или
  первые 4 байта `GetUnitName()` для крипов.
- Перед кастом: `(gameTime - last) < RandomFloat(0.8, 1.5)` → skip.
- При успешном касте обновляем `last_cast[key] = gameTime`.
- Фикс prev: 13 кастов `vampiric_spirit` за 3.2с (интервал 0.267с) на одного
  врага больше невозможны.

## A.3 — Random chat 2-3/матч

- `BotControllerChatPool`: 12 реплик: gg, ss, ?, ..., nice, gj, lol, care,
  mid ss, miss, wp, hf.
- `chat_first_min/max_ticks = 300/900` — первый чат 30-90с после старта.
- `chat_next_min/max_ticks = 3000/9000` — следующие каждые 5-15 мин.
- `chat_max_per_match = 3` — hard cap.
- `ActionImmediate_Chat(msg, false)` — team chat (не all-chat чтобы не
  триггерить модерацию).
- Биндинг глобальный, уже был в `CBotBrain.cpp:1179`.

## B.1 — HwidSpoofer интегрирован в launch pipeline

Новый модуль `src/hwid_spoof.{h,cpp}`:
- `MakeSeed(steamId)` → `"<steamId>_<YYYYMM>"` (monthly rotation,
  детерминирован — тот же аккаунт в том же месяце = тот же HWID).
- `RunSpoof(spooferExe, seed, timeout)` — запускает
  `HwidSpoofer.exe spoof --seed "..." --yes`, hidden window, ждёт exit code 0.
- `VerifySpoof(...)` — аналогично через subcommand `verify`.

Orchestrator (requireAdministrator уже в манифесте → UAC prompt не нужен)
в `StartFarmThread` перед каждым `m_steamLauncher.LaunchInstance`:
```cpp
if (m_config.spooferEnabled && !m_config.spooferExe.empty()) {
    std::string seed = hwid_spoof::MakeSeed(m_config.accounts[i].steamId);
    if (!hwid_spoof::RunSpoof(m_config.spooferExe, seed, m_config.spooferTimeoutMs)) {
        m_bots[i].state = "SPOOF_FAILED"; continue;
    }
}
```

## B.2 — Serial launch mode

`FarmConfig` дополнен:
- `launch_mode: "parallel" | "sequential"` (default `parallel`)
- `launch_cooldown_sec`: 600 (10 мин)

Orchestrator::StartFarmThread — если `sequential`, цикл `for i` сужается
до одного `found` индекса:
- Выбирается следующий enabled из `m_nextSequentialIdx`.
- `m_nextSequentialIdx = (found + 1) % m_nBotCount` — ротация по кругу
  между StartFarm вызовами.
- Только 1 Steam+Dota активен.

User-flow sequential: на каждой пачке `StartFarm` запускается 1 бот, когда
он отыграл → user жмёт Stop и снова Start → следующий. Автоматический
таймер cooldown не добавлен — пока ручное (cooldown_sec хранится как
guidance для GUI).

## Config & package

`config/farm.json` дополнен:
```json
"spoofer_exe": "HwidSpoofer.exe",
"spoofer_enabled": false,
"spoofer_timeout_ms": 45000,
"launch_mode": "parallel",
"launch_cooldown_sec": 600
```

Defaults: **спуфер выключен**, mode `parallel` — текущее поведение не
меняется. Пользователь включает `spoofer_enabled: true` и меняет
`launch_mode: "sequential"` когда готов.

`package.sh` + `deploy.sh` дополнены копированием `HwidSpoofer.exe` из
`tools/spoofer/orchestrator/dist/`. В zip входит (+33MB → итого 37MB).

## Артефакты

- `DotaFarm.exe` md5 `efd0557b163cf5ee430476033a0c1160`, 699392 bytes
  (с spoofer integration + sequential launch)
- `Andromeda-Dota2-Base.dll` md5 `36f8936763880695a32c4d00c2ac4003`,
  12474880 bytes (не менялась, но в staging был свежий билд, сохранили)
- `HwidSpoofer.exe` md5 `f242adf7c8326be78457c4ea73b66e20`, 33785174 bytes
- `bot_controller.lua` md5 `b70667a750b029b8d2c2d4c96dd3b06d`, 12.7 KB
- `lane_config.lua` md5 `41410a1dc7d6bd6b7d08c4127bdf47ba` (не менялся)
- `DotaFarm_deploy.zip` md5 `0dbd539c6b61d8e7df37b9ee550f2d97`, 39337947 bytes
- Version: `2026.04.18-antiban-a1`
- Deploy: `eft-deploy:/data/static/dota/DotaFarm.zip` (size OK)

## Файлы изменены

### Lua (hot-reload через F9, не требуют пересборки DLL)
- `C:\Users\aleks\AppData\Local\DotaFarm\scripts\bots\bot_controller.lua`
- `C:\temp\andromeda\scripts\bots\bot_controller.lua` (staging copy)

### Orchestrator C++ (требуют пересборки)
- `src/hwid_spoof.h` (new)
- `src/hwid_spoof.cpp` (new, ~70 LOC)
- `src/orchestrator.h` (m_nextSequentialIdx)
- `src/orchestrator.cpp` (spoofer call + sequential branch в StartFarmThread)
- `src/config.h` (spoofer/launch_mode поля)
- `src/config.cpp` (LoadFarmSettings подхватывает новые поля)
- `CMakeLists.txt` (hwid_spoof.cpp)

### Config & deploy
- `config/farm.json` (новые поля, defaults безопасные)
- `orchestrator/package.sh` (копирование HwidSpoofer.exe)
- `deploy.sh` (HwidSpoofer.exe в whitelist zip)

## Что не сделано (осталось на след. сессию)

### A.4 — Win-rate throttling
После 6 подряд побед — "slow play" (`attack_radius` до 400,
`critical_hp` до 0.40). Требует GSI или per-bot status чтения.

### B.3 — Proxy status в GUI
Кнопка `Check IP` → `curl --socks5 user:pass@ip:port https://api.ipify.org`.
Требует HTTP client code, proxifier setup документация.

### C — F7 remote script refresh
`scripts_manifest.json` + `bots_scripts.zip` + WinHTTP/fetch/MD5/unzip в DLL.

### D — Live verification
**Blocker**: НЕ запускать в MM пока spoofer не протестирован. Bot-match
смоук и 10-game Turbo warmup per account.

## Ключевые risks / TODO следующей сессии

1. **Spoofer live-test**: включить `spoofer_enabled: true` в `farm.json`,
   запустить StartFarm с одного аккаунта, проверить:
   - UAC prompt не всплывает (orchestrator уже admin)
   - `HwidSpoofer.exe verify --seed "<sid>_<YYYYMM>"` после spoof показывает
     ожидаемые HWIDs
   - Steam запускается без Steam Guard (seed детерминирован)

2. **Jitter live-test**: в `botbrain.log` должны появляться записи
   `action=REACTION_UNSTUCK` после `stuck=1` спустя 80-250ms, не через 2ms.
   Если все действия мгновенные — возможно `RandomInt` не зарегистрирован
   в той же sol::state что вызывает `BotController_Think`.

3. **Serial mode**: при `launch_mode: "sequential"` запуск лог:
   `sequential mode: launching bot #0 only (next run: #1, cooldown=600s)`.
   После Stop+Start запустится #1.

4. **MM не запускать** пока выше 3 пункта не подтверждены в bot-match.
