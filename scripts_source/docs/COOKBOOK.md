# Cookbook — 15 рецептов под частые задачи

Каждый рецепт: *что меняем*, *где*, *какой код*, *как проверить*.

Схема: копируешь → правишь → Reload Lua (F7) → смотришь `botbrain.log`.

---

## 1. Изменить HP пороги retreat'а

**Цель**: бот убегает раньше (защитить жизнь) или позже (агрессивнее).

**Файл**: `config.lua`

```lua
BotControllerConfig = BotControllerConfig or {
    critical_hp   = 0.15,   -- ← было 0.15 → сделать 0.25 (убегать при <25%)
    low_hp        = 0.30,   -- ← было 0.30 → 0.45
    tower_near_hp = 0.50,   -- ← было 0.50 → 0.65
    ...
}
```

**Проверка**: `botbrain.log` → `[LUA-ctrl] action=retreat:SAFE hp_pct=0.35` при HP=35%.

---

## 2. Расширить пул чата + изменить тайминги

**Файл**: `config.lua`

```lua
BotControllerChatPool = BotControllerChatPool or {
    "gg", "ss", "care", "nice", "gj",
    -- добавить новое:
    "дд", "сс", "за тебя", "гг", "пф",
}

BotControllerConfig.chat_first_min_ticks  = 600    -- первое сообщ. ≥1 минута
BotControllerConfig.chat_first_max_ticks  = 1500   -- до 2.5 минуты
BotControllerConfig.chat_max_per_match    = 5       -- было 3 → 5
```

**Проверка**: через 1-2 минуты после начала — в командном чате появится одно
из новых сообщений.

---

## 3. Отключить чат полностью

**Файл**: `config.lua`

```lua
BotControllerConfig.chat_enabled = false
```

Всё. Не удаляй пул — если понадобится вернуть, просто `true`.

---

## 4. Написать свой mode

**Цель**: добавить новый mode "ward_place" — бот идёт ставить варды в 8 минут.

**Шаг 1**: создай `modes/ward_place.lua`:

```lua
-- modes/ward_place.lua — в 8 минут идём к обсерваторной точке и ставим варды.

local M = { name = "ward_place" }
local Move = require("util.movement")
local AB   = require("util.anti_ban")

local WARD_SPOTS = {
    [2] = Vector(-1500, 3000, 128),  -- Radiant — отслеживание dire jungle
    [3] = Vector(1500, -3000, 128),   -- Dire — отслеживание radiant jungle
}

function M.GetDesire(bot, ctx)
    if ctx.game_time < 7 * 60 or ctx.game_time > 9 * 60 then return 0 end
    if ctx.hp_pct < 0.5 then return 0 end

    -- Есть ли варды в инвентаре?
    local slot = bot:FindItemSlot("item_ward_observer")
    if not slot or slot < 0 then return 0 end

    return 0.85  -- высокий приоритет в окне 7-9 мин
end

function M.Think(bot, ctx)
    local spot = WARD_SPOTS[ctx.team]
    if not spot then return "NO_SPOT" end

    local d = Move.Dist2D(ctx.pos, spot)
    if d > 400 then
        if not AB.HasPending() then
            AB.QueuePendingAction("WARD_WALK",
                function() bot:Action_MoveToLocation(spot) end)
        end
        return "WARD_WALK"
    end

    -- На месте — ставим вард
    local slot = bot:FindItemSlot("item_ward_observer")
    local ward = bot:GetItemInSlot(slot)
    if ward and not ward:IsNull() and ward:IsFullyCastable() then
        if not AB.HasPending() then
            AB.QueuePendingAction("WARD_PLACE",
                function() bot:Action_UseAbilityOnLocation(ward, spot) end)
        end
        return "WARD_PLACE"
    end

    return "IDLE"
end

return M
```

**Шаг 2**: зарегистрируй в `modes/init.lua`:

```lua
add("modes.ward_place")
```

**Шаг 3**: Reload Lua.

**Проверка**: `botbrain.log` → `[LUA-ctrl] action=ward_place:WARD_WALK` в окне 7-9 мин.

---

## 5. Per-hero item build (Pudge-specific)

**Файл**: `item_build.lua`

```lua
M.builds = {
    default = { ... },

    -- добавить блок:
    pudge = {
        starting = { "item_tango", "item_gauntlets", "item_circlet", "item_branches" },
        early    = { "item_boots", "item_urn_of_shadows", "item_magic_stick" },
        core     = { "item_tranquil_boots", "item_blink", "item_blade_mail", "item_rod_of_atos" },
        late     = { "item_aghanims_scepter", "item_octarine_core", "item_heart" },
    },
}
```

**Проверка**: `botbrain.log` → `[item_build] buy item_gauntlets` если герой — Pudge.

---

## 6. Per-hero level-up sequence

**Файл**: `ability_build.lua`

```lua
M.sequences = {
    default = { 1,2,3,1,6,1,2,2,3,3,6,3,2,4,4,6,4,4 },

    -- Crystal Maiden — Frostbite приоритет
    crystal_maiden = { 2, 1, 2, 3, 2, 6, 2, 3, 3, 3, 6, 1, 1, 1, 4, 6, 4, 4, 4, 5 },
}
```

**Значения**: 1..6 = slot index (1-based). 6 = ulti.

**Проверка**: `[ability_build] level crystal_maiden_frostbite (slot 2) at hero_level=1`.

---

## 7. Per-hero полный override

**Цель**: Pudge hook'ает low-HP врагов до rotation в lane.

**Файл**: `heroes/pudge.lua` (уже создан как template — посмотри). И в `heroes/init.lua`:

```lua
heroes["pudge"] = safeRequire("heroes.pudge")
```

Теперь Pudge bot первым делом каждый тик проверяет "есть ли low-HP enemy в hook range
и готов ли hook" — если да, hook'ает. Иначе dispatcher работает стандартно.

---

## 8. Stack jungle camp в xx:53

**Цель**: в 1:53, 2:53, 3:53, ... подходить к camp'у за 7с до спавна.

Это **НЕ новый mode** а расширение `jungle_farm`. Проще — добавить специальный override:

**Файл**: `modes/jungle_farm.lua` (отредактируй)

```lua
function M.GetDesire(bot, ctx)
    local C = BotControllerConfig
    local boost = C.desire_boost_jungle_farm or 1.0

    -- Stack window: 53-59 секунд каждой минуты
    local cycleSec = ctx.game_time % 60
    if cycleSec >= 53 and cycleSec < 60 then
        -- Высокий desire если рядом camp (500 единиц)
        if ctx.neutrals and #ctx.neutrals > 0 then
            return 0.95 * boost  -- stack mode
        end
    end

    -- далее — обычная логика...
end
```

**Проверка**: в 1:55 в логе `[LUA-ctrl] action=jungle_farm:ATTACK_NEUTRAL`.

---

## 9. Pull lane creeps

**Цель**: Radiant Support pull'ит easy camp в xx:15/xx:45.

**Файл**: `modes/pull.lua` (новый)

```lua
local M = { name = "pull" }
local Move = require("util.movement")
local AB   = require("util.anti_ban")

local PULL_CAMP_RADIANT = Vector(-3400, -2700, 128)
local PULL_TARGET_RADIANT = Vector(-4600, -3300, 128)  -- куда гнать

function M.GetDesire(bot, ctx)
    if ctx.team ~= 2 then return 0 end  -- только radiant sup пока
    if ctx.level < 3 then return 0 end
    if ctx.hp_pct < 0.6 then return 0 end

    local cycle = ctx.game_time % 60
    if cycle >= 10 and cycle < 25 then  -- xx:15 ±
        local d = Move.Dist2D(ctx.pos, PULL_CAMP_RADIANT)
        if d < 2500 then return 0.8 end
    end
    return 0
end

function M.Think(bot, ctx)
    local d = Move.Dist2D(ctx.pos, PULL_CAMP_RADIANT)
    if d > 200 then
        if not AB.HasPending() then
            AB.QueuePendingAction("PULL_WALK",
                function() bot:Action_AttackMove(PULL_CAMP_RADIANT) end)
        end
        return "PULL_WALK"
    end
    -- Атакуем любого нейтрала → потащит за собой
    if ctx.neutrals and ctx.neutrals[1] then
        bot:Action_AttackUnit(ctx.neutrals[1])
        return "PULL_HIT"
    end
    return "PULL_WAIT"
end

return M
```

Добавь в `modes/init.lua`: `add("modes.pull")`.

---

## 10. Rotation к ganku когда враг виден вне base >30с и HP<40%

Это уже делает `modes/gank.lua` (см. исходник). Чтобы ужесточить — правь пороги:

**Файл**: `modes/gank.lua`

```lua
function M.GetDesire(bot, ctx)
    if ctx.hp_pct < 0.7 then return 0 end          -- было 0.5, теперь 0.7
    if ctx.mana_pct < 0.5 then return 0 end        -- было 0.35 → 0.5
    -- ...
end

local function FindGankTarget(ctx)
    -- жёстче: только если враг был виден < 10с назад
    for pid = 0, 9 do
        local info = GetHeroLastSeenInfo(pid)
        if info and info.TimeSinceLastSeen < 10 then  -- было 20
            -- ...
```

---

## 11. Early-game: запретить TP на вражескую половину первые 10 мин

**Файл**: `modes/hold.lua` (правим TP-логику)

```lua
-- TP scroll при дальнем расстоянии
if d > C.far_lane_dist and ctx.hp_pct > 0.5 then
    -- EARLY GAME restriction: не TP на вражескую половину до 10 мин
    local enemy_half_y = (ctx.team == 2) and 0 or 0  -- Radiant side y<0, Dire y>0
    local tpPos = ctx.lane_front
    local isEnemyHalf = (ctx.team == 2 and tpPos.y > 0) or (ctx.team == 3 and tpPos.y < 0)
    if ctx.game_time < 10 * 60 and isEnemyHalf then
        return "NO_TP_EARLY"
    end
    -- ...
```

**Проверка**: `[LUA-ctrl] action=hold:NO_TP_EARLY` если бот в 8:00 собрался TP'ать к вражеской T1.

---

## 12. Отключить jungle_farm полностью

**Файл**: `config.lua`

```lua
BotControllerConfig.desire_boost_jungle_farm = 0  -- domain-scaled desire × 0
```

Mode всё ещё регистрируется, но `GetDesire()` всегда возвращает 0 (или очень мало).
Dispatcher никогда не выберет его.

---

## 13. Debug: вывести значения в лог каждый 30-й tick

**Файл**: любой mode или `bot_controller.lua`

```lua
if BotControllerState.tick % 30 == 0 then
    print("[DEBUG] hp=" .. ctx.hp .. " mana=" .. ctx.mana
          .. " gold=" .. ctx.gold .. " mode=" .. BotControllerState.last_mode)
end
```

`print()` пишет в **botbrain.log**:
```
[14:23:45.234] [Lua] [DEBUG] hp=600 mana=450 gold=2800 mode=lane_farm
```

Удобно для быстрой диагностики. Убрать — просто закомментируй.

---

## 14. Условный item build — Aghanim только если Pudge

Уже встроено в `item_build.lua::builds.pudge`. Но допустим хочешь: Aghanim
только если gold > 5000 И level > 15.

**Файл**: `item_build.lua`

```lua
function M.Think(bot)
    -- ...existing code...

    -- Перед item loop добавь специальные условные правила:
    local heroName = (bot:GetUnitName() or ""):gsub("^npc_dota_hero_", "")
    local gold = bot:GetGold()
    local level = bot:GetLevel()

    if heroName == "pudge" and level >= 15 and gold >= 4200 and not state.bought["item_aghanims_scepter"] then
        bot:ActionImmediate_PurchaseItem("item_aghanims_scepter")
        state.bought["item_aghanims_scepter"] = true
        return
    end

    -- ...regular phase loop...
end
```

---

## 15. Roshan: coordinate 5 ботов для start без лотереи

Проблема: в стандартном `modes/roshan.lua` каждый бот независимо решает — они могут
рассинхрониться (часть пошла к Рошу, часть нет).

**Решение**: общий shared flag через файл (между процессами).

**Файл**: `modes/roshan.lua`

```lua
local ROSHAN_COORDINATE_FILE = "C:/temp/andromeda/roshan_vote.flag"

-- Считать сколько ботов "проголосовали" за Роша
local function CountVotes()
    local f = io.open(ROSHAN_COORDINATE_FILE, "r")
    if not f then return 0 end
    local t = f:read("*all") or ""
    f:close()
    local n = 0
    for _ in t:gmatch("[^\n]+") do n = n + 1 end
    return n
end

local function Vote(instanceId)
    local f = io.open(ROSHAN_COORDINATE_FILE, "a")
    if not f then return end
    f:write(tostring(instanceId) .. "\n")
    f:close()
end

local function ClearVote()
    os.remove(ROSHAN_COORDINATE_FILE)
end

function M.GetDesire(bot, ctx)
    -- ... стандартные проверки ...

    local votes = CountVotes()
    if votes < 3 then
        -- Недостаточно — проголосуем за себя если hp/lvl ок
        if ctx.hp_pct > 0.7 and ctx.level >= 15 and ctx.game_time > 15 * 60 then
            Vote(GetInstanceId and GetInstanceId() or 0)
        end
        return 0.3  -- небольшой desire (хотим но ждём)
    end

    return 0.9  -- 3+ bot'а готовы — начинаем
end
```

**Важно**: флаг чистится когда Roshan убит или матч закончился — иначе воты
сохранятся и в следующей игре. Добавь где-нибудь:

```lua
-- В bot_controller.lua init section:
if not BotControllerState.rosh_flag_cleared then
    ClearVote()
    BotControllerState.rosh_flag_cleared = true
end
```

Более надёжный подход: использовать per-match file с timestamp'ом или DotaTime как
уникальным идентификатором матча.

---

## Расширенные темы

### Как узнать какие modes активны

`BotControllerState.mode_history` — последние 20 переключений.

```lua
-- В debug print:
print("[DEBUG] modes=" .. table.concat(BotControllerState.mode_history, ","))
```

→ `[DEBUG] modes=lane_farm,retreat,hold,lane_farm`

### Как добавить "cooldown" на выдачу приказа

Жёсткий throttle — пропускать тики. Уже есть `C.order_throttle`.

Гибкий — внутри mode:
```lua
function M.Think(bot, ctx)
    if (ctx.tick - (BotControllerState.last_my_order_tick or 0)) < 30 then
        return "COOLDOWN"  -- 30 ticks ≈ 3с skip
    end
    BotControllerState.last_my_order_tick = ctx.tick
    -- выдаём приказ...
end
```

### Как узнать что произошло 5 тиков назад

Нет встроенного hist'а. Записывай сам:

```lua
BotControllerState.hp_history = BotControllerState.hp_history or {}
table.insert(BotControllerState.hp_history, ctx.hp)
if #BotControllerState.hp_history > 50 then
    table.remove(BotControllerState.hp_history, 1)
end

local hpDelta5Tick = ctx.hp - (BotControllerState.hp_history[#BotControllerState.hp_history - 5] or ctx.hp)
```

### Отладка без игры

В реальной Dota нельзя — DLL инжектится в dota2.exe. Но можно писать unit-тесты для
Lua модулей на десктопе:

```bash
cd C:/temp/andromeda/scripts/bots
lua54 -e "package.path='./?.lua;./?/init.lua'" -l movement
```

Но большая часть API требует C++ globals. Для них проще пустить один тестовый бот в AP
vs bots и смотреть логи.

---

## Что НЕ получится

- Писать UI overlay из Lua (Lua → memory/orders only).
- Читать содержимое чата других игроков (не парсится).
- Блокировать вражеские способности (не можем влиять на их game state).
- Получать real-time projectile tracking (см. API_REFERENCE → "не работает").

Для таких задач нужно расширять **C++** layer — открой `LuaStubs.hpp` / `LuaUnitProxy.cpp`.
