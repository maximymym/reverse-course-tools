-- heroes/witch_doctor.lua — per-hero override для Witch Doctor.
-- Способности: Paralyzing Cask (Q, slot 0), Voodoo Restoration (W, slot 1 — TOGGLE),
-- Maledict (E, slot 2), Death Ward (R, slot 5 — channel, скип).

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

local function tryParalyzingCask(bot, ctx)
    if ctx.hp_pct <= 0.4 then return nil end
    local ab = bot:GetAbilityInSlot(0)
    if not ab or ab:IsNull() or ab:GetLevel() == 0 or not ab:IsFullyCastable() then return nil end
    local name = ab:GetName() or "witch_doctor_paralyzing_cask"

    -- Hero priority: hero-target в 800, hp_pct > 0.3, есть кому bounce.
    local target = ctx.nearest_enemy
    if target and not target:IsNull() and target:IsAlive() and target:IsHero()
       and not IsMagicImmuneSafe(target)
       and ctx.nearest_enemy_dist <= 800 then
        local hp, mx = target:GetHealth(), target:GetMaxHealth()
        local pct = (mx > 0) and (hp / mx) or 1
        if pct > 0.3 then
            local alive = 0
            for _, h in ipairs(ctx.nearby_enemies or {}) do
                if h and not h:IsNull() and h:IsAlive() then alive = alive + 1 end
            end
            if alive >= 1 then
                if not AB.CanCast(name, target, ctx.game_time) then return nil end
                AB.MarkCast(name, target, ctx.game_time)
                bot:Action_UseAbilityOnEntity(ab, target)
                return "PARALYZING_CASK"
            end
        end
        return nil
    end

    -- Jungle fallback: bounce между нейтралами (cask bounces по mini units).
    if Combat.JungleCastAllowed(ctx, 0.5) then
        local neut = Combat.StrongestNeutralInRadius(ctx, 800)
        local n = #(ctx.neutrals or {})
        if neut and not IsMagicImmuneSafe(neut) and n >= 2 then
            if not AB.CanCast(name, neut, ctx.game_time) then return nil end
            AB.MarkCast(name, neut, ctx.game_time)
            bot:Action_UseAbilityOnEntity(ab, neut)
            return "PARALYZING_CASK_NEUT"
        end
    end

    return nil
end

-- Toggle Voodoo Restoration ON один раз когда выучен. Хранит флаг в
-- BotControllerState.wd_voodoo_restoration_set.
local function tryVoodooRestorationToggle(bot, ctx)
    if BotControllerState and BotControllerState.wd_voodoo_restoration_set then
        return nil
    end
    local ab = bot:GetAbilityInSlot(1)
    if not ab or ab:IsNull() or ab:GetLevel() == 0 then return nil end
    -- IsFullyCastable необязателен для toggle, но проверим mana.
    if not ab:IsFullyCastable() then return nil end

    if bot.Action_ToggleAbility then
        bot:Action_ToggleAbility(ab)
    end
    if BotControllerState then
        BotControllerState.wd_voodoo_restoration_set = true
    end
    return "VOODOO_REST_ON"
end

local function tryMaledict(bot, ctx)
    if ctx.hp_pct <= 0.4 then return nil end
    local ab = bot:GetAbilityInSlot(2)
    if not ab or ab:IsNull() or ab:GetLevel() == 0 or not ab:IsFullyCastable() then return nil end
    local name = ab:GetName() or "witch_doctor_maledict"

    -- Hero priority.
    local target = ctx.nearest_enemy
    if target and not target:IsNull() and target:IsAlive() and target:IsHero()
       and not IsMagicImmuneSafe(target)
       and ctx.nearest_enemy_dist <= 600 then
        local hp, mx = target:GetHealth(), target:GetMaxHealth()
        local pct = (mx > 0) and (hp / mx) or 1
        if pct > 0.4 then
            if not AB.CanCast(name, target, ctx.game_time) then return nil end
            AB.MarkCast(name, target, ctx.game_time)
            bot:Action_UseAbilityOnLocation(ab, target:GetLocation())
            return "MALEDICT"
        end
        return nil
    end

    -- Jungle fallback: AOE DoT по centroid пачки нейтралов (Maledict — area
    -- 325, попадает в 2-3 крипа кампа).
    if Combat.JungleCastAllowed(ctx, 0.5) then
        local ncentr, ncount = Combat.NeutralsCentroidInRadius(ctx, 600)
        if ncentr and ncount >= 2 then
            if not AB.CanCast(name, bot, ctx.game_time) then return nil end
            AB.MarkCast(name, bot, ctx.game_time)
            bot:Action_UseAbilityOnLocation(ab, ncentr)
            return "MALEDICT_NEUT"
        end
    end

    return nil
end

-- Death Ward — channel, skip.

function M.Think(bot, ctx)
    if not bot or bot:IsNull() or not bot:IsAlive() then return nil end
    -- Toggle init (один раз за матч, до combat checks).
    local r = tryVoodooRestorationToggle(bot, ctx)
    if r then return "WD:" .. r end

    r = tryParalyzingCask(bot, ctx); if r then return "WD:" .. r end
    r = tryMaledict(bot, ctx);       if r then return "WD:" .. r end
    return nil
end

return M
