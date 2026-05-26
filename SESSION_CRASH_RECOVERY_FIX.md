# SESSION — Crash Recovery Fix (2026-05-26)

**Branch:** `feature/two-stand-coordination`
**Trigger:** Юзер сообщил: "крашит... в терминальной стадии 1 FPS в 10 сек, что-то стакается, дота крашит. Дота сама не подымается, оркестратор пишет про 3 попытки — попыток не было."

## Найденный root cause (race condition)

В `DotaFarm.log` 2026-05-22 03:30:56 два параллельных recovery thread'а получили **один и тот же PID**:
```
#1: recovery: dota2.exe PID 27816 spawned
#3: recovery: dota2.exe PID 27816 spawned   ← Одна dota на двух владельцев
```

`LaunchDotaOnly` (`dota_launcher.cpp:407`) после spawn возвращал "первый PID не из excludeExistingPids" — при concurrent recovery threads оба видели одну новую dota и оба её claim'или. Один из двух recovery остаётся без процесса → fast-fail → watchdog inkretmentит counter → `max_crashes_per_window=3` за 5 мин exhausted → `KILL_FOREVER` за минуту. В log это выглядело как "3 попытки", но реально dota не запускалась вообще.

Дополнительно: после terminal-stage crash Steam session state может быть отравлен (GC мёртв, IPC pipe stale). Recovery thread пропускал Steam restart если `steam_alive+gc_ready=1`, новая dota fast-fail на authorization.

## 5 fixes deployed

| # | Layer | Файл | Что |
|---|---|---|---|
| 1 | Code | `orchestrator.h`, `orchestrator.cpp:1612` | `std::mutex m_recoveryMx` — serialize all RecoveryThread'ы. Один recovery за раз. Заодно избегаем concurrent shader recompile на WARP. |
| 2 | Code | `dota_launcher.cpp:407` `LaunchDotaOnly` | Prefer `spawnedPid` если `IsProcessAlive` и не в exclude (наш CreateProcess — 100% наш). Fallback на FindDotaPids ∖ exclude — старое поведение для Steam single-instance respawn. |
| 3 | Code | `orchestrator.cpp:1626` `needSteamRestart` | Force Steam restart при `m_watchdog.GetCrashCount(idx) >= 2`. После 2-х fail'ов чистим session state даже если Steam "alive+gc_ready". |
| 4 | Config | `farm.json::crash_recovery` (на обоих стендах) | `max_crashes_per_window 3→10`, `crash_window_min 5→30 min`, `loading_state_grace_s 90→240`, `max_steam_relaunches_per_match 2→5`, `reconnect_cooldown_s 10→15`. Бот выживает 10 крашей за 30 мин вместо 3 за 5 мин. |
| 5 | OS | `HKLM\...\Windows Error Reporting\LocalDumps` (на обоих) | WerFault local dumps включены для `dota2.exe` (`C:\dumps\dota2\`) и `DotaFarm.exe` (`C:\dumps\dotafarm\`). Full dump (`DumpType=2`), keep 10 last. Следующие crashes → реальный stack trace через `cdb -z dump.dmp -c "!analyze -v"`. |

## Binaries / md5

| File | md5 | Deployed |
|---|---|---|
| `DotaFarm.exe` (new) | `c6b125834cc985c99835071736fade4e` | `.131` + `.152` ✓ |
| `Andromeda-Dota2-Base.dll` | `FBB0B460328346C41696AADD5DA5768B` | unchanged |
| `ProxyHook.dll` | (prev) | unchanged |

## Reusable script

`tools/dota2/scripts/relax_crash_recovery.ps1` — параметризованный скрипт применяющий Fix 4 + Fix 5 на любом VDS. Idempotent. Backup `farm.json.bak-<ts>-crashfix`.

## Verification plan

1. **Запуск ферм** на обоих стендах
2. **Watch logs**: при следующем crash в `DotaFarm.log` должно быть:
   ```
   #X: recovery: full restart — kill dota=... steam=... (steam_alive=Y gc_ready=Z crashes=N)
   ```
   Поле `crashes=N` — новое (CR-FIX 2026-05-26).
3. **WerFault dump generation**: после крёша проверить `ls C:\dumps\dota2\` — должны появиться `dota2.exe.<pid>.dmp`.
4. **Concurrent recovery**: если 2+ ботов crashed одновременно — в log будут sequential `recovery` сообщения, не overlapping. Каждый получает unique PID.

## Что НЕ сделано (хочется но юзер сказал "только рестарт")

- Layer 2 prevention: Lua tick-rate downshift на late game (10Hz → 5Hz после 25 мин) — снизило бы CPU/GPU load на terminal stage, но юзер пока не просил
- Layer 3 investigation: дождаться 1-2 fresh dumps (Fix 5), classify crash type (render lockup vs Andromeda race vs OOM) — определит долгосрочный fix
- Stagger N-th recovery: даже с mutex, после восстановления #1 → #2 → #3 → #4 → #5 это 5×120s = 10 мин. Можно добавить shorter wait между фазами (rerun mutex быстрее). Для текущего scope ок.

## Next

E2E pairing test (Phase F) — оба стенда + relay + новый recovery. Когда первый crash случится — увидим работают ли все 5 fixes в живую.
