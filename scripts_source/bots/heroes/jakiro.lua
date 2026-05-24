-- heroes/jakiro.lua — per-hero override для Jakiro.
-- Способности: Dual Breath (Q, slot 0), Ice Path (W, slot 1),
-- Liquid Fire (E, slot 2), Macropyre (R, slot 5).

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

-- Считает enemy creeps в forward-cone (приближение: в радиусе 800 ближе чем 1100).
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

-- Centroid живых вражеских героев (для Macropyre direction).
local function EnemyCentroid(ctx, radius)
    local sx, sy, sz, n = 0, 0, 0, 0
    for _, h in ipairs(ctx.nearby_enemies or {}) do
        if h and not h:IsNull() and h:IsAlive() then
            local d = Move.Dist2D(ctx.pos, h:GetLocation())
            if d <= radius then
                local p = h:GetLocation()
                sx, sy, sz, n = sx + p.x, sy + p.y, sz + p.z, n + 1
            end
        end
    end
    if n == 0 then return nil, 0 end
    return Vector(sx / n, sy / n, sz / n), n
end

local function tryDualBreath(bot, ctx)
    if ctx.hp_pct <= 0.4 then return nil end
    local cast_loc = nil
    local target = ctx.nearest_enemy
    local creeps_n = CountCreepsForward(ctx)

    if target and not target:IsNull() and target:IsAlive() and target:IsHero()
       and not IsMagicImmuneSafe(target)
       and ctx.nearest_enemy_dist >= 200 and ctx.nearest_enemy_dist <= 900 then
        cast_loc = target:GetLocation()
    elseif creeps_n >= 3 and ctx.lane_front then
        cast_loc = ctx.lane_front
    elseif Combat.JungleCastAllowed(ctx, 0.5) then
        local ncentr, ncount = Combat.NeutralsCentroidInRadius(ctx, 700)
        if ncentr and ncount >= 2 then cast_loc = ncentr end
    end

    if not cast_loc then return nil end
    local ab = bot:GetAbilityInSlot(0)
    if not ab or ab:IsNull() or ab:GetLevel() == 0 or not ab:IsFullyCastable() then return nil end
    local name = ab:GetName() or "jakiro_dual_breath"
    if not AB.CanCast(name, target or bot, ctx.game_time) then return nil end
    AB.MarkCast(name, target or bot, ctx.game_time)
    bot:Action_UseAbilityOnLocation(ab, cast_loc)
    return "DUAL_BREATH"
end

local function tryIcePath(bot, ctx)
    if ctx.hp_pct <= 0.4 then return nil end
    local target = ctx.nearest_enemy
    if not target or target:IsNull() or not target:IsAlive() then return nil end
    if not target:IsHero() then return nil end
    if IsMagicImmuneSafe(target) then return nil end
    if ctx.nearest_enemy_dist > 1100 then return nil end

    -- Lead prediction (delay ~0.5s): cast чуть впереди по движению.
    local loc = target:GetLocation()
    local ok, vel = pcall(function() return target:GetVelocity() end)
    if ok and vel then
        loc = Vector(loc.x + (vel.x or 0) * 0.5,
                     loc.y + (vel.y or 0) * 0.5,
                     loc.z)
    end

    local ab = bot:GetAbilityInSlot(1)
    if not ab or ab:IsNull() or ab:GetLevel() == 0 or not ab:IsFullyCastable() then return nil end
    local name = ab:GetName() or "jakiro_ice_path"
    if not AB.CanCast(name, target, ctx.game_time) then return nil end
    AB.MarkCast(name, target, ctx.game_time)
    bot:Action_UseAbilityOnLocation(ab, loc)
    return "ICE_PATH"
end

local function tryLiquidFire(bot, ctx)
    if ctx.hp_pct <= 0.4 then return nil end
    -- Приоритет: enemy tower в 800.
    local target = nil
    for _, t in ipairs(ctx.nearby_towers or {}) do
        if t and not t:IsNull() and t:IsAlive() then
            local d = Move.Dist2D(ctx.pos, t:GetLocation())
            if d < 800 then target = t; break end
        end
    end
    -- Fallback: enemy hero в 700.
    if not target then
        if ctx.nearest_enemy and ctx.nearest_enemy:IsAlive()
           and ctx.nearest_enemy:IsHero()
           and ctx.nearest_enemy_dist < 700 then
            target = ctx.nearest_enemy
        end
    end
    -- Jungle fallback: AOE splash + DoT очень эффективен в кампе.
    if not target and Combat.JungleCastAllowed(ctx, 0.5) then
        target = Combat.StrongestNeutralInRadius(ctx, 700)
    end
    if not target then return nil end

    local ab = bot:GetAbilityInSlot(2)
    if not ab or ab:IsNull() or ab:GetLevel() == 0 or not ab:IsFullyCastable() then return nil end
    local name = ab:GetName() or "jakiro_liquid_fire"
    if not AB.CanCast(name, target, ctx.game_time) then return nil end
    AB.MarkCast(name, target, ctx.game_time)
    bot:Action_UseAbilityOnEntity(ab, target)
    return "LIQUID_FIRE"
end

local function tryMacropyre(bot, ctx)
    if ctx.hp_pct <= 0.5 then return nil end
    local centroid, n = EnemyCentroid(ctx, 1100)
    if not centroid or n < 2 then return nil end
    local ab = bot:GetAbilityInSlot(5)
    if not ab or ab:IsNull() or ab:GetLevel() == 0 or not ab:IsFullyCastable() then return nil end
    local name = ab:GetName() or "jakiro_macropyre"
    if not AB.CanCast(name, ctx.nearest_enemy or bot, ctx.game_time) then return nil end
    AB.MarkCast(name, ctx.nearest_enemy or bot, ctx.game_time)
    bot:Action_UseAbilityOnLocation(ab, centroid)
    return "MACROPYRE"
end

function M.Think(bot, ctx)
    if not bot or bot:IsNull() or not bot:IsAlive() then return nil end
    local r
    r = tryDualBreath(bot, ctx); if r then return "JAKIRO:" .. r end
    r = tryIcePath(bot, ctx);    if r then return "JAKIRO:" .. r end
    r = tryLiquidFire(bot, ctx); if r then return "JAKIRO:" .. r end
    r = tryMacropyre(bot, ctx);  if r then return "JAKIRO:" .. r end
    return nil
end

return M
