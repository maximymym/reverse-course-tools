-- states/respawn.lua — после смерти. TP scroll если есть, иначе attack-move к safe_pos.
-- Exit transitions (в fsm.lua):
--   → LANE_FARM при reached_lane_with_creeps
--   → LANE_WAIT при reached_lane_no_creeps

local M = { name = "RESPAWN" }
local AB     = require("util.anti_ban")
local vec    = require("util.vec")
local Combat = require("util.combat")
local Dist2D = vec.Dist2D

function M.Run(bot, ctx)
    local C = BotControllerConfig

    -- TP scroll если бот далеко и cooldown истёк
    local d = Dist2D(ctx.pos, ctx.lane_front)
    if d > (C.far_lane_dist or 5000.0)
       and (ctx.game_time - (BotControllerState.last_tp_time or -999)) > (C.tp_cooldown or 80.0) then
        local tpSlot = bot:FindItemSlot("item_tpscroll")
        if tpSlot and tpSlot >= 0 then
            local tp = bot:GetItemInSlot(tpSlot)
            if tp and not tp:IsNull() and tp:IsFullyCastable() then
                bot:Action_UseAbilityOnLocation(tp, ctx.lane_front)
                BotControllerState.last_tp_time = ctx.game_time
                return "TP_LANE"
            end
        end
    end

    -- attack-move к safe_pos (а не lane_front: на респе крипов ещё нет, под tower не лезем).
    -- Через Combat.AttackMoveOnce — dedup 30 ticks / 200 unit. Голый re-issue каждый
    -- тик отменял auto-attack windup'ы и Viper никогда не выходил из фонтан-bubble.
    Combat.AttackMoveOnce(bot, ctx.safe_pos, ctx)
    return "WALK_TO_SAFE"
end

return M
