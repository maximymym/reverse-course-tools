-- heroes/ogre_magi.lua — per-hero override для Ogre Magi.
-- Способности: Fireblast (Q, slot 0), Ignite (W, slot 1),
-- Bloodlust (E, slot 2), Multicast (R, slot 5 — PASSIVE, скип).

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

-- Слабейший живой не-magic-immune crippe в радиусе R (от bot pos).
local function WeakestCreepInRadius(list, fromPos, radius)
    local best, bestPct, bestDist = nil, 1.01, 99999
    if not list then return nil, 0 end
    local count = 0
    for _, c in ipairs(list) do
        if c and not c:IsNull() and c:IsAlive() then
            local d = Move.Dist2D(fromPos, c:GetLocation())
            if d <= radius then
                count = count + 1
                if not IsMagicImmuneSafe(c) then
                    local hp, mx = c:GetHealth(), c:GetMaxHealth()
                    local pct = (mx > 0) and (hp / mx) or 1
                    if pct < bestPct or (pct == bestPct and d < bestDist) then
                        best, bestPct, bestDist = c, pct, d
                    end
                end
            end
        end
    end
    return best, count
end

local function tryFireblast(bot, ctx)
    if ctx.hp_pct <= 0.4 then return nil end
    local ab = bot:GetAbilityInSlot(0)
    if not ab or ab:IsNull() or ab:GetLevel() == 0 or not ab:IsFullyCastable() then return nil end
    local name = ab:GetName() or "ogre_magi_fireblast"

    -- Hero-priority: если есть hero-target в 700 — кастуем в него.
    local hero = ctx.nearest_enemy
    if hero and not hero:IsNull() and hero:IsAlive() and hero:IsHero()
       and not IsMagicImmuneSafe(hero) and (ctx.nearest_enemy_dist or 99999) <= 700 then
        if not AB.CanCast(name, hero, ctx.game_time) then return nil end
        AB.MarkCast(name, hero, ctx.game_time)
        bot:Action_UseAbilityOnEntity(ab, hero)
        return "FIREBLAST"
    end

    -- Creep-fallback: hero нет (или вне 700), но есть >=3 крипа в 700 →
    -- кастуем на самого хилого. Жжём wave/jungle.
    local target, count = WeakestCreepInRadius(ctx.nearby_creeps, ctx.pos, 700)
    if target and count >= 3 then
        if not AB.CanCast(name, target, ctx.game_time) then return nil end
        AB.MarkCast(name, target, ctx.game_time)
        bot:Action_UseAbilityOnEntity(ab, target)
        return "FIREBLAST_CREEP"
    end

    -- Jungle fallback: в JUNGLE_FARM, mana>0.5, нет hero-врагов → жжём
    -- крепкого нейтрала. Стандартный nearby_creeps жит lane creeps,
    -- neutrals идут отдельным списком.
    if Combat.JungleCastAllowed(ctx, 0.5) then
        local neut = Combat.StrongestNeutralInRadius(ctx, 700)
        if neut and not IsMagicImmuneSafe(neut) then
            if not AB.CanCast(name, neut, ctx.game_time) then return nil end
            AB.MarkCast(name, neut, ctx.game_time)
            bot:Action_UseAbilityOnEntity(ab, neut)
            return "FIREBLAST_NEUT"
        end
    end

    return nil
end

local function tryIgnite(bot, ctx)
    if ctx.hp_pct <= 0.4 then return nil end
    local ab = bot:GetAbilityInSlot(1)
    if not ab or ab:IsNull() or ab:GetLevel() == 0 or not ab:IsFullyCastable() then return nil end
    local name = ab:GetName() or "ogre_magi_ignite"

    -- Hero-priority: hero в 900.
    local hero = ctx.nearest_enemy
    if hero and not hero:IsNull() and hero:IsAlive() and hero:IsHero()
       and not IsMagicImmuneSafe(hero) and (ctx.nearest_enemy_dist or 99999) <= 900 then
        if not AB.CanCast(name, hero, ctx.game_time) then return nil end
        AB.MarkCast(name, hero, ctx.game_time)
        bot:Action_UseAbilityOnEntity(ab, hero)
        return "IGNITE"
    end

    -- Creep-fallback: AOE DoT на крипа в центре пачки (ближайшего к
    -- остальным = с минимальным средним расстоянием до соседей в 700).
    local creeps_in_range = {}
    for _, c in ipairs(ctx.nearby_creeps or {}) do
        if c and not c:IsNull() and c:IsAlive() then
            local d = Move.Dist2D(ctx.pos, c:GetLocation())
            if d <= 700 and not IsMagicImmuneSafe(c) then
                table.insert(creeps_in_range, c)
            end
        end
    end
    if #creeps_in_range < 3 then return nil end

    local best, bestSum = nil, 99999999
    for _, ci in ipairs(creeps_in_range) do
        local pi = ci:GetLocation()
        local sum = 0
        for _, cj in ipairs(creeps_in_range) do
            if cj ~= ci then
                sum = sum + Move.Dist2D(pi, cj:GetLocation())
            end
        end
        if sum < bestSum then best, bestSum = ci, sum end
    end
    if best then
        if not AB.CanCast(name, best, ctx.game_time) then return nil end
        AB.MarkCast(name, best, ctx.game_time)
        bot:Action_UseAbilityOnEntity(ab, best)
        return "IGNITE_CREEP"
    end

    -- Jungle fallback: крепкий нейтрал в 700, прочие условия Combat-helper.
    if Combat.JungleCastAllowed(ctx, 0.5) then
        local neut = Combat.StrongestNeutralInRadius(ctx, 700)
        if neut and not IsMagicImmuneSafe(neut) then
            if not AB.CanCast(name, neut, ctx.game_time) then return nil end
            AB.MarkCast(name, neut, ctx.game_time)
            bot:Action_UseAbilityOnEntity(ab, neut)
            return "IGNITE_NEUT"
        end
    end

    return nil
end

local function tryBloodlust(bot, ctx)
    -- Каст когда есть hero-враг в 1000 → бафф себя или ближайшего ally carry.
    local enemy = ctx.nearest_enemy
    if not enemy or enemy:IsNull() or not enemy:IsAlive() then return nil end
    if not enemy:IsHero() then return nil end
    if ctx.nearest_enemy_dist > 1000 then return nil end

    -- Цель: ally hero в 600 (carry friend); fallback — self.
    local buff_target = bot
    for _, a in ipairs(ctx.nearby_allies or {}) do
        if a and not a:IsNull() and a:IsAlive() and a ~= bot then
            local d = Move.Dist2D(ctx.pos, a:GetLocation())
            if d < 600 then
                buff_target = a
                break
            end
        end
    end

    local ab = bot:GetAbilityInSlot(2)
    if not ab or ab:IsNull() or ab:GetLevel() == 0 or not ab:IsFullyCastable() then return nil end
    local name = ab:GetName() or "ogre_magi_bloodlust"
    if not AB.CanCast(name, buff_target, ctx.game_time) then return nil end
    AB.MarkCast(name, buff_target, ctx.game_time)
    bot:Action_UseAbilityOnEntity(ab, buff_target)
    return "BLOODLUST"
end

-- Multicast — PASSIVE, не кастуем.

function M.Think(bot, ctx)
    if not bot or bot:IsNull() or not bot:IsAlive() then return nil end
    local r
    r = tryFireblast(bot, ctx); if r then return "OGRE:" .. r end
    r = tryIgnite(bot, ctx);    if r then return "OGRE:" .. r end
    r = tryBloodlust(bot, ctx); if r then return "OGRE:" .. r end
    return nil
end

return M
