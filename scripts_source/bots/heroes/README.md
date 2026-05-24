# Hero Overrides

Per-hero логика, которая заменяет стандартный dispatcher (если возвращает truthy).

## Как работает

В `bot_controller.lua` после Context.Build проверяется:

```lua
local heroName = bot:GetUnitName():gsub("npc_dota_hero_", "")
local heroOverride = Heroes[heroName]
if heroOverride and heroOverride.Think then
    local ok, result = pcall(heroOverride.Think, bot, ctx)
    if ok and result then
        return "hero:" .. heroName .. ":" .. result, true
    end
end
```

Если `heroOverride.Think(bot, ctx)` возвращает `nil` или `false` — управление переходит
стандартным mode'ам. Если возвращает string — принимается как action_name и dispatcher
пропускается на этот тик.

## Добавление нового героя

1. Создай файл `heroes/<hero_name>.lua`:

```lua
local M = {}
local Move = require("util.movement")
local AB = require("util.anti_ban")

function M.Think(bot, ctx)
    -- Твоя логика. Возвращай action_name (string) если сработала,
    -- иначе nil — тогда будет работать обычный dispatcher.

    if ctx.hp_pct > 0.8 and ctx.mana_pct > 0.5 then
        -- ...
        return "MY_ACTION"
    end

    return nil  -- default dispatcher
end

return M
```

2. В `heroes/init.lua` раскомментируй/добавь:

```lua
heroes["<hero_name>"] = safeRequire("heroes.<hero_name>")
```

3. Reload Lua (F7 или кнопка).

## Как писать осмысленно

- Не нужно переписывать весь dispatcher. Override только те ситуации которые требуют
  hero-specific поведения (hook setup у Pudge, ult combo у Tiny, burst у Lina).
- Возвращай `nil` чтобы dispatcher разобрался сам.
- Используй `ctx.game_time`, `ctx.mana_pct`, `ctx.hp_pct`, `ctx.nearest_enemy`.
- Респектируй cast cooldown через `AB.CanCast(name, target, gt)`.

См. `heroes/pudge.lua` как пример.
