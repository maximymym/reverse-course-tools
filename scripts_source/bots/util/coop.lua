-- util/coop.lua — co-op jungle farming координатор.
--
-- ПРОБЛЕМА: 5 ботов в JUNGLE_FARM ходят соло. Один бот приходит к hard/medium
-- camp'у, neutrals выносят его HP 1.0→0.53 за 13с пока он стоит замахом
-- (BAT+windup), он не успевает отвечать достаточно урона. Особенно страдают
-- mages/supports: lich/lion/witch_doctor/ogre_magi.
--
-- РЕШЕНИЕ: каждый тик каждый бот вычисляет ту же самую группу farm-партнёров
-- ДЕТЕРМИНИРОВАННО — без IPC между процессами, без shared memory. Все 5 ботов
-- вычисляют одинаковое разбиение синхронно (потому что вход одинаковый — список
-- alive ally hero PlayerID, отсортированный) и приходят к одному и тому же
-- camp с shared focus fire на одного нейтрала.
--
-- АЛГОРИТМ:
-- 1. GetUnitList(UNIT_LIST_ALLIED_HEROES) → все союзные герои (включая мёртвых,
--    их нужно отфильтровать по IsAlive() — мёртвый бот не участвует в фарме).
--    GetPlayerID() даёт стабильный 0..9 ID — наш «NetID».
-- 2. Sort по PlayerID ascending. Get index of self (через GetPlayerID==my_pid).
-- 3. Split на 2 группы: первые `group_a_size` (default 3) → группа A;
--    остальные → группа B. При размере общего списка <2 — fallback на solo.
-- 4. Для всей группы выбираем общий camp = ближайший к centroid группы (если
--    группа уже на camp'е — продолжаем фармить, не меняем). Camp идёт через
--    тот же blacklist что и solo (BotControllerState.camp_blacklist).
-- 5. Shared focus fire: лидер группы (member[1] по sorted PlayerID) выбирает
--    target нейтрала и пишет в shared cache (BotControllerState.coop_*). Все
--    остальные читают и атакуют того же. Это уменьшает overkill (3 бота лупят
--    одного 50hp нейтрала — лучше так чем каждый свой weakest).
--
-- ВАЖНО: caller (jungle_farm.lua) сам решает использовать ли coop. Если
-- group.size<2 → solo path с hp_pct>=0.7 threshold. Coop сам не вызывает
-- AttackUnit; возвращает данные, jungle_farm применяет.
--
-- Кеш группы пересчитывается раз в N тиков (group_recompute_ticks=15 ≈ 2с).
-- Между пересчётами лежит в BotControllerState.coop_group_cache.

local M = {}
local vec = require("util.vec")
local Dist2D = vec.Dist2D

-- Список alive ally hero proxy-объектов с PlayerID — глобально, через
-- GetUnitList(2)=UNIT_LIST_ALLIED_HEROES. Этот binding видит ВСЕХ союзников
-- по карте (не radius-limited как bot:GetNearbyHeroes). Если binding нет —
-- возвращаем nil чтобы caller перешёл на solo.
local function _gatherAllies()
    if not GetUnitList then return nil end
    local ok, list = pcall(GetUnitList, 2)  -- UNIT_LIST_ALLIED_HEROES
    if not ok or type(list) ~= "table" then return nil end
    local out = {}
    for _, u in ipairs(list) do
        if u and not u:IsNull() and u:IsAlive() and u.GetPlayerID then
            local pid = u:GetPlayerID()
            if pid and pid >= 0 then
                table.insert(out, { unit = u, pid = pid })
            end
        end
    end
    table.sort(out, function(a, b) return a.pid < b.pid end)
    return out
end

-- Вычислить group split по конфигу. allies = sorted list of {unit, pid}.
-- Возвращает {a = {...}, b = {...}}. group_a_size — первые N pid'ов идут в A.
local function _splitGroups(allies, group_a_size)
    local a, b = {}, {}
    for i, e in ipairs(allies) do
        if i <= group_a_size then table.insert(a, e) else table.insert(b, e) end
    end
    return { a = a, b = b }
end

-- Найти, в какой группе находится бот по pid. Возвращает (group_table,
-- group_name, my_index_in_group). Если не найден (странно — IsAlive=false на
-- момент _gatherAllies?) — nil.
local function _findMyGroup(groups, my_pid)
    for _, gname in ipairs({ "a", "b" }) do
        local g = groups[gname]
        for i, e in ipairs(g) do
            if e.pid == my_pid then return g, gname, i end
        end
    end
    return nil, nil, nil
end

-- Public: GetJungleGroup(bot) — возвращает {members={pid,...}, leader_pid,
-- group_name, my_index, size}. nil если allies API недоступен или бот не
-- найден в списке. Кеш в BotControllerState.coop_group_cache (15 тиков ≈ 2с).
function M.GetJungleGroup(bot, ctx)
    local C = BotControllerConfig or {}
    if C.jungle_coop_enabled == false then return nil end

    local tick = (ctx and ctx.tick) or 0
    local cached = BotControllerState.coop_group_cache
    if cached and (tick - (cached.tick or -999)) < (C.jungle_group_recompute_ticks or 15) then
        return cached.group
    end

    if not bot or bot:IsNull() or not bot.GetPlayerID then return nil end
    local my_pid = bot:GetPlayerID()
    if not my_pid or my_pid < 0 then return nil end

    local allies = _gatherAllies()
    if not allies or #allies == 0 then
        BotControllerState.coop_group_cache = { tick = tick, group = nil }
        return nil
    end

    local total = #allies
    local group_a_size = C.jungle_group_a_size or 3
    -- Sanity clamp: a_size в [1, total-1] чтобы у каждой группы было ≥1
    -- участник. Если total<2 — coop невозможен, возвращаем nil (solo).
    if total < 2 then
        BotControllerState.coop_group_cache = { tick = tick, group = nil }
        return nil
    end
    if group_a_size >= total then group_a_size = total - 1 end
    if group_a_size < 1 then group_a_size = 1 end

    local groups = _splitGroups(allies, group_a_size)
    local g, gname, idx = _findMyGroup(groups, my_pid)
    if not g then
        BotControllerState.coop_group_cache = { tick = tick, group = nil }
        return nil
    end

    local members = {}
    for _, e in ipairs(g) do table.insert(members, e.pid) end

    local result = {
        members      = members,        -- list of pid в том же порядке
        member_units = g,              -- list of {unit, pid} — caller может читать unit
        leader_pid   = members[1],     -- младший pid в группе
        group_name   = gname,          -- "a" / "b"
        my_index     = idx,            -- 1..size
        my_pid       = my_pid,
        size         = #members,
        is_leader    = (members[1] == my_pid),
    }
    BotControllerState.coop_group_cache = { tick = tick, group = result }
    return result
end

-- Centroid группы (среднее живых членов). Возвращает Vector or nil.
local function _groupCentroid(group)
    if not group or not group.member_units then return nil end
    local cx, cy, n = 0, 0, 0
    for _, e in ipairs(group.member_units) do
        if e.unit and not e.unit:IsNull() and e.unit:IsAlive() then
            local p = e.unit:GetLocation()
            if p then
                cx = cx + p.x; cy = cy + p.y; n = n + 1
            end
        end
    end
    if n == 0 then return nil end
    return Vector(cx / n, cy / n, 128)
end

-- Public: PickCampForGroup(group, ctx, all_camps_iter) — выбирает общий camp
-- для всей группы. all_camps_iter — функция (idx, camp_pos) iterator из
-- jungle_farm. Логика: ближайший к centroid, не blacklist'ed, не ancient если
-- mid-level бот в группе (берём min level).
--
-- Stickiness: если у группы уже есть зафиксированный camp идёт в кеш
-- BotControllerState.coop_group_<gname>_camp_idx. Лидер обновляет его. Не-лидеры
-- читают. Это предотвращает дрейф когда члены группы по-разному оценивают
-- "ближайший" из-за разных pos.
--
-- Lock TTL: 90 sec game-time или пока camp не blacklist'ed. Лидер может
-- сбросить если bot:JustArrived & camp empty — это делает jungle_farm через
-- ResetGroupCamp().
function M.PickCampForGroup(group, ctx, ownCamps, difficultyOf)
    if not group then return nil, nil end
    local C  = BotControllerConfig or {}
    local gt = (ctx and ctx.game_time) or 0
    local blacklist       = (BotControllerState and BotControllerState.camp_blacklist)       or {}
    local blacklist_ticks = (BotControllerState and BotControllerState.camp_blacklist_ticks) or {}
    local tick = (ctx and ctx.tick) or 0

    BotControllerState.coop_group_camp = BotControllerState.coop_group_camp or {}
    local locks = BotControllerState.coop_group_camp
    local lock = locks[group.group_name]

    local function _isCampValid(idx, camp)
        if not camp or not idx then return false end
        local until_gt   = blacklist[idx]
        local until_tick = blacklist_ticks[idx]
        if until_gt and until_gt > gt then return false end
        if until_tick and until_tick > tick then return false end
        return true
    end

    -- Sticky: если у группы уже выбран camp и он всё ещё валиден — держим.
    -- TTL ~30 sec game-time (если группа не дойдёт за 30s, что-то не так —
    -- пересчитываем).
    if lock and lock.idx and lock.camp then
        local age = gt - (lock.gt or 0)
        if age < (C.jungle_group_lock_seconds or 30) and _isCampValid(lock.idx, lock.camp) then
            return lock.camp, lock.idx
        end
    end

    -- Только лидер группы выбирает новый camp — иначе non-leader'ы могут
    -- read stale lock и leader конкурирует с non-leader'ом за разные camps.
    -- Non-leader'ы возвращают nil → jungle_farm fallback'нётся на solo
    -- NearestCamp до следующего тика когда лидер запишет lock.
    if not group.is_leader then
        return nil, nil
    end

    -- Min hero level в группе — для skip ancient camps.
    local min_level = 99
    for _, e in ipairs(group.member_units or {}) do
        if e.unit and not e.unit:IsNull() and e.unit:IsAlive() and e.unit.GetLevel then
            local l = e.unit:GetLevel() or 1
            if l < min_level then min_level = l end
        end
    end

    local centroid = _groupCentroid(group) or (ctx and ctx.pos)
    if not centroid then return nil, nil end

    local best, bestD, bestIdx = nil, 99999, nil
    for idx, camp in ipairs(ownCamps or {}) do
        if _isCampValid(idx, camp) then
            local diff = difficultyOf and difficultyOf(camp) or "medium"
            local skip_ancient = (diff == "ancient" and min_level < 6)
            if not skip_ancient then
                local d = Dist2D(centroid, camp)
                if d < bestD then best, bestD, bestIdx = camp, d, idx end
            end
        end
    end

    if best and bestIdx then
        locks[group.group_name] = { idx = bestIdx, camp = best, gt = gt }
    end
    return best, bestIdx
end

-- Public: ResetGroupCamp(group) — лидер вызывает когда camp пустой/мёртвый
-- чтобы non-leader'ы перестали идти к старому target'у. Просто стирает lock.
function M.ResetGroupCamp(group)
    if not group then return end
    local locks = BotControllerState and BotControllerState.coop_group_camp
    if not locks then return end
    locks[group.group_name] = nil
end

-- Public: GetGroupSharedTarget(group, neutrals) — лидер выбирает один target
-- (weakest alive) и пишет в BotControllerState.coop_group_target[gname]. Все
-- читают. Предотвращает overkill / damage-spread между нейтралами.
--
-- ВАЖНО: cache валиден только этот тик (target invalid frame-to-frame если
-- handle slot reuse'нулся). Записываем pid лидера + tick + handle. Non-leader
-- проверяет: tick совпадает ± допуск, handle alive, в радиусе 1200 от своего
-- pos (если нейтрал не виден через GetNearbyNeutralCreeps бота — его 1200
-- radius — то атаковать его нельзя; AttackUnitOnce затем перейдёт в
-- MoveToOnce и подтянется).
function M.GetGroupSharedTarget(group, ctx, neutrals_for_self, weakestPicker)
    if not group or group.size < 2 then return nil end
    local tick = (ctx and ctx.tick) or 0
    BotControllerState.coop_group_target = BotControllerState.coop_group_target or {}
    local cache = BotControllerState.coop_group_target

    local entry = cache[group.group_name]
    -- Лидер: каждый тик пере-выбирает (weakest alive в его neutrals_for_self —
    -- т.к. он первый прибывает и видит нейтралов).
    if group.is_leader then
        if neutrals_for_self and #neutrals_for_self > 0 and weakestPicker then
            local picked = weakestPicker(neutrals_for_self)
            if picked and not picked:IsNull() and picked:IsAlive() then
                cache[group.group_name] = {
                    target = picked, tick = tick, leader_pid = group.leader_pid
                }
                return picked
            end
        end
        cache[group.group_name] = nil
        return nil
    end

    -- Non-leader: читаем cache, проверяем валидность.
    if not entry then return nil end
    if entry.leader_pid ~= group.leader_pid then return nil end
    -- Допускаем staleness 5 тиков (~0.7с): лидер не успел в этом тике записать
    -- но в прошлом записал. Дольше — старый target вероятно мёртв.
    if (tick - (entry.tick or 0)) > 5 then return nil end
    local t = entry.target
    if not t or t:IsNull() or not t:IsAlive() then
        cache[group.group_name] = nil
        return nil
    end
    -- Проверка что non-leader бот видит этот target в своём 1200 радиусе.
    -- Если нет — возвращаем nil, jungle_farm выберет своего (всё равно
    -- лучше чем атаковать недосягаемого).
    if neutrals_for_self then
        for _, n in ipairs(neutrals_for_self) do
            if n and not n:IsNull() and n == t then return t end
        end
        return nil
    end
    return t
end

-- Public: HasArrivedMember(group, camp_pos, radius) — true если хотя бы один
-- член группы уже в радиусе camp'а. Используется для:
-- 1. Phase-switch ожидания: cond_phase_switch_to_jungle ждёт >=2 ally уже в
--    JUNGLE_FARM (или leader решает — fsm.lua).
-- 2. Follow-arrived в jungle_farm: если лидер уже на camp'е, подтянись быстрее.
function M.HasArrivedMember(group, camp_pos, radius)
    if not group or not camp_pos then return false end
    radius = radius or 700
    for _, e in ipairs(group.member_units or {}) do
        if e.unit and not e.unit:IsNull() and e.unit:IsAlive() then
            local p = e.unit:GetLocation()
            if p and Dist2D(p, camp_pos) < radius then return true end
        end
    end
    return false
end

-- Public: CountAlliesInState(state_name) — сколько ботов сейчас в JUNGLE_FARM
-- (кроме self). Используется в fsm.cond_phase_switch_to_jungle для требования
-- "хотя бы один союзник уже в лесу" — иначе бот рискует пойти соло.
--
-- Реализовано через bot.<key>: каждый бот пишет своё state в bot.fsm_state_name.
-- Lua-side это per-unit storage (entityIndex-keyed), читается из других ботов
-- через GetUnitList. UnitHandle '==' идентичность есть. Поскольку storage
-- глобальный (`_unit_storage[entityIndex]`), любой бот может прочитать чужой
-- ключ если у него есть UnitHandle на этого аллая.
function M.CountAlliesInState(group, state_name, my_pid)
    if not group then return 0 end
    local count = 0
    for _, e in ipairs(group.member_units or {}) do
        if e.pid ~= my_pid and e.unit and not e.unit:IsNull() and e.unit:IsAlive() then
            local s = e.unit.fsm_state_name
            if s == state_name then count = count + 1 end
        end
    end
    return count
end

-- Public: PublishMyState(bot, state_name) — записать своё текущее FSM state в
-- per-unit storage чтобы другие боты увидели. Вызывает bot_controller каждый
-- тик (один write — дёшево).
function M.PublishMyState(bot, state_name)
    if bot and not bot:IsNull() then
        bot.fsm_state_name = state_name
    end
    -- Также дублируем в BotControllerState (для self-introspection и логов).
    if BotControllerState then
        BotControllerState.fsm_state_name = state_name
    end
end

return M
