-- heroes/zuus.lua — per-hero override для Zeus.
-- Способности: Arc Lightning (Q, slot 0), Lightning Bolt (W, slot 1),
-- Static Field (E, slot 2 — PASSIVE, скип), Thundergod's Wrath (R, slot 5).

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

local function tryArcLightning(bot, ctx)
    -- Низкий CD (4с), используем агрессивно для harass + farm.
    if ctx.hp_pct <= 0.4 then return nil end
    -- Цель: ближайший enemy (hero или creep).
    local target = nil
    local bestDist = 99999
    -- Сначала heroes (приоритет).
    if ctx.nearest_enemy and ctx.nearest_enemy:IsAlive()
       and ctx.nearest_enemy_dist < 900
       and not IsMagicImmuneSafe(ctx.nearest_enemy) then
        target = ctx.nearest_enemy
        bestDist = ctx.nearest_enemy_dist
    end
    -- Иначе ближайший creep.
    if not target then
        for _, c in ipairs(ctx.nearby_creeps or {}) do
            if c and not c:IsNull() and c:IsAlive() then
                local d = Move.Dist2D(ctx.pos, c:GetLocation())
                if d < 900 and d < bestDist then
                    bestDist = d
                    target = c
                end
            end
        end
    end
    -- Jungle fallback: ни heroes, ни lane creeps → крепкий нейтрал.
    -- Arc Lightning low-cost (75 mana, 4с CD) — спам нормален, mana_floor 0.4.
    if not target and Combat.JungleCastAllowed(ctx, 0.4) then
        target = Combat.StrongestNeutralInRadius(ctx, 900)
    end
    if not target then return nil end

    local ab = bot:GetAbilityInSlot(0)
    if not ab or ab:IsNull() or ab:GetLevel() == 0 or not ab:IsFullyCastable() then return nil end
    local name = ab:GetName() or "zuus_arc_lightning"
    if not AB.CanCast(name, target, ctx.game_time) then return nil end
    AB.MarkCast(name, target, ctx.game_time)
    bot:Action_UseAbilityOnEntity(ab, target)
    return "ARC_LIGHTNING"
end

local function tryLightningBolt(bot, ctx)
    if ctx.hp_pct <= 0.4 then return nil end
    local ab = bot:GetAbilityInSlot(1)
    if not ab or ab:IsNull() or ab:GetLevel() == 0 or not ab:IsFullyCastable() then return nil end
    local name = ab:GetName() or "zuus_lightning_bolt"

    -- Hero priority.
    local target = ctx.nearest_enemy
    if target and not target:IsNull() and target:IsAlive() and target:IsHero()
       and not IsMagicImmuneSafe(target)
       and ctx.nearest_enemy_dist <= 850 then
        local hp, mx = target:GetHealth(), target:GetMaxHealth()
        local pct = (mx > 0) and (hp / mx) or 1
        if pct > 0.2 then
            if not AB.CanCast(name, target, ctx.game_time) then return nil end
            AB.MarkCast(name, target, ctx.game_time)
            bot:Action_UseAbilityOnEntity(ab, target)
            return "LIGHTNING_BOLT"
        end
        return nil
    end

    -- Jungle fallback: высокий-cost (125 mana) → mana_floor 0.6.
    if Combat.JungleCastAllowed(ctx, 0.6) then
        local neut = Combat.StrongestNeutralInRadius(ctx, 850)
        if neut and not IsMagicImmuneSafe(neut) then
            if not AB.CanCast(name, neut, ctx.game_time) then return nil end
            AB.MarkCast(name, neut, ctx.game_time)
            bot:Action_UseAbilityOnEntity(ab, neut)
            return "LIGHTNING_BOLT_NEUT"
        end
    end

    return nil
end

-- Static Field — PASSIVE, skip.

local function tryThundergodsWrath(bot, ctx)
    -- Global no-target ult — ищем hero с hp_pct < 0.25 в nearby_enemies.
    -- (Идеально global scan, но nearby_enemies — что у нас есть.)
    local snipe = false
    for _, h in ipairs(ctx.nearby_enemies or {}) do
        if h and not h:IsNull() and h:IsAlive() and h:IsHero()
           and not IsMagicImmuneSafe(h) then
            local hp, mx = h:GetHealth(), h:GetMaxHealth()
            local pct = (mx > 0) and (hp / mx) or 1
            if pct < 0.25 then
                snipe = true
                break
            end
        end
    end
    if not snipe then return nil end

    local ab = bot:GetAbilityInSlot(5)
    if not ab or ab:IsNull() or ab:GetLevel() == 0 or not ab:IsFullyCastable() then return nil end
    local name = ab:GetName() or "zuus_thundergods_wrath"
    if not AB.CanCast(name, bot, ctx.game_time) then return nil end
    AB.MarkCast(name, bot, ctx.game_time)
    bot:Action_UseAbility(ab)
    return "THUNDERGODS"
end

function M.Think(bot, ctx)
    if not bot or bot:IsNull() or not bot:IsAlive() then return nil end
    local r
    r = tryThundergodsWrath(bot, ctx); if r then return "ZEUS:" .. r end -- kill steal
    r = tryLightningBolt(bot, ctx);    if r then return "ZEUS:" .. r end
    r = tryArcLightning(bot, ctx);     if r then return "ZEUS:" .. r end -- harass spam
    return nil
end

return M
