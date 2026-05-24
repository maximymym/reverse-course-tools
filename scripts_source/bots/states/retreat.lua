-- states/retreat.lua — отступление. Порт логики из старого modes/retreat.Think.
-- CRITICAL → фонтан (или safe_pos если фонтан через tower); иначе → safe_pos.

local M = { name = "RETREAT" }
local vec = require("util.vec")
local Items = require("util.items")
local Combat = require("util.combat")
local Dist2D = vec.Dist2D

function M.Run(bot, ctx)
    local C = BotControllerConfig

    -- Tango heal: если в инвентаре есть, и не на critical (на critical уже не
    -- спасёт — heal 115/16s, лучше бежать на фонтан). Caster ставит ордер на
    -- дерево — бот сам подойдёт в cast range. Если дерева совсем нет рядом —
    -- skip, продолжаем стандартный retreat.
    if (ctx.hp_pct or 1) < 0.85
       and (ctx.hp_pct or 1) >= (C.critical_hp or 0.15) then
        local ok, label, treeLoc, dist = Items.TryEatTango(bot, ctx)
        -- Diagnostic: лог почему НЕ сработало — раз в 30 тиков пока в RETREAT.
        -- Без этого "тихо" (нет EAT_TANGO=ok) мы не знаем какая ветка вернула
        -- false: NO_TANGO (нет в инвентаре), TANGO_CD (rate-limit), NO_TREE
        -- (C++ кэш пуст?), TREE_FAR, TANGO_NOT_CASTABLE.
        if not ok and (BotControllerState._tango_diag_tick ~= ctx.tick) and
           ((ctx.tick or 0) - (BotControllerState._tango_diag_last or -999) >= 30) then
            BotControllerState._tango_diag_last = ctx.tick
            BotControllerState._tango_diag_tick = ctx.tick
            print(string.format(
                "[retreat] TANGO_SKIP reason=%s hp=%.2f pos=(%d,%d) treeFar=%d gt=%.1f",
                tostring(label or "?"), ctx.hp_pct or 0,
                math.floor(ctx.pos.x), math.floor(ctx.pos.y),
                math.floor(dist or 0), ctx.game_time or 0))
        end
        if ok then
            -- Tango cast выдан. Возвращаем — на следующем тике бот доест и
            -- продолжит retreat, no_tango branch сработает естественно.
            return label
        elseif label == "TREE_FAR" and treeLoc then
            -- Дерево есть в 600, но дальше cast range. Сделаем небольшой
            -- detour — двинемся к дереву (по пути домой если оно "по пути",
            -- иначе caller продолжит обычный retreat). Простая heuristic:
            -- если дерево ближе к фонтану чем мы — идём к нему; иначе skip.
            local dMeFountain = Dist2D(ctx.pos, ctx.fountain)
            local dTreeFountain = Dist2D(treeLoc, ctx.fountain)
            if dTreeFountain < dMeFountain - 100 then
                Combat.MoveToOnce(bot, treeLoc, ctx)
                if BotControllerState._tango_detour_logged ~= ctx.tick then
                    BotControllerState._tango_detour_logged = ctx.tick
                    print(string.format(
                        "[retreat] TANGO_DETOUR pos=(%d,%d) tree=(%d,%d) dist=%d hp=%.2f",
                        math.floor(ctx.pos.x), math.floor(ctx.pos.y),
                        math.floor(treeLoc.x), math.floor(treeLoc.y),
                        math.floor(dist or 0), ctx.hp_pct or 0))
                end
                return "TANGO_DETOUR"
            end
        end
    end

    -- Critical retreat — на фонтан, но если путь через enemy tower → safe_pos
    if ctx.hp_pct < (C.critical_hp or 0.15) then
        local dFountain = Dist2D(ctx.pos, ctx.fountain)
        local tgt = ctx.fountain
        if dFountain > (C.critical_fountain_maxdist or 3000.0)
           and (ctx.nearest_enemy_tower_dist or 99999) < 1500 then
            tgt = ctx.safe_pos
        end
        Combat.MoveToOnce(bot, tgt, ctx)
        return "CRITICAL"
    end

    -- Escalation: уже почти на safe_pos, но всё равно получаем урон → safe_pos
    -- сам в зоне опасности (вражеские крипы пушат через него). Эскалируем на
    -- фонтан, чтобы не стоять одной точке пока крипы добивают. Без этого был
    -- баг "STUCK 530 ticks под собственной T1": safe_pos=(-6700,-200) совпал
    -- с radiant top T1 waypoint, бот стоял где его и били.
    local dSafe = Dist2D(ctx.pos, ctx.safe_pos or ctx.pos)
    local e_creep = (ctx._counts and ctx._counts.nearby_creeps) or 0
    local at_safe = dSafe < 400
    local still_taking_dmg = ctx.took_damage_recent or e_creep >= 2
    if at_safe and still_taking_dmg and (ctx.hp_pct or 1) < 0.70 then
        if BotControllerState._retreat_escalated_logged ~= true then
            BotControllerState._retreat_escalated_logged = true
            print(string.format(
                "[retreat] ESCALATE→fountain | pos=(%d,%d) safe=(%d,%d) dSafe=%d "
                .. "hp=%.2f e_creep=%d td=%s",
                math.floor(ctx.pos.x), math.floor(ctx.pos.y),
                math.floor(ctx.safe_pos.x), math.floor(ctx.safe_pos.y),
                math.floor(dSafe), ctx.hp_pct or 0, e_creep,
                tostring(ctx.took_damage_recent)))
        end
        Combat.MoveToOnce(bot, ctx.fountain, ctx)
        return "ESCALATE_FOUNTAIN"
    end
    -- Не на safe_pos / без damage — сбросим маркер, чтобы повторная эскалация
    -- залогировалась.
    if not at_safe then
        BotControllerState._retreat_escalated_logged = nil
    end

    -- HP_STALL guard: уже не на safe_pos (escalate выше не сработал), HP не
    -- восстанавливается > N тиков. Нас либо тащит DoT (Necro ult / Viper orb),
    -- либо мы стоим в creep aggro в дороге, либо pathfind blocked другим ботом
    -- на той же rallying-точке (Ogre-кейс: HP_STALL=676 ticks=90с, MoveTo
    -- fountain в одну точку → squad-collision deadlock).
    --
    -- Multi-step escalation:
    --   ≥50 ticks  (~7s):  MoveTo(fountain)            HP_STALL_FOUNTAIN
    --   ≥100 ticks (~14s): MoveTo(fountain ± offset)   HP_STALL_SHUFFLE
    --   ≥300 ticks (~43s): cast item_tpscroll          HP_STALL_TP
    --   ≥500 ticks (~71s): DEV warn, accept stuck      HP_STALL_GIVEUP
    local hp_now = ctx.hp_pct or 1
    local hp_last = BotControllerState._retreat_hp_last
    local hp_stall_tick = BotControllerState._retreat_hp_stall_tick or (ctx.tick or 0)
    if hp_last and hp_now <= hp_last + 0.005 then
        local stall = (ctx.tick or 0) - hp_stall_tick
        local dFount = ctx.fountain and Dist2D(ctx.pos, ctx.fountain) or 99999
        local nearET = ctx.nearest_enemy_tower_dist or 99999
        local near_fount_or_safe = (dFount < 3000) or (nearET > 1500)

        -- Stage 4 (≥500 ticks) — DEV warn raz в 100 тиков, accept stuck.
        -- Возвращаем стандартный safe_pos move; больше ничего сделать нельзя
        -- без teleport-debug-команды. Дальнейшие тики будут продолжать стоять.
        if stall >= 500 then
            if (ctx.tick or 0) - (BotControllerState._retreat_giveup_log_tick or -999) >= 100 then
                BotControllerState._retreat_giveup_log_tick = ctx.tick or 0
                print(string.format(
                    "[retreat] HP_STALL_GIVEUP hp=%.2f stall=%d pos=(%d,%d) "
                    .. "fount_d=%d enemy_tower_d=%d — pathfind appears permanently blocked",
                    hp_now, stall,
                    math.floor(ctx.pos.x), math.floor(ctx.pos.y),
                    math.floor(dFount), math.floor(nearET)))
            end
            Combat.MoveToOnce(bot, ctx.safe_pos or ctx.fountain, ctx)
            return "HP_STALL_GIVEUP"

        -- Stage 3 (≥300 ticks) — TP scroll if available. Self-cast на base.
        -- Items.TryUseTPScroll возвращает (ok, label, target). Если нет в
        -- инвентаре или на cd — fall-through к Stage 2 shuffle.
        elseif stall >= 300 then
            local tp_ok, tp_label
            if Items and Items.TryUseTPScroll then
                tp_ok, tp_label = Items.TryUseTPScroll(bot, ctx)
            end
            if tp_ok then
                if BotControllerState._retreat_tp_log_tick ~= ctx.tick then
                    BotControllerState._retreat_tp_log_tick = ctx.tick
                    print(string.format(
                        "[retreat] HP_STALL_TP hp=%.2f stall=%d label=%s",
                        hp_now, stall, tostring(tp_label or "?")))
                end
                return "HP_STALL_TP"
            end
            -- TP не доступен — продолжаем shuffle (ниже)
        end

        -- Stage 2 (≥100 ticks) — fountain ± random offset (shuffle out of
        -- squad-collision). math.random нестабилен между тиков → переcчитываем
        -- offset раз в 30 тиков (накопленное shuffle-движение даст несколько
        -- разных направлений).
        if stall >= 100 and near_fount_or_safe then
            local off = BotControllerState._retreat_shuffle_off
            local off_tick = BotControllerState._retreat_shuffle_off_tick or 0
            if not off or (ctx.tick or 0) - off_tick >= 30 then
                local angle = math.random() * 2 * math.pi
                off = { x = math.cos(angle) * 200, y = math.sin(angle) * 200 }
                BotControllerState._retreat_shuffle_off = off
                BotControllerState._retreat_shuffle_off_tick = ctx.tick or 0
            end
            local tgt = {
                x = (ctx.fountain.x or 0) + off.x,
                y = (ctx.fountain.y or 0) + off.y,
                z = ctx.fountain.z or 0,
            }
            if BotControllerState._retreat_shuffle_log_tick ~= ctx.tick then
                BotControllerState._retreat_shuffle_log_tick = ctx.tick
                print(string.format(
                    "[retreat] HP_STALL_SHUFFLE hp=%.2f stall=%d offset=(%d,%d)",
                    hp_now, stall, math.floor(off.x), math.floor(off.y)))
            end
            Combat.MoveToOnce(bot, tgt, ctx)
            return "HP_STALL_SHUFFLE"
        end

        -- Stage 1 (≥50 ticks) — straight MoveTo(fountain) (legacy).
        if stall >= 50 and near_fount_or_safe then
            if BotControllerState._retreat_stall_logged ~= ctx.tick then
                BotControllerState._retreat_stall_logged = ctx.tick
                print(string.format("[retreat] HP_STALL_FOUNTAIN hp=%.2f stall=%d tick=%d",
                    hp_now, stall, ctx.tick or 0))
            end
            Combat.MoveToOnce(bot, ctx.fountain, ctx)
            return "HP_STALL_FOUNTAIN"
        end
    else
        BotControllerState._retreat_hp_stall_tick = ctx.tick or 0
        BotControllerState._retreat_shuffle_off = nil
        BotControllerState._retreat_giveup_log_tick = nil
    end
    BotControllerState._retreat_hp_last = hp_now

    -- Обычный retreat → safe_pos
    Combat.MoveToOnce(bot, ctx.safe_pos, ctx)
    return "SAFE"
end

return M
