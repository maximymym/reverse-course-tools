# Architecture — Andromeda Dota 2 Bot Brain

## High-level

Боты управляются из **двух слоёв**: C++ (низкий уровень — память/приказы игре)
и Lua (верхний уровень — решения "что делать"). Lua перезагружается без
пересборки DLL. Это архитектурный компромисс: мы хотим чтобы тестер мог переписать
**любое** поведение ботов (лэйнинг, ганг, фарм джунгля, покупки, реролы)
не имея VS и не компилируя C++.

```
┌─────────────────────────── Dota 2 process ───────────────────────────┐
│                                                                       │
│   ┌──────────────────┐   memory   ┌────────────────────┐              │
│   │ CDOTAGameRules   │ ←─────────→│  Andromeda DLL     │ ←─ инжект    │
│   │ CEntitySystem    │            │  (C++, injectDLL)  │              │
│   │ C_BaseEntity[]   │            │                    │              │
│   └──────────────────┘            │  CEntityCache      │              │
│                                   │  CGameState        │              │
│                                   │  CBotBrain         │              │
│                                   │  CPanoramaJS ─────→│ Panorama V8  │
│                                   │                    │   PrepareUnit│
│                                   │  sol2 Lua state    │   Orders.exec│
│                                   │    ▲               │              │
│                                   └────┼───────────────┘              │
│                                        │                              │
│                                        │ require/dofile               │
│                                        ▼                              │
│                        C:\temp\andromeda\scripts\bots\                │
│                        (bot_controller + modes/ + util/)              │
└───────────────────────────────────────────────────────────────────────┘
```

## Tick loop

Каждые ~100ms (10 Hz) CBotBrain::Think() вызывается:

```
CBotBrain::Think()
  ├── UpdateEnemyLastSeen()     — обновляем m_enemyLastSeen
  ├── If !m_bLuaScriptsLoaded   — lazy load на первом тике
  └── SafeThinkLua() (SEH):
        ├── AbilityLevelUpThink() [Lua]    — level up по sequence
        ├── Think() [Lua]                  — ГЛАВНАЯ (alias → BotController_Think)
        │     ├── tick++
        │     ├── CheckGameStateTransition() / RegisterOnGameStateChange callback
        │     │     — per-match reset item_build/anti_ban
        │     ├── MaybeChat()
        │     ├── TickPending()            — выполнить отложенные действия
        │     ├── Context.Build(bot)       — один раз за тик
        │     ├── Heroes[name].Think()?    — per-hero override (optional)
        │     ├── FSM.TryTransition(state, bot, ctx) → (new_state, reason)
        │     │     — first-match по transitions table; гистерезис min-ticks
        │     ├── if state changed: BrainLog "FSM X → Y (reason)"
        │     └── States[state].Run(bot, ctx) → (action_name, did_act)
        │           └── [enqueue jittered Action_*]
        ├── ItemPurchaseThink() [Lua]      — покупки per phase
        └── ThinkCppFallback() ← no-op safety net (37 LOC log + OrderStop), только если m_pBotControllerFn nil
```

Если Lua вернул ошибку на любом этапе — **ThinkCppFallback** не пытается рулить
ботом сам (это была старая 11-priority C++ логика, удалена 2026-04-22). Только
логирует `[CPP-FALLBACK]` + OrderStop. Бот стоит = Lua сломана → читать `[ERROR]`
в `botbrain.log` выше.

## Почему FSM, а не competing-desires mode dispatcher

Сначала был monolith из 11 приоритетов в `if/elseif` (см. `dev_log.md`).
Затем переписали в **competing-desires** mode dispatcher (modes/*.lua, каждый
GetDesire + Think, max-desire выигрывает). Проблемы competing-desires:
- 7 modes одновременно computing desires per tick → дорого + non-deterministic.
- Сложно гарантировать гистерезис: tick N выиграл LANE_FARM, tick N+1 — RETREAT, N+2 — снова LANE_FARM (флаппинг).
- Hard to debug "почему он туда пошёл" — ответ "у этого mode desire был выше" редко информативен.

Текущая архитектура (FSM v1, 2026-04-23) — explicit state machine:
- 7 состояний: DEAD, RESPAWN, LANE_FARM, LANE_WAIT, JUNGLE_FARM, PUSH, RETREAT.
- `fsm.lua::Transitions` — таблица `{cond_fn, to_state, reason}` per state. **First match wins.**
- `MinTicksInState(ctx, n)` гистерезис в условиях — нет флаппинга.
- Per-tick: только текущий state.Run() выполняется → дёшево.
- Логирование `BrainLog "FSM X → Y (reason)"` точно показывает *почему* перешли.
- `states/*.lua` — каждое состояние — `{name, Run(bot, ctx) → (action, did_act)}`.
- Тестер добавляет state = новый файл в `states/`, регистрация в `states/init.lua`,
  добавить enum в `fsm.lua::State` + transitions в/из.

**Common pitfall фиксы** (см. fsm.lua):
- `cond_jungle_lane_empty` НЕ требует `dLane<1500` — иначе бот АФК на пустом camp если линия далеко.
- `cond_waiting_jungle` НЕ требует `nearest_neutral_dist<1500` — иначе Sniper top АФК под T1 (camps в 3000+).
- `lane_wait.lua` имеет forward scout: после `no_creeps_streak>100/200/300` waitAmount шагает к мидлу (0.10/0.20/0.30 offset, capped 0.55/0.45). Иначе бот навечно стоит за T1 если линия push'ed.
- `context.lua::lane_front` использует radius=5000 (не 2000) для `GetCreepFrontLocation` — иначе global creep view не работает на push'ed lanes.

## GameContext

`context.lua::Build(bot)` вычисляет **один раз за тик** все что нужно modes:
hp%, mana%, расстояния до врагов/линии/башен/фонтана, списки nearby_*.

Каждый mode получает `(bot, ctx)`. В `GetDesire` — быстрая прикидка по ctx,
в `Think` — финальное решение + action.

Это избавляет от дублирующих сетей вызовов типа `bot:GetNearbyHeroes(1200, true)`
из 7 modes подряд (каждый вызов ходит в EntityCache).

## Persistence

Lua state живёт **между тиками** (пока DLL не пересоздан).

Глобалы:
- `BotControllerConfig` — загружается из `config.lua`, если уже существует не перезаписываем (F7-survival).
- `BotControllerState` — runtime state (tick, pending, last_cast, ...).
- `_unit_storage[entityIndex]` — per-unit dynamic поля (OpenHyperAI convention).

F7 (Reload Lua) **уничтожает** sol::state, создаёт новый, грузит скрипты заново.
Все глобалы сбрасываются. Единственное что переживает — персистент в `_G`
ниже, но новая Lua state — значит `_G` тоже новый.

## C++ ↔ Lua boundary

### C++ → Lua (regulars called by C++)
- `Think()` [global, alias → `BotController_Think`] — главная
- `ItemPurchaseThink()` — покупки
- `AbilityLevelUpThink()` — level up
- *(Phase 2)* зарегистрированный через `RegisterOnGameStateChange(fn)` callback
  вызывается из `CGameState::RefreshGameRules` при изменении `m_eGameState`.
  Сигнатура `fn(new_state, prev_state)`. Главный thread, безопасно для Lua.

> Старый `Modes` global (competing-desires mode list) больше не используется —
> FSM v1 заменил mode-dispatcher. Не писать `_G.Modes = ...` — имя занято Valve
> legacy AI и крашит match accept.

**Критичный invariant:** в `bot_controller.lua` в конце должно быть:
```lua
Think = BotController_Think   -- DLL ждёт global `Think`
```
Без этого alias'а DLL логирует `bot Think() = NO` и проваливается на C++ fallback —
бот делает один `ATTACKMOVE_LANE` и стоит.

**НЕ ПИСАТЬ `_G.Modes = Modes`!** Имя `Modes` занято Valve legacy AI (mode_*_generic.lua),
их scripts Dota engine подгружает при match start. Наш override ломает их → **все
5 dota crash одновременно при match accept**. DLL diagnostic "Modes loaded: 0" — просто лог,
работу не ломает.

### Lua → C++ (Lua scripts call)
- `GetBot()`, `DotaTime()`, `IsDaytime()`, etc — globals в `RegisterGlobals` + `LuaStubs`
- `bot:GetHealth()`, `bot:GetNearbyHeroes()`, `bot:Action_MoveToLocation()` — методы на UnitHandle
- `ab:IsFullyCastable()` — methods на AbilityHandle
- `GetScriptDirectory()`, `require(...)` — script loader shim

Полный список — `API_REFERENCE.md`.

## Anti-ban layer

Три меры (A.1, A.2, A.3) для man-like поведения:

### A.1: Jitter reactions
Когда mode хочет выдать приказ — не выдаёт сразу. Кладёт в `BotControllerState.pending_action`,
исполняется через `rand(8, 25)` ticks (~80-250ms). Единственное исключение: CRITICAL_RETREAT
обходит jitter (если HP<15%, реакция моментальная — человек тоже не ждёт).

### A.2: Cast cooldown
Нельзя кастовать одно и то же ability по одной и той же цели чаще чем `rand(0.8, 1.5)` секунд.
Ключ = `ability_name + playerID/hash(unit_name)`. Предотвращает spam-hook как был в бете.

### A.3: Random chat
Случайный чат из пула (gg, ss, care, nice, ...). 2-3 сообщения за матч,
рандомизированно по времени (30-90с первый, 5-15 мин между следующими).

## HotReload lifecycle

```
F7 → hook жмёт C:\temp\andromeda\reload_<PID>.flag
           ↓
CBotBrain::Think() обнаруживает flag
           ↓
CBotBrain::Shutdown()
     │   ├── DEL_LUA_FN все refs
     │   ├── delete sol::state
     │   └── m_bInitialized = false
           ↓
CBotBrain::Init()
     │   ├── new sol::state
     │   ├── RegisterVector/Proxies/Constants/Compat/Globals/Stubs
     │   ├── SetupScriptLoader (package.path)
     │   ├── LoadBotScripts
     │   │     ├── lane_config.lua
     │   │     ├── bot_controller.lua (→ m_pBotControllerFn)
     │   │     ├── item_build.lua (→ m_pItemPurchaseFn)
     │   │     └── ability_build.lua (→ m_pAbilityLevelUpFn)
     │   └── m_bInitialized = true
           ↓
на следующем тике — новая Lua state в действии.
```

Весь процесс ~200-500ms per bot.

## C++ Safety Net (ThinkCppFallback)

С 2026-04-22 (версия `lua-only-gameplay`) `ThinkCppFallback` — это **safe no-op**
(~37 LOC: log + `pjs->OrderStop()` + status `"OFF (Lua failed)"`). Все gameplay
правки → `scripts/bots/*.lua` + F7 reload.

Раньше fallback содержал ~290 LOC C++ brain (HP checks, tower aggro, TP, walk,
attack), но он **маскировал ошибки Lua** — выглядело будто бот работает, на деле
правки `states/*.lua` игнорировались. Удалили.

Item purchase и ability level-up живут строго в Lua (`item_build.lua` /
`ability_build.lua`). Если тестер сломает один из них — боты временно не покупают
/ не качают скиллы. Это **feature**: симптом виден сразу → читать `botbrain.log`.

**Diagnostic flow:** бот стоит + `[CPP-FALLBACK]` в `debug_<PID>.log` = Lua упала,
ищи `[ERROR]` строки выше в `botbrain.log` — там traceback из `pcall`.

## Files — summary

| Файл | Роль |
|------|------|
| `bot_controller.lua` | Dispatcher. FSM.TryTransition → States[state].Run. Wires `RegisterOnGameStateChange`. Override `GetNeutralCampLocations` через `util/camps.lua`. |
| `config.lua` | Все параметры (BotControllerConfig). Единственное что правит тестер на 80%. |
| `context.lua` | Pre-compute GameContext (один раз за тик). `lane_front` radius=5000. |
| `fsm.lua` | State enum + Transitions table + TryTransition. Pure predicates, без side-effects. |
| `lane_config.lua` | Waypoints (TOP/MID/BOT), helpers для amount/front. |
| `item_build.lua` | Phase-based покупки (starting/early/core/late). Имеет `Reset()`. |
| `ability_build.lua` | Level-up sequences. |
| `states/*.lua` | 7 состояний. Каждый — `{name, Run(bot, ctx) → (action, did_act)}`. |
| `states/init.lua` | Регистрация states (мап `state_id → module`). |
| `heroes/*.lua` | Per-hero полный override Think. |
| `heroes/init.lua` | Регистрация heroes. |
| `util/camps.lua` | Real neutral camp coords (16 camps, 8 radiant + 8 dire). Override C++ stub. |
| `util/combat.lua` | TryCastAbility, NearestAlive, WeakestAlive. |
| `util/movement.lua` | Dist2D, IsStuck, MoveToLocationSafe. |
| `util/vec.lua` | Dist2D/DistSqr2D/IsInRange2D/Extend2D/Lerp/Centroid. |
| `util/anti_ban.lua` | QueuePendingAction, TickPending, CanCast, MaybeChat, `Reset()`. |

## What C++ does NOT do anymore

- Item purchase (удалено из ThinkCppFallback)
- Ability level up (удалено, не вызывается из fallback)
- Ability casting (удалено — cast fallback полностью отдан Lua)

Оставлено в fallback: движение (retreat/walk/attack-move/hold). Минимум чтобы
бот не стоял на респе как манекен если Lua сломан.

## Дальше читай

- `API_REFERENCE.md` — исчерпывающий список функций.
- `COOKBOOK.md` — 15 рецептов.
- `TESTER_QUICKSTART.md` — если ещё не.
