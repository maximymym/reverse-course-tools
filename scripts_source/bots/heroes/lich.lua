-- heroes/lich.lua — per-hero override для Lich.
-- Способности: Frost Blast (Q, slot 0), Sinister Gaze (W, slot 1),
-- Frost Shield (E, slot 2), Chain Frost (R, slot 5).

local M = {}

local AB     = require("util.anti_ban")
local Move   = require("util.movement")
local Combat = require("util.combat")

-- Безопасная проверка magic immunity. У некоторых entity types метод может
-- отсутствовать → pcall с дефолтом false (consider не immune).
local function IsMagicImmuneSafe(t)
    if not t then return false end
    local ok, v = pcall(function() return t:IsMagicImmune() end)
    if ok then return v and true or false end
    return false
end

-- Найти "слабейшего живого" среди nearby_enemies (наименьший hp_pct).
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

local function tryFrostBlast(bot, ctx)
    if ctx.hp_pct <= 0.4 then return nil end
    local ab = bot:GetAbilityInSlot(0)
    if not ab or ab:IsNull() or ab:GetLevel() == 0 or not ab:IsFullyCastable() then return nil end
    local name = ab:GetName() or "lich_frost_blast"

    -- Hero priority.
    local target = nil
    if ctx.nearest_enemy and not ctx.nearest_enemy:IsNull() and ctx.nearest_enemy:IsAlive() then
        local hero = WeakestAlive(ctx.nearby_enemies) or ctx.nearest_enemy
        if hero and not IsMagicImmuneSafe(hero) then
            local d = Move.Dist2D(ctx.pos, hero:GetLocation())
            if d <= 800 then target = hero end
        end
    end

    -- Jungle fallback: бот в JUNGLE_FARM, нет вражеских героев → крепкий нейтрал.
    if not target and Combat.JungleCastAllowed(ctx, 0.5) then
        target = Combat.StrongestNeutralInRadius(ctx, 800)
    end

    if not target or target:IsNull() or not target:IsAlive() then return nil end
    if IsMagicImmuneSafe(target) then return nil end
    if not AB.CanCast(name, target, ctx.game_time) then return nil end
    AB.MarkCast(name, target, ctx.game_time)
    bot:Action_UseAbilityOnEntity(ab, target)
    return "FROSTBLAST"
end

local function trySinisterGaze(bot, ctx)
    if ctx.hp_pct <= 0.4 then return nil end
    local target = ctx.nearest_enemy
    if not target or target:IsNull() or not target:IsAlive() then return nil end
    if IsMagicImmuneSafe(target) then return nil end
    if ctx.nearest_enemy_dist > 700 then return nil end
    local hp, mx = target:GetHealth(), target:GetMaxHealth()
    local pct = (mx > 0) and (hp / mx) or 1
    if pct <= 0.3 then return nil end
    local ab = bot:GetAbilityInSlot(1)
    if not ab or ab:IsNull() or ab:GetLevel() == 0 or not ab:IsFullyCastable() then return nil end
    local name = ab:GetName() or "lich_sinister_gaze"
    if not AB.CanCast(name, target, ctx.game_time) then return nil end
    AB.MarkCast(name, target, ctx.game_time)
    bot:Action_UseAbilityOnEntity(ab, target)
    return "SINISTER_GAZE"
end

local function tryFrostShield(bot, ctx)
    -- Defensive: каст даже на низком HP (это suvival cooldown).
    if ctx.hp_pct >= 0.6 then return nil end
    if ctx.nearest_enemy_dist > 800 then return nil end
    local ab = bot:GetAbilityInSlot(2)
    if not ab or ab:IsNull() or ab:GetLevel() == 0 or not ab:IsFullyCastable() then return nil end
    local name = ab:GetName() or "lich_frost_shield"
    if not AB.CanCast(name, bot, ctx.game_time) then return nil end
    AB.MarkCast(name, bot, ctx.game_time)
    bot:Action_UseAbilityOnEntity(ab, bot)
    return "FROST_SHIELD"
end

local function tryChainFrost(bot, ctx)
    if ctx.hp_pct <= 0.5 then return nil end
    local target = ctx.nearest_enemy
    if not target or target:IsNull() or not target:IsAlive() then return nil end
    if IsMagicImmuneSafe(target) then return nil end
    if ctx.nearest_enemy_dist > 900 then return nil end
    -- Нужны жертвы для bounce
    local alive = 0
    for _, h in ipairs(ctx.nearby_enemies or {}) do
        if h and not h:IsNull() and h:IsAlive() then alive = alive + 1 end
    end
    if alive < 2 then return nil end
    local ab = bot:GetAbilityInSlot(5)
    if not ab or ab:IsNull() or ab:GetLevel() == 0 or not ab:IsFullyCastable() then return nil end
    local name = ab:GetName() or "lich_chain_frost"
    if not AB.CanCast(name, target, ctx.game_time) then return nil end
    AB.MarkCast(name, target, ctx.game_time)
    bot:Action_UseAbilityOnEntity(ab, target)
    return "CHAIN_FROST"
end

function M.Think(bot, ctx)
    if not bot or bot:IsNull() or not bot:IsAlive() then return nil end
    local r
    r = tryFrostBlast(bot, ctx);   if r then return "LICH:" .. r end
    r = trySinisterGaze(bot, ctx); if r then return "LICH:" .. r end
    r = tryFrostShield(bot, ctx);  if r then return "LICH:" .. r end
    r = tryChainFrost(bot, ctx);   if r then return "LICH:" .. r end
    return nil
end

return M
