-- states/push.lua — мои крипы под enemy tower и врагов нет в 1500.
-- Атакуем tower или enemy creep'ов под ней.

local M = { name = "PUSH" }
local Combat = require("util.combat")

function M.Run(bot, ctx)
    -- 1. Враг hero рядом — атакуем (не tower-dive). Dedup чтобы не отменять wind-up.
    if ctx.nearest_enemy and ctx.nearest_enemy_dist < 800 then
        Combat.AttackUnitOnce(bot, ctx.nearest_enemy, ctx)
        return "ATTACK_HERO"
    end

    -- 2. Есть tower — атакуем
    if ctx.nearby_towers and #ctx.nearby_towers > 0 then
        for _, t in ipairs(ctx.nearby_towers) do
            if t and not t:IsNull() and t:IsAlive() then
                Combat.AttackUnitOnce(bot, t, ctx)
                return "ATTACK_TOWER"
            end
        end
    end

    -- 3. Tower нет в radius — добиваем enemy creep'ов под башней
    local creep = Combat.NearestAlive(ctx.nearby_creeps, ctx.pos)
    if creep then
        Combat.AttackUnitOnce(bot, creep, ctx)
        return "ATTACK_CREEP"
    end

    -- 4. Никого нет — attack-move к push waypoint
    local pushAmount = (GetLaneAmountForBot and GetLaneAmountForBot(ctx.lane, ctx.team, "push")) or 0.6
    local pushPos = GetLocationAlongLane(ctx.lane, pushAmount)
    bot:Action_AttackMove(pushPos)
    return "WALK_PUSH"
end

return M
