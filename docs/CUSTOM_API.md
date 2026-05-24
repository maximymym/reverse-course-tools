# CUSTOM_API — наш слой поверх Valve Bot API

Этот документ описывает custom API, который добавляет Andromeda поверх стандартного `BOT_SCRIPTING_API.md`. Всё живёт в `C:\temp\andromeda\scripts\bots\`.

---

## Оглавление

1. [`BotControllerConfig`](#botcontrollerconfig) — 20+ параметров поведения
2. [`BotControllerState`](#botcontrollerstate) — per-bot runtime state
3. [11-priority system](#11-priority-system) — как устроен `BotController_Think()`
4. [Anti-ban hooks](#anti-ban-hooks) — A.1 jitter, A.2 cast cooldown, A.3 chat
5. [Sol2 glue](#sol2-glue) — что экспортируется из C++

---

## `BotControllerConfig`

Главная таблица настроек. Задаётся в `bot_controller.lua:13-44`. Правки применяются после **Reload Lua** (F7 / кнопка в GUI).

### Movement & HP

| Поле | Тип | Дефолт | Что делает | Диапазон |
|------|-----|--------|------------|----------|
| `critical_hp` | float | `0.15` | HP% при котором бот уходит в фонтан (bypass pending). | 0.05–0.30 |
| `low_hp` | float | `0.30` | HP% safe-retreat. | 0.20–0.50 |
| `tower_near_hp` | float | `0.50` | HP% когда рядом с вражеской башней — retreat раньше. | 0.30–0.70 |
| `tower_radius` | float | `900.0` | Радиус "рядом с башней" (dota units). | 700–1200 |
| `attack_radius` | float | `1200.0` | Радиус поиска врагов. | 800–1500 |
| `cast_radius` | float | `800.0` | Радиус применения скиллов. | 500–1200 |
| `friendly_creep_radius` | float | `900.0` | Радиус проверки "есть свои крипы". | 700–1200 |
| `far_lane_dist` | float | `5000.0` | Если далеко от линии > этого → TP scroll. | 3000–7000 |
| `walk_lane_dist` | float | `3000.0` | Если далеко > этого → attack-move к линии. | 2000–4000 |
| `hold_lane_dist` | float | `300.0` | Если ближе этого → HOLD. | 200–500 |
| `stuck_ticks` | int | `50` | Тиков без движения → UNSTUCK. 50 тиков ≈ 5с. | 30–100 |
| `tp_cooldown` | float | `80.0` | Секунд между TP. | 60–120 |

### Throttling

| Поле | Тип | Дефолт | Что делает |
|------|-----|--------|------------|
| `order_throttle` | int | `3` | Приказы move/attackmove отдаются каждые `tick % 3 == 0`. Больше = реже приказы. |
| `combat_throttle` | int | `2` | Приказы combat (attack hero/creep/cast) каждые `tick % 2 == 0`. |

### Anti-ban A.1 — Jitter

| Поле | Тип | Дефолт | Что делает |
|------|-----|--------|------------|
| `jitter_min_ticks` | int | `8` | Мин задержка на все реакции кроме CRITICAL_RETREAT. ~10Hz → 8 тиков = 800 мс. |
| `jitter_max_ticks` | int | `25` | Макс задержка. 25 тиков ≈ 2.5 сек. |

**Важно:** jitter применяется к *reaction* (осмыслил → отдал приказ), не к каждому приказу. Это имитирует человеческую реакцию.

### Anti-ban A.2 — Cast cooldown

| Поле | Тип | Дефолт | Что делает |
|------|-----|--------|------------|
| `cast_cooldown_min` | float | `0.8` | Мин секунд между применением *того же* скилла *по той же цели*. |
| `cast_cooldown_max` | float | `1.5` | Макс — случайный в диапазоне. |

Ключ — `CastKey(ability_name, target_entity_idx)`. Предотвращает "spam skeleton_king vampiric_spirit 13 раз за 3 сек" → детектор бота.

### Anti-ban A.3 — Chat

| Поле | Тип | Дефолт | Что делает |
|------|-----|--------|------------|
| `chat_first_min_ticks` | int | `300` | Первый чат через 300 тиков (~30 с) после Init. |
| `chat_first_max_ticks` | int | `900` | Или до 90 с. |
| `chat_next_min_ticks` | int | `3000` | Следующий через 3000 тиков (~5 мин). |
| `chat_next_max_ticks` | int | `9000` | Или до 15 мин. |
| `chat_max_per_match` | int | `3` | Максимум реплик за матч. |

Пул реплик — `BotControllerChatPool` (ниже), по умолчанию 12 штук.

---

## `BotControllerChatPool`

Lua-массив строк — что бот может сказать в team chat.

```lua
BotControllerChatPool = {
    "gg", "ss", "?", "...", "nice", "gj", "lol",
    "care", "mid ss", "miss", "wp", "hf",
}
```

Короткие реплики = более "дота-язычный" стиль. Изменения → Reload Lua.

---

## `BotControllerState`

Runtime state **per-bot**. Сохраняется между тиками одного инстанса, ресетится при Hot Reload.

| Поле | Тип | Назначение |
|------|-----|------------|
| `tick` | int | Счётчик тиков с момента Init (инкрементится первой строкой `Think()`). |
| `last_tp_time` | float | `DotaTime()` последнего TP (для `tp_cooldown`). |
| `tp_casted` | bool | Флаг "TP в пути" — чтобы не спамить. |
| `last_pos_x` / `last_pos_y` | float | Последняя позиция, где бот реально двигался (> 50 units дельта). |
| `last_move_tick` | int | Для stuck detection. |
| `tower_aggro` | bool | Получил хит от башни в последние 10 тиков. |
| `tower_aggro_tick` | int | Тик последнего aggro (для таймаута). |
| `last_hp` | int | HP на прошлом тике (для определения "хп падает"). |
| `pending_action` | string/nil | A.1: имя отложенного действия ("UNSTUCK", "CAST_<name>", ...). |
| `pending_fn` | function/nil | A.1: замыкание, которое выполнится через jitter_*_ticks тиков. |
| `pending_reaction_tick` | int | A.1: `tick` когда выполнить `pending_fn`. |
| `last_cast` | table | A.2: `{ ["ability_X_targetN"] = gameTime }`. |
| `chat_next_tick` | int | A.3: когда следующий чат (0 = не инициализирован). |
| `chat_sent_count` | int | A.3: сколько реплик уже отправлено. |

**Можно читать/писать из своего кода** — например хранить свой кастомный state между тиками:

```lua
BotControllerState.my_custom_counter = (BotControllerState.my_custom_counter or 0) + 1
```

---

## 11-priority system

Главная функция — `BotController_Think()` в `bot_controller.lua:217`. Вызывается C++ каждый тик. Возвращает `(action_name, did_act)`.

Приоритеты (от высшего к низшему):

```
1. CRITICAL_RETREAT   ← hp < critical_hp (bypass pending, мгновенно)
2. TOWER_AGGRO        ← получил хит от башни → safePos
3. TOWER_UNSAFE       ← рядом с башней, нет своих крипов → safePos
4. LOW_HP_RETREAT     ← hp < low_hp (или tower_near_hp условия)
4.5. CAST             ← есть hero в cast_radius, скилл готов, cd ок
5. TP_LANE            ← far_lane_dist + есть TP + cooldown прошёл
6. WALK_LANE          ← walk_lane_dist → attack-move к линии
7. UNSTUCK            ← stuck_ticks без движения
8. ATTACK_HERO        ← hero в 1200 units
9. ATTACK_CREEP       ← creep в 1200 units
10. ATTACKMOVE_LANE   ← > hold_lane_dist → attack-move
11. HOLD              ← всё остальное
```

**Логика потока:**

1. `tick++`, инициализация chat schedule, `MaybeSendChat()`.
2. Computes: `hp`, `hpPct`, `myPos`, `lane`, `laneFront`, `safePos`, `fountain`, `isStuck`, `nearEnemyTower`, `hasFriendlyCreeps`, `nearestHero`.
3. Проверяет `CRITICAL_RETREAT` → **return** (bypass всего).
4. Если есть `pending_action` и пришло время → выполняет, возвращает `REACTION_<name>`.
5. Иначе идёт по приоритетам 2–11. На первом совпадении — `QueuePendingAction(action, fn)` и **return**.
6. HOLD как дефолт.

### Как вставить свой приоритет

**Пример:** добавить `ITEM_USAGE` между CAST (4.5) и TP_LANE (5):

```lua
-- в bot_controller.lua, после блока CAST:

-- 4.6. ITEM_USAGE — активные предметы (blink, glimmer, bkb, ...)
if shouldOrderCombat and nearestHero and nearestHeroDist < 1000 then
    for slot = 0, 8 do
        local item = bot:GetItemInSlot(slot)
        if item and not item:IsNull() and item:IsFullyCastable() then
            local name = item:GetName()
            if name == "item_blink" or name == "item_bkb" then
                if not pending_blocks then
                    QueuePendingAction("ITEM_" .. name, function()
                        bot:Action_UseAbility(item)
                    end)
                end
                return "ITEM_USAGE", false
            end
        end
    end
end
```

Порядок важен: ставь выше или ниже в зависимости от того, должен ли твой приоритет быть раньше TP/Cast/etc.

---

## Anti-ban hooks

### A.1 — `QueuePendingAction(action_name, fn)`

Вместо немедленного выполнения действия — ставит его в очередь на `jitter_min_ticks..jitter_max_ticks` тиков вперёд. Когда наступит время — вызовет `fn()`. Между постановкой и выполнением бот ничего не делает (имитирует "подумал").

**Как использовать в своём коде:**

```lua
QueuePendingAction("MY_ACTION", function()
    bot:Action_MoveToLocation(somewhere)
    -- Можно и state менять:
    BotControllerState.my_flag = true
end)
return "MY_ACTION", false  -- did_act=false потому что выполнится потом
```

**Bypass для критичных реакций** — см. `CRITICAL_RETREAT` в коде: ставишь нужное условие ДО проверки pending, `return` сразу.

### A.2 — `TryCastAbility(bot, target, maxDist, gameTime)`

Перебирает слоты 0..5, ищет заклинание которое можно кастануть. Учитывает cooldown по `CastKey(ability_name, target)`. Возвращает `(ok, fn, ability_name)`.

Расширение — добавить ещё проверку "по-моему это скилл можно":

```lua
local function TryCastAbility(bot, target, maxDist, gameTime)
    -- ... существующий код ...
    for i = 0, 5 do
        local ab = bot:GetAbilityInSlot(i)
        if ab and not ab:IsNull() then
            local name = ab:GetName()
            -- Свой фильтр:
            if name == "skeleton_king_vampiric_spirit" and target:GetHealth() < 100 then
                goto skip  -- не добивать этим
            end
            -- ... rest ...
            ::skip::
        end
    end
end
```

### A.3 — `MaybeSendChat()` + `BotControllerChatPool`

Вызывается первой строкой `BotController_Think()`. Проверяет `chat_next_tick` и `chat_sent_count`, если пора — берёт случайную реплику из `BotControllerChatPool` и отправляет через `ActionImmediate_Chat(msg, false)` (false = team chat, не all).

**Отключить чат целиком:**
```lua
BotControllerConfig.chat_max_per_match = 0
```

**Расширить пул:**
```lua
BotControllerChatPool = { "gg", "ss", "?", "lol", "nice", "ez", "noob", "retry", "mid pls" }
```

---

## Sol2 glue

C++ экспортирует в Lua через sol2 (`CBotBrain::RegisterLuaBindings` в DLL):

### Функции верхнего уровня

| Функция | Возвращает | Описание |
|---------|-----------|----------|
| `GetBot()` | `UnitHandle` | Текущий бот (для кого вызван `Think()`). Может быть nil если dead/gone. |
| `GetBotTeam()` | int | 2=Radiant, 3=Dire. |
| `GetLocationAlongLane(lane, amount)` | `Vector` | Позиция на линии, `amount` ∈ [0..1]. lane: 1=bot, 2=mid, 3=top. |
| `GetCreepFrontLocation(bot, lane, radius)` | `Vector` | Передовая крипов для `bot`. |
| `GetLaneAmountForBot(lane, team, "safe"/"lane")` | float | Для safe/lane retreat amounts. |
| `GetOwnFountain()` | `Vector` | Фонтан *своей* команды. |
| `IsLocationNearEnemyTower(loc, radius)` | bool | Проверка близости вражеской башни. |
| `DotaTime()` | float | Игровое время в секундах. |
| `RandomInt(min, max)` | int | `[min, max]`. |
| `RandomFloat(min, max)` | float | `[min, max)`. |
| `Vector(x, y, z)` | `Vector` | Конструктор. |
| `ActionImmediate_Chat(msg, allChat)` | void | Отправить чат (team или all). |
| `print(...)` | void | → `botbrain.log`. |

### UnitHandle методы

(Подмножество — полный список в `BOT_SCRIPTING_API.md`.)

- `bot:GetHealth()`, `GetMaxHealth()`, `GetMana()`, `GetMaxMana()`
- `bot:GetLocation()` → `Vector`
- `bot:GetTeam()`, `GetPlayerID()`, `GetUnitName()`
- `bot:IsAlive()`, `IsNull()`
- `bot:GetAssignedLane()` → 1/2/3
- `bot:GetAbilityInSlot(i)` → `AbilityHandle`
- `bot:GetItemInSlot(i)` → `ItemHandle`
- `bot:FindItemSlot("item_name")` → int или -1
- `bot:GetNearbyHeroes(radius, enemiesOnly)` → array
- `bot:GetNearbyLaneCreeps(radius, enemiesOnly)` → array
- `bot:GetNearbyTowers(radius, enemiesOnly)` → array
- `bot:Action_MoveToLocation(vec)`
- `bot:Action_AttackMove(vec)`
- `bot:Action_AttackUnit(target)`
- `bot:Action_UseAbility(ab)`
- `bot:Action_UseAbilityOnEntity(ab, target)`
- `bot:Action_UseAbilityOnLocation(ab, vec)`

### `UnitHandle._unit_storage`

Per-unit таблица — можно прикреплять данные к юниту между тиками. Ключ — entity index юнита:

```lua
-- Записать
local me = bot:GetPlayerID()
_G._unit_storage = _G._unit_storage or {}
_G._unit_storage[me] = _G._unit_storage[me] or {}
_G._unit_storage[me].my_counter = (_G._unit_storage[me].my_counter or 0) + 1

-- Прочитать
local n = (_G._unit_storage[me] or {}).my_counter or 0
```

Для большинства задач достаточно `BotControllerState` (один на бот, очищается при reload).

---

## Файлы и порядок загрузки

`CBotBrain::Init()` (C++) вызывает `LoadBotScripts()` которая грузит в порядке:

1. `bot_controller.lua` — наш главный
2. `bot_generic.lua` — valve-style fallback
3. `ability_item_usage_generic.lua`
4. `lane_config.lua`

Всё в одной Lua state — функции/глобалы видны между файлами. `BotControllerConfig` / `BotControllerState` / `BotControllerChatPool` объявлены с `X = X or {...}` — при reload сохраняют старые значения если их вручную менял в runtime (что невозможно, но на всякий случай).
