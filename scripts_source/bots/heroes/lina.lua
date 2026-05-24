-- heroes/lina.lua — per-hero override для Lina.
-- Способности: Dragon Slave (Q, slot 0), Light Strike Array (W, slot 1),
-- Fiery Soul (E, slot 2 — PASSIVE, скип), Laguna Blade (R, slot 5).

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

-- Lead-prediction по target velocity (для line/AoE cast'ов).
local function PredictLocation(target, lead_seconds)
    local p = target:GetLocation()
    local ok, vel = pcall(function() return target:GetVelocity() end)
    if ok and vel then
        return Vector(p.x + (vel.x or 0) * lead_seconds,
                      p.y + (vel.y or 0) * lead_seconds,
                      p.z)
    end
    return p
end

local function CountCreepsForward(ctx)
    local n = 0
    for _, c in ipairs(ctx.nearby_creeps or {}) do
        if c and not c:IsNull() and c:IsAlive() then
            local d = Move.Dist2D(ctx.pos, c:GetLocation())
            if d < 800 then n = n + 1 end
        end
    end
    return n
end

local function tryDragonSlave(bot, ctx)
    if ctx.hp_pct <= 0.4 then return nil end
    local cast_loc = nil
    local target = ctx.nearest_enemy
    local creeps_n = CountCreepsForward(ctx)

    if target and not target:IsNull() and target:IsAlive() and target:IsHero()
       and not IsMagicImmuneSafe(target)
       and ctx.nearest_enemy_dist >= 200 and ctx.nearest_enemy_dist <= 1100 then
        cast_loc = PredictLocation(target, 0.4)
    elseif creeps_n >= 3 and ctx.lane_front then
        cast_loc = ctx.lane_front
    elseif Combat.JungleCastAllowed(ctx, 0.5) then
        -- Jungle path: line-AOE по centroid пачки нейтралов.
        local ncentr, ncount = Combat.NeutralsCentroidInRadius(ctx, 800)
        if ncentr and ncount >= 2 then cast_loc = ncentr end
    end

    if not cast_loc then return nil end
    local ab = bot:GetAbilityInSlot(0)
    if not ab or ab:IsNull() or ab:GetLevel() == 0 or not ab:IsFullyCastable() then return nil end
    local name = ab:GetName() or "lina_dragon_slave"
    if not AB.CanCast(name, target or bot, ctx.game_time) then return nil end
    AB.MarkCast(name, target or bot, ctx.game_time)
    bot:Action_UseAbilityOnLocation(ab, cast_loc)
    return "DRAGON_SLAVE"
end

local function tryLightStrikeArray(bot, ctx)
    if ctx.hp_pct <= 0.4 then return nil end
    local target = ctx.nearest_enemy
    if not target or target:IsNull() or not target:IsAlive() then return nil end
    if not target:IsHero() then return nil end
    if IsMagicImmuneSafe(target) then return nil end
    if ctx.nearest_enemy_dist > 600 then return nil end

    -- LSA: cast point + delay ~0.6s — даём упреждение.
    local cast_loc = PredictLocation(target, 0.6)

    local ab = bot:GetAbilityInSlot(1)
    if not ab or ab:IsNull() or ab:GetLevel() == 0 or not ab:IsFullyCastable() then return nil end
    local name = ab:GetName() or "lina_light_strike_array"
    if not AB.CanCast(name, target, ctx.game_time) then return nil end
    AB.MarkCast(name, target, ctx.game_time)
    bot:Action_UseAbilityOnLocation(ab, cast_loc)
    return "LSA"
end

-- Fiery Soul — PASSIVE, skip.

local function tryLagunaBlade(bot, ctx)
    local target = ctx.nearest_enemy
    if not target or target:IsNull() or not target:IsAlive() then return nil end
    if not target:IsHero() then return nil end
    if IsMagicImmuneSafe(target) then return nil end
    if ctx.nearest_enemy_dist > 600 then return nil end
    local hp, mx = target:GetHealth(), target:GetMaxHealth()
    local pct = (mx > 0) and (hp / mx) or 1
    if pct >= 0.5 then return nil end

    local ab = bot:GetAbilityInSlot(5)
    if not ab or ab:IsNull() or ab:GetLevel() == 0 or not ab:IsFullyCastable() then return nil end
    local name = ab:GetName() or "lina_laguna_blade"
    if not AB.CanCast(name, target, ctx.game_time) then return nil end
    AB.MarkCast(name, target, ctx.game_time)
    bot:Action_UseAbilityOnEntity(ab, target)
    return "LAGUNA"
end

function M.Think(bot, ctx)
    if not bot or bot:IsNull() or not bot:IsAlive() then return nil end
    local r
    r = tryLagunaBlade(bot, ctx);       if r then return "LINA:" .. r end -- finisher
    r = tryLightStrikeArray(bot, ctx);  if r then return "LINA:" .. r end -- setup stun
    r = tryDragonSlave(bot, ctx);       if r then return "LINA:" .. r end
    return nil
end

return M
