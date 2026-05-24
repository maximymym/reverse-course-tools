-- heroes/viper.lua — per-hero override для Viper.
-- Способности: Poison Attack (Q, slot 0, ORB/AUTOCAST),
-- Nethertoxin (W, slot 1, POINT_AOE),
-- Corrosive Skin (E, slot 2, PASSIVE — скип),
-- Nose Dive (slot 3, innate, обычно hidden — скип / escape try),
-- Predator (slot 4, PASSIVE — скип),
-- Viper Strike (R, slot 5, UNIT_TARGET ULT).

local M = {}

local AB     = require("util.anti_ban")
local Move   = require("util.movement")
local Combat = require("util.combat")

local function IsMagicImmuneSafe(t)
    if not t then return false end
    local ok, v = pcall(function() return t:IsMagicImmune() end)
    if ok then return v and true or false end
    return false
end

local function WeakestAlive(list)
    local best, bestPct = nil, 1.01
    if not list then return nil end
    for _, h in ipairs(list) do
        if h and not h:IsNull() and h:IsAlive() then
            local hp, mx = h:GetHealth(), h:GetMaxHealth()
            local pct = (mx > 0) and (hp / mx) or 1
            if pct < bestPct then
                best, bestPct = h, pct
            end
        end
    end
    return best
end

local function Centroid(list, fromPos, radius)
    local sx, sy, sz, n = 0, 0, 0, 0
    if not list then return nil end
    for _, c in ipairs(list) do
        if c and not c:IsNull() and c:IsAlive() then
            local p = c:GetLocation()
            if p then
                local d = Move.Dist2D(fromPos, p)
                if d <= radius then
                    sx = sx + (p.x or 0)
                    sy = sy + (p.y or 0)
                    sz = sz + (p.z or 0)
                    n  = n + 1
                end
            end
        end
    end
    if n == 0 then return nil end
    return { x = sx / n, y = sy / n, z = sz / n }, n
end

-- Poison Attack — ORB / AUTOCAST. Включается централизованно через
-- Combat.EnsureAutocasts (combat.lua:232) по AbilityBehavior bit AUTOCAST,
-- общий sticky-словарь BotControllerState.autocast_set + re-verify через
-- GetAutoCastState. Локальный tryPoisonAttack удалён — он флипал orb каждые
-- 130ms из-за конфликта собственного viper_orb_on flag с autocast_set.

-- Nethertoxin — POINT_AOE. Кастуем когда ≥1 враг ИЛИ ≥3 крипа в радиусе 600.
local function tryNethertoxin(bot, ctx)
    if (ctx.hp_pct or 0) <= 0.4 then return nil end
    local ab = bot:GetAbilityInSlot(1)
    if not ab or ab:IsNull() or ab:GetLevel() == 0 or not ab:IsFullyCastable() then return nil end

    -- Подсчёт целей в 600.
    local enemies_close = 0
    local nearest_enemy = nil
    local nearest_dist  = 99999
    for _, h in ipairs(ctx.nearby_enemies or {}) do
        if h and not h:IsNull() and h:IsAlive() and not IsMagicImmuneSafe(h) then
            local d = Move.Dist2D(ctx.pos, h:GetLocation())
            if d <= 600 then enemies_close = enemies_close + 1 end
            if d < nearest_dist then nearest_dist, nearest_enemy = d, h end
        end
    end

    local creeps_close = 0
    for _, c in ipairs(ctx.nearby_creeps or {}) do
        if c and not c:IsNull() and c:IsAlive() then
            local d = Move.Dist2D(ctx.pos, c:GetLocation())
            if d <= 600 then creeps_close = creeps_close + 1 end
        end
    end

    -- Точка каста: heroes / lane creeps / neutrals (последнее — opt-in jungle).
    local cast_loc = nil
    if enemies_close >= 1 or creeps_close >= 3 then
        if nearest_enemy and nearest_dist < 900 then
            cast_loc = nearest_enemy:GetLocation()
        else
            local centroid = Centroid(ctx.nearby_creeps, ctx.pos, 600)
            if centroid then
                local d = Move.Dist2D(ctx.pos, centroid)
                if d < 900 then cast_loc = centroid end
            end
        end
    elseif Combat.JungleCastAllowed(ctx, 0.5) then
        -- Jungle path: нет ни heroes, ни lane creeps, но есть нейтралы.
        local ncentr, ncount = Combat.NeutralsCentroidInRadius(ctx, 600)
        if ncentr and ncount >= 2 then
            local d = Move.Dist2D(ctx.pos, ncentr)
            if d < 900 then cast_loc = ncentr end
        end
    end
    if not cast_loc then return nil end

    local name = ab:GetName() or "viper_nethertoxin"
    local key_target = nearest_enemy or bot
    if not AB.CanCast(name, key_target, ctx.game_time) then return nil end
    AB.MarkCast(name, key_target, ctx.game_time)
    bot:Action_UseAbilityOnLocation(ab, cast_loc)
    return "NETHERTOXIN"
end

-- Nose Dive — escape try при низком HP. Если innate hidden → IsFullyCastable=false,
-- pcall защитит от ошибок binding'а.
local function tryNoseDive(bot, ctx)
    if (ctx.hp_pct or 1) >= 0.3 then return nil end
    local ab = bot:GetAbilityInSlot(3)
    if not ab or ab:IsNull() then return nil end
    local lvl_ok, lvl = pcall(ab.GetLevel, ab)
    if not lvl_ok or (lvl or 0) == 0 then return nil end
    local cast_ok, castable = pcall(ab.IsFullyCastable, ab)
    if not cast_ok or not castable then return nil end
    local name = ab:GetName() or "viper_nose_dive"
    if not AB.CanCast(name, bot, ctx.game_time) then return nil end
    -- Точка отступа: к фонтану или safe_pos.
    local escape_loc = ctx.safe_pos or ctx.fountain
    if not escape_loc then return nil end
    AB.MarkCast(name, bot, ctx.game_time)
    local ok = pcall(bot.Action_UseAbilityOnLocation, bot, ab, escape_loc)
    if not ok then return nil end
    return "NOSE_DIVE"
end

-- Viper Strike — UNIT_TARGET ULT. Бот hp_pct>0.5, цель hp_pct<0.6, dist<800.
local function tryViperStrike(bot, ctx)
    if (ctx.hp_pct or 0) <= 0.5 then return nil end
    local ab = bot:GetAbilityInSlot(5)
    if not ab or ab:IsNull() or ab:GetLevel() == 0 or not ab:IsFullyCastable() then return nil end

    -- Лучшая hero-цель: weakest hp_pct < 0.6 в 800.
    local best, bestPct = nil, 0.61
    for _, h in ipairs(ctx.nearby_enemies or {}) do
        if h and not h:IsNull() and h:IsAlive() and h:IsHero() and not IsMagicImmuneSafe(h) then
            local d = Move.Dist2D(ctx.pos, h:GetLocation())
            if d < 800 then
                local hp, mx = h:GetHealth(), h:GetMaxHealth()
                local pct = (mx > 0) and (hp / mx) or 1
                if pct < bestPct then best, bestPct = h, pct end
            end
        end
    end
    if not best then return nil end
    local name = ab:GetName() or "viper_viper_strike"
    if not AB.CanCast(name, best, ctx.game_time) then return nil end
    AB.MarkCast(name, best, ctx.game_time)
    bot:Action_UseAbilityOnEntity(ab, best)
    return "VIPER_STRIKE"
end

function M.Think(bot, ctx)
    if not bot or bot:IsNull() or not bot:IsAlive() then return nil end
    -- Poison Attack autocast управляется централизованно — см. util/combat.lua
    -- Combat.EnsureAutocasts. Здесь только activeable способности.
    local r
    r = tryViperStrike(bot, ctx);  if r then return "VIPER:" .. r end
    r = tryNethertoxin(bot, ctx);  if r then return "VIPER:" .. r end
    r = tryNoseDive(bot, ctx);     if r then return "VIPER:" .. r end
    return nil
end

-- Сброс per-match (вызывается bot_controller на game_state reset, если он
-- знает hero modules — backward compat: nil-safe, не обязателен).
function M.Reset()
    -- Локальные sticky-флаги Viper'а удалены — autocast управляется
    -- централизованным Combat.EnsureAutocasts через BotControllerState.autocast_set,
    -- который сбрасывается на match boundary в bot_controller.
end

return M
