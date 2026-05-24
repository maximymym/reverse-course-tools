-- heroes/gyrocopter.lua — Gyrocopter override.
-- Rocket Barrage (Q) + Homing Missile (W) + Flak Cannon (E) + Call Down (R).

local M = {}

local AB   = require("util.anti_ban")
local Move = require("util.movement")

-- ── Q: Rocket Barrage — NO_TARGET self-area (R=400) ─────────────
local BARRAGE_RANGE = 400

local function tryBarrage(bot, ctx)
    local enemy = ctx.nearest_enemy
    if not enemy or enemy:IsNull() or not enemy:IsAlive() then return nil end
    if (ctx.nearest_enemy_dist or 99999) > BARRAGE_RANGE then return nil end

    local ab = bot:GetAbilityInSlot(0)
    if not ab or ab:IsNull() or ab:GetLevel() == 0 then return nil end
    if not ab:IsFullyCastable() then return nil end
    local gt = ctx.game_time
    if not AB.CanCast("gyrocopter_rocket_barrage", bot, gt) then return nil end
    local ok = pcall(function()
        bot:Action_UseAbility(ab)
        AB.MarkCast("gyrocopter_rocket_barrage", bot, gt)
    end)
    if ok then return "GYRO:BARRAGE" end
    return nil
end

-- ── W: Homing Missile — UNIT_TARGET, range 1100 ─────────────────
local HOMING_RANGE = 1100
local HOMING_HP_THRESH = 0.6

local function tryHoming(bot, ctx)
    local ab = bot:GetAbilityInSlot(1)
    if not ab or ab:IsNull() or ab:GetLevel() == 0 then return nil end
    if not ab:IsFullyCastable() then return nil end
    local gt = ctx.game_time

    -- Низкий HP enemy hero в range
    local best, bestPct = nil, HOMING_HP_THRESH
    for _, h in ipairs(ctx.nearby_enemies or {}) do
        if h and not h:IsNull() and h:IsAlive() then
            local d = Move.Dist2D(ctx.pos, h:GetLocation())
            if d < HOMING_RANGE then
                local hp, maxHp = h:GetHealth(), h:GetMaxHealth()
                local pct = (maxHp > 0) and (hp / maxHp) or 1
                if pct < bestPct then
                    best, bestPct = h, pct
                end
            end
        end
    end
    if not best then return nil end

    if not AB.CanCast("gyrocopter_homing_missile", best, gt) then return nil end
    local ok = pcall(function()
        bot:Action_UseAbilityOnEntity(ab, best)
        AB.MarkCast("gyrocopter_homing_missile", best, gt)
    end)
    if ok then return "GYRO:HOMING" end
    return nil
end

-- ── E: Flak Cannon — NO_TARGET self-buff ────────────────────────
local FLAK_RANGE = 1200
local FLAK_CREEP_MIN = 3

local function tryFlak(bot, ctx)
    -- Считаем creeps в 1200
    local n = 0
    for _, c in ipairs(ctx.nearby_creeps or {}) do
        if c and not c:IsNull() and c:IsAlive() then
            local d = Move.Dist2D(ctx.pos, c:GetLocation())
            if d < FLAK_RANGE then n = n + 1 end
        end
    end
    if n < FLAK_CREEP_MIN then return nil end

    local ab = bot:GetAbilityInSlot(2)
    if not ab or ab:IsNull() or ab:GetLevel() == 0 then return nil end
    if not ab:IsFullyCastable() then return nil end
    local gt = ctx.game_time
    if not AB.CanCast("gyrocopter_flak_cannon", bot, gt) then return nil end
    local ok = pcall(function()
        bot:Action_UseAbility(ab)
        AB.MarkCast("gyrocopter_flak_cannon", bot, gt)
    end)
    if ok then return "GYRO:FLAK" end
    return nil
end

-- ── R: Call Down — POINT_AOE ────────────────────────────────────
local CALLDOWN_RANGE = 1000
local CALLDOWN_HP_THRESH = 0.7

local function tryCallDown(bot, ctx)
    local ab = bot:GetAbilityInSlot(5)
    if not ab or ab:IsNull() or ab:GetLevel() == 0 then return nil end
    if not ab:IsFullyCastable() then return nil end
    local gt = ctx.game_time

    -- Ищем enemy hero в 1000 с HP < 0.7
    local best, bestPct = nil, CALLDOWN_HP_THRESH
    for _, h in ipairs(ctx.nearby_enemies or {}) do
        if h and not h:IsNull() and h:IsAlive() then
            local d = Move.Dist2D(ctx.pos, h:GetLocation())
            if d < CALLDOWN_RANGE then
                local hp, maxHp = h:GetHealth(), h:GetMaxHealth()
                local pct = (maxHp > 0) and (hp / maxHp) or 1
                if pct < bestPct then
                    best, bestPct = h, pct
                end
            end
        end
    end
    if not best then return nil end

    if not AB.CanCast("gyrocopter_call_down", best, gt) then return nil end
    local loc = best:GetLocation()
    if not loc then return nil end
    local ok = pcall(function()
        bot:Action_UseAbilityOnLocation(ab, loc)
        AB.MarkCast("gyrocopter_call_down", best, gt)
    end)
    if ok then return "GYRO:CALLDOWN" end
    return nil
end

function M.Think(bot, ctx)
    if not bot or bot:IsNull() then return nil end
    if not ctx then return nil end
    if (ctx.hp_pct or 1) < 0.4 then return nil end

    -- Высокоприоритетный nuke на low-HP цели
    local r = tryCallDown(bot, ctx); if r then return r end
    r = tryHoming(bot, ctx);   if r then return r end
    r = tryBarrage(bot, ctx);  if r then return r end
    r = tryFlak(bot, ctx);     if r then return r end
    return nil
end

return M
