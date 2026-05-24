-- states/lane_farm.lua — атака врагов/крипов на линии + push с крипами.
-- Ключевое отличие от старого modes/lane_farm.lua: вместо "HOLD" если в 300
-- от centroid'а — идём ВПЕРЁД с крипами (push на 300 unit'ов в сторону enemy base).

local M = { name = "LANE_FARM" }
local Combat = require("util.combat")
local vec    = require("util.vec")

function M.Run(bot, ctx)
    local C = BotControllerConfig
    local gt = ctx.game_time

    -- 1. Скилл в close enemy hero
    if ctx.nearest_enemy and ctx.nearest_enemy_dist < (C.cast_radius or 800.0) then
        local ok, name = Combat.TryCastAnyAbility(bot, ctx.nearest_enemy,
                                                   C.cast_radius or 800.0, gt)
        if ok then return "CAST_" .. (name or "?") end
    end

    -- 2. Атаковать hero в attack range
    if ctx.nearest_enemy and ctx.nearest_enemy_dist < (C.attack_radius or 1200.0) then
        Combat.AttackUnitOnce(bot, ctx.nearest_enemy, ctx)
        return "ATTACK_HERO"
    end

    -- 3. Last-hit / атака enemy creep — со stable target tracking + dedup
    -- ордеров. Раньше каждый тик NearestAlive возвращал разного крипа +
    -- bot:Action_AttackUnit пере-выдавался → wind-up анимации перезапускались
    -- бесконечно (STUCK 730 ticks для Sniper). Теперь target stable + ордер
    -- выдаётся только при смене target или потере (10 тиков idle).
    do
        local cur = BotControllerState.lane_attack_target
        local creep
        if cur and not cur:IsNull() and cur:IsAlive() then
            local d = vec.Dist2D(ctx.pos, cur:GetLocation())
            if d < (C.attack_radius or 1200.0) then
                creep = cur
            end
        end
        if not creep then
            creep = Combat.NearestAlive(ctx.nearby_creeps, ctx.pos)
            BotControllerState.lane_attack_target = creep
        end
        if creep then
            Combat.AttackUnitOnce(bot, creep, ctx)
            return "ATTACK_CREEP"
        else
            BotControllerState.lane_attack_target = nil
            Combat.ResetAttackOrder()
        end
    end

    -- 4. Целей в range нет — идём ВПЕРЁД с крипами (300 unit ahead of friendly centroid).
    --    Лечит главный баг старого кода "HOLD если в 300 от centroid'а".
    if ctx.has_friendly_creeps then
        local centroid, n = vec.Centroid(ctx.friendly_creeps)
        if centroid then
            -- Если centroid и lane_front совпадают → вектор вырожден,
            -- Extend2D вернёт centroid — и мы правильно стоим рядом.
            local target = vec.Extend2D(centroid, ctx.lane_front, 300)
            bot:Action_AttackMove(target)
            if vec.Dist2D(centroid, ctx.lane_front) < 1 then
                return "HOLD_AT_CREEPS"
            end
            return "PUSH_WITH_CREEPS"
        end
    end

    -- 5. Крипов нет — attack-move к waypoint
    bot:Action_AttackMove(ctx.lane_front)
    return "MOVE_TO_LANE"
end

return M
