-- util/movement.lua — helpers для перемещения, stuck detection, tower avoidance.

local M = {}

function M.Dist2D(a, b)
    if not a or not b then return 99999 end
    local dx = (a.x or 0) - (b.x or 0)
    local dy = (a.y or 0) - (b.y or 0)
    return math.sqrt(dx * dx + dy * dy)
end

function M.Dist3D(a, b)
    if not a or not b then return 99999 end
    local dx = (a.x or 0) - (b.x or 0)
    local dy = (a.y or 0) - (b.y or 0)
    local dz = (a.z or 0) - (b.z or 0)
    return math.sqrt(dx * dx + dy * dy + dz * dz)
end

-- Обновляет last_pos / last_move_tick. Возвращает true если бот zastrял > stuck_ticks.
function M.IsStuck(bot, ctx)
    local C = BotControllerConfig
    local tick = ctx.tick
    local pdx = ctx.pos.x - (BotControllerState.last_pos_x or 0)
    local pdy = ctx.pos.y - (BotControllerState.last_pos_y or 0)
    if math.sqrt(pdx * pdx + pdy * pdy) > 50.0 then
        BotControllerState.last_pos_x = ctx.pos.x
        BotControllerState.last_pos_y = ctx.pos.y
        BotControllerState.last_move_tick = tick
    end
    return (tick - (BotControllerState.last_move_tick or 0)) > C.stuck_ticks
end

-- Проверка что локация не слишком близко к enemy tower.
function M.IsLocationSafe(loc, radius)
    radius = radius or 700
    if IsLocationNearEnemyTower then
        return not IsLocationNearEnemyTower(loc, radius)
    end
    return true  -- conservative fallback — считаем safe
end

-- Безопасный move: если target рядом с вражеской башней, делаем attack-move
-- вместо straight move, чтобы бот не ходил прямо под турель.
function M.MoveToLocationSafe(bot, loc)
    if not M.IsLocationSafe(loc) then
        bot:Action_AttackMove(loc)
    else
        bot:Action_MoveToLocation(loc)
    end
end

return M
