-- states/side_bait.lua — LOSE-команда: расход по сайдам, не помогаем команде.
--
-- Активный при team_strategy=="LOSE" + game_time >= strategy_enable_time.
-- Per-bot lane assignment по ctx.team_slot (PlayerID % 5):
--   slot 0,1 → top (lane 1)
--   slot 2,3 → bot (lane 3)
--   slot 4   → jungle (safe-side нейтралы)
--
-- Бот сидит на safe_pos своей assigned lane, не идёт в mid, не пушит волну.
-- Если враг ближе side_bait_retreat_dist — retreat к фонтану.

local M = { name = "SIDE_BAIT" }

local vec = require("util.vec")
local Dist2D = vec.Dist2D

function M.Run(bot, ctx)
    local C = BotControllerConfig or {}

    -- 1. Враг рядом → фонтан (NEVER fight в LOSE — мы саботируем).
    local retreatDist = C.side_bait_retreat_dist or 1200
    if ctx.nearest_enemy and ctx.nearest_enemy_dist < retreatDist then
        bot:Action_MoveToLocation(ctx.fountain)
        return "BAIT_RETREAT"
    end

    -- 2. JUNGLE bot (slot 4) — фарм безопасный neutral camp своей стороны.
    --    Идём к nearest own camp, если он есть. Иначе fallback к safe_pos
    --    своей default lane (можно сидеть в base — лучше чем mid push).
    if ctx.side_bait_kind == "jungle" then
        if ctx.nearest_own_camp_pos and (ctx.nearest_own_camp_dist or 99999) < 5000 then
            -- Если уже на камп'е и есть нейтралы — attack-move (бот сам выберет цель).
            if ctx.neutrals and #ctx.neutrals > 0 then
                bot:Action_AttackMove(ctx.nearest_own_camp_pos)
                return "BAIT_JUNGLE_FARM"
            end
            -- Просто идём к camp'у.
            bot:Action_MoveToLocation(ctx.nearest_own_camp_pos)
            return "BAIT_JUNGLE_GOTO"
        end
        -- Fallback: к фонтану. Лучше idle в базе чем help команде.
        bot:Action_MoveToLocation(ctx.fountain)
        return "BAIT_IDLE_FOUNTAIN"
    end

    -- 3. LANE bot (slot 0..3) — стоим на safe_pos assigned lane.
    local lane = ctx.side_bait_lane or 1
    local safeAmount = C.side_bait_safe_amount or 0.2
    -- GetLaneAmountForBot учитывает team — "safe" даёт ближе к своему фонтану.
    local actualAmount = (GetLaneAmountForBot and GetLaneAmountForBot(lane, ctx.team, "safe"))
                         or safeAmount
    local laneSafePos = GetLocationAlongLane(lane, actualAmount)

    -- Если уже на safe_pos с tolerance — idle (просто стоим, ничего не делаем).
    local distToSafe = Dist2D(ctx.pos, laneSafePos)
    if distToSafe < 250 then
        -- Hold position: даём Stop-ордер вместо AttackMove чтобы не пушить волну.
        -- Action_MoveToLocation на текущую pos = effective stop.
        bot:Action_MoveToLocation(ctx.pos)
        return "BAIT_HOLD_SAFE"
    end

    -- Идём к safe_pos.
    bot:Action_MoveToLocation(laneSafePos)
    return "BAIT_GOTO_SAFE"
end

return M
