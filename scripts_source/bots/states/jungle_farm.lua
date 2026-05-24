-- states/jungle_farm.lua — фарм нейтралов. Порт из старого modes/jungle_farm.lua,
-- но без GetDesire (transition gate теперь в fsm.lua).
--
-- Self-healing blacklist: камп считается мёртвым если бот пришёл (dist <
-- camp_arrival_radius) и через camp_empty_check_ticks ctx.neutrals всё ещё
-- пуст. После camp_miss_threshold подряд таких промахов — blacklist на
-- camp_blacklist_seconds. Координаты в util/camps.lua больше не нужно
-- верифицировать вручную; сам себя лечит.

local M = { name = "JUNGLE_FARM" }
local Combat = require("util.combat")
local vec    = require("util.vec")
local Dist2D = vec.Dist2D
local _CoopOk, Coop = pcall(require, "util.coop")
if not _CoopOk then Coop = nil end

-- Backwards-compat shim: C++ exposes IsEntityValid(handle) → bool после
-- CEntityCache fix. Если DLL не задеплоена с биндингом — функция отсутствует
-- и valid=true (старое поведение, IsNull/IsAlive остаются единственным гейтом).
local _IsEntityValid = _G.IsEntityValid or function() return true end

-- Camps metadata (difficulty lookup). pcall — если util/camps.lua не загружен
-- через bot_controller override, DifficultyOf() возвращает "medium" graceful.
local _CampsOk, Camps = pcall(require, "util.camps")
if not _CampsOk then Camps = nil end
local function _difficultyOf(pos)
    if Camps and Camps.DifficultyOf then return Camps.DifficultyOf(pos) end
    return "medium"
end

-- Кэш плоского списка кампов своей команды. F9-reload пересоздаёт upvalue →
-- автоматом инвалидируется. Индекс — стабильный ключ для blacklist'а в одной
-- катке (ResetForNewMatch обнуляет blacklist целиком).
--
-- КРИТИЧНО: фильтруем по своей команде. Раньше итерировались ВСЕ кампы
-- (radiant + dire), и для radiant top-laner'а pushed вперёд "ближайшим"
-- становился dire top easy V(-3901, 4903) — он стоит в 200 unit от dire top
-- T2 (-3700, 4900). Бот шёл туда, попадал под аггро T2, стоял на arrival_radius
-- т.к. neutrals=0 (это не его лес) → AFK под чужой башней. Это и есть баг
-- "стоят АФК под Т2 думают что фармят лес".
local _campCacheByTeam = {}  -- {[team]: list}
local function GetOwnCamps(team)
    if _campCacheByTeam[team] then return _campCacheByTeam[team] end
    local list = {}
    if not GetNeutralCampLocations then
        _campCacheByTeam[team] = list
        return list
    end
    local tbl = GetNeutralCampLocations()
    if type(tbl) ~= "table" then
        _campCacheByTeam[team] = list
        return list
    end
    -- team=2 Radiant, team=3 Dire
    local key = (team == 2) and "radiant" or "dire"
    local teamTbl = tbl[key]
    if type(teamTbl) == "table" then
        for _, pts in pairs(teamTbl) do
            if type(pts) == "table" then
                for _, p in ipairs(pts) do
                    table.insert(list, p)
                end
            end
        end
    end
    _campCacheByTeam[team] = list
    print(string.format("[jungle] cached %d own camps for team=%d (%s)",
        #list, team, key))
    return list
end

local function NearestCamp(ctx)
    local gt = ctx.game_time or 0
    local tick = ctx.tick or 0
    local blacklist = (BotControllerState and BotControllerState.camp_blacklist) or {}
    -- Отдельный tick-based blacklist (set by stuck-detection в M.Run). Не
    -- конфликтует с gt-based camp_blacklist (camp_miss_threshold path).
    local blacklist_ticks = (BotControllerState and BotControllerState.camp_blacklist_ticks) or {}
    local hero_level = ctx.level or 1
    local best, bestD, bestIdx = nil, 99999, nil
    for idx, c in ipairs(GetOwnCamps(ctx.team or 2)) do
        local until_gt   = blacklist[idx]
        local until_tick = blacklist_ticks[idx]
        local diff = _difficultyOf(c)
        -- Skip ancient camps until level 6+. Low-level WK получит burst damage
        -- от Granite/Mud Golem'ов, не успев их даунить — типичная смерть в логе
        -- jungle_critical hp=0.28.
        local skip_ancient = (diff == "ancient" and hero_level < 6)
        local gt_skip   = (until_gt   and until_gt   > gt)
        local tick_skip = (until_tick and until_tick > tick)
        if not skip_ancient and not gt_skip and not tick_skip then
            local d = Dist2D(ctx.pos, c)
            if d < bestD then best, bestD, bestIdx = c, d, idx end
        end
    end
    return best, bestD, bestIdx
end

-- Состояние live-цикла WALK_CAMP. Ключи лежат в BotControllerState чтобы
-- переживать reload и быть видимыми в diagnostics.
local function _resetTarget()
    BotControllerState.current_target_camp_idx     = nil
    BotControllerState.current_target_arrived_tick = nil
end

function M.Run(bot, ctx)
    local gt = ctx.game_time or 0
    local C  = BotControllerConfig or {}
    BotControllerState.camp_misses    = BotControllerState.camp_misses    or {}
    BotControllerState.camp_blacklist = BotControllerState.camp_blacklist or {}

    -- Co-op group resolution. Раз в 15 тиков перерасчёт. Если allies API не
    -- доступен / total<2 / coop disabled — group=nil → solo path. Solo path
    -- сохраняет hp_pct>=0.7 threshold для входа в JUNGLE_FARM (это решает
    -- fsm.cond_phase_switch_to_jungle и cond_jungle_critical), здесь же мы
    -- просто живём solo логикой.
    local group = (Coop and C.jungle_coop_enabled ~= false) and Coop.GetJungleGroup(bot, ctx) or nil
    local coop_active = group and group.size >= 2 or false

    -- 1. Есть нейтралы рядом — атакуем. Это validates active camp: если бот
    --    пришёл к target_idx и видит нейтралов → camp живой, miss-counter
    --    сбрасываем.
    if ctx.neutrals and #ctx.neutrals > 0 then
        local curCamp = BotControllerState.current_target_camp_idx
        if curCamp then BotControllerState.camp_misses[curCamp] = 0 end

        -- Shared focus fire: лидер группы выбирает target; non-leader'ы
        -- атакуют того же. Уменьшает overkill, ускоряет camp clear → меньше
        -- damage на ботов.
        local target
        if coop_active then
            local shared = Coop.GetGroupSharedTarget(group, ctx, ctx.neutrals,
                                                    Combat.WeakestAlive)
            if shared and _IsEntityValid(shared) then
                target = shared
                -- Sync кеш чтобы legacy stable-target trace продолжал работать.
                BotControllerState.jungle_attack_target = shared
            end
        end

        -- Solo fallback / coop пока без shared target (лидер ещё не пришёл).
        if not target then
            -- Stable target: пока цель жива и в радиусе, продолжаем её бить. Без
            -- этого WeakestAlive/NearestAlive каждый тик мог возвращать разного
            -- нейтрала (HP%-ties / порядок в массиве), Action_AttackUnit cancel'ил
            -- замах. Это и есть "мили в лесу делают бесконечные замахи без удара".
            local cur = BotControllerState.jungle_attack_target
            -- Handle revalidation (C++ CEntityCache может вернуть stale handle на
            -- сущность которая была разыграна и теперь slot занят другим объектом).
            if cur and not _IsEntityValid(cur) then
                cur = nil
                BotControllerState.jungle_attack_target = nil
            end
            if cur and not cur:IsNull() and cur:IsAlive() then
                local d = Dist2D(ctx.pos, cur:GetLocation())
                if d < 1200 then target = cur end
            end
            if not target then
                local picked = Combat.WeakestAlive(ctx.neutrals)
                               or Combat.NearestAlive(ctx.neutrals, ctx.pos)
                -- Reject невалидный пик (handle slot reused / freed) — иначе
                -- BotControllerState.jungle_attack_target залипнет на мусоре.
                if picked and _IsEntityValid(picked) then
                    target = picked
                end
                BotControllerState.jungle_attack_target = target
            end
        end

        if target then
            -- Stuck-target detection: цель в attack range, AttackUnit летит
            -- каждые ~4с но HP цели не падает >4с (target_hp_unchanged_ticks
            -- считается в context.lua). Значит damage не доходит — windup
            -- cancel, target unreachable из-за tree collision, или мы стоим
            -- вне attack range и MoveToOnce-fallback не довозит. Blacklist
            -- camp на 90с (tick-based), сброс target и переход в LANE_WAIT
            -- через cond_stuck_jungle на следующем тике.
            if (ctx.target_hp_unchanged_ticks or 0) >= 30 then
                BotControllerState.camp_blacklist_ticks = BotControllerState.camp_blacklist_ticks or {}
                local curIdx = BotControllerState.current_target_camp_idx
                if curIdx then
                    -- ~7 ticks/sec по проекту → 90s ≈ 630 ticks
                    BotControllerState.camp_blacklist_ticks[curIdx] = (ctx.tick or 0) + (90 * 7)
                end
                BotControllerState.jungle_attack_target = nil
                BotControllerState.target_hp_unchanged_ticks = 0
                _resetTarget()
                print(string.format(
                    "[jungle] CAMP_TARGET_STUCK blacklisting camp_idx=%s tick=%d gt=%.1f",
                    tostring(curIdx), ctx.tick or 0, gt))
                return "CAMP_TARGET_STUCK"
            end

            -- НЕ кастуем generic spells на нейтралов: smart dispatcher (Phase 4)
            -- увидит UNIT_TARGET nuke как «готов» каждый тик cooldown'а →
            -- Action_UseAbilityOnEntity(spell, neutral) спамится → cancel
            -- auto-attack windup → нейтрал не умирает. Hero overrides
            -- (sniper Shrapnel, gyrocopter Rocket Barrage, и т.п.) кастуют
            -- свою farm-AoE логику ДО handler.Run в bot_controller — там у
            -- них есть точечные правила. Здесь только auto-attack.
            --
            -- AttackUnitOnce dedup'ит ордер; если target дальше attack_range,
            -- сам fallback'нёт на MoveToOnce(target.pos) чтобы добежать.
            Combat.AttackUnitOnce(bot, target, ctx)
            if coop_active then
                return group.is_leader and "ATTACK_NEUTRAL_COOP_LEAD"
                                        or "ATTACK_NEUTRAL_COOP_FOLLOW"
            end
            return "ATTACK_NEUTRAL"
        end
    else
        -- Нет нейтралов в радиусе — старый target неактуален.
        BotControllerState.jungle_attack_target = nil
        Combat.ResetAttackOrder()
    end

    -- 2. Идём к camp'у. Coop path: лидер выбирает общий через PickCampForGroup
    --    и пишет в lock, не-лидер читает lock. Если lock пуст (ещё не записан) —
    --    fallback на solo NearestCamp до следующего тика.
    local camp, dist, idx
    if coop_active then
        local ownCamps = GetOwnCamps(ctx.team or 2)
        camp, idx = Coop.PickCampForGroup(group, ctx, ownCamps, _difficultyOf)
        if camp and idx then
            dist = Dist2D(ctx.pos, camp)
        end
    end
    if not camp then
        camp, dist, idx = NearestCamp(ctx)
    end
    if not camp or not idx then
        -- Все кампы blacklist'ed → IDLE. fsm.cond_jungle_lane_empty подавляется
        -- семафором пока phase=jungle, но если phase=lane вернётся — переход
        -- сработает быстро. Отдельный fail-safe не нужен.
        if BotControllerState._last_jungle_log_tick ~= ctx.tick then
            BotControllerState._last_jungle_log_tick = ctx.tick
            print(string.format(
                "[jungle] ALL_CAMPS_BLACKLISTED tick=%d pos=(%d,%d) gt=%.1f",
                ctx.tick or 0,
                math.floor(ctx.pos.x), math.floor(ctx.pos.y), gt))
        end
        return "ALL_CAMPS_BLACKLISTED"
    end

    -- Сменили target — ресет arrival tracker + лог.
    if BotControllerState.current_target_camp_idx ~= idx then
        local prev = BotControllerState.current_target_camp_idx
        BotControllerState.current_target_camp_idx     = idx
        BotControllerState.current_target_arrived_tick = nil
        -- Diagnostic: per-type breakdown EntityCache (новый Lua-binding из C++).
        -- Раньше cache_neut=1 показывал "1 нейтрал в кэше" но это был Roshan
        -- (NEUTRAL+ROSHAN объединены в GetNeutrals). Теперь раздельно: H/C/N/R
        -- = hero/creep/neutral/roshan + T = tree. Если N=0 на 50-й минуте при
        -- активном джунгле = OnAddEntity hook не ловит jungle creep'ов.
        local s = (GetEntityCacheStats and GetEntityCacheStats()) or {}
        print(string.format(
            "[jungle] PICK_CAMP idx=%d (was=%s) target=(%d,%d) dist=%d pos=(%d,%d) team=%d gt=%.1f"
            .. " | cache: H=%d C=%d N=%d R=%d T=%d B=%d Tr=%d",
            idx, tostring(prev),
            math.floor(camp.x), math.floor(camp.y),
            math.floor(dist),
            math.floor(ctx.pos.x), math.floor(ctx.pos.y),
            ctx.team or 0, gt,
            s.hero or 0, s.creep or 0, s.neutral or 0, s.roshan or 0,
            s.tower or 0, s.building or 0, s.tree or 0))
    end

    -- Прибыли (dist < arrival_radius) — короткая проверка наличия нейтралов и
    -- сразу blacklist если пусто. Раньше ждали camp_empty_check_ticks (60 t)
    -- × 3 misses = 180 тиков на камп — даже на полностью мёртвых картах. Это
    -- AttackMove-эра логика когда `ctx.neutrals` мог отстать на 1-2 тика после
    -- спавна. Сейчас (по жалобе юзера) AttackMove ненадёжен и ждать смысла
    -- мало — если на arrival нейтралов нет, значит camp реально пустой,
    -- идём к следующему. Hysteresis: 5 тиков grace period (~0.7с) на случай
    -- задержки EntityCache.
    --
    -- Strict-stand gate (dist <= 200): Viper-кейс — бот ПРОХОДИЛ ТРАНЗИТОМ
    -- через arrivalR=600 далёкого камп-A, идя к камп-B на 2120 unit. На пике
    -- транзита dist падал до 500, тригерил arrival → grace 5 ticks истекал →
    -- camp_A blacklist → next camp → опять транзит → 12 тиков, 3 камп-точки
    -- помечены пустыми, бот вышел в LANE_WAIT при якобы фарме. Истинная цель
    -- arrival — бот РЕАЛЬНО СТОИТ на камп-точке (ближе 200 unit), а не просто
    -- пересёк radius. Поэтому блокируем blacklist пока dist > 200 — пусть
    -- продолжает идти (WALK_CAMP fall-through ниже).
    local arrivalR = C.camp_arrival_radius or 600.0
    local strictStand = C.camp_strict_stand_radius or 200.0
    if dist < arrivalR and dist <= strictStand then
        local arrived = BotControllerState.current_target_arrived_tick
        if not arrived then
            BotControllerState.current_target_arrived_tick = ctx.tick or 0
            print(string.format(
                "[jungle] ARRIVED camp #%d at tick=%d (dist=%d) check pending",
                idx, ctx.tick or 0, math.floor(dist)))
        else
            local elapsed_ticks = (ctx.tick or 0) - arrived
            -- 5-тик grace: возможно EntityCache ещё не отрисовал свежеспавнившихся.
            if elapsed_ticks >= 5 then
                local bl = BotControllerState.camp_blacklist
                local misses = BotControllerState.camp_misses
                misses[idx] = (misses[idx] or 0) + 1
                local threshold = C.camp_miss_threshold or 3
                print(string.format(
                    "[jungle] CAMP_EMPTY camp #%d miss=%d/%d (waited=%d ticks, grace=5)",
                    idx, misses[idx], threshold, elapsed_ticks))
                if misses[idx] >= threshold then
                    local secs = C.camp_blacklist_seconds or 60
                    bl[idx] = gt + secs
                    print(string.format(
                        "[jungle] BLACKLIST camp #%d for %ds (gt=%.1f, until=%.1f)",
                        idx, secs, gt, bl[idx]))
                    misses[idx] = 0
                end
                -- Coop: сбрасываем group lock чтобы лидер выбрал свежий camp.
                if coop_active and Coop and group.is_leader then
                    Coop.ResetGroupCamp(group)
                end
                _resetTarget()
                return "CAMP_EMPTY"
            end
        end
    elseif BotControllerState.current_target_arrived_tick then
        -- Бот ранее вошёл в strict-stand радиус, но сейчас вышел (push,
        -- knockback, изменение target позиции после re-pick). Сбрасываем
        -- arrival tracker, иначе stale tick срабатывал бы elapsed_ticks>=5
        -- мгновенно при следующем входе.
        BotControllerState.current_target_arrived_tick = nil
    end

    -- Pure MOVE (OrderType 1) вместо AttackMove (OrderType 3). По наблюдениям
    -- юзера: AttackMove ненадёжен — бот может залипнуть на дереве, отвлечься
    -- на крипа волны проходящей через jungle, прервать движение от tower-aggro
    -- проверки. Чистый MOVE доходит до точки гарантированно. Когда дойдём,
    -- arrival check проверит neutrals — если есть, шаг 1 (`ctx.neutrals > 0`)
    -- атакует, если нет — blacklist и идём к следующему. Никакого автонаведения
    -- по пути не нужно: jungle creep'ы между нашим стартом и target'ом — не
    -- наша забота, бот не должен на них отвлекаться, ему надо до КАМПА дойти.
    Combat.MoveToOnce(bot, camp, ctx)
    if coop_active then
        return group.is_leader and "WALK_CAMP_COOP_LEAD" or "WALK_CAMP_COOP_FOLLOW"
    end
    return "WALK_CAMP"
end

return M
