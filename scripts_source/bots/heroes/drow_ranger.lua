-- heroes/drow_ranger.lua — Drow Ranger override.
-- Frost Arrows autocast (Q) + Gust (W) + Multishot (E).
-- Marksmanship (R) — passive, ignored.

local M = {}

local AB   = require("util.anti_ban")
local Move = require("util.movement")

-- ── Q: Frost Arrows — AUTOCAST one-shot ─────────────────────────
local function tryFrostArrows(bot, ctx)
    BotControllerState = BotControllerState or {}
    if BotControllerState.drow_frostarrows_set then return nil end
    local ab = bot:GetAbilityInSlot(0)
    if not ab or ab:IsNull() or ab:GetLevel() == 0 then return nil end
    local ok = pcall(function()
        bot:Action_ToggleAutoCast(ab)
        BotControllerState.drow_frostarrows_set = true
    end)
    if ok then return "DROW:FROSTARROWS_AUTOCAST" end
    return nil
end

-- ── W: Gust — POINT cone forward, retreat purpose ────────────────
local GUST_RANGE = 800

local function tryGust(bot, ctx)
    -- Используем при retreat (low HP) с близким enemy hero
    if (ctx.hp_pct or 1) > 0.5 then return nil end
    local enemy = ctx.nearest_enemy
    if not enemy or enemy:IsNull() or not enemy:IsAlive() then return nil end
    local d = ctx.nearest_enemy_dist or 99999
    if d > GUST_RANGE then return nil end

    local ab = bot:GetAbilityInSlot(1)
    if not ab or ab:IsNull() or ab:GetLevel() == 0 then return nil end
    if not ab:IsFullyCastable() then return nil end

    local gt = ctx.game_time
    if not AB.CanCast("drow_ranger_wave_of_silence", enemy, gt) then return nil end

    -- POINT: каст в направлении противника от себя
    local botLoc = ctx.pos
    local enemyLoc = enemy:GetLocation()
    if not enemyLoc or not botLoc then return nil end
    local dx = (enemyLoc.x or 0) - (botLoc.x or 0)
    local dy = (enemyLoc.y or 0) - (botLoc.y or 0)
    local mag = math.sqrt(dx * dx + dy * dy)
    if mag < 1 then return nil end
    -- Каст-точка чуть дальше противника по той же оси (cone forward)
    local cast_dist = math.min(mag + 100, 800)
    local cx = (botLoc.x or 0) + (dx / mag) * cast_dist
    local cy = (botLoc.y or 0) + (dy / mag) * cast_dist
    local cz = botLoc.z or 0
    local target_loc = Vector and Vector(cx, cy, cz) or { x = cx, y = cy, z = cz }

    local ok = pcall(function()
        bot:Action_UseAbilityOnLocation(ab, target_loc)
        AB.MarkCast("drow_ranger_wave_of_silence", enemy, gt)
    end)
    if ok then return "DROW:GUST_RETREAT" end
    return nil
end

-- ── E: Multishot — POINT line forward ────────────────────────────
local MULTISHOT_RANGE = 1500
local MULTISHOT_CREEP_MIN = 3

local function tryMultishot(bot, ctx)
    local ab = bot:GetAbilityInSlot(2)
    if not ab or ab:IsNull() or ab:GetLevel() == 0 then return nil end
    if not ab:IsFullyCastable() then return nil end
    local gt = ctx.game_time

    -- Priority 1: enemy hero в range
    local heroTarget = nil
    if ctx.nearest_enemy and not ctx.nearest_enemy:IsNull() and ctx.nearest_enemy:IsAlive() then
        if (ctx.nearest_enemy_dist or 99999) < MULTISHOT_RANGE then
            heroTarget = ctx.nearest_enemy
        end
    end

    local target_loc, target_key
    if heroTarget then
        target_loc = heroTarget:GetLocation()
        target_key = heroTarget
    else
        -- Priority 2: ≥3 enemy creeps в 1500
        local creeps = ctx.nearby_creeps or {}
        local cx, cy, cz, n = 0, 0, 0, 0
        for _, c in ipairs(creeps) do
            if c and not c:IsNull() and c:IsAlive() then
                local p = c:GetLocation()
                if p then
                    local dx = (p.x or 0) - (ctx.pos.x or 0)
                    local dy = (p.y or 0) - (ctx.pos.y or 0)
                    if math.sqrt(dx * dx + dy * dy) < MULTISHOT_RANGE then
                        cx = cx + (p.x or 0); cy = cy + (p.y or 0); cz = cz + (p.z or 0)
                        n = n + 1
                    end
                end
            end
        end
        if n < MULTISHOT_CREEP_MIN then return nil end
        cx, cy, cz = cx / n, cy / n, cz / n
        target_loc = Vector and Vector(cx, cy, cz) or { x = cx, y = cy, z = cz }
        target_key = bot
    end
    if not target_loc then return nil end

    if not AB.CanCast("drow_ranger_multishot", target_key, gt) then return nil end
    local ok = pcall(function()
        bot:Action_UseAbilityOnLocation(ab, target_loc)
        AB.MarkCast("drow_ranger_multishot", target_key, gt)
    end)
    if ok then return heroTarget and "DROW:MULTISHOT_HERO" or "DROW:MULTISHOT_CREEPS" end
    return nil
end

function M.Think(bot, ctx)
    if not bot or bot:IsNull() then return nil end
    if not ctx then return nil end

    local r = tryFrostArrows(bot, ctx); if r then return r end

    -- Gust работает в retreat — НЕ блокируем по hp_pct
    r = tryGust(bot, ctx); if r then return r end

    if (ctx.hp_pct or 1) < 0.4 then return nil end
    r = tryMultishot(bot, ctx); if r then return r end
    return nil
end

return M
