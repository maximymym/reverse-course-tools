-- util/combat.lua — поиск целей, кастинг скиллов, оценка урона.

local Move = require("util.movement")
local AB   = require("util.anti_ban")

local M = {}

-- Возвращает ближайшего живого врага из списка. nil если никого.
function M.NearestAlive(units, fromPos)
    local best, bestD = nil, 99999
    for _, u in ipairs(units) do
        if u and not u:IsNull() and u:IsAlive() then
            local d = Move.Dist2D(fromPos, u:GetLocation())
            if d < bestD then best, bestD = u, d end
        end
    end
    return best, bestD
end

-- Возвращает слабейшего (по HP%) живого врага из списка.
function M.WeakestAlive(units)
    local best, bestPct = nil, 2.0
    for _, u in ipairs(units) do
        if u and not u:IsNull() and u:IsAlive() then
            local hp, max = u:GetHealth(), u:GetMaxHealth()
            if max > 0 then
                local pct = hp / max
                if pct < bestPct then best, bestPct = u, pct end
            end
        end
    end
    return best, bestPct
end

-- Проверяет что цель — валидная для каста (жива, в радиусе, не immune).
function M.IsGoodCastTarget(bot, target, maxRange)
    if not target or target:IsNull() or not target:IsAlive() then return false end
    local d = Move.Dist2D(bot:GetLocation(), target:GetLocation())
    if d > maxRange or d < 1 then return false end
    -- Если есть status-checks, учитываем magic immunity
    if target.IsMagicImmune and target:IsMagicImmune() then return false end
    if target.IsInvulnerable and target:IsInvulnerable() then return false end
    return true
end

-- ── Behavior bitmask constants (LuaConstants.hpp) ─────────────
local BH_PASSIVE       = 1     -- 1<<0
local BH_NO_TARGET     = 2     -- 1<<1
local BH_TOGGLE        = 4     -- 1<<2
local BH_UNIT_TARGET   = 8     -- 1<<3
local BH_POINT         = 16    -- 1<<4
local BH_AOE           = 32    -- 1<<5
local BH_NOT_LEARNABLE = 64    -- 1<<6
local BH_HIDDEN        = 128   -- 1<<7
local BH_ITEM          = 256   -- 1<<8
local BH_IMMEDIATE     = 512   -- 1<<9
local BH_CHANNELLED    = 2048  -- 1<<11
local BH_AUTOCAST      = 4096  -- 1<<12

-- Lua 5.1 не имеет bitwise — используем арифметический helper.
local function HasBit(mask, bit)
    if not mask or mask == 0 then return false end
    return math.floor(mask / bit) % 2 == 1
end

-- Безопасный геттер: pcall + fallback при отсутствии binding'а.
local function SafeGet(obj, method, fallback)
    if not obj or not obj[method] then return fallback end
    local ok, v = pcall(obj[method], obj)
    if ok and v ~= nil then return v end
    return fallback
end

-- Lead prediction: куда полетит target за cast_point.
-- Если GetVelocity не экспонирован — без lead. Cap на 200 unit (anti-overshoot).
local function PredictLocation(target, cast_point)
    local loc = target:GetLocation()
    if not loc or not cast_point or cast_point < 0.1 then return loc end
    if not target.GetVelocity then return loc end
    local ok, vel = pcall(target.GetVelocity, target)
    if not ok or not vel then return loc end
    local lead_x = (vel.x or 0) * cast_point
    local lead_y = (vel.y or 0) * cast_point
    -- Cap длину
    local lead_d = math.sqrt(lead_x * lead_x + lead_y * lead_y)
    if lead_d > 200 then
        local s = 200 / lead_d
        lead_x, lead_y = lead_x * s, lead_y * s
    end
    return { x = loc.x + lead_x, y = loc.y + lead_y, z = loc.z or 0 }
end

-- ── M.SmartCastAbility ────────────────────────────────────────
-- Generic dispatcher по behavior bitmask. Возвращает (cast: bool, mode: string).
-- ctx: { game_time = number } минимум.
function M.SmartCastAbility(bot, ab, target, ctx)
    if not ab or ab:IsNull() then return false, "NULL" end
    local name = ab:GetName() or ""
    if name == "" or name == "generic_hidden" then return false, "HIDDEN_NAME" end
    if string.find(name, "innate", 1, true) then return false, "INNATE" end
    if string.find(name, "special_bonus", 1, true) then return false, "TALENT" end

    local behavior = SafeGet(ab, "GetBehavior", 0) or 0

    -- Skip non-castable behaviors.
    if HasBit(behavior, BH_PASSIVE)       then return false, "PASSIVE" end
    if HasBit(behavior, BH_HIDDEN)        then return false, "HIDDEN" end
    if HasBit(behavior, BH_NOT_LEARNABLE) then return false, "NOT_LEARNABLE" end
    if HasBit(behavior, BH_CHANNELLED)    then return false, "CHANNELLED" end

    -- Cooldown / mana / уровни.
    local castable_ok, castable = pcall(ab.IsFullyCastable, ab)
    if not castable_ok or not castable then return false, "NOT_READY" end

    local gt = (ctx and ctx.game_time) or 0

    -- Anti-ban CD.
    if not AB.CanCast(name, target, gt) then return false, "CD_AB" end

    -- AUTOCAST handling — toggle ON один раз за матч.
    if HasBit(behavior, BH_AUTOCAST) then
        BotControllerState.autocast_set = BotControllerState.autocast_set or {}
        if BotControllerState.autocast_set[name] ~= true then
            local ok, err = pcall(bot.Action_ToggleAutoCast, bot, ab)
            if ok then
                BotControllerState.autocast_set[name] = true
                AB.MarkCast(name, nil, gt)
                print(string.format("[cast] %s mode=AUTOCAST_ON behavior=0x%x", name, behavior))
                return true, "AUTOCAST_ON"
            else
                print("[cast] ToggleAutoCast failed: " .. tostring(err))
                return false, "AUTOCAST_FAIL"
            end
        end
        -- AUTOCAST уже set — не пытаемся ручной cast (Frost Arrows и пр. сами стреляют).
        return false, "AUTOCAST_ALREADY"
    end

    -- TOGGLE — generic не справится без per-hero context.
    if HasBit(behavior, BH_TOGGLE) then return false, "TOGGLE" end

    -- POINT / AOE.
    if HasBit(behavior, BH_POINT) or HasBit(behavior, BH_AOE) then
        local cast_range = SafeGet(ab, "GetCastRange", 600) or 600
        local cast_point = SafeGet(ab, "GetCastPoint", 0.3) or 0.3
        local cast_loc

        if target and not target:IsNull() and target:IsAlive() then
            local d = Move.Dist2D(bot:GetLocation(), target:GetLocation())
            if d > cast_range + 100 then return false, "OUT_OF_RANGE" end
            cast_loc = PredictLocation(target, cast_point)
        else
            -- Self-AoE fallback (Echo Slam etc).
            if not HasBit(behavior, BH_AOE) then return false, "NO_TARGET_NEEDED" end
            cast_loc = bot:GetLocation()
        end

        local ok, err = pcall(bot.Action_UseAbilityOnLocation, bot, ab, cast_loc)
        if not ok then
            print("[cast] UseAbilityOnLocation failed: " .. tostring(err))
            return false, "POINT_FAIL"
        end
        AB.MarkCast(name, target, gt)
        print(string.format("[cast] %s mode=POINT_AOE behavior=0x%x cd=%.1f",
            name, behavior, SafeGet(ab, "GetCooldownTimeRemaining", 0) or 0))
        return true, "POINT_AOE"
    end

    -- UNIT_TARGET.
    if HasBit(behavior, BH_UNIT_TARGET) then
        if not target or target:IsNull() or not target:IsAlive() then
            return false, "NO_TARGET"
        end
        local cast_range = SafeGet(ab, "GetCastRange", 600) or 600
        local d = Move.Dist2D(bot:GetLocation(), target:GetLocation())
        if d > cast_range + 100 then return false, "OUT_OF_RANGE" end
        if target.IsMagicImmune and target:IsMagicImmune() then return false, "MAGIC_IMMUNE" end
        if target.IsInvulnerable and target:IsInvulnerable() then return false, "INVULNERABLE" end

        local ok, err = pcall(bot.Action_UseAbilityOnEntity, bot, ab, target)
        if not ok then
            print("[cast] UseAbilityOnEntity failed: " .. tostring(err))
            return false, "UNIT_FAIL"
        end
        AB.MarkCast(name, target, gt)
        print(string.format("[cast] %s mode=UNIT_TARGET behavior=0x%x cd=%.1f",
            name, behavior, SafeGet(ab, "GetCooldownTimeRemaining", 0) or 0))
        return true, "UNIT_TARGET"
    end

    -- NO_TARGET / IMMEDIATE.
    if HasBit(behavior, BH_NO_TARGET) or HasBit(behavior, BH_IMMEDIATE) then
        local ok, err = pcall(bot.Action_UseAbility, bot, ab)
        if not ok then
            print("[cast] UseAbility failed: " .. tostring(err))
            return false, "NO_TARGET_FAIL"
        end
        AB.MarkCast(name, nil, gt)
        print(string.format("[cast] %s mode=NO_TARGET behavior=0x%x cd=%.1f",
            name, behavior, SafeGet(ab, "GetCooldownTimeRemaining", 0) or 0))
        return true, "NO_TARGET"
    end

    -- Fallback (behavior == 0 / unknown). Старое поведение: попытка по target.
    if target and not target:IsNull() and target:IsAlive() then
        local ok, err = pcall(bot.Action_UseAbilityOnEntity, bot, ab, target)
        if not ok then return false, "FALLBACK_FAIL" end
        AB.MarkCast(name, target, gt)
        print(string.format("[cast] %s mode=FALLBACK_UNIT behavior=0x%x", name, behavior))
        return true, "FALLBACK_UNIT"
    end

    return false, "UNKNOWN_BEHAVIOR"
end

-- ── M.EnsureAutocasts ────────────────────────────────────────────────
-- Проходит по всем выученным абилкам бота и тогглит AUTOCAST там, где он
-- ещё не на. Per-ability sticky в BotControllerState.autocast_set (тот же
-- словарь что использует SmartCastAbility — общий контракт). Throttle
-- ~30 тиков (≈4с) чтобы не молотить C++ каждый game-tick.
--
-- Цель: даже если hero override (sniper/drow/etc.) не вызвался — например
-- бот в RESPAWN, в фонтане, без enemy targets — autocast всё равно включится
-- сразу как ability:GetLevel() > 0. Без этого Sniper'у Take Aim никогда не
-- toggled, потому что hero override.Think проходит только когда есть
-- ctx.nearest_enemy / ctx.nearby_creeps в нужных состояниях.
--
-- Re-verify: сбрасываем sticky если GetAutoCastState вернул false (autocast
-- слетел после respawn / refresher / debuff). При отсутствии биндинга —
-- timer-based recheck не делаем здесь (hero-override это уже делает на
-- per-ability granularity, см. tryTakeAim в heroes/sniper.lua).
function M.EnsureAutocasts(bot, ctx)
    if not bot or bot:IsNull() then return end
    BotControllerState.autocast_set        = BotControllerState.autocast_set or {}
    BotControllerState.autocast_check_tick = BotControllerState.autocast_check_tick or -999

    local tick = (ctx and ctx.tick) or BotControllerState.tick or 0
    if (tick - BotControllerState.autocast_check_tick) < 30 then return end
    BotControllerState.autocast_check_tick = tick

    local gt = (ctx and ctx.game_time) or 0

    for slot = 0, 5 do
        local ab = bot:GetAbilityInSlot(slot)
        if ab and not ab:IsNull() then
            local lvl_ok, lvl = pcall(ab.GetLevel, ab)
            local level = (lvl_ok and lvl) or 0
            if level > 0 then
                local name     = SafeGet(ab, "GetName", "") or ""
                local behavior = SafeGet(ab, "GetBehavior", 0) or 0

                -- Skip non-autocast / hidden / talents / non-learnable.
                if name ~= ""
                   and name ~= "generic_hidden"
                   and not string.find(name, "special_bonus", 1, true)
                   and not string.find(name, "innate", 1, true)
                   and HasBit(behavior, BH_AUTOCAST)
                   and not HasBit(behavior, BH_HIDDEN)
                   and not HasBit(behavior, BH_NOT_LEARNABLE)
                then
                    -- Re-verify, если есть биндинг.
                    if BotControllerState.autocast_set[name] == true then
                        if ab.GetAutoCastState then
                            local sok, state = pcall(ab.GetAutoCastState, ab)
                            if sok and state == false then
                                print(string.format(
                                    "[ensure_autocast] %s slot=%d state=false → reset sticky (gt=%.1f)",
                                    name, slot, gt))
                                BotControllerState.autocast_set[name] = nil
                            end
                        end
                    end

                    if BotControllerState.autocast_set[name] ~= true then
                        local ok, err = pcall(bot.Action_ToggleAutoCast, bot, ab)
                        if ok then
                            BotControllerState.autocast_set[name] = true
                            print(string.format(
                                "[ensure_autocast] %s slot=%d behavior=0x%x toggled "
                                .. "lvl=%d tick=%d gt=%.1f",
                                name, slot, behavior, level, tick, gt))
                            print(string.format(
                                "[cast] %s mode=AUTOCAST_ON ok=true source=ensure_autocast",
                                name))
                        else
                            print(string.format(
                                "[ensure_autocast] %s slot=%d FAIL: %s",
                                name, slot, tostring(err)))
                        end
                    end
                end
            end
        end
    end
end

-- Пытается скастовать любой готовый скилл бота по target. Соблюдает cast_cooldown.
-- Возвращает (bool, name, mode) — сработало ли, имя скилла, mode.
-- Backward compat: signature TryCastAnyAbility(bot, target, maxDist, gameTime [, ctx_opt])
-- ctx_opt опционален; при его отсутствии создаём stub с game_time = gameTime.
function M.TryCastAnyAbility(bot, target, maxDist, gameTime, ctx_opt)
    -- Target check — IsGoodCastTarget оставляем для backward compat.
    -- Но: для AOE/no-target target может быть nil → IsGoodCastTarget false → bail.
    -- Caller'ы (lane_farm/jungle_farm) всегда передают live target, поэтому
    -- сохраняем гейт чтобы не ломать их логику "только если есть враг".
    if not M.IsGoodCastTarget(bot, target, maxDist) then return false end

    local ctx = ctx_opt
    if not ctx then ctx = { game_time = gameTime } end
    -- Если ctx есть, но не имеет game_time — добавим.
    if not ctx.game_time then ctx.game_time = gameTime end

    -- Уровень бота для skip ulta при <6.
    local lvl = SafeGet(bot, "GetLevel", 1) or 1

    for i = 0, 5 do
        local ab = bot:GetAbilityInSlot(i)
        if ab and not ab:IsNull() then
            local name = ab:GetName() or ""
            -- Slot 5 = ulta; обычно требует уровня 6.
            -- Slot 3,4 это в новой dota — talents/extra; SmartCast и так фильтрует
            -- по NOT_LEARNABLE/special_bonus, поэтому extra slot-skip не нужен.
            local skip = false
            if i == 5 and lvl < 6 and SafeGet(ab, "GetLevel", 0) == 0 then
                skip = true
            end
            if not skip then
                local ok, mode = M.SmartCastAbility(bot, ab, target, ctx)
                if ok then return true, name, mode end
            end
        end
    end
    return false
end

-- ── Jungle-skill helpers ─────────────────────────────────────────────
-- Условия opt-in jungle-каста: бот в JUNGLE_FARM, видит нейтралов, нет
-- enemy heroes рядом, mana в запасе после каста. Ult-skip параметр для
-- финишер-ультов (Lion Finger, Lich Chain Frost, Viper Strike, etc.) —
-- их трата на нейтрала запрещена.
function M.JungleCastAllowed(ctx, mana_floor)
    if not ctx then return false end
    if ctx.fsm_state ~= "JUNGLE_FARM" then return false end
    if not ctx.neutrals or #ctx.neutrals == 0 then return false end
    if ctx.nearby_enemies and #ctx.nearby_enemies > 0 then return false end
    local floor = mana_floor or 0.5
    if (ctx.mana_pct or 0) < floor then return false end
    return true
end

-- Самый "крепкий" живой нейтрал в радиусе R от bot.pos. Для UNIT_TARGET
-- скиллов — кастуем в большого/центрального крипа кампа, чтобы не сжечь
-- mana на satyr-mini'ка с 30 hp.
function M.StrongestNeutralInRadius(ctx, radius)
    if not ctx or not ctx.neutrals then return nil end
    local r = radius or 800
    local best, bestMax = nil, -1
    for _, n in ipairs(ctx.neutrals) do
        if n and not n:IsNull() and n:IsAlive() then
            local p = n:GetLocation()
            local dx, dy = (ctx.pos.x or 0) - (p.x or 0), (ctx.pos.y or 0) - (p.y or 0)
            local dist = math.sqrt(dx * dx + dy * dy)
            if dist <= r then
                local mx = SafeGet(n, "GetMaxHealth", 0) or 0
                if mx > bestMax then best, bestMax = n, mx end
            end
        end
    end
    return best
end

-- Centroid живых нейтралов в радиусе R — для POINT/AOE скиллов.
function M.NeutralsCentroidInRadius(ctx, radius)
    if not ctx or not ctx.neutrals then return nil, 0 end
    local r = radius or 800
    local sx, sy, sz, n = 0, 0, 0, 0
    for _, u in ipairs(ctx.neutrals) do
        if u and not u:IsNull() and u:IsAlive() then
            local p = u:GetLocation()
            local dx, dy = (ctx.pos.x or 0) - (p.x or 0), (ctx.pos.y or 0) - (p.y or 0)
            local dist = math.sqrt(dx * dx + dy * dy)
            if dist <= r then
                sx = sx + (p.x or 0)
                sy = sy + (p.y or 0)
                sz = sz + (p.z or 0)
                n  = n + 1
            end
        end
    end
    if n == 0 then return nil, 0 end
    return { x = sx / n, y = sy / n, z = sz / n }, n
end

-- Оценка урона за один auto attack (базовая, без крита/тени).
function M.EstimateAutoDamage(attacker, target)
    if not attacker or not target then return 0 end
    local atk = attacker:GetAttackDamage() or 0
    if target.GetActualIncomingDamage then
        return target:GetActualIncomingDamage(atk, 1) -- DAMAGE_TYPE_PHYSICAL
    end
    return atk * 0.7  -- rough armor mitigation
end

-- Считает "можно ли добить" — бот атакует target который с HP < damage-per-hit.
function M.CanLastHit(bot, target)
    if not target or target:IsNull() or not target:IsAlive() then return false end
    local dmg = M.EstimateAutoDamage(bot, target)
    return target:GetHealth() <= dmg * 1.1
end

-- Dedup-обёртка над bot:Action_AttackUnit.
-- ПРОБЛЕМА: каждый вызов Action_AttackUnit → OrderAttackTarget в C++ пере-выдаёт
-- ордер, что **перезапускает wind-up анимацию** — бот делает бесконечные замахи
-- без удара. Sniper особенно страдает: BAT=1.7с, длинный wind-up + projectile.
--
-- Identity: сравниваем UnitHandle через `==` — sol2 пробрасывает C++
-- `UnitHandle::operator==(entityIndex == other.entityIndex)` (LuaUnitProxy.hpp:84)
-- через __eq, поэтому `last_handle == new_handle` истинно для разных Lua-обёрток
-- одной и той же сущности. Раньше пробовали `target.entityIndex` напрямую — это
-- поле НЕ экспонировано в sol2, всегда возвращало nil → idx=-1 const → dedup
-- сваливался на чистый таймер.
--
-- Threshold 30 тиков ≈ 4с при 7.5Hz — длиннее любого BAT с любыми atk-speed
-- баффами; покрывает полный attack cycle (windup + backswing + projectile).
function M.AttackUnitOnce(bot, target, ctx)
    if not target or target:IsNull() then return false end
    local tick = (ctx and ctx.tick) or 0
    local last = BotControllerState.last_attack_target_handle
    local last_tick = BotControllerState.last_attack_tick or -999
    local IDLE_THRESHOLD = 30  -- ~4с при 7.5Hz — покрывает BAT+projectile с буфером
    local same_target = last and not last:IsNull() and last == target

    -- Out-of-range fallback: если target дальше attack_range, выдать MOVE к
    -- target.pos. Чистый Action_AttackUnit формально должен заставить бота
    -- идти, но по живым логам бот часто стоит на месте 100+с (Sniper в
    -- jungle: target=87, AttackUnit каждые 4с, бот не двигается, neut=3
    -- стабильно). Возможно order silently отменяется (tower aggro check,
    -- LOS, Source 2 quirks). Чтобы не зависнуть — если range > attack +
    -- буфер, делаем MoveToOnce(target.pos). Когда дойдём — следующий тик
    -- AttackUnit сработает.
    local atkRange = SafeGet(bot, "GetAttackRange", 600)
    local pos = bot:GetLocation()
    local tloc = target:GetLocation()
    local dx = (pos.x or 0) - (tloc.x or 0)
    local dy = (pos.y or 0) - (tloc.y or 0)
    local d  = math.sqrt(dx * dx + dy * dy)
    if d > (atkRange + 75) then
        -- За пределами range — идём к target. Не блокируем dedup tracker:
        -- когда дойдём, dedup window истечёт сам, AttackUnit re-issue'нётся.
        M.MoveToOnce(bot, tloc, ctx)
        return true
    end

    if same_target and (tick - last_tick) < IDLE_THRESHOLD then
        return false  -- уже атакуем этого, ордер не пере-выдаём
    end
    BotControllerState.last_attack_target_handle = target
    BotControllerState.last_attack_tick          = tick
    bot:Action_AttackUnit(target)
    return true
end

-- Сброс — когда бот меняет state, target unset.
function M.ResetAttackOrder()
    BotControllerState.last_attack_target_handle = nil
    BotControllerState.last_attack_tick          = nil
end

-- Dedup для Action_AttackMove(pos). Re-issue только при значительной смене
-- destination (>200 unit) или каждые 30 тиков (~4с). Логика та же что у
-- AttackUnitOnce: сырой re-issue каждый тик отменяет auto-attack windup'ы.
function M.AttackMoveOnce(bot, pos, ctx)
    if not bot or not pos then return false end
    local tick     = (ctx and ctx.tick) or 0
    local last_pos = BotControllerState.last_attackmove_pos
    local last_t   = BotControllerState.last_attackmove_tick or -999
    local IDLE_THRESHOLD = 30
    local same = last_pos
        and math.abs(last_pos.x - pos.x) < 200
        and math.abs(last_pos.y - pos.y) < 200
    if same and (tick - last_t) < IDLE_THRESHOLD then
        return false
    end
    BotControllerState.last_attackmove_pos  = {x = pos.x, y = pos.y}
    BotControllerState.last_attackmove_tick = tick
    bot:Action_AttackMove(pos)
    return true
end

function M.ResetAttackMove()
    BotControllerState.last_attackmove_pos  = nil
    BotControllerState.last_attackmove_tick = nil
end

-- Dedup для Action_MoveToLocation. Та же логика что AttackMoveOnce, но для
-- pure MOVE (OrderType 1). Используется jungle_farm.lua для надёжного
-- перехода между кампами без AttackMove-distractions.
function M.MoveToOnce(bot, pos, ctx)
    if not bot or not pos then return false end
    local tick     = (ctx and ctx.tick) or 0
    local last_pos = BotControllerState.last_moveto_pos
    local last_t   = BotControllerState.last_moveto_tick or -999
    local IDLE_THRESHOLD = 30
    local same = last_pos
        and math.abs(last_pos.x - pos.x) < 200
        and math.abs(last_pos.y - pos.y) < 200
    if same and (tick - last_t) < IDLE_THRESHOLD then
        return false
    end
    BotControllerState.last_moveto_pos  = {x = pos.x, y = pos.y}
    BotControllerState.last_moveto_tick = tick
    bot:Action_MoveToLocation(pos)
    return true
end

function M.ResetMoveTo()
    BotControllerState.last_moveto_pos  = nil
    BotControllerState.last_moveto_tick = nil
end

return M
