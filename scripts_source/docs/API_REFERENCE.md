nfr # API Reference — Andromeda Dota 2 Lua API

Все функции и методы которые доступны в scripts. Делится на:

1. [Lifecycle](#lifecycle) — точки входа
2. [Globals REAL](#globals-real) — работающие глобалы
3. [Globals STUB](#globals-stub) — заглушки (вернут дефолт)
4. [UnitHandle](#unithandle) — герой/крип/башня
5. [AbilityHandle](#abilityhandle) — способности
6. [ItemHandle](#itemhandle) — предметы
7. [Vector3](#vector3) — геометрия
8. [Constants](#constants) — TEAM_*, LANE_*, UNIT_LIST_*, etc.
9. [Persistence](#persistence) — что живёт между тиками

Условные обозначения:
- ✅ REAL — реально работает, читает игру
- ⚠️ APPROX — приближение (gold — formula, не real)
- ❌ STUB — возвращает дефолт, не читает память

---

## Lifecycle

Три Lua-функции, которые C++ вызывает каждый тик:

### `Think() → (action_name, did_act)` *(alias для `BotController_Think`)*

Главный dispatcher. Определён в `bot_controller.lua`. DLL ищет **global `Think`**
(стандартный Valve bot API), поэтому в конце файла прописан alias:

```lua
Think = BotController_Think
```

**Важно:** если alias'а нет, DLL падает на C++ fallback (`ATTACKMOVE_LANE` один раз, бот стоит).

**НЕ добавляй `_G.Modes = Modes`** — имя `Modes` занято Valve legacy AI
(`mode_*_generic.lua` загружается Dota engine при match start). Наш override
ломает их → **синхронный crash всех 5 dota при match accept**. DLL diagnostic
`Modes loaded: 0` — просто диагностика, работу не блокирует.

Возвращает:
- `action_name` (string) — "<mode>:<action>"
- `did_act` (bool) — выдан ли приказ

### `ItemPurchaseThink()`

Вызывается C++. Определён в `item_build.lua`. Throttled внутри (3с между attempts).
Читает `bot:GetGold()` и покупает следующий предмет из phase build'а.

### `AbilityLevelUpThink()`

Вызывается C++. Определён в `ability_build.lua`. Активна когда
`bot:GetAbilityPoints() > 0`. Levels up следующий slot из sequence.

**Тик частота**: ~10 Hz (каждые ~100ms). Бот мёртв → `Think` не вызывается.

---

## Globals REAL

### Time

**`DotaTime() → float`** ✅
Real game time в секундах. Отрицательный в pre-game, 0 на horn. Пример:
```lua
local gt = DotaTime()
if gt > 60 then -- после первой минуты end
```

**`GameTime() → float`** ✅ alias к DotaTime.

**`RealTime() → float`** ✅ wall-clock elapsed с момента init.

**`GetTimeOfDay() → float`** ✅
Fraction 0.0-1.0 в day/night cycle (600с). 0.0-0.5 = day, 0.5-1.0 = night.

**`IsDaytime() → bool`** ✅
True если сейчас day phase. `if IsDaytime() then ... end`.

### Bot identity

**`GetBot() → UnitHandle`** ✅
Возвращает контролируемого бота. Всегда тот же UnitHandle между тиками.

**`GetBotTeam() → int`** ✅ 2 (Radiant) или 3 (Dire).

**`GetTeam() → int`** ✅ alias.

**`GetOpposingTeam() → int`** ✅ 3 если мы 2, иначе 2.

**`GetInstanceId() → int`** ✅ Orchestrator-assigned ID (0..4).

**`GetBotRole() → string`** ✅ "mid" / "carry" / "hard_support" / "offlane" / "soft_support" по instance_id.

### Lane

**`GetLaneFrontLocation(team, lane, offset) → Vector3`** ✅
Позиция фронта линии. team-relative. Offset shift'ит от середины.

**`GetLocationAlongLane(lane, amount) → Vector3`** ✅
Интерполяция по waypoints. amount=0 начало (Radiant side), 1=конец.

**`GetLaneAmountForBot(lane, team, phase) → float`** ✅ (определён в lane_config.lua)
Возвращает amount для `"lane"` / `"push"` / `"safe"` phase.

**`GetCreepFrontLocation(bot, lane, radius) → Vector3`** ✅ (определён в lane_config.lua)
Центр массы союзных крипов, fallback на waypoint.

### Fountains / Shops

**`GetOwnFountain() → Vector3`** ✅
**`GetEnemyFountain() → Vector3`** ✅
**`GetShopLocation(team) → Vector3`** ✅ home shop.
**`GetSecretShopLocation(team) → Vector3`** ✅

### Towers

**`GetTower(team, towerIdx) → UnitHandle`** ✅
Tower данной команды по индексу. Возвращает null UnitHandle если не найден.

**`GetAncient(team) → UnitHandle`** ✅
Fort (трон) данной команды.

**`IsLocationNearEnemyTower(loc, radius) → bool`** ✅
Есть ли вражеская башня в radius от loc.

**`GetNearestFriendlyTower(lane) → UnitHandle | nil`** ✅

**`GetFriendlyLaneCreepFront(lane) → Vector3`** ✅ center mass союзных крипов.

### Neutral camps / jungle

**`GetNeutralCampLocations() → table`** ✅
```lua
{
    radiant = { easy={Vec,Vec}, medium={...}, hard={...}, ancient={...} },
    dire    = { easy={...}, medium={...}, hard={...}, ancient={...} },
    roshan  = Vector(-2500, -2000, 128)
}
```

**`GetRoshanLocation() → Vector3`** ✅ `(-2500, -2000, 128)`.

**`GetRoshanEntity() → UnitHandle | nil`** ✅ Live Roshan entity if alive.

**`IsRoshan(unit) → bool`** ✅ Проверка на Роша по designerName.

**`GetNeutralSpawners() → table`** ❌ STUB.

> **⚠️ C++ stub координаты устарели** — `LuaStubs::GetNeutralCampLocations` имеет
> attached к старой карте (например radiant hard `(-3800,-300)` — такого camp нет).
> На load `bot_controller.lua` глобал переопределяется на актуальные coords из
> `util/camps.lua` (16 camps, разбито по level easy/medium/hard). Если редактируешь
> карты — правь `util/camps.lua` (Lua, F7 reload), не C++ stub.

### Courier

**`GetCourier() → UnitHandle | nil`** ✅
Возвращает live courier бот-team. nil если мёртв/нет.

**`GetNumCouriers() → int`** ✅ count alive couriers своей команды.

**`IsCourierAvailable() → bool`** ✅

### Hero tracking

**`GetHeroLastSeenInfo(playerID) → table`** ✅
```lua
{
    location         = Vector3,  -- последняя known позиция
    TimeSinceLastSeen = float,    -- секунд с последнего раза
    IsCurrentlyVisible = bool,
    IsVisibleToTeam   = bool,     -- alias
}
```
Работает для enemy heroes (0..9 player IDs). Для своих возвращает дефолт.

**`GetSelectedHeroName(playerID) → string`** ❌ STUB возвращает "".
**`GetHeroKills/Deaths(playerID) → int`** ❌ STUB 0.
**`GetHeroLevel(playerID) → int`** ❌ STUB 1.

**`Heroes_InRadius(pos, radius, filter) → table<UnitHandle>`** ✅ *(Phase 2)*
Глобальный radius-query по героям без явного UnitHandle. Не требует `bot:` —
полезно для target selection из произвольной точки (centroid группы, predicted
spot и т.п.). Возвращает sequential array живых героев в радиусе.

`filter`:
- `"enemy"` / `"enemies"` / `0` — только герои чужой команды
- `"ally"` / `"allies"` / `"friendly"` / `1` — только своей
- `"both"` / `"all"` / `2` — все (default; также если бот ещё не нашёл свою команду)

```lua
local enemies = Heroes_InRadius(bot:GetLocation(), 1600, "enemy")
for _, h in ipairs(enemies) do
    if h:GetHealth() < 200 then bot:Action_AttackUnit(h); break end
end
```
Команда бота берётся из `CBotBrain::GetBotTeam()`. Squared distance check —
быстрее чем `GetNearbyHeroes`-цикл.

### Economy

**`GetItemCost(name) → int`** ✅
Цена из БД: `GetItemCost("item_blink")` → 2250.

**`GetItemPurchaseCost(name) → int`** ✅ alias.

**`IsItemPurchasedFromSecretShop(name) → bool`** ✅.

### Game state

**`GetGameState() → int`** ✅
Real EDOTAGameState. См. dota SDK (DOTA_GAMERULES_STATE_*).

**`GetGameMode() → int`** ⚠️ всегда 1 (AP).

**`GetDifficulty() → int`** ⚠️ всегда 2 (HARD).

**`RegisterOnGameStateChange(fn)` → void** ✅ *(Phase 2)*
Event-callback при изменении `m_eGameState` (триггер из `CGameState::RefreshGameRules`).
Вызывается в **main game thread** — тот же тред что и `Think`, безопасно дёргать
любой Lua код. Сигнатура: `fn(new_state, prev_state)` — оба int, EDOTAGameState.

```lua
RegisterOnGameStateChange(function(new_state, prev_state)
    if new_state == 2 and prev_state == 6 then  -- POST_GAME → HERO_SELECTION
        ItemBuild.Reset()
        AntiBan.Reset()
    end
end)
```

`bot_controller.lua` уже wires это для per-match reset (state.bought / last_cast).
Ownership: heap-копия `sol::protected_function`, очищается на `CBotBrain::Shutdown`
(F7 reload) — после reload Lua скрипт перерегистрирует на module load.

**`UnregisterOnGameStateChange()` → void** ✅ убрать callback.

### RNG

**`RandomInt(a, b) → int`** ✅ [a, b] inclusive.
**`RandomFloat(a, b) → float`** ✅ [a, b).
**`RandomVector(length) → Vector3`** ✅ random unit vector scaled by length, z=0.

### Math

**`Min(a, b)`, `Max(a, b)`, `Clamp(v, lo, hi)`** ✅
**`RemapVal(v, imin, imax, omin, omax)`** ✅
**`RemapValClamped(v, imin, imax, omin, omax)`** ✅

### Distance

**`GetUnitToUnitDistance(u1, u2) → float`** ✅
**`GetUnitToLocationDistance(u, loc) → float`** ✅
**`GetLocationToLocationDistance(a, b) → float`** ✅

### Actions — global

**`ActionImmediate_PurchaseItem(name) → int`** ✅
Возвращает 0 (SUCCESS). Шлёт OrderPurchaseItem через PanoramaJS.

**`ActionImmediate_Chat(msg, allChat)`** ✅
`allChat=true` — ALL, `false` — TEAM.

**`ActionImmediate_Buyback()`** ✅ `dota_buyback` console command.
**`ActionImmediate_Glyph()`** ✅ `dota_glyph`.
**`ActionImmediate_LevelAbility(nameOrHandle)`** ✅

### Unit list

**`GetUnitList(listType) → table`** ✅
`listType`:
- 0 = UNIT_LIST_ALL (all alive units)
- 1 = UNIT_LIST_ALLIES
- 2 = UNIT_LIST_ALLIED_HEROES
- 3 = UNIT_LIST_ALLIED_CREEPS
- 5 = UNIT_LIST_ALLIED_BUILDINGS
- 7 = UNIT_LIST_ENEMIES
- 8 = UNIT_LIST_ENEMY_HEROES
- 9 = UNIT_LIST_ENEMY_CREEPS
- 11 = UNIT_LIST_ENEMY_BUILDINGS

### Team / Player API

**`GetTeamPlayers(team) → table`** ⚠️ returns {0..4}.
**`GetTeamMember(playerId) → UnitHandle`** ⚠️ approx.
**`IsPlayerBot(pid) → bool`** ⚠️ всегда true.
**`IsHeroAlive(pid) → bool`** ❌ STUB true.

### Debug

**`Msg(...)`** ✅ пишет в botbrain.log.
**`print(...)`** ✅ пишет в botbrain.log (наш override).

---

## Globals STUB

Эти функции **существуют** (скрипты не упадут при вызове), но возвращают defaults.
**Не полагайся** на их значения для серьёзной логики.

| Функция | Возвращает |
|---------|-----------|
| `GetTreeLocation(tree)` | `Vector(0,0,128)` |
| `IsLocationPassable(loc)` | `true` |
| `IsLocationVisible(loc)` | `false` |
| `GetHeightLevel(loc)` | `0` |
| `GetRuneStatus(idx)` | `0` |
| `GetRuneTimeSinceSeen(idx)` | `999` |
| `GetRuneSpawnLocation(idx)` | `Vector(0,0,128)` |
| `GetNearbyFilledRune(u, r)` | `nil` |
| `GetNearbyUnfilledRune(u, r)` | `nil` |
| `GetDroppedItemList()` | `{}` |
| `GetItemComponents(item)` | `{}` |
| `GetItemStockCount(item)` | `1` |
| `IsItemPurchasedFromSideShop(item)` | `false` |
| `GetLinearProjectiles()` | `{}` |
| `GetAvoidanceZones()` | `{}` |
| `GetIncomingTeleports()` | `{}` |
| `GetIncomingTrackingProjectiles(u)` | `{}` |
| `GetDefendLaneDesire(lane)` | `0` |
| `GetPushLaneDesire(lane)` | `0` |
| `GetFarmLaneDesire(lane)` | `0` |
| `GetGameModeDesire(mode)` | `0` |
| `IsModeActive(mode)` | `false` |
| `GetHeroPickState()` | `0` |
| `SelectHero(pid, name)` | nothing |
| `DebugDrawText/Circle/Line(...)` | nothing |
| `DebugPause()` | nothing |
| `InstallChatCallback(...)` | nothing |
| `GetBarracksHealthPercent(team, idx)` | `1.0` |
| `GetTowerHealthPercent(team, idx)` | `1.0` |
| `GetPowerOfTeam(team)` | `1.0` |
| `GetRoshanKillTime()` | `-999` |
| `GetRoshanDesire()` | `0` |
| `IsNightstalkerNight()` | `false` |
| `GetWorldBounds()` | `[Vec(-8288,...), Vec(8288,...)]` |

---

## UnitHandle

`local bot = GetBot()` — получили UnitHandle. Всё что ниже — методы.

### Identity

| Метод | Возврат | Описание |
|-------|---------|----------|
| `GetUnitName()` ✅ | string | "npc_dota_hero_pudge" |
| `GetTeam()` ✅ | int | 2/3/4 |
| `GetPlayerID()` ✅ | int | 0..9 (heroes), -1 others |
| `IsNull()` ✅ | bool | entity destroyed / invalid |
| `IsBot()` ⚠️ | bool | всегда true |

### Health / Mana / Regen

| Метод | Возврат |
|-------|---------|
| `GetHealth()` ✅ | int |
| `GetMaxHealth()` ✅ | int |
| `GetHealthPercent()` ✅ | float 0..1 |
| `GetMana()` ✅ | float |
| `GetMaxMana()` ✅ | float |
| `GetManaPercent()` ✅ | float 0..1 |
| `GetHealthRegen()` ✅ | float |
| `GetManaRegen()` ✅ | float |

### Status

| Метод | Возврат |
|-------|---------|
| `IsAlive()` ✅ | bool |
| `IsHero()` ✅ | bool |
| `IsCreep()` ✅ | bool (lane creep) |
| `IsNeutral()` ✅ | bool (jungle creep) |
| `IsRoshan()` ✅ | bool |
| `IsCourier()` ✅ | bool |
| `IsTower()` ✅ | bool |
| `IsBuilding()` ✅ | bool |
| `IsFort()` ✅ | bool |
| `IsIllusion()` ✅ | bool |
| `GetCreepType()` ✅ | string: "lane", "neutral", "ancient", "roshan", "siege", "mega", "super", "" |

### CC / Состояние

Читается из unitState64 bitmask. Все возвращают bool.

| Метод | Значение бита |
|-------|---------------|
| `IsStunned()` ✅ | 5 |
| `IsRooted()` ✅ | 0 |
| `IsSilenced()` ✅ | 3 |
| `IsHexed()` ✅ | 6 |
| `IsMuted()` ✅ | 4 |
| `IsDisarmed()` ✅ | 1 |
| `IsInvisible()` ✅ | 7 |
| `IsMagicImmune()` ✅ | 9 |
| `IsAttackImmune()` ✅ | 2 |
| `IsInvulnerable()` ✅ | 8 |
| `IsNightmared()` ✅ | 11 |
| `IsBlind()` ✅ | 28 |
| `IsChanneling()` ✅ | 18 |
| `HasSilence()` ✅ | 3 |

### Damage history

| Метод | Возврат |
|-------|---------|
| `WasRecentlyDamagedByHero(_)` ✅ | bool (last 2с) |
| `WasRecentlyDamagedByAnyHero(...)` ✅ | bool |
| `WasRecentlyDamagedByCreep(...)` ✅ | bool |
| `WasRecentlyDamagedByTower(...)` ✅ | bool (last 3с) |
| `TimeSinceDamagedByAnyHero()` ✅ | float |
| `TimeSinceDamagedByCreep()` ✅ | float |
| `TimeSinceDamagedByTower()` ✅ | float |

### Position / Movement

| Метод | Возврат |
|-------|---------|
| `GetLocation()` ✅ | Vector3 |
| `GetAbsOrigin()` ✅ | Vector3 (alias) |
| `GetFacing()` ⚠️ | float (0 — not parsed) |
| `GetAttackRange()` ✅ | int |
| `GetMoveSpeed()` ✅ | int |
| `GetBoundingRadius()` ⚠️ | 24 |
| `DistanceFromFountain()` ✅ | float |
| `GetXUnitsTowardsLocation(target, dist)` ✅ | Vector3 |

### Combat stats

| Метод | Возврат |
|-------|---------|
| `GetLevel()` ✅ | int |
| `GetAttackDamage()` ✅ | float (avg min+max / 2) |
| `GetArmor()` ✅ | int |
| `GetAttackSpeed()` ⚠️ | 100 |
| `GetSecondsPerAttack()` ⚠️ | 1.7 |
| `GetMagicResist()` ✅ | float |
| `GetActualIncomingDamage(amount, type)` ⚠️ | `amount * 0.8` |

### Vision

| Метод | Возврат |
|-------|---------|
| `GetDayTimeVisionRange()` ✅ | int |
| `GetNightTimeVisionRange()` ✅ | int |
| `GetCurrentVisionRange()` ✅ | int |

### Economy

| Метод | Возврат |
|-------|---------|
| `GetGold()` ⚠️ APPROX | `625 + level * 400` (real gold на PlayerResource не парсится) |
| `GetNetWorth()` ❌ | `0` |
| `GetLastHits()` ❌ | `0` |
| `GetDenies()` ❌ | `0` |
| `GetStashValue()` ❌ | `0` |

### Abilities

| Метод | Возврат |
|-------|---------|
| `GetAbilityByName(name)` ✅ | AbilityHandle |
| `GetAbilityInSlot(slot0based)` ✅ | AbilityHandle |
| `GetAbilityCount()` ✅ | int |
| `GetAbilityPoints()` ✅ | int |
| `HasScepter()` ✅ | bool |

### Items

| Метод | Возврат |
|-------|---------|
| `GetItemInSlot(slot0based)` ✅ | ItemHandle |
| `GetItemCount()` ✅ | int |
| `FindItemSlot(name)` ✅ | int (−1 если нет) |
| `GetItemSlotType(slot)` ✅ | 0=main/1=backpack/2=stash |

### Nearby queries

| Метод | Возврат |
|-------|---------|
| `GetNearbyHeroes(radius, isEnemy)` ✅ | table<UnitHandle> |
| `GetNearbyCreeps(radius, isEnemy)` ✅ | table<UnitHandle> |
| `GetNearbyLaneCreeps(r, e)` ✅ | alias |
| `GetNearbyTowers(radius, isEnemy)` ✅ | table<UnitHandle> |
| `GetNearbyNeutralCreeps(radius, _)` ✅ | table<UnitHandle> (includes Roshan) |

### Core actions

| Метод | OrderType | Описание |
|-------|-----------|----------|
| `Action_MoveToLocation(loc)` ✅ | 1 | идёт к loc |
| `Action_AttackUnit(target)` ✅ | 4 | атакует юнит |
| `Action_AttackMove(loc)` ✅ | 3 | attack-move |
| `Action_UseAbility(ab)` ✅ | 8 | no-target скилл |
| `Action_UseAbilityOnEntity(ab, target)` ✅ | 6 | unit-target |
| `Action_UseAbilityOnLocation(ab, loc)` ✅ | 5 | point-target |
| `Action_MoveToUnit(u)` ✅ | 1 | move to target.loc |
| `Action_ClearActions(_)` ✅ | 21 | stop current orders |
| `Action_ToggleAbility(ab)` ✅ *(Phase 2)* | 9 | toggle on/off — Armlet/Radiance/Soul Ring/Ring of Basilius |
| `Action_ToggleAutoCast(ab)` ✅ *(Phase 2)* | 20 | autocast on/off — Sniper Shrapnel/Necro Heartstopper/Warlock Soul Link |
| `Action_VectorTargetCast(ab, from, to)` ✅ *(Phase 2)* | 30 | vector cast direction — Mirana Arrow / Pudge Dismember |

`Action_VectorTargetCast` payload shape:
`{OrderType: 30, Position: from, Position2: to, AbilityIndex: ab.entIndex}`. Если
конкретный спелл не реагирует — попробовать `Position2`→`TargetPosition` в
`CPanoramaJS.cpp:OrderVectorTargetCast` + DLL rebuild. JS-строка логируется.

Все Action_* идут через `CPanoramaJS::Order*` (Game.PrepareUnitOrders в JS).
Native CPrepareOrders путь не используется — после Dota patch 2026-04-22 AOB-scanned
fn оказывался не той функцией → orders уходили в /dev/null. Один путь = меньше
точек отказа.

Push/Queue варианты: `ActionPush_*` / `ActionQueue_*` — пока то же что и `Action_*`.
Будущее: полноценный order queue.

Алиас `Action_VectorTargetPosition` = `Action_VectorTargetCast` (UC Zone naming).

### Courier actions (на courier UnitHandle)

| Метод | Описание |
|-------|---------|
| `Action_Courier_TakeStashItems()` ✅ | console: dota_courier_take_stash_items |
| `Action_Courier_TransferItems()` ✅ | console: dota_courier_transfer_items |
| `Action_Courier_Return()` ✅ | dota_courier_return |
| `Action_Courier_Burst()` ✅ | dota_courier_burst |
| `Action_Courier_Shield()` ✅ | dota_courier_shield |

### Immediate actions

| Метод | Описание |
|-------|---------|
| `ActionImmediate_PurchaseItem(name)` ✅ | покупка |
| `ActionImmediate_Buyback()` ✅ | buyback |
| `ActionImmediate_LevelAbility(name/handle)` ✅ | level up |
| `ActionImmediate_Chat(msg, all)` ❌ | no-op (см. global ActionImmediate_Chat) |
| `ActionImmediate_Ping(...)` ❌ | no-op |

### Queries (STUB)

| Метод | Возврат |
|-------|---------|
| `GetModifierByName(name)` ❌ | nil |
| `HasModifier(name)` ❌ | false |
| `NumModifiers()` ❌ | 0 |
| `GetTarget()` ❌ | nil |
| `GetAttackTarget()` ❌ | nil |
| `GetRespawnTime()` ❌ | 0 |
| `HasBuyback()` ❌ | false |
| `IsCastingAbility()` ❌ | false |
| `GetAnimActivity()` ❌ | -1 |

### Dynamic fields (per-unit storage)

Ты можешь писать **свои** поля на UnitHandle — они сохраняются в глобальный
`_unit_storage[entityIndex]`:

```lua
local bot = GetBot()
bot.myCustomState = "exploring"
bot.stuckCount = (bot.stuckCount or 0) + 1

-- позже:
if bot.myCustomState == "exploring" then ... end
```

Внимание: переживают F7 только для того же entityIndex. Если бот респаунился
entity index может измениться (редко, но бывает) — тогда state потерян.

### Misc

| Метод | Возврат |
|-------|---------|
| `GetActiveMode()` ⚠️ | 1 (LANING) |
| `GetActiveModeDesire()` ⚠️ | 0.5 |
| `GetAssignedLane()` ✅ | 1/2/3 (из Settings::nInstanceId) |
| `GetDifficulty()` ⚠️ | 2 |

---

## AbilityHandle

`local ab = bot:GetAbilityInSlot(0)` — Q ability.

### Queries

| Метод | Возврат |
|-------|---------|
| `GetName()` ✅ | string |
| `GetLevel()` ✅ | int |
| `GetCooldownTimeRemaining()` ✅ | float |
| `GetCooldown()` ✅ | float (base) |
| `GetManaCost()` ✅ | int |
| `GetCurrentCharges()` ✅ | int |
| `GetBehavior()` ✅ | int (ABILITY_BEHAVIOR_* bitmask) |
| `GetCastRange()` ✅ | float (level-based) |
| `GetSpecialValueFor(key)` ✅ | float (level-based ability value) |
| `IsFullyCastable()` ✅ | bool (lvl>0 + cd=0 + mana ok + activated) |
| `IsActivated()` ✅ | bool |
| `IsInAbilityPhase()` ✅ | bool |
| `IsToggle()` ✅ | bool |
| `IsHidden()` ✅ | bool |
| `IsTalent()` ✅ | bool (contains "special_bonus") |
| `IsPassive()` ✅ | bool |
| `IsUltimate()` ✅ | bool (slot == 5) |
| `IsNull()` ✅ | bool |
| `GetEntIndex()` ✅ | int |

---

## ItemHandle

`local it = bot:GetItemInSlot(0)` — первый слот inventory.

| Метод | Возврат |
|-------|---------|
| `GetName()` ✅ | string |
| `GetLevel()` ✅ | int |
| `GetCooldownTimeRemaining()` ✅ | float |
| `GetCurrentCharges()` ✅ | int |
| `IsFullyCastable()` ✅ | bool |
| `IsEnabled()` ✅ | bool |
| `GetCost()` ✅ | int (from DB) |
| `IsNull()` ✅ | bool |
| `GetEntIndex()` ✅ | int |

---

## Vector3

Конструктор: `Vector(x, y, z)`. Поля: `.x`, `.y`, `.z`.

Арифметика:
```lua
local a = Vector(100, 200, 128)
local b = Vector(150, 250, 128)
local sum = a + b  -- (250, 450, 256)
local diff = a - b
local scaled = a * 2
```

Методы:
| Метод | Возврат |
|-------|---------|
| `Length()` | float |
| `Length2D()` | float (xy only) |
| `Distance(other)` | float |
| `Normalized()` | Vector3 |

---

## Constants

### Teams
- `TEAM_RADIANT` = 2
- `TEAM_DIRE` = 3
- `TEAM_NEUTRAL` = 4

### Lanes
- `LANE_NONE` = 0
- `LANE_TOP` = 1
- `LANE_MID` = 2
- `LANE_BOT` = 3

### Unit lists
`UNIT_LIST_ALL/ALLIES/ALLIED_HEROES/ALLIED_CREEPS/.../ENEMY_BUILDINGS` — см. выше.

### Damage types
- `DAMAGE_TYPE_PHYSICAL` = 1
- `DAMAGE_TYPE_MAGICAL` = 2
- `DAMAGE_TYPE_PURE` = 4
- `DAMAGE_TYPE_ALL` = 7

### Bot actions
- `BOT_ACTION_TYPE_NONE/IDLE/MOVE_TO/MOVE_TO_DIRECTLY/ATTACK/ATTACKMOVE/USE_ABILITY/PICK_UP_RUNE/PICK_UP_ITEM/DROP_ITEM/SHRINE/DELAY`

### Bot modes
- `BOT_MODE_NONE/LANING/ATTACK/ROAM/RETREAT/SECRET_SHOP/SIDE_SHOP/PUSH_TOWER_TOP/MID/BOT/DEFEND_TOWER_*/ASSEMBLE/TEAM_ROAM/FARM/DEFEND_ALLY/EVASIVE_MANEUVERS/ROSHAN/ITEM/WARD/OUTPOST/1V1MID`

### Desire levels
- `BOT_ACTION_DESIRE_NONE` = 0.0
- `VERYLOW` = 0.1, `LOW` = 0.25, `MODERATE` = 0.5, `HIGH` = 0.75, `VERYHIGH` = 0.9
- `ABSOLUTE` = 1.0

### Ability behaviors
`ABILITY_BEHAVIOR_HIDDEN/PASSIVE/NO_TARGET/UNIT_TARGET/POINT/AOE/CHANNELLED/TOGGLE/AUTOCAST/AURA/ATTACK/NOT_LEARNABLE`

### Unit state flags
`UNIT_STATE_STUNNED/ROOTED/SILENCED/MUTED/HEXED/INVISIBLE/INVULNERABLE/MAGIC_IMMUNE/NIGHTMARED/ATTACK_IMMUNE/BLIND/DISARMED/COMMAND_RESTRICTED`

### Courier actions
`COURIER_ACTION_RETURN/SECRET_SHOP/RETURN_STASH_ITEMS/TAKE_STASH_ITEMS/TAKE_AND_TRANSFER/TRANSFER_ITEMS/BURST/SIDE_SHOP/SIDE_SHOP2`

### Tower indices
`TOWER_TOP_1/2/3, MID_1/2/3, BOT_1/2/3, BASE_1, BASE_2`

### Runes
`RUNE_POWERUP_1/2, BOUNTY_1/2/3/4, RUNE_STATUS_NONE/AVAILABLE/MISSING`

### Fountains
- `FOUNTAIN_RADIANT` = Vector(-7200, -6700, 128)
- `FOUNTAIN_DIRE` = Vector(7200, 6700, 128)

---

## Persistence

Lua VM живёт между тиками. F7 (reload) уничтожает state и создаёт заново.

### Глобальные таблицы

- `BotControllerConfig` — настройки из config.lua
- `BotControllerChatPool` — пул чата
- `BotControllerState` — runtime state (tick, pending, last_cast, ...)
- `LaneWaypoints` — из lane_config.lua

### Per-unit storage

`bot.myField = value` сохраняется в глобальный `_unit_storage[entityIndex][key]`.
Переживает тики. F7 сбрасывает. Другой бот (другой entityIndex) не видит твои поля.

### Конвенции

- **Не** засоряй `_G` просто так. Если нужен модуль — `require("mymod")`.
- Если нужен per-bot state переживающий require — используй `bot.myState`.
- Если нужен global cross-mode state — добавь поле в `BotControllerState`.

---

## Что НЕ работает

Главные gaps:

1. **Real gold** — есть только approximation по level.
2. **Rune timers** — GetRuneStatus всегда 0, не работает рунная логика.
3. **Modifier inspection** — HasModifier всегда false. Нельзя проверить активные баффы/дебаффы.
4. **Projectile tracking** — GetLinearProjectiles/GetIncomingTrackingProjectiles пустые.
5. **Tree map** — GetTreeLocation всегда (0,0,128). Нет knowledge о деревьях.
6. **Vision map** — IsLocationVisible всегда false. Нельзя узнать что в fog.
7. **Stun duration calculation** — GetStunDuration возвращает 0.

Когда нужна фича из "не работает" — пиши на ReSuerve, мы реализуем через очередную итерацию.
