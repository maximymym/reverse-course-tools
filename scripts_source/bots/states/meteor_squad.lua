-- states/meteor_squad.lua — координированный mid push с meteor_hammer cast.
--
-- Активный при team_strategy=="WIN" + game_time >= strategy_enable_time.
-- Все 5 WIN-ботов rally в mid lane перед enemy T1, util/items.lua сам
-- выстрелит meteor_hammer когда tower в 500r и условия выполнены
-- (cooldown ready, hp > 0.5, no enemy heroes in 1200, <2 creeps in 600).
--
-- Этот handler отвечает ТОЛЬКО за движение — cast делает UseActiveItems
-- dispatcher до handler.Run (см. bot_controller.lua:561, "ITEM:..." return
-- skips handler.Run чтобы не отменить channel).

local M = { name = "METEOR_SQUAD" }

local vec = require("util.vec")
local Dist2D = vec.Dist2D

function M.Run(bot, ctx)
    local C = BotControllerConfig or {}
    local lane = C.meteor_squad_lane or 2  -- mid
    local amount = C.meteor_squad_amount or 0.6

    -- Целевая точка: "push" position на mid lane (между T1 и T2 врага).
    -- GetLaneAmountForBot учитывает team — для radiant amount=0.6 означает
    -- "60% по линии от своей базы".
    local pushAmount = (GetLaneAmountForBot and GetLaneAmountForBot(lane, ctx.team, "push"))
                       or amount
    local midPos = GetLocationAlongLane(lane, pushAmount)

    -- Если ближайший союзник дальше rally_radius — сначала кластеруемся к нему
    -- (centroid alignment). Это гарантирует что 5 ботов придут одновременно,
    -- meteor cast случится одновременно — tower быстро падает.
    local rallyRadius = C.meteor_squad_rally_radius or 600
    if ctx.nearby_allies and #ctx.nearby_allies > 0 then
        -- Centroid живых союзников
        local cx, cy, cn = 0, 0, 0
        for _, a in ipairs(ctx.nearby_allies) do
            if a and not a:IsNull() and a.IsAlive and a:IsAlive() then
                local al = a:GetLocation()
                if al then
                    cx = cx + al.x
                    cy = cy + al.y
                    cn = cn + 1
                end
            end
        end
        if cn >= 1 then
            cx, cy = cx / cn, cy / cn
            local distToCentroid = math.sqrt((ctx.pos.x - cx)^2 + (ctx.pos.y - cy)^2)
            -- Если кластер сильно сместился вперёд (ближе к врагу чем midPos) —
            -- идём к нему. Иначе midPos = anchor.
            if distToCentroid > rallyRadius then
                bot:Action_MoveToLocation(Vector(cx, cy, ctx.pos.z or 128))
                return "RALLY_TO_CLUSTER"
            end
        end
    end

    -- Если в радиусе атаки уже есть enemy tower — даём AttackMove на её локацию.
    -- meteor cast в это время произойдёт через UseActiveItems (приоритет до
    -- handler.Run в bot_controller.lua).
    if ctx.nearby_towers and #ctx.nearby_towers > 0 then
        for _, t in ipairs(ctx.nearby_towers) do
            if t and not t:IsNull() and t.IsAlive and t:IsAlive() then
                bot:Action_AttackUnit(t, false)
                return "ATTACK_TOWER_MID"
            end
        end
    end

    -- Враг hero рядом — атакуем, но не tower-dive: меньше 800r и без meteor
    -- channel risk. UseActiveItems с meteor проверяет nearest_enemy_dist<1200
    -- → cast skip когда враг рядом, AttackUnit нормально работает.
    if ctx.nearest_enemy and ctx.nearest_enemy_dist < 600 then
        bot:Action_AttackUnit(ctx.nearest_enemy, false)
        return "ATTACK_ENEMY_HERO"
    end

    -- Просто attack-move к mid push position.
    bot:Action_AttackMove(midPos)
    return "WALK_TO_MID"
end

return M
