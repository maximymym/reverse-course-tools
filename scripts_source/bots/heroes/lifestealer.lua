-- heroes/lifestealer.lua — Lifestealer override.
-- Rage (Q) + Open Wounds (E).
-- Feast (W) — passive, ignored.
-- Infest (R) — слишком сложно автоматизировать, ignored.

local M = {}

local AB   = require("util.anti_ban")
local Move = require("util.movement")

-- ── Q: Rage — NO_TARGET magic immunity ──────────────────────────
local RAGE_ESCAPE_HP    = 0.4
local RAGE_ESCAPE_RANGE = 600
local RAGE_ENGAGE_RANGE = 500

local function tryRage(bot, ctx)
    local ab = bot:GetAbilityInSlot(0)
    if not ab or ab:IsNull() or ab:GetLevel() == 0 then return nil end
    if not ab:IsFullyCastable() then return nil end
    local gt = ctx.game_time

    local hp = ctx.hp_pct or 1
    local enemy = ctx.nearest_enemy
    local d = ctx.nearest_enemy_dist or 99999
    local enemyOK = enemy and not enemy:IsNull() and enemy:IsAlive()

    local label = nil
    -- Escape (от стана/disable)
    if enemyOK and hp < RAGE_ESCAPE_HP and d < RAGE_ESCAPE_RANGE then
        label = "LS:RAGE_ESCAPE"
    -- Engage (вход в teamfight)
    elseif enemyOK and d < RAGE_ENGAGE_RANGE and hp >= RAGE_ESCAPE_HP then
        label = "LS:RAGE_ENGAGE"
    end
    if not label then return nil end

    if not AB.CanCast("life_stealer_rage", bot, gt) then return nil end
    local ok = pcall(function()
        bot:Action_UseAbility(ab)
        AB.MarkCast("life_stealer_rage", bot, gt)
    end)
    if ok then return label end
    return nil
end

-- ── E: Open Wounds — UNIT_TARGET, range 600 ─────────────────────
local OPENW_RANGE = 600
local OPENW_SELF_HP_MIN = 0.5

local function tryOpenWounds(bot, ctx)
    if (ctx.hp_pct or 1) <= OPENW_SELF_HP_MIN then return nil end
    local ab = bot:GetAbilityInSlot(2)
    if not ab or ab:IsNull() or ab:GetLevel() == 0 then return nil end
    if not ab:IsFullyCastable() then return nil end
    local gt = ctx.game_time

    -- Ищем enemy hero в 600
    local best, bestDist = nil, OPENW_RANGE
    for _, h in ipairs(ctx.nearby_enemies or {}) do
        if h and not h:IsNull() and h:IsAlive() then
            local d = Move.Dist2D(ctx.pos, h:GetLocation())
            if d < bestDist then
                best, bestDist = h, d
            end
        end
    end
    if not best then return nil end

    if not AB.CanCast("life_stealer_open_wounds", best, gt) then return nil end
    local ok = pcall(function()
        bot:Action_UseAbilityOnEntity(ab, best)
        AB.MarkCast("life_stealer_open_wounds", best, gt)
    end)
    if ok then return "LS:OPEN_WOUNDS" end
    return nil
end

function M.Think(bot, ctx)
    if not bot or bot:IsNull() then return nil end
    if not ctx then return nil end

    -- Rage работает в low HP (escape) — НЕ блокируем по hp_pct
    local r = tryRage(bot, ctx); if r then return r end

    if (ctx.hp_pct or 1) < 0.4 then return nil end
    r = tryOpenWounds(bot, ctx); if r then return r end
    return nil
end

return M
