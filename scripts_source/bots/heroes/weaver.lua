-- heroes/weaver.lua — Weaver override.
-- The Swarm (Q) + Shukuchi (W) + Time Lapse (R).
-- Geminate Attack (E) — passive, ignored.

local M = {}

local AB   = require("util.anti_ban")
local Move = require("util.movement")

-- ── Q: The Swarm — POINT line forward ────────────────────────────
local SWARM_HERO_RANGE = 1200
local SWARM_CREEP_RANGE = 800
local SWARM_CREEP_MIN = 2

local function trySwarm(bot, ctx)
    local ab = bot:GetAbilityInSlot(0)
    if not ab or ab:IsNull() or ab:GetLevel() == 0 then return nil end
    if not ab:IsFullyCastable() then return nil end
    local gt = ctx.game_time

    -- Priority 1: enemy hero в 1200
    local heroTarget, hero_d = nil, 99999
    if ctx.nearest_enemy and not ctx.nearest_enemy:IsNull() and ctx.nearest_enemy:IsAlive() then
        local d = ctx.nearest_enemy_dist or 99999
        if d < SWARM_HERO_RANGE then
            heroTarget, hero_d = ctx.nearest_enemy, d
        end
    end

    -- Priority 2: ≥2 creeps в 800
    local creep_cluster = 0
    if not heroTarget then
        for _, c in ipairs(ctx.nearby_creeps or {}) do
            if c and not c:IsNull() and c:IsAlive() then
                local d = Move.Dist2D(ctx.pos, c:GetLocation())
                if d < SWARM_CREEP_RANGE then
                    creep_cluster = creep_cluster + 1
                end
            end
        end
        if creep_cluster < SWARM_CREEP_MIN then return nil end
    end

    -- Каст в направлении target / centroid от бота
    local target_loc, target_key, label
    if heroTarget then
        target_loc = heroTarget:GetLocation()
        target_key = heroTarget
        label = "WEAVER:SWARM_HERO"
    else
        -- Centroid creep
        local cx, cy, cz, n = 0, 0, 0, 0
        for _, c in ipairs(ctx.nearby_creeps or {}) do
            if c and not c:IsNull() and c:IsAlive() then
                local d = Move.Dist2D(ctx.pos, c:GetLocation())
                if d < SWARM_CREEP_RANGE then
                    local p = c:GetLocation()
                    cx = cx + (p.x or 0); cy = cy + (p.y or 0); cz = cz + (p.z or 0)
                    n = n + 1
                end
            end
        end
        if n < 1 then return nil end
        cx, cy, cz = cx / n, cy / n, cz / n
        target_loc = Vector and Vector(cx, cy, cz) or { x = cx, y = cy, z = cz }
        target_key = bot
        label = "WEAVER:SWARM_CREEPS"
    end

    if not AB.CanCast("weaver_the_swarm", target_key, gt) then return nil end
    local ok = pcall(function()
        bot:Action_UseAbilityOnLocation(ab, target_loc)
        AB.MarkCast("weaver_the_swarm", target_key, gt)
    end)
    if ok then return label end
    return nil
end

-- ── W: Shukuchi — NO_TARGET self invis ───────────────────────────
local function tryShukuchi(bot, ctx)
    local ab = bot:GetAbilityInSlot(1)
    if not ab or ab:IsNull() or ab:GetLevel() == 0 then return nil end
    if not ab:IsFullyCastable() then return nil end
    local gt = ctx.game_time

    local hp = ctx.hp_pct or 1
    local enemy = ctx.nearest_enemy
    local d = ctx.nearest_enemy_dist or 99999

    local label = nil
    -- Escape mode: low HP + враг близко
    if hp < 0.4 and d < 700 then
        label = "WEAVER:SHUKUCHI_ESCAPE"
    -- Engage mode: high HP, hero rangewise 600..1200
    elseif hp > 0.7 and enemy and not enemy:IsNull() and enemy:IsAlive()
            and d >= 600 and d <= 1200 then
        label = "WEAVER:SHUKUCHI_ENGAGE"
    end
    if not label then return nil end

    if not AB.CanCast("weaver_shukuchi", bot, gt) then return nil end
    local ok = pcall(function()
        bot:Action_UseAbility(ab)
        AB.MarkCast("weaver_shukuchi", bot, gt)
    end)
    if ok then return label end
    return nil
end

-- ── R: Time Lapse — NO_TARGET panic save ─────────────────────────
local function tryTimeLapse(bot, ctx)
    if (ctx.hp_pct or 1) >= 0.18 then return nil end
    if not ctx.took_damage_recent then return nil end
    local ab = bot:GetAbilityInSlot(5)
    if not ab or ab:IsNull() or ab:GetLevel() == 0 then return nil end
    if not ab:IsFullyCastable() then return nil end
    local gt = ctx.game_time
    if not AB.CanCast("weaver_time_lapse", bot, gt) then return nil end
    local ok = pcall(function()
        bot:Action_UseAbility(ab)
        AB.MarkCast("weaver_time_lapse", bot, gt)
    end)
    if ok then return "WEAVER:TIMELAPSE_PANIC" end
    return nil
end

function M.Think(bot, ctx)
    if not bot or bot:IsNull() then return nil end
    if not ctx then return nil end

    -- Time Lapse — самый высокий приоритет (panic)
    local r = tryTimeLapse(bot, ctx); if r then return r end

    -- Shukuchi: escape работает в low HP, поэтому ДО hp_pct gate
    r = tryShukuchi(bot, ctx); if r then return r end

    if (ctx.hp_pct or 1) < 0.4 then return nil end
    r = trySwarm(bot, ctx); if r then return r end
    return nil
end

return M
