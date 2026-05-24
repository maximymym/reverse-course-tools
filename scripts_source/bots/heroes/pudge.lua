-- heroes/pudge.lua — пример per-hero override.
-- Активируется только если в heroes/init.lua раскомментирован `heroes["pudge"] = ...`.
--
-- Логика: если есть враг в 1000 с HP<40%, пытаемся hook (Q=slot 0).
-- Если его нет — возвращаем nil и dispatcher работает стандартно.

local M = {}

local Move = require("util.movement")
local AB   = require("util.anti_ban")

function M.Think(bot, ctx)
    -- Ищем low-HP врага в 1100 радиусе (hook range = 1300, но стреляем чуть ближе)
    local best, bestPct = nil, 1.0
    for _, h in ipairs(ctx.nearby_enemies) do
        if h and h:IsAlive() then
            local hp, maxHp = h:GetHealth(), h:GetMaxHealth()
            local pct = (maxHp > 0) and hp / maxHp or 1
            local d = Move.Dist2D(ctx.pos, h:GetLocation())
            if d < 1100 and pct < 0.4 and pct < bestPct then
                best, bestPct = h, pct
            end
        end
    end

    if not best then return nil end  -- дефолтный dispatcher работает

    local hook = bot:GetAbilityInSlot(0)  -- slot 0 = Meat Hook
    if not hook or hook:IsNull() or not hook:IsFullyCastable() then
        return nil  -- hook на кулдауне → пусть lane_farm работает
    end

    local gt = ctx.game_time
    if not AB.CanCast("pudge_meat_hook", best, gt) then return nil end
    AB.MarkCast("pudge_meat_hook", best, gt)

    bot:Action_UseAbilityOnLocation(hook, best:GetLocation())
    -- Hook требует windup ~0.5с + projectile flight; чтобы FSM не выдал
    -- AttackUnit/MoveTo и не отменил cast — блокирующий return.
    return "BLOCK:HOOK_LOW_HP"
end

return M
