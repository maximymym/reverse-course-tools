-- states/lane_wait.lua — на линии у своей T1, ждём wave.
-- waitPos: amount=0.28 для Radiant / 0.72 для Dire (между T1 и T2 с safe-стороны).

local M = { name = "LANE_WAIT" }
local vec = require("util.vec")
local Combat = require("util.combat")
local Dist2D = vec.Dist2D

function M.Run(bot, ctx)
    -- waitPos — pre-T1 на стороне базы. Базовый amount: Radiant 0.28 / Dire 0.72.
    -- Forward scout: если no_creeps_streak долгий (линия push'ed противником,
    -- крипы умирают далеко >2000 от waitPos → бот их не видит, lane_front броски),
    -- сдвигаем waitAmount к мидлу — бот сам пойдёт навстречу линии. Шаги:
    -- 0 / +0.10 / +0.15 / +0.17.
    -- Cap: НЕ ПЕРЕСЕКАЕМ midline (0.5). Раньше cap был 0.55/0.45 — для top/bot
    -- лайнов это амаунт между чужими T1 и T2 (top 0.55 ≈ (-200, 6250), а dire
    -- top T2 на (-3700, 4900)) → бот стоял под чужой T2 если streak долгий.
    -- Теперь max=0.45 для radiant, min=0.55 для dire — никогда не пересекаем
    -- среднюю точку лайна. Tower-danger гейт в fsm.lua подстрахует если
    -- что-то всё равно вытолкнет бота за midline.
    local baseAmount = (ctx.team == 2) and 0.28 or 0.72
    local streak = ctx.no_creeps_streak or 0
    local pushOffset = 0.0
    if streak > 300 then     pushOffset = 0.17
    elseif streak > 200 then pushOffset = 0.15
    elseif streak > 100 then pushOffset = 0.10
    end

    local waitAmount
    if ctx.team == 2 then
        waitAmount = math.min(0.45, baseAmount + pushOffset)
    else
        waitAmount = math.max(0.55, baseAmount - pushOffset)
    end
    local waitPos = GetLocationAlongLane(ctx.lane, waitAmount)

    -- Diagnostic: сменился bucket pushOffset'а или waitAmount → лог.
    local prevBucket = BotControllerState._lane_wait_bucket
    local curBucket  = string.format("%.2f", waitAmount)
    if prevBucket ~= curBucket then
        BotControllerState._lane_wait_bucket = curBucket
        print(string.format(
            "[lane_wait] waitAmount=%.2f base=%.2f pushOff=%.2f streak=%d "
            .. "waitPos=(%d,%d) pos=(%d,%d) team=%d lane=%d tower_danger=%s f_creeps=%s",
            waitAmount, baseAmount, pushOffset, streak,
            math.floor(waitPos.x), math.floor(waitPos.y),
            math.floor(ctx.pos.x), math.floor(ctx.pos.y),
            ctx.team or 0, ctx.lane or 0,
            tostring(ctx.near_enemy_tower_danger),
            tostring(ctx.has_friendly_creeps)))
    end

    -- Если враг в attack range — отвечаем (last-hit'em если получится)
    if ctx.nearest_enemy and ctx.nearest_enemy_dist < 600 then
        Combat.AttackUnitOnce(bot, ctx.nearest_enemy, ctx)
        return "ATTACK_HARASSER"
    end

    -- Уже на waitPos — стоим
    if Dist2D(ctx.pos, waitPos) < 200 then
        bot:Action_MoveToLocation(waitPos)
        return "WAITING"
    end

    -- Идём на waitPos (возможно — push'ed forward по streak)
    bot:Action_MoveToLocation(waitPos)
    return (pushOffset > 0) and "SCOUT_FORWARD" or "MOVE_TO_WAIT"
end

return M
