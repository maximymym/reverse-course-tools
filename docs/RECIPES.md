# RECIPES — готовые правки

Пошаговые сниппеты для частых задач. Все правки в `C:\temp\andromeda\scripts\bots\`, применяются через **Reload Lua** (кнопка в DotaFarm) или F7 в окне Dota.

Если не понимаешь что происходит — сначала прочитай `CUSTOM_API.md`.

---

## 1. Боты отступают раньше (HP < 40% вместо 30%)

**Файл:** `bot_controller.lua`

```lua
BotControllerConfig = BotControllerConfig or {
    -- ...
    low_hp          = 0.40,   -- было 0.30
    -- ...
}
```

Заодно можно поднять `critical_hp` (порог фонтана) и `tower_near_hp`:

```lua
    critical_hp     = 0.20,   -- было 0.15
    low_hp          = 0.40,
    tower_near_hp   = 0.60,   -- было 0.50
```

**Reload Lua** → боты в тестовой игре при урагане начнут убегать заметно раньше.

---

## 2. Добавить hero в фильтр "не атаковать под башней"

Если конкретный бот (например Pudge) слишком смело лезет под вышку.

**Файл:** `BotLib/hero_pudge.lua` (создать если нет) — Dota грузит per-hero файлы автоматически.

```lua
-- hero_pudge.lua — переопределение поведения под башней для Пуджа

local base_Think = BotController_Think

function BotController_Think()
    local bot = GetBot()
    if not bot or not bot:IsAlive() then return base_Think() end

    local name = bot:GetUnitName() or ""
    if name:find("pudge", 1, true) then
        -- Для Пуджа — более агрессивный tower retreat:
        local towers = bot:GetNearbyTowers(1100, true)  -- расширенный радиус
        if towers and #towers > 0 then
            local team = bot:GetTeam()
            local lane = bot:GetAssignedLane() or 2
            local safePos = GetLocationAlongLane(lane, team == 2 and 0.2 or 0.8)
            bot:Action_MoveToLocation(safePos)
            return "PUDGE_TOWER_RETREAT", true
        end
    end

    return base_Think()
end
```

---

## 3. Свой пул реплик для чата

**Файл:** `bot_controller.lua`

```lua
BotControllerChatPool = {
    "gg", "wp", "ty", "ez", "retry", "one more",
    "smurf", "noob team", "report mid", "no gg",
    -- русский тоже ок:
    "лагает", "кто керри", "фарм", "не катим",
}
```

**Важно:** реплики должны быть *короткие* (1-3 слова). Длинные = нетипично для доты → палево.

---

## 4. Более "нервный" jitter (200-500 мс)

Делает реакции более дёрганными — имитация игрока-новичка.

**Файл:** `bot_controller.lua`

```lua
BotControllerConfig = BotControllerConfig or {
    -- ...
    -- Jitter при ~10Hz tick: 2 тика ≈ 200мс, 5 тиков ≈ 500мс
    jitter_min_ticks = 2,   -- было 8
    jitter_max_ticks = 5,   -- было 25
    -- ...
}
```

Ещё вариант — "более расслабленный" (0.5–3.5 с):
```lua
    jitter_min_ticks = 5,
    jitter_max_ticks = 35,
```

---

## 5. Новый приоритет — использование предметов

Вставить ITEM_USAGE между CAST (4.5) и TP_LANE (5).

**Файл:** `bot_controller.lua`

Найти блок:
```lua
    -- 4.5. CAST (с A.2 cooldown)
    local gameTime = DotaTime()
    if shouldOrderCombat and nearestHero and nearestHeroDist < C.cast_radius then
        -- ...
    end

    -- 5. TP_LANE
```

Вставить между:

```lua
    -- 4.6. ITEM_USAGE — активные боевые предметы
    if shouldOrderCombat and nearestHero and nearestHeroDist < 900 then
        local activeItems = { "item_blink", "item_glimmer_cape", "item_ghost" }
        for _, itemName in ipairs(activeItems) do
            local slot = bot:FindItemSlot(itemName)
            if slot and slot >= 0 then
                local item = bot:GetItemInSlot(slot)
                if item and not item:IsNull() and item:IsFullyCastable() then
                    if not pending_blocks then
                        QueuePendingAction("ITEM_" .. itemName, function()
                            bot:Action_UseAbility(item)
                        end)
                    end
                    return "ITEM_USAGE_" .. itemName, false
                end
            end
        end
    end
```

---

## 6. Запретить TP на вражескую половину первые 10 мин

Избегает early-game ганк-вылетов.

**Файл:** `bot_controller.lua`

Найти блок:
```lua
    -- 5. TP_LANE
    if distToLane > C.far_lane_dist and not BotControllerState.tp_casted
       and hpPct > 0.5
       and (gameTime - BotControllerState.last_tp_time) > C.tp_cooldown then
```

Заменить на:
```lua
    -- 5. TP_LANE (с early-game safety)
    local earlyGame = gameTime < 600.0   -- первые 10 минут
    local team = bot:GetTeam()
    local enemyHalf
    if team == 2 then
        enemyHalf = laneFront.x > 0 or laneFront.y > 0   -- Radiant → вражеская половина = + коорды
    else
        enemyHalf = laneFront.x < 0 or laneFront.y < 0
    end
    if earlyGame and enemyHalf then
        -- Skip TP — слишком рискованно
    elseif distToLane > C.far_lane_dist and not BotControllerState.tp_casted
           and hpPct > 0.5
           and (gameTime - BotControllerState.last_tp_time) > C.tp_cooldown then
```

(не забудь закрыть `elseif` существующими веткам — структура `if ... elseif ... end` должна остаться логически консистентной).

---

## 7. Другой стартовый предмет

**Файл:** `item_purchase_generic.lua`

Найди блок с начальными предметами (`sStartingItems`). Пример — купить Stout Shield вместо Quelling Blade:

```lua
local sStartingItems = {
    "item_tango",
    "item_stout_shield",   -- было "item_quelling_blade"
    "item_branches",
    "item_branches",
    "item_ward_observer",
}
```

Per-hero: отредактируй `BotLib/hero_<name>.lua`, там в `function ItemPurchaseThink(npcBot)` обычно свой `sStartingItems`.

---

## 8. Дебаг-принт в botbrain.log каждый тик

**Файл:** `bot_controller.lua`

В начало `BotController_Think()` (после `tick = tick + 1`):

```lua
    if tick % 30 == 0 then   -- раз в 30 тиков = ~3 сек
        print(string.format(
            "[DEBUG] tick=%d hp=%d/%d pos=%.0f,%.0f lane=%d stuck=%s",
            tick, hp, maxHp, myPos.x, myPos.y, lane, tostring(isStuck)))
    end
```

`print()` пишет в `C:\temp\andromeda\botbrain.log`. Не ставь без условия `tick % N == 0` — иначе spam на 10 строк/сек.

---

## 9. Per-hero: агрессивный Pudge с хуками

**Файл:** `BotLib/hero_pudge.lua`

```lua
-- hero_pudge.lua — агрессивный laning для Пуджа

local base_Think = BotController_Think

function BotController_Think()
    local bot = GetBot()
    if not bot or not bot:IsAlive() then return base_Think() end

    local name = bot:GetUnitName() or ""
    if not name:find("pudge", 1, true) then return base_Think() end

    -- Сначала пробуем кинуть хук если есть цель
    local hook = bot:GetAbilityInSlot(0)  -- meat_hook обычно в 0 слоте
    if hook and not hook:IsNull() and hook:IsFullyCastable() then
        local enemies = bot:GetNearbyHeroes(1300, true)
        if enemies then
            for _, e in ipairs(enemies) do
                if e:IsAlive() then
                    local d = ((bot:GetLocation().x - e:GetLocation().x)^2
                            + (bot:GetLocation().y - e:GetLocation().y)^2)^0.5
                    if d > 400 and d < 1300 then
                        bot:Action_UseAbilityOnLocation(hook, e:GetLocation())
                        return "PUDGE_HOOK", true
                    end
                end
            end
        end
    end

    -- Иначе — стандартный controller
    return base_Think()
end

print("[hero_pudge] loaded — aggressive hook laning")
```

---

## 10. Выключить чат полностью

Если тестеру не нужно тратить тики на чат.

**Файл:** `bot_controller.lua`

```lua
BotControllerConfig.chat_max_per_match = 0
```

Альтернативно, задать пустой пул (тогда если чат сработает — отправит "gg" дефолт):
```lua
BotControllerChatPool = {}
```

---

## 11. Включить обратно F9 как единственный hot-reload (убрать F7)

Если F7 конфликтует с твоим бинд-меню в Dota.

**Файл DLL** (надо пересобрать): `Andromeda/BotFarm/CBotFarmClient.cpp` строки ~252:

```cpp
// Было:
if ( ( GetAsyncKeyState( VK_F7 ) & 1 ) || ( GetAsyncKeyState( VK_F9 ) & 1 ) )
    GetBotBrain()->HotReload();

// Стало:
if ( GetAsyncKeyState( VK_F9 ) & 1 )
    GetBotBrain()->HotReload();
```

Потом `MSBuild Andromeda-Dota2-Base.sln -p:Configuration=Release -p:Platform=x64`.

**Без пересборки** — просто не нажимай F7 в окне Dota, используй только кнопку "Reload Lua" в DotaFarm GUI. Она работает всегда.

---

## Общие советы

- **Всегда делай бэкап** перед большой правкой — `cp bot_controller.lua bot_controller.lua.bak`.
- **Тестируй на одном боте** (выключи остальные в `accounts.json` или в DotaFarm). Если сломается — меньше риска.
- **Малые правки** — легче откатить чем 50 строк за раз.
- **Читай `botbrain.log`** после каждой правки — особенно первые 5 сек после Reload: там видно `=== HOT RELOAD ===` и любые `[LUA ERROR]`.
- **Ошибки в Lua не крашат Dota** — DLL ловит их через `pcall`, бот просто переходит в C++ fallback (тупое поведение).
