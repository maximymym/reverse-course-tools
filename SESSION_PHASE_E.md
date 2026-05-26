# SESSION_PHASE_E — Lua FSM states METEOR_SQUAD + SIDE_BAIT

**Date:** 2026-05-26
**Branch:** `feature/two-stand-coordination`
**Scope:** Phase E плана `vivid-knitting-mountain.md`

## Что сделано

Добавлены **2 новых FSM state'а** для 2-стенд сценария координации через `team_strategy`:

### METEOR_SQUAD (id=7) — WIN команда
- Активный при `team_strategy=="WIN"` + `game_time >= strategy_enable_time` (default 1200с = 20 мин)
- Все 5 ботов rally в mid lane перед enemy T1 (centroid alignment если разъехались на >600r)
- AttackUnit на enemy tower когда видна
- `util/items.lua::HANDLERS.item_meteor_hammer` УЖЕ существует и сам стреляет meteor когда tower в 500r + не channelling-risk (без enemy heroes в 1200, <2 creeps в 600). Cast приоритетнее handler.Run (см. `bot_controller.lua:561`), channel не отменяется.
- Exit: hp_critical → RETREAT; strategy снят → LANE_FARM

### SIDE_BAIT (id=8) — LOSE команда
- Активный при `team_strategy=="LOSE"` + `game_time >= strategy_enable_time`
- Per-bot lane assignment по `bot:GetPlayerID() % 5`:
  - slot 0,1 → top (lane 1)
  - slot 2,3 → bot (lane 3)
  - slot 4 → jungle (safe-side neutrals)
- Сидит на `safe_pos` своей assigned lane (amount=0.2 = ближе к своей базе)
- Если враг ближе `side_bait_retreat_dist` (1200) → фонтан
- NEVER идёт в mid, NEVER пушит волну
- Exit: hp_critical → RETREAT; strategy снят → LANE_WAIT

## Изменённые / новые файлы

```
M  scripts_source/bots/config.lua         — strategy_enable_time 1800→1200; +meteor_squad_*; +side_bait_*
M  scripts_source/bots/fsm.lua            — enum METEOR_SQUAD/SIDE_BAIT; conds; Transitions ВО ВСЕ states
M  scripts_source/bots/context.lua        — ctx.player_id, team_slot, side_bait_lane, side_bait_kind
M  scripts_source/bots/states/init.lua    — registration 2 new handlers
A  scripts_source/bots/states/meteor_squad.lua  (новый, ~70 LOC)
A  scripts_source/bots/states/side_bait.lua     (новый, ~60 LOC)
```

## Архитектурные находки

1. **`item_meteor_hammer` уже cast'ится** через `util/items.lua` UseActiveItems dispatcher для всех ботов. Phase E не дублирует cast — только **движение** к mid делает handler.
2. **`cond_strategy_win_push` уже существовал** (fsm.lua) — оставлен как safety net (fire'ит если бот уже на линии с креп волной). METEOR_SQUAD приоритетнее в transitions.
3. **`cond_lose_mid_block` (legacy)** — оставлен как safety net для случая когда SIDE_BAIT не зашёл (mid laner в mid → RETREAT). После SIDE_BAIT integration этот fallback редко срабатывает.
4. **strategy.json polling** уже работает в `bot_controller.lua:82-103` (per-pid match). Lua-side IPC не требует доп. работы.
5. **`item_meteor_hammer` уже в стандартном purchase order** (`item_build.lua:174`) — все боты покупают сразу после PT.

## Verification

### Offline smoke test (lua 5.4)
- `luac -p` на 6 файлах: pass
- Stubbed Valve API + mock UnitHandle: ALL PASS
  - config loads с правильными default'ами
  - FSM enum/Name/Transitions работают
  - 8 state handlers loaded (было 6)
  - per-slot side_bait_lane mapping: pid 0/1→top, 2/3→bot, 4→jungle ✓
  - **LANE_FARM (WIN, gt=1300) → METEOR_SQUAD (strategy_win_meteor)** ✓
  - **LANE_WAIT (LOSE, gt=1300) → SIDE_BAIT (strategy_lose_side_bait)** ✓
  - `meteor_squad.Run` returns `WALK_TO_MID` ✓
  - `side_bait.Run` returns `BAIT_GOTO_SAFE` ✓

### Live verify (TODO Phase F)

1. F7 hot-reload на стенде .131 в Demo Hero
2. Touch `C:\temp\andromeda\strategy.json` `{"<pid>": "WIN", ...}`
3. В `botbrain_<PID>.log` через ~5с (next strategy poll) должно появиться:
   - `[STRATEGY] team_strategy: DEBOOST → WIN`
4. По прошествии 20 мин game_time:
   - `[BRAIN] FSM LANE_FARM → METEOR_SQUAD (strategy_win_meteor)` на 5 ботах
   - Боты идут в mid → `WALK_TO_MID` / `RALLY_TO_CLUSTER` actions
   - `[items] USED item_meteor_hammer slot=X fsm=METEOR_SQUAD` хотя бы 1 раз
5. Симметрично для LOSE → SIDE_BAIT, action `BAIT_GOTO_SAFE`, отсутствие движений на mid (`dLane` mid > 2500 всегда).

## Что НЕ сделано в Phase E

- DLL пересборка не нужна — это pure Lua change, F7 hot-reload подхватит
- Production deploy через `bash deploy.sh` отложен до Phase F (после E2E verify)
- Live test на стенде ждёт Phase F (с включенным pairing)

## Next: Phase C — deploy `dota_relay` Docker на eft-deploy

`tools/dota2/relay/` готов (Go, multi-tenant, Docker). README описывает deploy на eft-deploy. Шаги в plan file Phase C task.
