-- heroes/lion.lua — per-hero override для Lion.
-- Способности: Earth Spike (Q, slot 0), Hex (W, slot 1),
-- Mana Drain (E, slot 2 — channel, скип), Finger of Death (R, slot 5).

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

local function tryEarthSpike(bot, ctx)
    if ctx.hp_pct <= 0.4 then return nil end
    local ab = bot:GetAbilityInSlot(0)
    if not ab or ab:IsNull() or ab:GetLevel() == 0 or not ab:IsFullyCastable() then return nil end
    local name = ab:GetName() or "lion_impale"

    -- Hero priority.
    local target = ctx.nearest_enemy
    if target and not target:IsNull() and target:IsAlive() and target:IsHero()
       and not IsMagicImmuneSafe(target)
       and ctx.nearest_enemy_dist <= 700 then
        if not AB.CanCast(name, target, ctx.game_time) then return nil end
        AB.MarkCast(name, target, ctx.game_time)
        bot:Action_UseAbilityOnLocation(ab, target:GetLocation())
        return "EARTH_SPIKE"
    end

    -- Jungle fallback: line POINT по centroid пачки нейтралов
    -- (Earth Spike line ширина 125, проходит сквозь крипов кампа).
    if Combat.JungleCastAllowed(ctx, 0.5) then
        local ncentr, ncount = Combat.NeutralsCentroidInRadius(ctx, 700)
        if ncentr and ncount >= 2 then
            if not AB.CanCast(name, bot, ctx.game_time) then return nil end
            AB.MarkCast(name, bot, ctx.game_time)
            bot:Action_UseAbilityOnLocation(ab, ncentr)
            return "EARTH_SPIKE_NEUT"
        end
    end

    return nil
end

local function tryHex(bot, ctx)
    if ctx.hp_pct <= 0.4 then return nil end
    local target = ctx.nearest_enemy
    if not target or target:IsNull() or not target:IsAlive() then return nil end
    if not target:IsHero() then return nil end
    if IsMagicImmuneSafe(target) then return nil end
    if ctx.nearest_enemy_dist > 700 then return nil end
    local hp, mx = target:GetHealth(), target:GetMaxHealth()
    local pct = (mx > 0) and (hp / mx) or 1
    if pct <= 0.2 then return nil end

    local ab = bot:GetAbilityInSlot(1)
    if not ab or ab:IsNull() or ab:GetLevel() == 0 or not ab:IsFullyCastable() then return nil end
    local name = ab:GetName() or "lion_voodoo"
    if not AB.CanCast(name, target, ctx.game_time) then return nil end
    AB.MarkCast(name, target, ctx.game_time)
    bot:Action_UseAbilityOnEntity(ab, target)
    return "HEX"
end

-- Mana Drain — channel, skip.

local function tryFingerOfDeath(bot, ctx)
    -- Defensive-ok: можно ulteать даже на низком HP (это finisher).
    local target = ctx.nearest_enemy
    if not target or target:IsNull() or not target:IsAlive() then return nil end
    if not target:IsHero() then return nil end
    if IsMagicImmuneSafe(target) then return nil end
    if ctx.nearest_enemy_dist > 700 then return nil end
    local hp, mx = target:GetHealth(), target:GetMaxHealth()
    local pct = (mx > 0) and (hp / mx) or 1
    if pct >= 0.4 then return nil end

    local ab = bot:GetAbilityInSlot(5)
    if not ab or ab:IsNull() or ab:GetLevel() == 0 or not ab:IsFullyCastable() then return nil end
    local name = ab:GetName() or "lion_finger_of_death"
    if not AB.CanCast(name, target, ctx.game_time) then return nil end
    AB.MarkCast(name, target, ctx.game_time)
    bot:Action_UseAbilityOnEntity(ab, target)
    return "FINGER"
end

function M.Think(bot, ctx)
    if not bot or bot:IsNull() or not bot:IsAlive() then return nil end
    local r
    r = tryFingerOfDeath(bot, ctx); if r then return "LION:" .. r end  -- finisher first
    r = tryHex(bot, ctx);           if r then return "LION:" .. r end  -- setup ult
    r = tryEarthSpike(bot, ctx);    if r then return "LION:" .. r end
    return nil
end

return M
