-- context.lua — pre-computed GameContext. Один раз за тик, передаётся в FSM/handlers.
-- Цель: handlers не дёргают одни и те же геттеры (раньше bot:GetHealth() вызывался
-- 50 раз/тик в 7 modes + utils — perf и спам в логе).

local M = {}

local vec = require("util.vec")
local Dist2D = vec.Dist2D
local _CoopOk, Coop = pcall(require, "util.coop")
if not _CoopOk then Coop = nil end
local _FsmOk, FSM = pcall(require, "fsm")
if not _FsmOk then FSM = nil end

-- Возвращает таблицу ctx с полями. Все значения "safe": 0/false/nil если данных нет.
function M.Build(bot)
    if not bot or bot:IsNull() then return nil end

    local pos  = bot:GetLocation()
    local team = bot:GetTeam()
    local lane = bot:GetAssignedLane() or 2
    local hp, maxHp     = bot:GetHealth(), bot:GetMaxHealth()
    local mana, maxMana = bot:GetMana(), bot:GetMaxMana()
    local gt   = DotaTime()

    local ctx = {
        bot          = bot,
        pos          = pos,
        team         = team,
        lane         = lane,
        tick         = BotControllerState and BotControllerState.tick or 0,

        -- HP / Mana
        hp           = hp,
        maxHp        = maxHp,
        hp_pct       = (maxHp > 0) and (hp / maxHp) or 0,
        mana         = mana,
        maxMana      = maxMana,
        mana_pct     = (maxMana > 0) and (mana / maxMana) or 0,

        -- Economy / progression
        gold         = bot:GetGold() or 0,
        level        = bot:GetLevel() or 0,

        -- Time of day
        game_time    = gt,
        is_day       = IsDaytime and IsDaytime() or true,
        day_fraction = GetTimeOfDay and GetTimeOfDay() or 0.0,

        -- Nearby queries (cached per-tick)
        nearby_enemies  = bot:GetNearbyHeroes(1600, true)  or {},
        nearby_allies   = bot:GetNearbyHeroes(1600, false) or {},
        nearby_creeps   = bot:GetNearbyLaneCreeps(1200, true)  or {},
        friendly_creeps = bot:GetNearbyLaneCreeps(1200, false) or {},
        nearby_towers   = bot:GetNearbyTowers(900, true)  or {},
        allied_towers   = bot:GetNearbyTowers(900, false) or {},
        neutrals        = (bot.GetNearbyNeutralCreeps and bot:GetNearbyNeutralCreeps(1200, false)) or {},

        -- Lane geometry
        lane_amount = (GetLaneAmountForBot and GetLaneAmountForBot(lane, team, "lane")) or 0.5,
    }

    -- Lane front (creep centroid → fallback waypoint).
    -- Radius 5000: top/bot lanes ~12000 unit длиной, push'ed wave может быть
    -- на 4000+ от бота. radius=2000 раньше делал lane_front=fallback waypoint
    -- → lane_has_creeps_global=false → no_creeps_streak растёт навсегда → бот
    -- АФК в LANE_WAIT под T1.
    if GetCreepFrontLocation then
        ctx.lane_front = GetCreepFrontLocation(bot, lane, 5000)
    else
        ctx.lane_front = GetLocationAlongLane(lane, ctx.lane_amount)
    end

    -- Fountain
    ctx.fountain = (GetOwnFountain and GetOwnFountain())
                   or ((team == 2) and Vector(-7200, -6700, 128) or Vector(7200, 6700, 128))

    -- Safe retreat
    local safeAmount = (GetLaneAmountForBot and GetLaneAmountForBot(lane, team, "safe")) or 0.2
    ctx.safe_pos = GetLocationAlongLane(lane, safeAmount)

    -- Ближайший враг
    local best, bestDist = nil, 99999
    for _, h in ipairs(ctx.nearby_enemies) do
        if h and not h:IsNull() and h:IsAlive() then
            local hp2 = h:GetLocation()
            local dx, dy = pos.x - hp2.x, pos.y - hp2.y
            local d = math.sqrt(dx * dx + dy * dy)
            if d < bestDist then bestDist = d; best = h end
        end
    end
    ctx.nearest_enemy      = best
    ctx.nearest_enemy_dist = bestDist

    -- Союзные крипы рядом
    ctx.has_friendly_creeps = false
    for _, c in ipairs(ctx.friendly_creeps) do
        if c and c:IsAlive() then ctx.has_friendly_creeps = true; break end
    end

    -- Tower проверки
    ctx.near_enemy_tower = #ctx.nearby_towers > 0
    local dangerRadius = (BotControllerConfig and BotControllerConfig.tower_danger_radius) or 1400.0
    local tower_danger = bot:GetNearbyTowers(dangerRadius, true) or {}
    ctx.near_enemy_tower_danger = #tower_danger > 0

    ctx.nearest_enemy_tower_dist = 99999
    for _, t in ipairs(tower_danger) do
        if t and not t:IsNull() and t:IsAlive() then
            local d = Dist2D(ctx.pos, t:GetLocation())
            if d < ctx.nearest_enemy_tower_dist then ctx.nearest_enemy_tower_dist = d end
        end
    end

    -- Recent damage flag
    local last_hp = BotControllerState.last_hp_seen or hp
    ctx.took_damage_recent = (hp < last_hp - 5)
    BotControllerState.last_hp_seen = hp

    -- Position-stuck tracking. pos уже извлечён в строке 14 как bot:GetLocation().
    -- pos может быть Vector userdata (sol2 binding) — .x/.y читаются.
    if pos then
        local last_pos = BotControllerState.last_pos_seen
        if last_pos then
            local moved = math.abs(pos.x - last_pos.x) + math.abs(pos.y - last_pos.y) > 50
            if moved then
                BotControllerState.pos_unchanged_ticks = 0
            else
                BotControllerState.pos_unchanged_ticks = (BotControllerState.pos_unchanged_ticks or 0) + 1
            end
        else
            BotControllerState.pos_unchanged_ticks = 0
        end
        BotControllerState.last_pos_seen = {x = pos.x, y = pos.y}
        ctx.pos_unchanged_ticks = BotControllerState.pos_unchanged_ticks or 0
    else
        ctx.pos_unchanged_ticks = 0
    end

    -- Jungle target HP-progress tracking. Если активная attack-цель (jungle camp
    -- neutral) не теряет HP > N тиков — значит цель в attack range, но мы её не
    -- бьём (cancel замаха, неверный target, AttackUnit ушёл в /dev/null). Это
    -- сигнал для cond_stuck_jungle / blacklist camp.
    local jt = BotControllerState.jungle_attack_target
    if jt and not jt:IsNull() and jt:IsAlive() then
        local thp = jt:GetHealth()
        local last_thp = BotControllerState.last_target_hp_seen or thp
        if thp < last_thp - 5 then
            BotControllerState.target_hp_unchanged_ticks = 0
        else
            BotControllerState.target_hp_unchanged_ticks = (BotControllerState.target_hp_unchanged_ticks or 0) + 1
        end
        BotControllerState.last_target_hp_seen = thp
        ctx.target_hp_unchanged_ticks = BotControllerState.target_hp_unchanged_ticks
    else
        BotControllerState.target_hp_unchanged_ticks = 0
        BotControllerState.last_target_hp_seen = nil
        ctx.target_hp_unchanged_ticks = 0
    end

    ctx.safe_to_advance = ctx.has_friendly_creeps
        or Dist2D(ctx.pos, ctx.fountain) < 2500

    -- ── Team strategy (paired-orchestrators) ──────────────────────
    -- ctx.dLane дублирует локальный helper из fsm.lua, но нужен в новых
    -- cond_strategy_*: они работают по дистанции до lane_front.
    ctx.dLane = Dist2D(ctx.pos, ctx.lane_front)
    ctx.team_strategy   = (BotControllerConfig and BotControllerConfig.team_strategy) or "DEBOOST"
    ctx.strategy_active = (gt or 0) >= ((BotControllerConfig and BotControllerConfig.strategy_enable_time) or 1200)

    -- ── Two-stand scenario per-bot role derivation ────────────────
    -- player_id из C++ binding (LuaUnitProxy::GetPlayerID, line 728 в LuaUnitProxy.cpp).
    -- Один из 0..9 (radiant 0-4, dire 5-9). pid%5 даёт slot 0..4 внутри команды,
    -- что детерминистично распределяет 5 ботов по METEOR_SQUAD (все на mid) /
    -- SIDE_BAIT lanes (0,1 → top, 2,3 → bot, 4 → jungle).
    do
        local pid = -1
        if bot.GetPlayerID then
            local ok, p = pcall(function() return bot:GetPlayerID() end)
            if ok and type(p) == "number" then pid = p end
        end
        ctx.player_id = pid
        ctx.team_slot = (pid >= 0) and (pid % 5) or 0  -- 0..4 within team

        -- SIDE_BAIT lane mapping (для LOSE):
        --   slot 0,1 → top (lane 1)
        --   slot 2,3 → bot (lane 3)
        --   slot 4   → jungle (специальный маркер "J", handler пойдёт в safe jungle camp)
        local slot = ctx.team_slot
        if slot == 0 or slot == 1 then
            ctx.side_bait_lane = 1  -- top
            ctx.side_bait_kind = "lane"
        elseif slot == 2 or slot == 3 then
            ctx.side_bait_lane = 3  -- bot
            ctx.side_bait_kind = "lane"
        else
            ctx.side_bait_lane = nil
            ctx.side_bait_kind = "jungle"
        end
    end

    -- ── FSM-specific extensions ───────────────────────────────────

    -- Lane-wide creep detection. Раньше использовали heuristic "creep_front
    -- (radius=5000) != fallback waypoint by >200 unit" → "крипы где-то на линии
    -- есть". Это ломалось: дружественные крипы умирали в зоне 0-1200 от бота,
    -- но ещё живы в 1200-5000 → hasGlobal=true → no_creeps_streak=0 навсегда →
    -- LANE_FARM держит бота на пустом спот'е без support'а пока не сработает
    -- семафор farm_phase. Лог WK 380 тиков на (-2816,-2616), f_creep=0,
    -- nstreak=0, glob=true.
    --
    -- Решение: считать только tight (1200) подтверждение. Если дружественных
    -- крипов рядом нет — streak растёт, через 50 тиков LANE_WAIT, через 150 +
    -- camp близко — JUNGLE_FARM. Дальние крипы не "спасают" от exit'а, бот
    -- сам подойдёт к ним когда зайдёт в LANE_WAIT/SCOUT_FORWARD.
    ctx.lane_has_creeps_global = ctx.has_friendly_creeps

    -- Streak tick counter для гистерезиса перехода в JUNGLE/WAIT
    if ctx.lane_has_creeps_global then
        BotControllerState.no_creeps_streak = 0
    else
        BotControllerState.no_creeps_streak = (BotControllerState.no_creeps_streak or 0) + 1
    end
    ctx.no_creeps_streak = BotControllerState.no_creeps_streak

    -- Pushable: мои крипы есть, есть enemy tower рядом, нет hero-врагов в 1500
    do
        local enemyTowerDist = ctx.nearest_enemy_tower_dist or 99999
        local enemyHeroNear = ctx.nearest_enemy and ctx.nearest_enemy_dist < 1500
        ctx.pushable = ctx.has_friendly_creeps
            and enemyTowerDist < 1500
            and not enemyHeroNear
    end

    -- Nearest neutral camp distance — для transition в JUNGLE_FARM.
    -- ВАЖНО: учитываем ТОЛЬКО кампы своей команды. Если бот radiant pushed
    -- forward в dire jungle — не должен видеть dire camps как "ближайшие" и
    -- триггерить JUNGLE_FARM в чужой лес (это и был баг "AFK под T2").
    do
        ctx.nearest_neutral_dist     = 99999
        ctx.nearest_own_camp_dist    = 99999
        ctx.nearest_own_camp_pos     = nil
        if GetNeutralCampLocations then
            local tbl = GetNeutralCampLocations()
            local ownKey = (team == 2) and "radiant" or "dire"
            if type(tbl) == "table" and type(tbl[ownKey]) == "table" then
                for _, pts in pairs(tbl[ownKey]) do
                    if type(pts) == "table" then
                        for _, p in ipairs(pts) do
                            local d = Dist2D(ctx.pos, p)
                            if d < ctx.nearest_own_camp_dist then
                                ctx.nearest_own_camp_dist = d
                                ctx.nearest_own_camp_pos  = p
                            end
                        end
                    end
                end
            end
        end
        ctx.nearest_neutral_dist = ctx.nearest_own_camp_dist
        -- Если нейтралы видны прямо сейчас — расстояние = 0
        if ctx.neutrals and #ctx.neutrals > 0 then
            ctx.nearest_neutral_dist = 0
        end
    end

    -- Current FSM state name — для hero overrides (jungle-skill opt-in path).
    -- Hero overrides выполняются ДО FSM.TryTransition в bot_controller, поэтому
    -- читаем последнее зафиксированное состояние из BotControllerState.
    if FSM and FSM.Name and BotControllerState and BotControllerState.fsm_state ~= nil then
        ctx.fsm_state = FSM.Name[BotControllerState.fsm_state] or "?"
    else
        ctx.fsm_state = "?"
    end

    -- Sanity counts — для diagnostic snapshots
    ctx._counts = {
        nearby_enemies   = #ctx.nearby_enemies,
        nearby_allies    = #ctx.nearby_allies,
        nearby_creeps    = #ctx.nearby_creeps,
        friendly_creeps  = #ctx.friendly_creeps,
        nearby_towers    = #ctx.nearby_towers,
        neutrals         = #(ctx.neutrals or {}),
    }

    -- ── Farm phase semaphore ──────────────────────────────────────
    -- 5/5 min cycle линия ↔ лес. started_gt==0 = ещё не инициализировано
    -- (первый тик после ResetForNewMatch). started_gt > gt = новый матч с
    -- gt сбросом, старая метка осталась — пересинхронизируемся к now.
    BotControllerState.farm_phase = BotControllerState.farm_phase or "lane"
    local started = BotControllerState.farm_phase_started_gt or 0
    if started == 0 or started > gt then
        BotControllerState.farm_phase_started_gt = gt
        started = gt
    end
    ctx.farm_phase         = BotControllerState.farm_phase
    ctx.farm_phase_elapsed = math.max(0, gt - started)

    -- Все ли кампы своей команды в blacklist'е сейчас. Семафор использует это
    -- как escape: нечего фармить в нашем лесу — отпустить семафор, вернуться
    -- на линию досрочно. ВАЖНО: total считаем ТОЛЬКО по своим кампам, иначе
    -- JUNGLE_FARM полностью вне семафора может попасть в дедлок (всё своё
    -- забанено, но active>0 за счёт чужих → escape не сработает).
    do
        local bl = BotControllerState.camp_blacklist or {}
        local total = 0
        if GetNeutralCampLocations then
            local tbl = GetNeutralCampLocations()
            local ownKey = (team == 2) and "radiant" or "dire"
            if type(tbl) == "table" and type(tbl[ownKey]) == "table" then
                for _, pts in pairs(tbl[ownKey]) do
                    if type(pts) == "table" then total = total + #pts end
                end
            end
        end
        local active = 0
        for idx = 1, total do
            local until_gt = bl[idx]
            if not until_gt or until_gt <= gt then active = active + 1 end
        end
        ctx.all_camps_blacklisted = (total > 0 and active == 0)
        ctx._counts.own_camps_total  = total
        ctx._counts.own_camps_active = active
    end

    -- ── Co-op jungle group cache ─────────────────────────────────
    -- Coop.GetJungleGroup сам кешируется внутри (jungle_group_recompute_ticks).
    -- Здесь делаем дешёвый snapshot для diagnostics + чтобы fsm/jungle_farm не
    -- считали group дважды (один раз тут, второй раз в jungle_farm.M.Run).
    -- Coop disabled / no allies API → ctx.coop_group=nil → solo path.
    if Coop then
        local g = Coop.GetJungleGroup(bot, ctx)
        ctx.coop_group  = g
        ctx.coop_active = g and g.size >= 2 or false
        if g then
            ctx.coop_size       = g.size
            ctx.coop_is_leader  = g.is_leader
            ctx.coop_group_name = g.group_name
        end
    else
        ctx.coop_active = false
    end

    return ctx
end

return M
