-- heroes/sniper.lua — Sniper override.
-- Take Aim autocast (W) + Shrapnel (Q) + Assassinate (R).
-- Headshot (E) — passive, ignored.

local M = {}

local AB   = require("util.anti_ban")
local Move = require("util.movement")

-- ── Q: Shrapnel — POINT_AOE charges-based ─────────────────────────────
local SHRAPNEL_RADIUS    = 400
local SHRAPNEL_RANGE     = 1500
local SHRAPNEL_CAST_TIME = 0.4
local SHRAPNEL_CREEP_MIN = 3
local SHRAPNEL_CENTROID_R= 200

-- ── R: Assassinate — UNIT_TARGET ────────────────────────────────────
local ASSASSINATE_HP_THRESH = 0.35
local ASSASSINATE_SELF_HP_MIN = 0.4

-- Predict cast location for moving target (lead-prediction).
local function predictedLoc(target)
    local loc = target:GetLocation()
    if not loc then return nil end
    local px, py, pz = loc.x or 0, loc.y or 0, loc.z or 0
    if target.GetVelocity then
        local ok, v = pcall(function() return target:GetVelocity() end)
        if ok and v then
            px = px + (v.x or 0) * SHRAPNEL_CAST_TIME
            py = py + (v.y or 0) * SHRAPNEL_CAST_TIME
        end
    end
    if Vector then return Vector(px, py, pz) end
    return { x = px, y = py, z = pz }
end

-- ── E (slot 2): Take Aim — AUTOCAST passive (toggle ON один раз) ─────
-- Sticky устанавливается ТОЛЬКО при ok+result==true, чтобы silent-fail (ok=true,
-- но Action не дошёл до C++) не блокировал retry. Дополнительно — re-verify
-- каждые ~60 секунд: если по `GetAutoCastState` autocast выключен, сбрасываем
-- sticky и пробуем заново (защита от respawn / refresher).
local TAKEAIM_RECHECK_INTERVAL = 60.0  -- секунд игрового времени

local function tryTakeAim(bot, ctx)
    BotControllerState = BotControllerState or {}
    local gt = (ctx and ctx.game_time) or 0

    local ab = bot:GetAbilityInSlot(2)
    local ab_null = (not ab) or ab:IsNull()
    local ab_lvl  = (ab and not ab_null and ab:GetLevel()) or 0
    local sticky  = BotControllerState.sniper_takeaim_set == true
    local last_v  = BotControllerState.sniper_takeaim_verified_gt or -999

    if ab_null or ab_lvl == 0 then
        -- Лог только при изменении состояния, чтобы не спамить.
        if BotControllerState.sniper_takeaim_log_ab ~= "missing" then
            print(string.format(
                "[sniper] tryTakeAim WAIT: ability slot=2 null=%s level=%d sticky=%s gt=%.1f",
                tostring(ab_null), ab_lvl, tostring(sticky), gt))
            BotControllerState.sniper_takeaim_log_ab = "missing"
        end
        return nil
    end
    BotControllerState.sniper_takeaim_log_ab = "present"

    -- Re-verify: если sticky=true, но GetAutoCastState (если экспонирован) говорит
    -- false, или прошло ≥ TAKEAIM_RECHECK_INTERVAL без верификации — сбрасываем.
    if sticky then
        local autocast_state_ok, autocast_state = pcall(function()
            if ab.GetAutoCastState then return ab:GetAutoCastState() end
            return nil
        end)
        if autocast_state_ok and autocast_state == false then
            print(string.format(
                "[sniper] tryTakeAim RECHECK: GetAutoCastState=false → sticky reset (gt=%.1f)", gt))
            BotControllerState.sniper_takeaim_set = false
            sticky = false
        elseif (gt - last_v) >= TAKEAIM_RECHECK_INTERVAL then
            -- Нет API GetAutoCastState или вернул nil — таймер-based fallback.
            print(string.format(
                "[sniper] tryTakeAim RECHECK: %.1fs since last verify → sticky reset (gt=%.1f)",
                gt - last_v, gt))
            BotControllerState.sniper_takeaim_set = false
            sticky = false
        end
    end

    if sticky then return nil end

    -- Backoff: не пробуем чаще чем раз в 5 тиков (≈ 0.7с) чтобы при долгом
    -- failure-loop не флудить лог и C++ ордерами.
    local last_try_tick = BotControllerState.sniper_takeaim_last_try_tick or -999
    local cur_tick      = BotControllerState.tick or 0
    if (cur_tick - last_try_tick) < 5 then return nil end
    BotControllerState.sniper_takeaim_last_try_tick = cur_tick

    local ok, err = pcall(bot.Action_ToggleAutoCast, bot, ab)
    print(string.format(
        "[sniper] tryTakeAim TRY: ok=%s err=%s level=%d gt=%.1f tick=%d",
        tostring(ok), tostring(err), ab_lvl, gt, cur_tick))
    if ok then
        BotControllerState.sniper_takeaim_set         = true
        BotControllerState.sniper_takeaim_verified_gt = gt
        print(string.format(
            "[cast] sniper_take_aim mode=AUTOCAST_ON ok=true tick=%d gt=%.1f",
            cur_tick, gt))
        return "SNIPER:TAKEAIM_AUTOCAST"
    end
    print(string.format("[cast] sniper_take_aim mode=AUTOCAST_ON ok=false err=%s", tostring(err)))
    return nil
end

local function tryShrapnel(bot, ctx)
    local ab = bot:GetAbilityInSlot(0)
    if not ab or ab:IsNull() or ab:GetLevel() == 0 then return nil end
    if not ab:IsFullyCastable() then return nil end

    local gt = ctx.game_time

    -- Priority 1: low-HP enemy hero in range
    local heroTarget = nil
    if ctx.nearest_enemy and not ctx.nearest_enemy:IsNull() and ctx.nearest_enemy:IsAlive() then
        local d = ctx.nearest_enemy_dist or 99999
        if d < SHRAPNEL_RANGE then
            local hp, maxHp = ctx.nearest_enemy:GetHealth(), ctx.nearest_enemy:GetMaxHealth()
            local pct = (maxHp > 0) and (hp / maxHp) or 1
            if pct < 0.7 then heroTarget = ctx.nearest_enemy end
        end
    end

    if heroTarget then
        if not AB.CanCast("sniper_shrapnel", heroTarget, gt) then return nil end
        local loc = predictedLoc(heroTarget) or heroTarget:GetLocation()
        if not loc then return nil end
        local ok = pcall(function()
            bot:Action_UseAbilityOnLocation(ab, loc)
            AB.MarkCast("sniper_shrapnel", heroTarget, gt)
        end)
        if ok then return "SNIPER:SHRAPNEL_HERO" end
        return nil
    end

    -- Priority 2: creep cluster (≥3 в 200 от centroid)
    local creeps = ctx.nearby_creeps or {}
    if #creeps < SHRAPNEL_CREEP_MIN then return nil end
    -- Centroid через простой mean
    local cx, cy, cz, n = 0, 0, 0, 0
    for _, c in ipairs(creeps) do
        if c and not c:IsNull() and c:IsAlive() then
            local p = c:GetLocation()
            if p then
                cx = cx + (p.x or 0); cy = cy + (p.y or 0); cz = cz + (p.z or 0)
                n = n + 1
            end
        end
    end
    if n < SHRAPNEL_CREEP_MIN then return nil end
    cx, cy, cz = cx / n, cy / n, cz / n
    -- Сколько крипов в SHRAPNEL_CENTROID_R от центра
    local cluster = 0
    for _, c in ipairs(creeps) do
        if c and not c:IsNull() and c:IsAlive() then
            local p = c:GetLocation()
            if p then
                local dx, dy = (p.x or 0) - cx, (p.y or 0) - cy
                if math.sqrt(dx * dx + dy * dy) < SHRAPNEL_CENTROID_R then
                    cluster = cluster + 1
                end
            end
        end
    end
    if cluster < SHRAPNEL_CREEP_MIN then return nil end

    -- Дистанция до centroid должна быть в range
    local botLoc = ctx.pos
    local ddx, ddy = cx - (botLoc.x or 0), cy - (botLoc.y or 0)
    if math.sqrt(ddx * ddx + ddy * ddy) > SHRAPNEL_RANGE then return nil end

    -- Anti-ban: фиктивный target = bot self (как отсутствие hero target)
    if not AB.CanCast("sniper_shrapnel_creeps", bot, gt) then return nil end
    local target_loc = Vector and Vector(cx, cy, cz) or { x = cx, y = cy, z = cz }
    local ok = pcall(function()
        bot:Action_UseAbilityOnLocation(ab, target_loc)
        AB.MarkCast("sniper_shrapnel_creeps", bot, gt)
    end)
    if ok then return "SNIPER:SHRAPNEL_CREEPS" end
    return nil
end

local function tryAssassinate(bot, ctx)
    local ab = bot:GetAbilityInSlot(5)
    if not ab or ab:IsNull() or ab:GetLevel() == 0 then return nil end
    if not ab:IsFullyCastable() then return nil end
    if (ctx.hp_pct or 1) < ASSASSINATE_SELF_HP_MIN then return nil end

    local range = ab:GetCastRange() or 2500
    local gt = ctx.game_time

    -- Ищем low-HP hero в range
    local best, bestPct = nil, ASSASSINATE_HP_THRESH
    for _, h in ipairs(ctx.nearby_enemies or {}) do
        if h and not h:IsNull() and h:IsAlive() then
            local hp, maxHp = h:GetHealth(), h:GetMaxHealth()
            local pct = (maxHp > 0) and (hp / maxHp) or 1
            if pct < bestPct then
                local d = Move.Dist2D(ctx.pos, h:GetLocation())
                if d <= range then
                    best, bestPct = h, pct
                end
            end
        end
    end
    if not best then return nil end

    if not AB.CanCast("sniper_assassinate", best, gt) then return nil end
    local ok = pcall(function()
        bot:Action_UseAbilityOnEntity(ab, best)
        AB.MarkCast("sniper_assassinate", best, gt)
    end)
    if ok then return "SNIPER:ASSASSINATE" end
    return nil
end

function M.Think(bot, ctx)
    if not bot or bot:IsNull() then return nil end
    if not ctx then return nil end

    local r = tryTakeAim(bot, ctx); if r then return r end
    if (ctx.hp_pct or 1) < 0.4 then return nil end  -- low HP — generic FSM retreat

    r = tryAssassinate(bot, ctx); if r then return r end
    r = tryShrapnel(bot, ctx);   if r then return r end
    return nil
end

return M
