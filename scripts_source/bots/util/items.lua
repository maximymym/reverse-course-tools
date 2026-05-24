-- util/items.lua — item-use helpers (tango, healing, active items dispatcher).
--
-- Tango: heal-over-time (115 HP / 16s) от поедания дерева. Cooldown 16s
-- per charge, 4 charges на один stack. Cast range 165 от бота.
--
-- C++ binding'и (Andromeda DLL):
--   bot:GetNearbyTrees(radius)         → {treeId, ...}  (entityIndex'ы)
--   GetTreeLocation(treeId)            → Vector3
--   bot:Action_UseAbilityOnTree(item, treeId)  → OrderType 7
--
-- Action_UseAbility / OnEntity / OnLocation — после расширения принимают
-- ItemHandle напрямую (sol::object polymorphism). Action_ToggleAbility
-- работает через item handle (ToggleAbility binding в LuaUnitProxy).
--
-- Без этих C++ функций tango-логика будет no-op (GetNearbyTrees вернёт пустую
-- таблицу) — старые stub'ы возвращали {} и {}, новая реализация (после fix
-- 2026-04-27) реальная.

local M = {}

local vec = require("util.vec")
local AB  = require("util.anti_ban")
local Dist2D = vec.Dist2D

-- Tango cast range — после Patch 7.21 равен 165. Используем 200 как safety
-- margin (если бот в 200 от дерева, точно дотянется).
local TANGO_CAST_RANGE = 200.0
local TANGO_SEARCH_RADIUS = 600.0   -- сколько ищем дерево вокруг бота
local TANGO_COOLDOWN_SECS = 16.0    -- per-charge cooldown

-- Имена items которые проверяем по charges+cooldown а не через IsFullyCastable
-- (для consumables m_bItemEnabled может быть false в C++ кэше — см. tango fix).
local CONSUMABLE_NAMES = {
    item_magic_stick     = true,
    item_magic_wand      = true,
    item_enchanted_mango = true,
    item_faerie_fire     = true,
    item_clarity         = true,
    item_flask           = true,
    item_bottle          = true,
    item_tango           = true,
    item_tango_single    = true,
}

-- ── Tango (existing, unchanged behaviour) ──────────────────────────────────
-- Найти tango в инвентаре. Возвращает (item, slot) или (nil, nil).
-- Tango может быть item_tango (full stack) или item_tango_single (поделённое).
function M.FindTango(bot)
    if not bot or bot:IsNull() then return nil, nil end
    for _, name in ipairs({"item_tango", "item_tango_single"}) do
        local slot = bot:FindItemSlot(name)
        if slot and slot >= 0 then
            local item = bot:GetItemInSlot(slot)
            if item and not item:IsNull() then
                return item, slot
            end
        end
    end
    return nil, nil
end

-- Ближайшее дерево (по 2D distance), возвращает (treeId, distance, treeLoc).
function M.NearestTree(bot, fromPos, radius)
    radius = radius or TANGO_SEARCH_RADIUS
    if not bot or bot:IsNull() or not fromPos then return nil, 99999, nil end
    if not bot.GetNearbyTrees then return nil, 99999, nil end

    local trees = bot:GetNearbyTrees(radius) or {}
    local bestId, bestD, bestLoc = nil, 99999, nil
    for _, treeId in ipairs(trees) do
        local loc = GetTreeLocation and GetTreeLocation(treeId)
        if loc then
            local d = Dist2D(fromPos, loc)
            if d < bestD then
                bestId, bestD, bestLoc = treeId, d, loc
            end
        end
    end
    return bestId, bestD, bestLoc
end

-- Главная функция: попытаться съесть танго прямо сейчас.
function M.TryEatTango(bot, ctx)
    local gt = (ctx and ctx.game_time) or 0

    -- Hard rate-limit. IsFullyCastable отражает движковый cooldown, но при
    -- F9-reload скриптов может вернуться неконсистентное состояние.
    local lastCast = BotControllerState.last_tango_cast_gt or -999.0
    if (gt - lastCast) < TANGO_COOLDOWN_SECS then
        return false, "TANGO_CD"
    end

    local item, slot = M.FindTango(bot)
    if not item then return false, "NO_TANGO" end

    local charges = (item.GetCurrentCharges and item:GetCurrentCharges()) or 0
    local cd      = (item.GetCooldownTimeRemaining and item:GetCooldownTimeRemaining()) or 0
    if charges <= 0 then return false, "TANGO_NO_CHARGES" end
    if cd > 0.5 then return false, "TANGO_CD_INTERNAL" end
    if slot and slot > 5 then return false, "TANGO_NOT_IN_MAIN" end

    local treeId, dist, treeLoc = M.NearestTree(bot, ctx.pos, TANGO_SEARCH_RADIUS)
    if not treeId then return false, "NO_TREE" end

    if dist > TANGO_CAST_RANGE then
        return false, "TREE_FAR", treeLoc, dist
    end

    bot:Action_UseAbilityOnTree(item, treeId)
    BotControllerState.last_tango_cast_gt = gt
    print(string.format(
        "[items] EAT_TANGO tree=%d dist=%d hp=%.2f gt=%.1f",
        treeId, math.floor(dist), ctx.hp_pct or 0, gt))
    return true, "EAT_TANGO"
end

-- ── TP Scroll (boots of travel fallback) ───────────────────────────────────
-- Используется retreat HP_STALL Stage 3 как escape когда pathfind заблочен
-- squad-collision'ом на rallying-точке (Ogre HP_STALL=676 ticks кейс).
-- TP scroll: Location-target, channel 3.0с, cooldown 80с (+ 80с per charge).
-- Кастуем на ctx.fountain. Если в инвентаре нет / cd / channel прерванный
-- (бой) — просто возвращаем false, caller fall-through к shuffle.
function M.TryUseTPScroll(bot, ctx)
    if not bot or bot:IsNull() then return false, "NO_BOT" end
    local gt = (ctx and ctx.game_time) or 0
    if not ctx or not ctx.fountain then return false, "NO_FOUNTAIN" end

    -- Найти TP scroll или Boots of Travel.
    local item, slot = nil, nil
    for _, name in ipairs({"item_tpscroll", "item_travel_boots", "item_travel_boots_2"}) do
        if bot.FindItemSlot then
            local ok, s = pcall(function() return bot:FindItemSlot(name) end)
            if ok and s and s >= 0 then
                local it = bot:GetItemInSlot(s)
                if it and not it:IsNull() then
                    item, slot = it, s
                    break
                end
            end
        end
    end
    if not item then return false, "NO_TP" end

    local cd = (item.GetCooldownTimeRemaining and item:GetCooldownTimeRemaining()) or 0
    if cd > 0.5 then return false, "TP_CD" end

    -- Hard rate-limit на повторные попытки (если канал прервался — не спамим
    -- кастом каждый тик, ждём 5с перед повтором).
    local lastCast = BotControllerState._last_tp_cast_gt or -999.0
    if (gt - lastCast) < 5.0 then
        return false, "TP_RECENT"
    end

    bot:Action_UseAbilityOnLocation(item, ctx.fountain)
    BotControllerState._last_tp_cast_gt = gt
    print(string.format(
        "[items] TP_SCROLL_CAST slot=%d dst=(%d,%d) hp=%.2f gt=%.1f",
        slot or -1,
        math.floor(ctx.fountain.x or 0), math.floor(ctx.fountain.y or 0),
        ctx.hp_pct or 0, gt))
    return true, "TP_FOUNTAIN"
end

-- ────────────────────────────────────────────────────────────────────
-- Active items dispatcher
-- ────────────────────────────────────────────────────────────────────

-- ── Generic ready-check ──────────────────────────────────────────
-- Для consumable items IsFullyCastable ненадёжен — fallback на charges+cd.
-- Для остальных используем IsFullyCastable если доступен.
local function isItemReady(item, name)
    if not item or item:IsNull() then return false end
    if CONSUMABLE_NAMES[name] then
        local charges = (item.GetCurrentCharges and item:GetCurrentCharges()) or 0
        local cd      = (item.GetCooldownTimeRemaining and item:GetCooldownTimeRemaining()) or 0
        if charges <= 0 then return false end
        if cd > 0.5 then return false end
        return true
    end
    -- Non-consumable: check IsFullyCastable если есть.
    if item.IsFullyCastable then
        local ok = item:IsFullyCastable()
        if ok then return true end
        -- fallback на cd-проверку если IsFullyCastable вернул false (бывает
        -- неконсистентным после reload).
        local cd = (item.GetCooldownTimeRemaining and item:GetCooldownTimeRemaining()) or 0
        return cd <= 0.5
    end
    -- последний fallback
    local cd = (item.GetCooldownTimeRemaining and item:GetCooldownTimeRemaining()) or 0
    return cd <= 0.5
end

-- ── Helpers: cast + anti-ban ──────────────────────────────────────
local function castOnSelf(bot, item, name, gt)
    if not AB.CanCast(name, bot, gt) then return false end
    AB.MarkCast(name, bot, gt)
    bot:Action_UseAbilityOnEntity(item, bot)
    return true
end

local function castNoTarget(bot, item, name, gt)
    if not AB.CanCast(name, nil, gt) then return false end
    AB.MarkCast(name, nil, gt)
    bot:Action_UseAbility(item)
    return true
end

local function castOnEntity(bot, item, target, name, gt)
    if not target or target:IsNull() then return false end
    if not AB.CanCast(name, target, gt) then return false end
    AB.MarkCast(name, target, gt)
    bot:Action_UseAbilityOnEntity(item, target)
    return true
end

local function castOnLocation(bot, item, pos, name, gt)
    if not pos then return false end
    if not AB.CanCast(name, nil, gt) then return false end
    AB.MarkCast(name, nil, gt)
    bot:Action_UseAbilityOnLocation(item, pos)
    return true
end

local function toggleSafe(bot, item, name, gt)
    if not AB.CanCast(name, nil, gt) then return false end
    AB.MarkCast(name, nil, gt)
    bot:Action_ToggleAbility(item)
    return true
end

-- ── Helpers: ctx introspection ───────────────────────────────────
local function safeIsHero(ent)
    if not ent or ent:IsNull() then return false end
    if ent.IsHero then
        local ok, res = pcall(function() return ent:IsHero() end)
        if ok then return res end
    end
    return false
end

-- HP/MaxHP ratio для произвольной entity (target). Возвращает 1.0 если nil.
local function entHpFrac(ent)
    if not ent or ent:IsNull() then return 1.0 end
    local hp, max = 0, 0
    if ent.GetHealth then hp = ent:GetHealth() or 0 end
    if ent.GetMaxHealth then max = ent:GetMaxHealth() or 0 end
    if max <= 0 then return 1.0 end
    return hp / max
end

-- Считает врагов в радиусе r вокруг bot.
local function countEnemiesInRadius(ctx, r)
    local n = 0
    local list = ctx.nearby_enemies or {}
    for _, e in ipairs(list) do
        if e and not e:IsNull() then
            local p = e.GetLocation and e:GetLocation()
            if p and Dist2D(ctx.pos, p) <= r then
                n = n + 1
            end
        end
    end
    return n
end

-- Escape-направление = от nearest enemy (в локальный фрейм).
-- range — длина блинка (max 1200 для Blink Dagger).
local function escapeLocation(bot, ctx, range)
    range = range or 1200
    if not ctx.pos then return nil end
    local enemy = ctx.nearest_enemy
    local ex, ey
    if enemy and not enemy:IsNull() and enemy.GetLocation then
        local ep = enemy:GetLocation()
        if ep then ex, ey = ep.x or 0, ep.y or 0 end
    end
    local dx, dy
    if ex then
        dx = (ctx.pos.x or 0) - ex
        dy = (ctx.pos.y or 0) - ey
    else
        -- нет enemy — блинк назад по последнему движению (fallback в строну (0,0))
        dx = -(ctx.pos.x or 0)
        dy = -(ctx.pos.y or 0)
    end
    local len = math.sqrt(dx * dx + dy * dy)
    if len < 1.0 then
        dx, dy, len = 1.0, 0.0, 1.0
    end
    local nx, ny = dx / len, dy / len
    local dist = math.min(range, 1200)
    return {
        x = (ctx.pos.x or 0) + nx * dist,
        y = (ctx.pos.y or 0) + ny * dist,
        z = ctx.pos.z or 0,
    }
end

-- Toggle stickiness: вспомогательный wrapper для toggle-items, чтобы не
-- щёлкать туда-сюда каждый тик. Хранит boolean в BotControllerState[key].
local function setToggleState(bot, item, name, gt, want_on, throttle)
    BotControllerState[name .. "_toggled"] = BotControllerState[name .. "_toggled"] or false
    local cur = BotControllerState[name .. "_toggled"]
    if cur == want_on then return false end
    -- AB.CanCast уже даёт randomized cooldown — этого достаточно.
    if not AB.CanCast(name, nil, gt) then return false end
    AB.MarkCast(name, nil, gt)
    bot:Action_ToggleAbility(item)
    BotControllerState[name .. "_toggled"] = want_on
    return true
end

-- ── Per-item handlers ─────────────────────────────────────────────
-- Каждый: fn(bot, ctx, fsm, item, slot) → bool (true если касстанули).

M.HANDLERS = {}

-- Magic Stick / Wand: heal+mana on low resources, charges based.
local function magicStickWand(bot, ctx, fsm, item, slot)
    local gt = ctx.game_time or 0
    local charges = (item.GetCurrentCharges and item:GetCurrentCharges()) or 0
    if charges <= 0 then return false end
    if not ((ctx.hp_pct or 1) < 0.55 or (ctx.mana_pct or 1) < 0.4) then
        return false
    end
    if not isItemReady(item, item:GetName() or "") then return false end
    return castNoTarget(bot, item, item:GetName() or "magic_stick_wand", gt)
end
M.HANDLERS.item_magic_stick = magicStickWand
M.HANDLERS.item_magic_wand  = magicStickWand

-- Enchanted Mango: instant +150 mana.
M.HANDLERS.item_enchanted_mango = function(bot, ctx, fsm, item, slot)
    local gt = ctx.game_time or 0
    if (ctx.mana_pct or 1) >= 0.3 then return false end
    if not isItemReady(item, "item_enchanted_mango") then return false end
    return castNoTarget(bot, item, "item_enchanted_mango", gt)
end

-- Faerie Fire: instant +85 hp + small damage. Emergency only.
M.HANDLERS.item_faerie_fire = function(bot, ctx, fsm, item, slot)
    local gt = ctx.game_time or 0
    if (ctx.hp_pct or 1) >= 0.18 then return false end
    if not isItemReady(item, "item_faerie_fire") then return false end
    return castNoTarget(bot, item, "item_faerie_fire", gt)
end

-- Clarity: mana regen, не в бою.
M.HANDLERS.item_clarity = function(bot, ctx, fsm, item, slot)
    local gt = ctx.game_time or 0
    if (ctx.mana_pct or 1) >= 0.3 then return false end
    if (ctx.nearest_enemy_dist or 99999) <= 800 then return false end
    if not isItemReady(item, "item_clarity") then return false end
    return castOnSelf(bot, item, "item_clarity", gt)
end

-- Healing salve: hp regen, не в бою.
M.HANDLERS.item_flask = function(bot, ctx, fsm, item, slot)
    local gt = ctx.game_time or 0
    if (ctx.hp_pct or 1) >= 0.5 then return false end
    if (ctx.nearest_enemy_dist or 99999) <= 800 then return false end
    if not isItemReady(item, "item_flask") then return false end
    return castOnSelf(bot, item, "item_flask", gt)
end

-- Bottle: heal+mana, не в бою.
M.HANDLERS.item_bottle = function(bot, ctx, fsm, item, slot)
    local gt = ctx.game_time or 0
    local charges = (item.GetCurrentCharges and item:GetCurrentCharges()) or 0
    if charges <= 0 then return false end
    if not ((ctx.hp_pct or 1) < 0.7 or (ctx.mana_pct or 1) < 0.5) then return false end
    if (ctx.nearest_enemy_dist or 99999) <= 700 then return false end
    if not isItemReady(item, "item_bottle") then return false end
    return castOnSelf(bot, item, "item_bottle", gt)
end

-- Black King Bar: magic immunity. Combat or retreat panic.
M.HANDLERS.item_black_king_bar = function(bot, ctx, fsm, item, slot)
    local gt = ctx.game_time or 0
    if not isItemReady(item, "item_black_king_bar") then return false end
    local enemy = ctx.nearest_enemy
    local enemyDist = ctx.nearest_enemy_dist or 99999
    -- Retreat panic
    if fsm == "RETREAT" and enemy and safeIsHero(enemy) and enemyDist < 700 then
        return castNoTarget(bot, item, "item_black_king_bar", gt)
    end
    -- Crit hp + enemy hero close
    if (ctx.hp_pct or 1) < 0.35 and enemy and safeIsHero(enemy) and enemyDist < 1000 then
        return castNoTarget(bot, item, "item_black_king_bar", gt)
    end
    -- Combat engage
    if enemy and safeIsHero(enemy) and enemyDist < 600 and (ctx.hp_pct or 1) < 0.7 then
        return castNoTarget(bot, item, "item_black_king_bar", gt)
    end
    return false
end

-- Blink: escape on retreat OR engage on combat.
local function blinkHandler(bot, ctx, fsm, item, slot)
    local gt = ctx.game_time or 0
    if not isItemReady(item, "item_blink") then return false end
    local enemy = ctx.nearest_enemy
    local enemyDist = ctx.nearest_enemy_dist or 99999
    -- Retreat: blink away from enemy
    if fsm == "RETREAT" then
        local pos = escapeLocation(bot, ctx, 1200)
        if not pos then return false end
        return castOnLocation(bot, item, pos, "item_blink", gt)
    end
    -- Engage: gap-close на ослабленного героя
    if enemy and safeIsHero(enemy) and enemyDist >= 1000 and enemyDist <= 1200
       and (ctx.hp_pct or 1) > 0.6 and entHpFrac(enemy) < 0.6 then
        if enemy.GetLocation then
            local ep = enemy:GetLocation()
            if ep then
                return castOnLocation(bot, item, ep, "item_blink", gt)
            end
        end
    end
    return false
end
M.HANDLERS.item_blink        = blinkHandler
M.HANDLERS.item_blink_dagger = blinkHandler

-- Force Staff: self-push при close enemy.
M.HANDLERS.item_force_staff = function(bot, ctx, fsm, item, slot)
    local gt = ctx.game_time or 0
    if not isItemReady(item, "item_force_staff") then return false end
    if (ctx.hp_pct or 1) >= 0.4 then return false end
    if (ctx.nearest_enemy_dist or 99999) >= 600 then return false end
    return castOnSelf(bot, item, "item_force_staff", gt)
end

-- Glimmer Cape: stealth on retreat.
M.HANDLERS.item_glimmer_cape = function(bot, ctx, fsm, item, slot)
    local gt = ctx.game_time or 0
    if not isItemReady(item, "item_glimmer_cape") then return false end
    if fsm ~= "RETREAT" then return false end
    if (ctx.hp_pct or 1) >= 0.5 then return false end
    if (ctx.nearest_enemy_dist or 99999) >= 800 then return false end
    return castOnSelf(bot, item, "item_glimmer_cape", gt)
end

-- Eul / Cyclone: target chasing enemy hero, disable + vuln self.
local function eulHandler(bot, ctx, fsm, item, slot)
    local gt = ctx.game_time or 0
    local enemy = ctx.nearest_enemy
    if not enemy or enemy:IsNull() or not safeIsHero(enemy) then return false end
    if (ctx.nearest_enemy_dist or 99999) >= 575 then return false end
    if not isItemReady(item, "item_eul") then return false end
    return castOnEntity(bot, item, enemy, "item_eul", gt)
end
M.HANDLERS.item_cyclone = eulHandler
M.HANDLERS.item_eul     = eulHandler

-- Manta Style: dispel slow/silence/root on retreat (heuristic).
M.HANDLERS.item_manta = function(bot, ctx, fsm, item, slot)
    local gt = ctx.game_time or 0
    if not isItemReady(item, "item_manta") then return false end
    if fsm ~= "RETREAT" then return false end
    if (ctx.hp_pct or 1) >= 0.4 then return false end
    if (ctx.nearest_enemy_dist or 99999) >= 800 then return false end
    return castNoTarget(bot, item, "item_manta", gt)
end

-- Ethereal Blade: damage amp + ethereal target.
M.HANDLERS.item_ethereal_blade = function(bot, ctx, fsm, item, slot)
    local gt = ctx.game_time or 0
    local enemy = ctx.nearest_enemy
    if not enemy or enemy:IsNull() or not safeIsHero(enemy) then return false end
    if (ctx.nearest_enemy_dist or 99999) >= 700 then return false end
    if not isItemReady(item, "item_ethereal_blade") then return false end
    return castOnEntity(bot, item, enemy, "item_ethereal_blade", gt)
end

-- Dagon: nuke. Все levels одинаковая логика.
local function dagonHandler(bot, ctx, fsm, item, slot)
    local gt = ctx.game_time or 0
    local enemy = ctx.nearest_enemy
    if not enemy or enemy:IsNull() or not safeIsHero(enemy) then return false end
    if (ctx.nearest_enemy_dist or 99999) >= 700 then return false end
    if entHpFrac(enemy) >= 0.5 then return false end
    local name = (item.GetName and item:GetName()) or "item_dagon"
    if not isItemReady(item, name) then return false end
    return castOnEntity(bot, item, enemy, name, gt)
end
M.HANDLERS.item_dagon   = dagonHandler
M.HANDLERS.item_dagon_1 = dagonHandler
M.HANDLERS.item_dagon_2 = dagonHandler
M.HANDLERS.item_dagon_3 = dagonHandler
M.HANDLERS.item_dagon_4 = dagonHandler
M.HANDLERS.item_dagon_5 = dagonHandler

-- Sheep Stick: hex.
M.HANDLERS.item_sheepstick = function(bot, ctx, fsm, item, slot)
    local gt = ctx.game_time or 0
    local enemy = ctx.nearest_enemy
    if not enemy or enemy:IsNull() or not safeIsHero(enemy) then return false end
    if (ctx.nearest_enemy_dist or 99999) >= 800 then return false end
    if entHpFrac(enemy) >= 0.5 then return false end
    if not isItemReady(item, "item_sheepstick") then return false end
    return castOnEntity(bot, item, enemy, "item_sheepstick", gt)
end
-- Также под именем item_scythe_of_vyse (старое имя но в коде иногда встречается).
M.HANDLERS.item_scythe_of_vyse = M.HANDLERS.item_sheepstick

-- Orchid / Bloodthorn: silence + amp.
local function orchidHandler(bot, ctx, fsm, item, slot)
    local gt = ctx.game_time or 0
    local enemy = ctx.nearest_enemy
    if not enemy or enemy:IsNull() or not safeIsHero(enemy) then return false end
    if (ctx.nearest_enemy_dist or 99999) >= 900 then return false end
    local name = (item.GetName and item:GetName()) or "item_orchid"
    if not isItemReady(item, name) then return false end
    return castOnEntity(bot, item, enemy, name, gt)
end
M.HANDLERS.item_orchid_malevolence = orchidHandler
M.HANDLERS.item_orchid             = orchidHandler
M.HANDLERS.item_bloodthorn         = orchidHandler

-- Heaven's Halberd: maim+disarm на melee.
M.HANDLERS.item_heavens_halberd = function(bot, ctx, fsm, item, slot)
    local gt = ctx.game_time or 0
    local enemy = ctx.nearest_enemy
    if not enemy or enemy:IsNull() or not safeIsHero(enemy) then return false end
    if (ctx.nearest_enemy_dist or 99999) >= 600 then return false end
    -- skip ranged: предпочтение melee. Heuristic: IsMelee если binding есть.
    if enemy.IsRanged then
        local ok, isr = pcall(function() return enemy:IsRanged() end)
        if ok and isr then return false end
    end
    if not isItemReady(item, "item_heavens_halberd") then return false end
    return castOnEntity(bot, item, enemy, "item_heavens_halberd", gt)
end
M.HANDLERS.item_heaven_halberd = M.HANDLERS.item_heavens_halberd

-- Shiva's Guard: AoE freeze.
M.HANDLERS.item_shivas_guard = function(bot, ctx, fsm, item, slot)
    local gt = ctx.game_time or 0
    if countEnemiesInRadius(ctx, 800) < 2 then return false end
    if not isItemReady(item, "item_shivas_guard") then return false end
    return castNoTarget(bot, item, "item_shivas_guard", gt)
end

-- Lotus Orb: reflect spells.
M.HANDLERS.item_lotus_orb = function(bot, ctx, fsm, item, slot)
    local gt = ctx.game_time or 0
    local enemy = ctx.nearest_enemy
    if not enemy or enemy:IsNull() or not safeIsHero(enemy) then return false end
    if (ctx.nearest_enemy_dist or 99999) >= 800 then return false end
    if (ctx.hp_pct or 1) >= 0.6 then return false end
    if not isItemReady(item, "item_lotus_orb") then return false end
    return castOnSelf(bot, item, "item_lotus_orb", gt)
end

-- Solar Crest: armor debuff на nearest enemy hero.
M.HANDLERS.item_solar_crest = function(bot, ctx, fsm, item, slot)
    local gt = ctx.game_time or 0
    local enemy = ctx.nearest_enemy
    if not enemy or enemy:IsNull() or not safeIsHero(enemy) then return false end
    if (ctx.nearest_enemy_dist or 99999) >= 1000 then return false end
    if not isItemReady(item, "item_solar_crest") then return false end
    return castOnEntity(bot, item, enemy, "item_solar_crest", gt)
end

-- Satanic: lifesteal heal.
M.HANDLERS.item_satanic = function(bot, ctx, fsm, item, slot)
    local gt = ctx.game_time or 0
    if (ctx.hp_pct or 1) >= 0.25 then return false end
    if (ctx.nearest_enemy_dist or 99999) >= 700 then return false end
    if not isItemReady(item, "item_satanic") then return false end
    return castNoTarget(bot, item, "item_satanic", gt)
end

-- Meteor Hammer: POINT_AOE channel 2.0с, AbilityCastRange=600. Используем как
-- tower-pusher: бот-фармер бьёт ближайшую enemy-tower когда нет угрозы (нет
-- вражеских героев в 1200, мало крипов в 600 — иначе наша channel-pose
-- легко прервётся). Учим именно на tower'е чтобы кастовать point_target —
-- пригодится в будущем для AoE-применения по героям.
M.HANDLERS.item_meteor_hammer = function(bot, ctx, fsm, item, slot)
    local gt = ctx.game_time or 0

    -- 1. Cooldown / mana ready
    if not isItemReady(item, "item_meteor_hammer") then return false end

    -- 2. HP-комфорт (channel 2с — нельзя быть на грани смерти)
    if (ctx.hp_pct or 1) <= 0.5 then return false end

    -- 3. Нет вражеских героев в 1200 (channel прерывается уроном)
    local nearest_enemy_dist = ctx.nearest_enemy_dist or 99999
    local enemy = ctx.nearest_enemy
    if enemy and not enemy:IsNull() and safeIsHero(enemy) and nearest_enemy_dist < 1200 then
        return false
    end

    -- 4. Меньше 2 крипов в 600 (бот не на лайне в раздаче)
    if countEnemiesInRadius(ctx, 600) >= 2 then return false end

    -- 5. Найти ближайшую вражескую tower в 500 unit от бота с hp_pct < 0.95.
    local towers = ctx.nearby_towers or {}
    local bestTower, bestDist = nil, 99999
    for _, tw in ipairs(towers) do
        if tw and not tw:IsNull() and tw.IsAlive and tw:IsAlive() then
            local twLoc = tw.GetLocation and tw:GetLocation()
            if twLoc then
                local d = Dist2D(ctx.pos, twLoc)
                if d <= 500 and d < bestDist then
                    -- HP-фильтр: tower hp_pct < 0.95 (полная башня бьётся 5 минут,
                    -- бьём только повреждённую — экономим заряд meteor cooldown 24с).
                    local hpFrac = entHpFrac(tw)
                    if hpFrac < 0.95 then
                        bestTower, bestDist = tw, d
                    end
                end
            end
        end
    end

    if not bestTower then return false end

    local twLoc = bestTower:GetLocation()
    if not twLoc then return false end

    return castOnLocation(bot, item, twLoc, "item_meteor_hammer", gt)
end

-- Refresher Orb: reset cooldowns.
M.HANDLERS.item_refresher = function(bot, ctx, fsm, item, slot)
    local gt = ctx.game_time or 0
    if (ctx.hp_pct or 1) <= 0.7 then return false end
    if (ctx.mana_pct or 1) <= 0.4 then return false end
    if fsm ~= "LANE_FARM" and fsm ~= "PUSH" then return false end
    local enemy = ctx.nearest_enemy
    if not enemy or not safeIsHero(enemy) then return false end
    if (ctx.nearest_enemy_dist or 99999) >= 1500 then return false end
    if not isItemReady(item, "item_refresher") then return false end
    return castNoTarget(bot, item, "item_refresher", gt)
end

-- ── Toggle items ─────────────────────────────────────────────────

-- Armlet: toggle ON в combat когда HP средний, OFF при критическом HP.
local function armletHandler(bot, ctx, fsm, item, slot)
    local gt = ctx.game_time or 0
    local enemy = ctx.nearest_enemy
    local enemyDist = ctx.nearest_enemy_dist or 99999
    local hpFrac = ctx.hp_pct or 1.0
    local cur = BotControllerState.item_armlet_of_mordiggian_toggled or false
    local want
    if hpFrac < 0.3 then
        want = false  -- армлет жрёт, при низком HP убираем
    elseif enemy and safeIsHero(enemy) and enemyDist < 600 and hpFrac >= 0.5 then
        want = true
    else
        want = cur  -- держим текущее состояние
    end
    if want == cur then return false end
    return setToggleState(bot, item, "item_armlet_of_mordiggian", gt, want)
end
M.HANDLERS.item_armlet                 = armletHandler
M.HANDLERS.item_armlet_of_mordiggian   = armletHandler

-- Mask of Madness: toggle ON для farm/combat, OFF при retreat.
M.HANDLERS.item_mask_of_madness = function(bot, ctx, fsm, item, slot)
    local gt = ctx.game_time or 0
    local cur = BotControllerState.item_mask_of_madness_toggled or false
    local enemy = ctx.nearest_enemy
    local enemyDist = ctx.nearest_enemy_dist or 99999
    local hpFrac = ctx.hp_pct or 1.0
    local want = false
    if fsm == "RETREAT" then
        want = false
    elseif fsm == "JUNGLE_FARM" then
        local n = ctx.neutrals and #ctx.neutrals or 0
        if n > 0 then want = true end
    elseif fsm == "LANE_FARM" then
        if enemy and safeIsHero(enemy) and enemyDist < 1200 and hpFrac > 0.5 then
            want = true
        end
    elseif fsm == "PUSH" then
        want = true
    end
    if want == cur then return false end
    return setToggleState(bot, item, "item_mask_of_madness", gt, want)
end

-- Phase Boots: toggle ON при движении к/от противника.
M.HANDLERS.item_phase_boots = function(bot, ctx, fsm, item, slot)
    local gt = ctx.game_time or 0
    if not isItemReady(item, "item_phase_boots") then return false end
    -- AB.CanCast уже даёт randomized cooldown, доп throttle не нужен — но
    -- не активируем чаще чем раз в (cast_cooldown_min..max) сек. Это покрывает
    -- 4-6с throttle без отдельной переменной.
    local enemyDist = ctx.nearest_enemy_dist or 99999
    if not (fsm == "LANE_FARM" or fsm == "RETREAT" or fsm == "JUNGLE_FARM" or fsm == "PUSH") then
        return false
    end
    if enemyDist <= 100 or enemyDist >= 1500 then return false end
    -- Phase — stateless active (не "toggle" в стрикт-смысле, ON-only). Просто
    -- кастуем no-target через AB cooldown.
    return castNoTarget(bot, item, "item_phase_boots", gt)
end

-- ── Public API ─────────────────────────────────────────────────────────────

-- Iterates main inventory (slots 0..5), при первом успешном касте останавливается.
-- Возвращает (used:bool, name:string|nil).
function M.UseActiveItems(bot, ctx, fsm_state)
    if not bot or bot:IsNull() then return false, nil end
    if bot.IsAlive then
        local ok, alive = pcall(function() return bot:IsAlive() end)
        if ok and not alive then return false, nil end
    end
    if not ctx then return false, nil end

    for slot = 0, 5 do
        local item = nil
        if bot.GetItemInSlot then
            local ok, ret = pcall(function() return bot:GetItemInSlot(slot) end)
            if ok then item = ret end
        end
        if item and not item:IsNull() then
            local name = ""
            if item.GetName then
                local ok, n = pcall(function() return item:GetName() end)
                if ok and n then name = n end
            end
            local handler = M.HANDLERS[name]
            if handler then
                local ok, used = pcall(handler, bot, ctx, fsm_state, item, slot)
                if ok and used then
                    print(string.format("[items] USED %s slot=%d fsm=%s",
                        name, slot, tostring(fsm_state)))
                    return true, name
                elseif not ok then
                    print(string.format("[items] handler error %s: %s",
                        name, tostring(used)))
                end
            end
        end
    end
    return false, nil
end

return M
