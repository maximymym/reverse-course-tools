-- fsm.lua — explicit Finite State Machine для бота-фармера v1.
-- Заменяет competing-desires архитектуру modes/ — одно текущее состояние,
-- detеrministic переходы по таблице, никакого max-desire конкурса.
--
-- API:
--   FSM.State.<NAME>           — enum значений состояния
--   FSM.Name[idx]              — обратное имя для логов
--   FSM.TryTransition(s, b, c) — возвращает (новое_состояние, reason или nil)
--
-- Transitions проверяются по порядку, первый match выигрывает.
-- DEAD/RESPAWN переходы делает bot_controller (alive change).

local M = {}

-- ── State enum ──────────────────────────────────────────────────
M.State = {
    DEAD         = 0,
    RESPAWN      = 1,
    LANE_FARM    = 2,
    LANE_WAIT    = 3,
    JUNGLE_FARM  = 4,
    PUSH         = 5,
    RETREAT      = 6,
    METEOR_SQUAD = 7,  -- WIN team @ strategy_enable_time: rally в mid, meteor cast (via util/items)
    SIDE_BAIT    = 8,  -- LOSE team @ strategy_enable_time: расход по сайдам, mid avoidance
}

M.Name = {
    [0] = "DEAD",
    [1] = "RESPAWN",
    [2] = "LANE_FARM",
    [3] = "LANE_WAIT",
    [4] = "JUNGLE_FARM",
    [5] = "PUSH",
    [6] = "RETREAT",
    [7] = "METEOR_SQUAD",
    [8] = "SIDE_BAIT",
}

-- ── Helpers (локальные, чистые predicates) ──────────────────────
local vec = require("util.vec")
local Dist2D = vec.Dist2D
local _CoopOk, Coop = pcall(require, "util.coop")
if not _CoopOk then Coop = nil end

local function dLane(ctx)
    return Dist2D(ctx.pos, ctx.lane_front)
end

local function MinTicksInState(ctx, n)
    -- Гистерезис: state должен пожить минимум n тиков перед exit
    local enter = (BotControllerState and BotControllerState.fsm_enter_tick) or 0
    local cur   = (BotControllerState and BotControllerState.tick) or 0
    return (cur - enter) >= n
end

-- Семафор-удержание: текущая фаза семафора ещё не истекла → подавляем
-- reactive-переход в противоположную зону. Без этого "5 мин линии"
-- ломаются за 5 секунд: крипы вернулись в JUNGLE_FARM → cond_jungle_creepsBack
-- → LANE_FARM → семафор think we're still in jungle phase → flop назад.
local function _semaphore_holds_us(ctx, in_phase)
    if not ctx or ctx.farm_phase ~= in_phase then return false end
    -- Escape: в jungle-фазе все кампы blacklist'ed (нечего фармить) — отпускаем
    -- семафор, разрешаем reactive exit на линию. Без этого бот висит 5 минут в
    -- лесу пока кампы не разбанятся, теряет gold/exp.
    if in_phase == "jungle" and ctx.all_camps_blacklisted then return false end
    local secs = (BotControllerConfig and BotControllerConfig.farm_phase_seconds) or 300
    return (ctx.farm_phase_elapsed or 0) < secs
end

-- ── Condition functions ─────────────────────────────────────────
-- Каждая возвращает true → переход срабатывает. Чистые predicates.

-- RESPAWN exits — триггерим по прибытию на safe_pos, не по lane_front.
-- Старая версия `dLane < 700` ломалась: respawn.lua идёт на ctx.safe_pos,
-- а safe_pos сидит в 1500-2000 от lane_front (центра массы крипов на mid).
-- Бот доходил до safe_pos, AttackMove на свою же pos каждый тик, а exit
-- cond никогда не выполнялся. Правильный gate = "дошёл туда куда шёл".
local function cond_reachedSafe_withCreeps(bot, ctx)
    if not ctx.safe_pos then return false end
    return Dist2D(ctx.pos, ctx.safe_pos) < 300 and ctx.has_friendly_creeps
end

local function cond_reachedSafe_noCreeps(bot, ctx)
    if not ctx.safe_pos then return false end
    return Dist2D(ctx.pos, ctx.safe_pos) < 300
end

-- Safety fallback. Если safe_pos сломан (стаб вернул NaN, или бот не может
-- дойти из-за коллизии) — после 10 сек жизни в RESPAWN всё равно выпускаем
-- в LANE_WAIT. lane_wait.lua сам довезёт до waitPos через streak-based
-- forward scout. Лучше «не туда» чем «AttackMove на свою же pos навсегда».
local function cond_respawn_stuck_fallback(bot, ctx)
    return MinTicksInState(ctx, 100)
end

-- LANE_FARM exits
local function cond_hpCriticalOrTowerTick(bot, ctx)
    local C = BotControllerConfig
    if ctx.hp_pct < (C.low_hp or 0.30) then return true end
    if ctx.took_damage_recent and ctx.near_enemy_tower_danger
       and ctx.hp_pct < (C.tower_near_hp or 0.50) then
        return true
    end
    return false
end

local function cond_pushable(bot, ctx)
    return ctx.pushable == true and MinTicksInState(ctx, 30)
end

-- ── Team strategy: WIN forced push (legacy fallback) ───────────
-- WIN-режим: мгновенный PUSH override если бот уже на линии + есть крипы.
-- ВАЖНО: cond_meteor_squad ниже приоритетнее — после strategy_enable_time
-- 5 WIN-ботов идут в mid, а не push'ат свою random lane. cond_strategy_win_push
-- остаётся как safety net (если METEOR_SQUAD по какой-то причине не сработал,
-- бот хотя бы не АФК-фармит).
local function cond_strategy_win_push(bot, ctx)
    return ctx.strategy_active
       and ctx.team_strategy == "WIN"
       and ctx.has_friendly_creeps
       and (ctx.dLane or 9999) < 800
end

-- ── Team strategy: METEOR_SQUAD (5 WIN-ботов в mid) ────────────
-- При strategy_enable_time (~20 мин) WIN-команда rally'ится в mid lane перед
-- enemy T1, util/items.lua сам выстрелит meteor_hammer когда tower в 500r.
-- Безусловный pull в state из ЛЮБОЙ ноды (LANE_FARM/LANE_WAIT/JUNGLE_FARM/PUSH).
-- Exit условия — внутри cond_meteor_squad_exit (hp_critical / mid tower уничтожен
-- → возвращаемся в LANE_FARM continuous push).
local function cond_meteor_squad(bot, ctx)
    if not ctx.strategy_active then return false end
    if ctx.team_strategy ~= "WIN" then return false end
    if (ctx.hp_pct or 1) < ((BotControllerConfig and BotControllerConfig.meteor_squad_min_hp) or 0.40) then
        return false  -- HP слишком низкое — пускаем RETREAT-условия выиграть
    end
    return true
end

-- ── Team strategy: SIDE_BAIT (5 LOSE-ботов по сайдам) ──────────
-- При strategy_enable_time (~20 мин) LOSE-команда расходится по сайдам.
-- Per-bot lane assignment по GetPlayerID() % 5 (см. context.lua → ctx.side_bait_lane).
-- Никогда не идут в mid, не пушат волну, сидят на safe_pos своей lane.
local function cond_side_bait(bot, ctx)
    if not ctx.strategy_active then return false end
    if ctx.team_strategy ~= "LOSE" then return false end
    -- HP-gate такой же: если умираем — RETREAT выигрывает приоритетом.
    if (ctx.hp_pct or 1) < 0.25 then return false end
    return true
end

-- ── Team strategy: LOSE mid block (legacy fallback) ────────────
-- Сохранено как safety net: если SIDE_BAIT по какой-то причине не сработал
-- и mid-laner всё-таки оказался в mid — выгнать его в RETREAT.
local function cond_lose_mid_block(bot, ctx)
    return ctx.strategy_active
       and ctx.team_strategy == "LOSE"
       and (ctx.lane == 2)
       and (ctx.dLane or 9999) < ((BotControllerConfig and BotControllerConfig.lose_mid_block_radius) or 1500)
end

-- Exit-condition для METEOR_SQUAD: WIN снят / strategy_active=false / мёртв.
-- Назад в LANE_FARM (continuous push) — если strategy всё ещё WIN, но мы вышли
-- через hp_critical → попадём в RETREAT по hpCriticalOrTowerTick.
local function cond_meteor_squad_exit(bot, ctx)
    if not ctx.strategy_active then return true end
    if ctx.team_strategy ~= "WIN" then return true end
    return false
end

local function cond_side_bait_exit(bot, ctx)
    if not ctx.strategy_active then return true end
    if ctx.team_strategy ~= "LOSE" then return true end
    return false
end

local function cond_noCreepsStreak_jungle(bot, ctx)
    -- 15 сек без крипов И есть camp в 2000.
    -- Anti-flip-flop: семафор держит на линии — игнорим, ждём ротации.
    if _semaphore_holds_us(ctx, "lane") then return false end
    if (ctx.no_creeps_streak or 0) < 150 then return false end
    return (ctx.nearest_neutral_dist or 99999) < 2000
end

-- Семафор: phase=lane ≥ farm_phase_seconds → принудительно в лес.
-- Не свитчиться при:
--  - низком hp (RETREAT в приоритете, а если RETREAT не сработал — добиваем
--    здесь, не убегаем в лес умирать);
--  - tower_danger=true (бот в зоне чужой башни, push'ил волну → если переключим
--    на JUNGLE_FARM, cond_jungle_tower_danger МГНОВЕННО даст RETREAT, а phase
--    уже коммитнут в "jungle" → cond_phase_switch_to_jungle requires phase=="lane"
--    → больше никогда не сработает, бот залипает в lane всю катку. Подождём
--    пока бот выйдет из tower зоны, тогда фазу переключим).
local function cond_phase_switch_to_jungle(bot, ctx)
    -- Pre-game guard: strategy time DotaTime() < 0 → ничего не свитчим, иначе
    -- get JUNGLE_FARM↔LANE_WAIT ping-pong: farm_phase_elapsed может быть
    -- огромным относительно отрицательного gt, semaphore expired-as-если 5min
    -- прошло, бот гоняется по лесу до spawn.
    if (ctx.game_time or 0) < 0 then return false end
    if ctx.farm_phase ~= "lane" then return false end
    local C = BotControllerConfig or {}
    -- LOSE strategy ускоряет уход в лес (×0.4 ≈ 2 мин вместо 5).
    -- WIN strategy не трогает фарм-фазы — push override работает поверх FSM.
    local farm_seconds = C.farm_phase_seconds or 300
    if ctx.strategy_active and ctx.team_strategy == "LOSE" then
        farm_seconds = farm_seconds * 0.4
    end
    if (ctx.farm_phase_elapsed or 0) < farm_seconds then return false end
    if ctx.near_enemy_tower_danger then return false end

    -- Co-op aware HP gate. Solo (нет группы) → требуем повышенный hp_pct >= 0.7
    -- (jungle_solo_min_hp_pct) — лес соло опасен (см. лог HP 1.0→0.53 за 13с
    -- на medium camp). В составе группы 2+ ботов hp threshold обычный
    -- farm_phase_min_hp (0.5) — товарищи прикроют.
    local group = (Coop and C.jungle_coop_enabled ~= false)
                  and Coop.GetJungleGroup(bot, ctx) or nil
    local coop_active = group and group.size >= 2 or false

    local hp_min
    if coop_active then
        hp_min = C.farm_phase_min_hp or 0.50
    else
        hp_min = C.jungle_solo_min_hp_pct or 0.70
    end
    if (ctx.hp_pct or 0) < hp_min then return false end

    -- Coop sync: non-leader ждёт пока в JUNGLE_FARM окажется хотя бы 1 ally
    -- (обычно лидер группы) — иначе не-лидер уйдёт первым, лидер останется
    -- на линии, group lock не запишется, бот пойдёт solo NearestCamp без
    -- координации. Лидер идёт первым (без waiting).
    --
    -- Fallback: если ждать слишком долго (≥ jungle_coop_wait_ticks ≈ 70 ≈ 9s),
    -- идём всё равно — лидер мог умереть/зависнуть, не блокируем партию.
    if coop_active and not group.is_leader then
        local count = Coop.CountAlliesInState(group, "JUNGLE_FARM", group.my_pid)
        if count >= 1 then return true end
        -- timer: считаем сколько тиков мы уже "хотим" перейти (фаза coop wait)
        BotControllerState.coop_phase_wait_since = BotControllerState.coop_phase_wait_since
                                                 or (ctx.tick or 0)
        local since = BotControllerState.coop_phase_wait_since
        local elapsed = (ctx.tick or 0) - since
        local timeout = C.jungle_coop_wait_ticks or 70
        if elapsed >= timeout then
            BotControllerState.coop_phase_wait_since = nil
            return true
        end
        return false
    end

    -- Лидер группы или solo: idem старая логика — переходим сразу.
    BotControllerState.coop_phase_wait_since = nil
    return true
end

local function cond_noCreepsStreak_wait(bot, ctx)
    return (ctx.no_creeps_streak or 0) > 50
end

-- LANE_WAIT exits
local function cond_creepsArrived(bot, ctx)
    return ctx.has_friendly_creeps and MinTicksInState(ctx, 5)
end

local function cond_waiting_jungle(bot, ctx)
    -- Pre-game guard: strategy-time нет крипов и нет смысла идти в лес —
    -- бот ещё не заспавнен, всё что произойдёт это ping-pong к камп-точкам
    -- которые тоже пустые до spawn'а.
    if (ctx.game_time or 0) < 0 then return false end
    -- 15 сек ожидания → уходим в jungle. Раньше требовался camp в 1500 от
    -- waitPos, но это держало Sniper'а top lane АФК под T1 — ближайший radiant
    -- top camp на dist ~3060 → exit не срабатывал. Если jungle тоже пуст,
    -- cond_jungle_lane_empty вернёт обратно через 5 сек — лучше циркуляция
    -- чем deadlock у башни.
    return MinTicksInState(ctx, 150)
end

local function cond_lowHpHarassed(bot, ctx)
    local C = BotControllerConfig
    return ctx.hp_pct < (C.tower_near_hp or 0.50)
end

-- LANE_WAIT не должен включать стояние под T2 врага. Если без крипов и в
-- зоне tower_danger (1400 от чужой башни) → RETREAT, без ожидания низкого HP.
-- С крипами игнорим — бот толкает волну вместе с ними, это легит push.
local function cond_lane_wait_tower_danger(bot, ctx)
    if ctx.has_friendly_creeps then return false end
    return ctx.near_enemy_tower_danger == true
end

-- JUNGLE_FARM exits
local function cond_jungle_critical(bot, ctx)
    local C = BotControllerConfig
    if ctx.hp_pct < (C.low_hp or 0.30) then return true end
    if ctx.nearest_enemy and ctx.nearest_enemy_dist < 1000 then return true end
    -- Creep damage в лесу: вражеские крипы пробежали через линию, дерут бота.
    -- nearest_enemy не учитывает крипов (это только heroes 1600). Раньше бот
    -- стоял на пустом camp с hp 1.00 → 0.30 без RETREAT. Теперь: ≥2 e_creep'а
    -- рядом + получил damage в этом тике + hp<0.70 → отступаем.
    local e_creep = (ctx._counts and ctx._counts.nearby_creeps) or 0
    if e_creep >= 2 and ctx.took_damage_recent and (ctx.hp_pct or 1) < 0.70 then
        return true
    end
    return false
end

-- Бот в JUNGLE_FARM не должен видеть вражеские башни в принципе — свой лес
-- без чужих T1/T2. Если tower_danger detected → бот забрёл в чужой jungle
-- (например после форвард-пуша picked nearby camp в зоне врага). Эвакуация
-- безусловная, не ждём пока HP упадёт <30%. Симметричный сейф для LANE_WAIT
-- (cond_lane_wait_tower_danger) — ниже.
local function cond_jungle_tower_danger(bot, ctx)
    return ctx.near_enemy_tower_danger == true
end

local function cond_jungle_creepsBack(bot, ctx)
    if _semaphore_holds_us(ctx, "jungle") then return false end
    if not ctx.has_friendly_creeps then return false end
    return dLane(ctx) < 1500
end

-- Бот стоит >7s без движения И цель не теряет HP >4s → blacklist camp (через
-- jungle_farm.Run handler) и переход в LANE_WAIT. Это спасает от ситуации
-- когда AttackUnit летит в neutral каждые 4с но damage не доходит (windup
-- cancel, target unreachable из-за collision на дереве, etc).
local function cond_stuck_jungle(bot, ctx)
    return (ctx.pos_unchanged_ticks or 0) >= 50
       and (ctx.target_hp_unchanged_ticks or 0) >= 30
end

local function cond_jungle_lane_empty(bot, ctx)
    -- Camps пусты (neutrals=0) → exit. Раньше требовалось dLane < 1500, но это
    -- держало бота АФК на пустом camp если линия далеко (WK на radiant jungle,
    -- dLane=3811 → infinite WALK_CAMP). 50 тиков ≈ 5 сек гистерезис.
    if _semaphore_holds_us(ctx, "jungle") then return false end
    return (ctx._counts and ctx._counts.neutrals or 0) == 0
        and MinTicksInState(ctx, 50)
end

-- Симметрично _to_jungle.
local function cond_phase_switch_to_lane(bot, ctx)
    if ctx.farm_phase ~= "jungle" then return false end
    local C = BotControllerConfig or {}
    if (ctx.farm_phase_elapsed or 0) < (C.farm_phase_seconds or 300) then return false end
    if (ctx.hp_pct or 0) < (C.farm_phase_min_hp or 0.50) then return false end
    return true
end

-- PUSH exits
local function cond_push_retreat(bot, ctx)
    if ctx.hp_pct < 0.40 then return true end
    if ctx.nearest_enemy and ctx.nearest_enemy_dist < 1500 then return true end
    return false
end

local function cond_push_done(bot, ctx)
    -- Tower упала ИЛИ мои крипы все мёртвы
    if (ctx._counts and ctx._counts.nearby_towers or 0) == 0 then
        return MinTicksInState(ctx, 30)
    end
    if not ctx.has_friendly_creeps and MinTicksInState(ctx, 30) then return true end
    return false
end

-- RETREAT exits.
-- ВАЖНО: не выходим из RETREAT пока бот ещё в зоне чужой башни без крипов —
-- иначе flapping JUNGLE_FARM/LANE_WAIT → RETREAT → recover → tower_danger →
-- RETREAT, где RETREAT и WAIT/JUNGLE чередуются каждый тик но бот de-facto
-- стоит. has_friendly_creeps=true — push с волной, даже tower_danger ОК.
--
-- Hysteresis 30 тиков (~4с): после входа в RETREAT не возвращаемся в lane
-- хотя бы 4 секунды. За это время бот успевает уехать на safe_pos с запасом
-- от 1400-unit tower_danger границы. Без gate'а Sniper флапал LANE_WAIT↔
-- RETREAT 30 раз за 30с потому что (-4760,4720) сидит ровно на границе
-- tower_danger зоны: LANE_WAIT движет к waitPos (вперёд → td=true), RETREAT
-- к safe_pos (назад → td=false), цикл каждые 0.7с.
local function cond_retreat_recovered_lane(bot, ctx)
    if not MinTicksInState(ctx, 30) then return false end
    return ctx.hp_pct > 0.70 and ctx.has_friendly_creeps
end

local function cond_retreat_recovered_wait(bot, ctx)
    if not MinTicksInState(ctx, 30) then return false end
    if (ctx.hp_pct or 0) <= 0.70 then return false end
    if ctx.near_enemy_tower_danger and not ctx.has_friendly_creeps then
        return false
    end
    return true
end

-- Wildcard timeout: > 120с в любом state без damage taken и без движения →
-- принудительный recovery в LANE_WAIT. lane_wait сам разберётся куда дальше.
-- Защита от deadlock'ов которые мы не предусмотрели conditions выше.
local function cond_stuck_state_timeout(bot, ctx)
    local enter_tick = (BotControllerState and BotControllerState.fsm_enter_tick) or (ctx.tick or 0)
    local in_state = (ctx.tick or 0) - enter_tick
    return in_state >= 900
       and not ctx.took_damage_recent
       and (ctx.pos_unchanged_ticks or 0) >= 50
end

-- ── Transitions table ──────────────────────────────────────────
-- {cond_fn, to_state, reason} — первый match выигрывает.
-- Приоритет (сверху вниз): safety (RETREAT) → семафор (phase_switch) → reactive.
-- bot_controller.lua коммитит phase change в state по reason==phase_switch_*.
-- ВАЖНО: cond_meteor_squad / cond_side_bait в каждом state ставятся ВЫШЕ
-- остальных transitions (кроме hp_critical retreat). После strategy_enable_time
-- бот мгновенно переходит в team-coordinated state из ЛЮБОЙ farm-фазы.
M.Transitions = {
    [M.State.RESPAWN] = {
        { cond_meteor_squad,            M.State.METEOR_SQUAD, "strategy_win_meteor" },
        { cond_side_bait,               M.State.SIDE_BAIT,    "strategy_lose_side_bait" },
        { cond_reachedSafe_withCreeps,  M.State.LANE_FARM,    "reached_safe_with_creeps" },
        { cond_reachedSafe_noCreeps,    M.State.LANE_WAIT,    "reached_safe_no_creeps" },
        { cond_respawn_stuck_fallback,  M.State.LANE_WAIT,    "respawn_stuck_fallback" },
    },
    [M.State.LANE_FARM] = {
        { cond_hpCriticalOrTowerTick,   M.State.RETREAT,      "hp_or_tower" },
        { cond_meteor_squad,            M.State.METEOR_SQUAD, "strategy_win_meteor" },
        { cond_side_bait,               M.State.SIDE_BAIT,    "strategy_lose_side_bait" },
        { cond_lose_mid_block,          M.State.RETREAT,      "strategy_lose_mid_block" },
        { cond_stuck_state_timeout,     M.State.LANE_WAIT,    "timeout_recovery" },
        { cond_strategy_win_push,       M.State.PUSH,         "strategy_win_push" },
        { cond_phase_switch_to_jungle,  M.State.JUNGLE_FARM,  "phase_switch_to_jungle" },
        { cond_pushable,                M.State.PUSH,         "pushable" },
        { cond_noCreepsStreak_jungle,   M.State.JUNGLE_FARM,  "no_creeps_15s" },
        { cond_noCreepsStreak_wait,     M.State.LANE_WAIT,    "no_creeps_5s" },
    },
    [M.State.LANE_WAIT] = {
        { cond_lane_wait_tower_danger,  M.State.RETREAT,      "lane_wait_tower_danger" },
        { cond_lowHpHarassed,           M.State.RETREAT,      "harassed" },
        { cond_meteor_squad,            M.State.METEOR_SQUAD, "strategy_win_meteor" },
        { cond_side_bait,               M.State.SIDE_BAIT,    "strategy_lose_side_bait" },
        { cond_lose_mid_block,          M.State.RETREAT,      "strategy_lose_mid_block" },
        { cond_stuck_state_timeout,     M.State.LANE_WAIT,    "timeout_recovery" },
        { cond_strategy_win_push,       M.State.PUSH,         "strategy_win_push" },
        { cond_phase_switch_to_jungle,  M.State.JUNGLE_FARM,  "phase_switch_to_jungle" },
        { cond_creepsArrived,           M.State.LANE_FARM,    "creeps_arrived" },
        { cond_waiting_jungle,          M.State.JUNGLE_FARM,  "wait_too_long" },
    },
    [M.State.JUNGLE_FARM] = {
        { cond_jungle_tower_danger,     M.State.RETREAT,      "jungle_tower_danger" },
        { cond_jungle_critical,         M.State.RETREAT,      "jungle_critical" },
        { cond_meteor_squad,            M.State.METEOR_SQUAD, "strategy_win_meteor" },
        { cond_side_bait,               M.State.SIDE_BAIT,    "strategy_lose_side_bait" },
        { cond_stuck_jungle,            M.State.LANE_WAIT,    "jungle_stuck" },
        { cond_stuck_state_timeout,     M.State.LANE_WAIT,    "timeout_recovery" },
        { cond_phase_switch_to_lane,    M.State.LANE_FARM,    "phase_switch_to_lane" },
        { cond_jungle_creepsBack,       M.State.LANE_FARM,    "lane_creeps_back" },
        { cond_jungle_lane_empty,       M.State.LANE_WAIT,    "camps_empty" },
    },
    [M.State.PUSH] = {
        { cond_push_retreat,            M.State.RETREAT,      "push_retreat" },
        { cond_meteor_squad,            M.State.METEOR_SQUAD, "strategy_win_meteor" },
        { cond_stuck_state_timeout,     M.State.LANE_WAIT,    "timeout_recovery" },
        { cond_push_done,               M.State.LANE_FARM,    "push_done" },
    },
    [M.State.RETREAT] = {
        { cond_retreat_recovered_lane,  M.State.LANE_FARM,    "recovered_with_creeps" },
        { cond_retreat_recovered_wait,  M.State.LANE_WAIT,    "recovered_no_creeps" },
    },
    -- Новые team-coordinated states. Exit только через hp_critical (RETREAT) или
    -- strategy снят. Гистерезис 30 тиков — не флапаем между LANE_FARM и METEOR_SQUAD
    -- если strategy_active поплывёт около time границы.
    [M.State.METEOR_SQUAD] = {
        { cond_hpCriticalOrTowerTick,   M.State.RETREAT,   "hp_or_tower" },
        { cond_meteor_squad_exit,       M.State.LANE_FARM, "strategy_win_cleared" },
    },
    [M.State.SIDE_BAIT] = {
        { cond_hpCriticalOrTowerTick,   M.State.RETREAT,   "hp_or_tower" },
        { cond_side_bait_exit,          M.State.LANE_WAIT, "strategy_lose_cleared" },
    },
}

-- ── TryTransition ───────────────────────────────────────────────
-- Возвращает (new_state, reason). Если нет перехода — (from_state, nil).
function M.TryTransition(fromState, bot, ctx)
    local list = M.Transitions[fromState]
    if not list then return fromState, nil end
    for _, t in ipairs(list) do
        local fn, to, reason = t[1], t[2], t[3]
        local ok, fire = pcall(fn, bot, ctx)
        if ok and fire then return to, reason end
    end
    return fromState, nil
end

return M
