-- bot_controller.lua — FSM dispatcher v2 (фарм-бот v1).
-- Заменяет competing-desires архитектуру modes/ — одно текущее состояние,
-- detеrministic переходы по таблице, никакого max-desire конкурса.
--
-- Вызывается C++ каждые ~100ms (10Hz). Возвращает (action_name, did_act).
--
-- Тестер обычно правит:
--   config.lua            — пороги HP, радиусы, jitter
--   states/<name>.lua     — handler конкретного state'а
--   fsm.lua               — transitions table / condition functions
--   item_build.lua        — покупки
--   ability_build.lua     — скиллы
--   heroes/*.lua          — per-hero short-circuit override

-- ── Log auto-rotate on script load/reload ─────────────────────────────
-- C++ Andromeda пишет per-PID `botbrain_<PID>.log` — этот же файл сразу же
-- truncate'нул на старте процесса (в BrainLog при первом write). Lua-side
-- truncate БОЛЬШЕ НЕ нужен (был нужен когда C++ писал в общий botbrain.log
-- и multi-instance процессы стирали друг другу логи). Оставляем только
-- session-marker чтобы grep отделял F7-перезагрузки внутри одной DLL-сессии.
print(string.format(
    "[bot_controller] === SESSION START %s (Lua reload) ===",
    os.date("%Y-%m-%d %H:%M:%S")))

local _cfgOk, _cfgErr = pcall(require, "config")
if not _cfgOk then print("[bot_controller] config error: " .. tostring(_cfgErr)) end

local Context = require("context")
local FSM     = require("fsm")
local AB      = require("util.anti_ban")
local States  = require("states.init")
local vec     = require("util.vec")
local _ItemsOk, Items = pcall(require, "util.items")
if not _ItemsOk then
    print("[bot_controller] util.items error: " .. tostring(Items))
    Items = nil
end
local _HeroesOk, Heroes = pcall(require, "heroes.init")
if not _HeroesOk then Heroes = {} end
local _CombatOk, Combat = pcall(require, "util.combat")
if not _CombatOk then
    print("[bot_controller] util.combat error: " .. tostring(Combat))
    Combat = nil
end
local _CoopOk, Coop = pcall(require, "util.coop")
if not _CoopOk then
    print("[bot_controller] util.coop error: " .. tostring(Coop))
    Coop = nil
end

-- ── Team strategy file poller (paired-orchestrators 5v5 self-play) ────
-- Orchestrator (C++) пишет per-pid JSON `C:\temp\andromeda\strategy.json`
-- вида { "27924": "WIN", "27928": "WIN", ... } — каждая запись принадлежит
-- одному dota2.exe процессу. На single-machine setup мастер и slave пишут
-- общий файл, поэтому матч по своему PID обязателен — иначе бот возьмёт
-- стратегию чужой команды.
--
-- PID берётся из `_G.ANDROMEDA_PID` (set CBotBrain::RegisterGlobals при
-- создании Lua state) либо `os.getenv("ANDROMEDA_PID")` (set
-- CBotFarmClient::OnInit) — оба способа держит DLL для надёжности.
--
-- Backward-compat: если strategy.json нет либо в нём нет нашего PID —
-- fallback на `strategy.txt` (старое одно-слово поведение, single-instance).
--
-- "rb" — игнор BOM/EOL; lightweight match без JSON-парсера (json не вшит
-- в Andromeda Lua state, тащить только ради этого — оверкилл).
local function _own_pid()
    if _G.ANDROMEDA_PID then return tostring(_G.ANDROMEDA_PID) end
    local env = os.getenv and os.getenv("ANDROMEDA_PID")
    if env and env ~= "" then return env end
    return nil
end

local function _read_file(path)
    local f = io.open(path, "rb")
    if not f then return nil end
    local s = f:read("*a")
    f:close()
    return s
end

local function _read_strategy()
    -- Path 1 (preferred): per-pid JSON match.
    local pid = _own_pid()
    if pid then
        local content = _read_file("C:\\temp\\andromeda\\strategy.json")
        if content and content ~= "" then
            -- pattern: "<pid>" : "VALUE"
            -- %s* — толерантно к whitespace вокруг ":" (json.dump может варьировать).
            local pattern = '"' .. pid .. '"%s*:%s*"(%w+)"'
            local v = content:match(pattern)
            if v then return v end
        end
        -- json есть но нашего pid там нет → orchestrator не знает про нас
        -- (например ProxyHook дал PID, а DLL ещё не передал в orchestrator
        -- через ReadStatusFiles). Падаем в txt-fallback.
    end

    -- Path 2 (back-compat): plain-text strategy.txt — единая стратегия для всех.
    local content = _read_file("C:\\temp\\andromeda\\strategy.txt")
    if content then return content:match("^(%w+)") end
    return nil
end

-- Camp coords override: C++ stub LuaStubs::GetNeutralCampLocations содержит
-- устаревшие точки (например radiant hard (-3800,-300) — такого camp в текущей
-- карте нет → бот WK стоял АФК на пустом spot, neut=0). util/camps.lua даёт
-- актуальные координаты из user dump'а. До DLL rebuild — Lua override.
do
    local _ok, CAMPS = pcall(require, "util.camps")
    if _ok and CAMPS then
        local _origCamps = GetNeutralCampLocations
        function GetNeutralCampLocations() return CAMPS end
        if _origCamps then
            print("[bot_controller] camp coords overridden (was C++ stub)")
        end
    end
end

-- Анти-детект A.4: seed math.random os.clock'ом + PID-like энтропия.
-- Без seed все 5 ботов стартуют с одинаковой последовательностью RandomInt
-- → jitter buckets синхронны → weaker anti-detect variance.
-- Делается один раз на load/reload модуля.
math.randomseed(math.floor((os.clock() or 0) * 1000000) + (os.time() or 0))
for _ = 1, 5 do math.random() end  -- прогрев PRNG

-- Per-bot state. BotControllerState уже создан C++ side, но добавляем поля.
BotControllerState = BotControllerState or {}
BotControllerState.tick                   = BotControllerState.tick or 0
BotControllerState.last_tp_time           = BotControllerState.last_tp_time or -999
BotControllerState.last_hp                = BotControllerState.last_hp or 0
BotControllerState.last_cast              = BotControllerState.last_cast or {}
BotControllerState.chat_next_tick         = BotControllerState.chat_next_tick or 0
BotControllerState.chat_sent_count        = BotControllerState.chat_sent_count or 0

-- Pending action queue (anti-ban A.1 jitter)
BotControllerState.pending_action         = BotControllerState.pending_action or nil
BotControllerState.pending_fn             = BotControllerState.pending_fn or nil
BotControllerState.pending_reaction_tick  = BotControllerState.pending_reaction_tick or 0

-- FSM state — стартуем с RESPAWN (бот ещё в фонтане при первом тике).
BotControllerState.fsm_state              = BotControllerState.fsm_state or FSM.State.RESPAWN
BotControllerState.fsm_enter_tick         = BotControllerState.fsm_enter_tick or 0

-- Match-state tracking (для detection "новая катка" через GetGameState() polling).
-- -1 = EDOTAGameState::NONE (ещё не читали).
BotControllerState.last_game_state        = BotControllerState.last_game_state or -1

-- Diagnostics state
BotControllerState.log_tick_last          = BotControllerState.log_tick_last or -999
BotControllerState.log_was_alive          = BotControllerState.log_was_alive  -- nil на первом тике
BotControllerState.log_had_ctx            = BotControllerState.log_had_ctx

local function LogBrain(msg)
    print("[BRAIN] " .. tostring(msg))
end

local Dist2D = vec.Dist2D

-- Per-match reset: item_build/anti_ban + FSM в RESPAWN. Вызывается из
-- HandleGameStateChange при переходе POST_GAME/DISCONNECT/INIT/WAIT_LOAD →
-- HERO_SELECTION/STRATEGY_TIME/PRE_GAME (и из event-callback, и из polling).
local function ResetForNewMatch()
    LogBrain("NEW MATCH DETECTED -- resetting per-match state")

    -- Не делаем require("item_build") — state-event callback может прилететь
    -- ровно в тот ms, когда C++ ScriptLoader сам параллельно парсит item_build.lua
    -- (Step 1 в LoadScripts). Recursive require того же файла → corruption Lua state →
    -- dota2.exe SIGSEGV. Crash 2026-04-29 11:11:07 (WK leader, PID 35336): event пришёл
    -- в тот же ms что и Step 1 — только partial "[Lua] " попал в лог, dump
    -- C:\BotSteam\0\dumps\crash_dota2.exe_20260429111107_1.dmp. К моменту первого Think
    -- C++ loader гарантированно положит item_build в package.loaded — берём оттуда.
    local ItemBuild = package.loaded["item_build"]
    if ItemBuild and ItemBuild.Reset then
        pcall(ItemBuild.Reset)
    end

    if AB and AB.Reset then pcall(AB.Reset) end

    BotControllerState.fsm_state = FSM.State.RESPAWN
    BotControllerState.fsm_enter_tick = 0
    BotControllerState.tick = 0
    -- last_hp_seen / took_damage_recent — natural reset на первом тике.

    -- Семафор: новый матч → стартовая фаза = линия, started=0 (context.lua
    -- snap'нет к gt на первом тике).
    BotControllerState.farm_phase            = "lane"
    BotControllerState.farm_phase_started_gt = 0

    -- Camp self-healing: stale blacklist'ы из прошлой катки (другая карта,
    -- pull/push разный) могут заблокировать кампы которые сейчас живые.
    BotControllerState.camp_misses              = {}
    BotControllerState.camp_blacklist           = {}
    BotControllerState.current_target_camp_idx  = nil
    BotControllerState.current_target_arrived_tick = nil

    -- Stable attack target tracking — мёртвый handle из старой катки бьётся
    -- IsNull/IsAlive checks, но обнулим явно.
    BotControllerState.lane_attack_target   = nil
    BotControllerState.jungle_attack_target = nil
end

-- Дедуп: и event-callback, и polling fallback зовут одну функцию. Дубликаты
-- (event пришёл первым, polling видит то же изменение через ~1 tick) гасятся
-- через last_game_state — второй вызов с тем же cur просто no-op.
local function HandleGameStateChange(cur_state, prev_state, source)
    if cur_state == BotControllerState.last_game_state then
        return  -- уже обработали этот переход
    end

    LogBrain(string.format("%s: %d -> %d",
        source == "event" and "STATE-EVENT" or "GAME_STATE",
        prev_state or BotControllerState.last_game_state, cur_state))

    local entering_pre = (cur_state == 2 or cur_state == 3 or cur_state == 4)
    -- leaving_end: prev_state НЕ был pre-game (2/3/4) и НЕ in-progress (5).
    -- Включает all "ended"/"loading" states: 0=INIT, 1=WAIT_LOAD, 6=POST_GAME,
    -- 7=DISCONNECT (исторический), 10=DISCONNECT (Source 2 actual), 11=LAST,
    -- -1=uninitialized. Раньше list был хардкодом [6,7,0,1,-1] — пропускал 10
    -- → Reset не звался при переходе 10→4 (наблюдалось 2026-04-27 у WK после
    -- закрытия прошлой катки, item_build.state.bought оставался "late",
    -- starting buy скипался, бот сидел на 1025 gold с одним TP).
    local leaving_end = prev_state ~= 2 and prev_state ~= 3
                    and prev_state ~= 4 and prev_state ~= 5
    if entering_pre and leaving_end then
        ResetForNewMatch()
    end

    BotControllerState.last_game_state = cur_state
end

-- Detect переход через polling — fallback если RegisterOnGameStateChange
-- не доступен (старый DLL) или event-callback по какой-то причине не пришёл.
local function CheckGameStateTransition()
    local cur_state = (GetGameState and GetGameState()) or 5  -- default GAME_IN_PROGRESS
    HandleGameStateChange(cur_state, BotControllerState.last_game_state, "poll")
end

-- Event registration — выполняется один раз при load/reload модуля.
-- Если RegisterOnGameStateChange не доступен (старый DLL без Phase 2), polling
-- fallback в BotController_Think() всё равно ловит переходы. Guard через
-- BotControllerState чтобы повторный require не плодил callbacks.
if RegisterOnGameStateChange and not BotControllerState.state_change_registered then
    local ok, err = pcall(RegisterOnGameStateChange, function(new_state, prev_state)
        -- Сюда приходит из C++ thread = main game thread (тот же что Think) —
        -- безопасно вызывать любую игровую функцию. pcall защищает от ошибок,
        -- которые иначе пробьются в C++ как dispatch error.
        local _ok, _err = pcall(HandleGameStateChange, new_state, prev_state, "event")
        if not _ok then
            print("[BRAIN] OnGameStateChange handler error: " .. tostring(_err))
        end
    end)
    if ok then
        BotControllerState.state_change_registered = true
        print("[BRAIN] RegisterOnGameStateChange wired (event-driven)")
    else
        print("[BRAIN] RegisterOnGameStateChange failed: " .. tostring(err))
    end
end

-- Основная функция — вызывается из C++ каждый тик.
function BotController_Think()
    -- Match state polling (до всего остального — делать при любом состоянии bot)
    pcall(CheckGameStateTransition)

    local bot = GetBot()
    if not bot or bot:IsNull() then
        if BotControllerState.log_was_alive ~= "no_bot" then
            LogBrain("NO_BOT — GetBot() returned nil or IsNull")
            BotControllerState.log_was_alive = "no_bot"
        end
        return "NO_BOT", false
    end

    local isAlive = bot:IsAlive()

    -- Alive transitions (смерть / respawn)
    if BotControllerState.log_was_alive == nil then
        LogBrain(string.format("first tick | isAlive=%s name=%s",
            tostring(isAlive), tostring(bot:GetUnitName())))
    elseif BotControllerState.log_was_alive ~= isAlive
       and BotControllerState.log_was_alive ~= "no_bot" then
        LogBrain(string.format("ALIVE: %s → %s at tick=%d",
            tostring(BotControllerState.log_was_alive),
            tostring(isAlive),
            BotControllerState.tick))
    end
    BotControllerState.log_was_alive = isAlive

    -- DEAD handling
    if not isAlive then
        if BotControllerState.fsm_state ~= FSM.State.DEAD then
            LogBrain("FSM " .. (FSM.Name[BotControllerState.fsm_state] or "?") .. " → DEAD")
            BotControllerState.fsm_state = FSM.State.DEAD
            BotControllerState.fsm_enter_tick = BotControllerState.tick or 0
        end
        return "DEAD", false
    end

    -- Exit DEAD → RESPAWN при оживлении
    if BotControllerState.fsm_state == FSM.State.DEAD then
        LogBrain(string.format("FSM DEAD → RESPAWN (respawn detected, hp=%d)",
            bot:GetHealth() or 0))
        BotControllerState.fsm_state = FSM.State.RESPAWN
        BotControllerState.fsm_enter_tick = BotControllerState.tick or 0
    end

    BotControllerState.tick = (BotControllerState.tick or 0) + 1
    local tick = BotControllerState.tick

    -- Strategy file poll: каждые 50 тиков (≈5с), I/O оптимизация — раньше
    -- читали бы 10 раз/с. tick%50==0 на 1-м тике даёт мгновенный pickup
    -- свежего значения после старта DLL/F7-reload.
    if (tick % 50) == 0 then
        local s = _read_strategy()
        if s and (s == "WIN" or s == "LOSE" or s == "DEBOOST") then
            if BotControllerConfig.team_strategy ~= s then
                LogBrain(string.format("[STRATEGY] team_strategy: %s → %s",
                    tostring(BotControllerConfig.team_strategy), s))
                BotControllerConfig.team_strategy = s
            end
        end
    end

    -- Anti-ban A.3: chat
    AB.MaybeChat(bot)

    -- Anti-ban A.1: pending action (jitter reaction)
    if AB.TickPending() then
        local last = BotControllerState.last_executed_action or ""
        return "PENDING:" .. last, true
    end

    -- Build context
    local ctxOk, ctx = pcall(Context.Build, bot)
    if not ctxOk then
        LogBrain("CTX-ERR: " .. tostring(ctx))
        return "CTX_ERR", false
    end
    if not ctx then
        if BotControllerState.log_had_ctx ~= false then
            LogBrain("NO_CTX — Context.Build returned nil")
        end
        BotControllerState.log_had_ctx = false
        return "NO_CTX", false
    end
    BotControllerState.log_had_ctx = true

    -- Hero-specific override — **side-effect**, FSM продолжается. Раньше
    -- truthy result полностью замещал FSM (return ..., true) — Sniper после
    -- одного Shrapnel'а на пустой camp оставался стоять и не шёл к следующему,
    -- WK без override залипал бы тоже. Hero override теперь только кастует
    -- скиллы (toggle, autocast, point/unit_target casts), а FSM dispatch
    -- по-прежнему даёт движение/auto-attack. BLOCK:<name> prefix — explicit
    -- opt-in для full-tick lockout (channels, setups типа Pudge Hook'а).
    local heroName = (bot:GetUnitName() or ""):gsub("npc_dota_hero_", "")
    local heroOverride = Heroes[heroName]
    local hero_blocking = nil
    if heroOverride and heroOverride.Think then
        local hOk, hResult = pcall(heroOverride.Think, bot, ctx)
        if not hOk then
            LogBrain("HERO-ERR " .. heroName .. ": " .. tostring(hResult))
        elseif hResult then
            local s = tostring(hResult)
            if s:sub(1, 6) == "BLOCK:" then
                hero_blocking = s
            else
                LogBrain("HERO " .. heroName .. ": " .. s)
            end
        end
    end
    if hero_blocking then
        return "hero:" .. heroName .. ":" .. hero_blocking, true
    end

    -- Publish current FSM state name в per-unit storage чтобы другие боты в
    -- coop-группе видели наше состояние (Coop.CountAlliesInState читает это).
    -- Делаем ДО TryTransition: actor's state read другими ботами в этом тике
    -- отражает текущее (а не newState — это обновим после транзита).
    if Coop and Coop.PublishMyState then
        pcall(Coop.PublishMyState, bot, FSM.Name[BotControllerState.fsm_state] or "?")
    end

    -- FSM transition check
    local cur = BotControllerState.fsm_state
    local newState, reason = FSM.TryTransition(cur, bot, ctx)
    if newState ~= cur then
        LogBrain(string.format(
            "FSM %s → %s (%s) tick=%d hp=%.2f pos=(%d,%d) dLane=%d "
            .. "near_eT=%d creeps=%s td=%s streak=%d camp_idx=%s gt=%.1f",
            FSM.Name[cur] or "?",
            FSM.Name[newState] or "?",
            reason or "?",
            tick,
            ctx.hp_pct or 0,
            math.floor(ctx.pos.x), math.floor(ctx.pos.y),
            math.floor(Dist2D(ctx.pos, ctx.lane_front)),
            math.floor(ctx.nearest_enemy_tower_dist or 0),
            tostring(ctx.has_friendly_creeps),
            tostring(ctx.near_enemy_tower_danger),
            ctx.no_creeps_streak or 0,
            tostring(BotControllerState.current_target_camp_idx),
            ctx.game_time or 0
        ))
        BotControllerState.fsm_state = newState
        BotControllerState.fsm_enter_tick = tick
        if Coop and Coop.PublishMyState then
            pcall(Coop.PublishMyState, bot, FSM.Name[newState] or "?")
        end

        -- Сброс dedup-trackers attack-ордера: target из старого state может быть
        -- невалидным в новом (например LANE_FARM креп → JUNGLE_FARM нейтрал).
        -- Без сброса последняя команда могла бы блокировать новую (idle window).
        BotControllerState.last_attack_target_handle = nil
        BotControllerState.last_attack_tick          = nil
        BotControllerState.last_attackmove_pos       = nil
        BotControllerState.last_attackmove_tick      = nil
        BotControllerState.last_moveto_pos           = nil
        BotControllerState.last_moveto_tick          = nil
        BotControllerState.lane_attack_target        = nil
        BotControllerState.jungle_attack_target      = nil

        -- Семафор-коммит: fsm.lua возвращает только predicate-результат, само
        -- переключение фазы делается здесь по reason, чтобы predicates оставались
        -- pure (TryTransition вызывает их через pcall, side-effects съест). После
        -- коммита elapsed обнуляется на следующем тике через context.lua.
        if reason == "phase_switch_to_jungle" then
            BotControllerState.farm_phase            = "jungle"
            BotControllerState.farm_phase_started_gt = ctx.game_time or 0
            LogBrain(string.format("[SEMAPHORE] phase: lane → jungle (gt=%.1f)", ctx.game_time or 0))
        elseif reason == "phase_switch_to_lane" then
            BotControllerState.farm_phase            = "lane"
            BotControllerState.farm_phase_started_gt = ctx.game_time or 0
            LogBrain(string.format("[SEMAPHORE] phase: jungle → lane (gt=%.1f)", ctx.game_time or 0))
        elseif (reason == "jungle_tower_danger" or reason == "jungle_critical")
               and BotControllerState.farm_phase == "jungle" then
            -- Попытка зайти в лес провалилась немедленно (tower_danger / hp_critical).
            -- Откатываем фазу в "lane" чтобы cond_phase_switch_to_jungle мог
            -- сработать снова. БЕЗ rollback'а phase=="jungle" остаётся, но бот
            -- застрял в LANE_FARM/RETREAT/LANE_WAIT — никогда не вернётся в лес.
            -- started_gt отматываем назад на (farm_phase_seconds - 60) → retry
            -- через ~60 сек после провала, не надо ждать полные 5 мин.
            local C = BotControllerConfig or {}
            local retry_after = 60.0
            local seconds = C.farm_phase_seconds or 300
            BotControllerState.farm_phase            = "lane"
            BotControllerState.farm_phase_started_gt = (ctx.game_time or 0) - (seconds - retry_after)
            LogBrain(string.format(
                "[SEMAPHORE] jungle attempt failed (%s) → rollback phase=lane, retry in %ds (gt=%.1f)",
                reason, retry_after, ctx.game_time or 0))
        end
    end

    -- Periodic CTX snapshot (раз в 50 тиков ≈ 5 сек). Расширенный — с полями
    -- которые нужны для диагностики "AFK под T2" и "залип в чужом jungle":
    -- nearest_e_tower (расстояние до ближайшей вражеской T1/T2), camp_idx
    -- (текущая цель в jungle_farm), camps_active/total (сколько кампов своей
    -- команды живо для blacklist).
    if tick - BotControllerState.log_tick_last >= 50 then
        BotControllerState.log_tick_last = tick
        local c = ctx._counts or {}
        LogBrain(string.format(
            "CTX t=%d state=%s phase=%s/%.0fs hp=%.2f pos=(%d,%d) dLane=%d team=%d lane=%d | "
            .. "counts: enem=%d ally=%d e_creep=%d f_creep=%d e_tower=%d neut=%d | "
            .. "fc=%s glob=%s td=%s push=%s nstreak=%d near_e=%d near_eT=%d "
            .. "camp_idx=%s near_camp=%d camps=%d/%d gt=%.1f",
            tick,
            FSM.Name[BotControllerState.fsm_state] or "?",
            ctx.farm_phase or "?",
            ctx.farm_phase_elapsed or 0,
            ctx.hp_pct or 0,
            math.floor((ctx.pos and ctx.pos.x) or 0),
            math.floor((ctx.pos and ctx.pos.y) or 0),
            math.floor(Dist2D(ctx.pos, ctx.lane_front)),
            ctx.team or 0, ctx.lane or 0,
            c.nearby_enemies or 0, c.nearby_allies or 0,
            c.nearby_creeps or 0, c.friendly_creeps or 0,
            c.nearby_towers or 0, c.neutrals or 0,
            tostring(ctx.has_friendly_creeps),
            tostring(ctx.lane_has_creeps_global),
            tostring(ctx.near_enemy_tower_danger),
            tostring(ctx.pushable),
            ctx.no_creeps_streak or 0,
            math.floor(ctx.nearest_enemy_dist or 0),
            math.floor(ctx.nearest_enemy_tower_dist or 0),
            tostring(BotControllerState.current_target_camp_idx),
            math.floor(ctx.nearest_own_camp_dist or 0),
            c.own_camps_active or 0, c.own_camps_total or 0,
            ctx.game_time or 0
        ))
    end

    -- ALERT-уровневые события — на изменение, не на тик. Диагностика частых
    -- баг-сценариев без захламления лога.
    -- 1. Tower-danger transition: вошли/вышли из 1400 от чужой башни.
    do
        local prev = BotControllerState._was_tower_danger
        local cur  = ctx.near_enemy_tower_danger == true
        if prev ~= cur then
            BotControllerState._was_tower_danger = cur
            LogBrain(string.format(
                "[ALERT] tower_danger %s | state=%s phase=%s pos=(%d,%d) hp=%.2f "
                .. "near_eT=%d f_creeps=%s tick=%d gt=%.1f",
                cur and "ENTER" or "EXIT",
                FSM.Name[BotControllerState.fsm_state] or "?",
                ctx.farm_phase or "?",
                math.floor(ctx.pos.x), math.floor(ctx.pos.y),
                ctx.hp_pct or 0,
                math.floor(ctx.nearest_enemy_tower_dist or 0),
                tostring(ctx.has_friendly_creeps),
                tick, ctx.game_time or 0))
        end
    end

    -- 2. Stuck detection: позиция почти не меняется N тиков. Помогает увидеть
    -- "стоит АФК": если бот в 100 unit радиусе того же спот'а 30 тиков (3 сек)
    -- и ни в combat (no enemy <600) ни в attack-loop (state не FARM) — это
    -- настоящий АФК. Лог раз в 50 тиков пока стоит, чтобы видеть длительность.
    do
        local last_pos = BotControllerState._stuck_last_pos
        local last_t   = BotControllerState._stuck_last_changed_tick or tick
        local moved = (not last_pos) or
            (math.abs(ctx.pos.x - last_pos.x) > 100) or
            (math.abs(ctx.pos.y - last_pos.y) > 100)
        if moved then
            BotControllerState._stuck_last_pos = {x = ctx.pos.x, y = ctx.pos.y}
            BotControllerState._stuck_last_changed_tick = tick
            BotControllerState._stuck_logged_at = nil
        else
            local stuck_ticks = tick - last_t
            local should_log =
                stuck_ticks >= 30 and
                (not BotControllerState._stuck_logged_at or
                 tick - BotControllerState._stuck_logged_at >= 50)
            if should_log then
                BotControllerState._stuck_logged_at = tick
                LogBrain(string.format(
                    "[ALERT] STUCK %d ticks | state=%s phase=%s pos=(%d,%d) hp=%.2f "
                    .. "near_e=%d near_eT=%d camp_idx=%s f_creeps=%s e_creeps=%d gt=%.1f",
                    stuck_ticks,
                    FSM.Name[BotControllerState.fsm_state] or "?",
                    ctx.farm_phase or "?",
                    math.floor(ctx.pos.x), math.floor(ctx.pos.y),
                    ctx.hp_pct or 0,
                    math.floor(ctx.nearest_enemy_dist or 0),
                    math.floor(ctx.nearest_enemy_tower_dist or 0),
                    tostring(BotControllerState.current_target_camp_idx),
                    tostring(ctx.has_friendly_creeps),
                    (ctx._counts and ctx._counts.nearby_creeps) or 0,
                    ctx.game_time or 0))
            end
        end
    end

    -- Active-item dispatcher. Запускается перед handler.Run — magic_stick/wand,
    -- mango, BKB, blink, force_staff, glimmer, eul, manta, mask_of_madness toggle,
    -- armlet toggle, refresher и ~30 других кастуются по state-aware условиям из
    -- util/items.lua. Anti-ban cooldown через AB.CanCast (per-item).
    -- Если item использован — НЕ запускаем handler.Run в этом же тике, чтобы не
    -- отменить animation повторным ордером.
    if Items and Items.UseActiveItems then
        local fsm_name = FSM.Name[BotControllerState.fsm_state] or "?"
        local iOk, iUsed, iName = pcall(Items.UseActiveItems, bot, ctx, fsm_name)
        if not iOk then
            LogBrain("ITEMS-ERR " .. fsm_name .. ": " .. tostring(iUsed))
        elseif iUsed then
            return "ITEM:" .. tostring(iName or "?"), true
        end
    end

    -- Autocast safety net — гарантирует что AUTOCAST-абилки (Sniper Take Aim,
    -- Drow Frost Arrows, WK Vampiric Spirit, Necro Heartstopper, etc.) включены
    -- даже если hero override не выполнился (нет ctx.nearest_enemy / state не
    -- подходит). Throttle ~30 тиков (≈4с) внутри функции — дёшево вызывать
    -- каждый тик. Не блокируем dispatch (handler.Run всегда запускается дальше).
    if Combat and Combat.EnsureAutocasts then
        local eOk, eErr = pcall(Combat.EnsureAutocasts, bot, ctx)
        if not eOk then LogBrain("ENSURE_AUTOCAST-ERR: " .. tostring(eErr)) end
    end

    -- Run current state handler
    local handler = States[BotControllerState.fsm_state]
    if not handler or not handler.Run then
        LogBrain("NO_HANDLER state=" .. (FSM.Name[BotControllerState.fsm_state] or "?"))
        return "NO_HANDLER", false
    end

    local rOk, action = pcall(handler.Run, bot, ctx)
    if not rOk then
        LogBrain("RUN-ERR " .. (FSM.Name[BotControllerState.fsm_state] or "?")
                 .. ": " .. tostring(action))
        return (FSM.Name[BotControllerState.fsm_state] or "?") .. ":ERROR", false
    end

    return (FSM.Name[BotControllerState.fsm_state] or "?")
           .. ":" .. tostring(action or "?"), true
end

-- ── Global alias для Andromeda DLL ──
-- DLL ищет standard Valve bot API: global `Think()` функция.
Think = BotController_Think

-- ВАЖНО: НЕ переопределяем `_G.Modes` — оно используется Valve legacy AI scripts.
-- Наш override ломает их → синхронный crash при match accept.

print("[bot_controller] loaded v2 FSM (7 states, deterministic transitions)")
print("[bot_controller] exported global Think (Modes left to Valve legacy)")
