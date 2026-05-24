-- util/anti_ban.lua — jitter reactions, cast cooldowns, random chat.
-- Три анти-бан меры (A.1/A.2/A.3). Состояние живёт в BotControllerState.

local M = {}

-- ── A.1: Pending action queue ───────────────────────────────────
-- Когда логика решает что делать, не вызываем Action_* сразу — кладём в pending
-- и исполняем через rand(jitter_min, jitter_max) ticks. Человек не реагирует за 0ms.

function M.QueuePendingAction(action_name, fn)
    local C = BotControllerConfig
    local t = BotControllerState.tick
    local delay = RandomInt(C.jitter_min_ticks, C.jitter_max_ticks)
    -- Sub-tick offset: os.clock() даёт ~15ms precision на Windows. У 5 ботов
    -- с одинаковой jitter bucket этот доп. сдвиг 0-3 tick'а разбрасывает
    -- execution moments внутри tick'а. Малое значение (0-3) чтобы не ломать
    -- combat reaction.
    local subtick = math.floor(((os.clock() or 0) * 1000) % 4)
    BotControllerState.pending_action = action_name
    BotControllerState.pending_fn = fn
    BotControllerState.pending_reaction_tick = t + delay
    BotControllerState.pending_subtick = subtick
end

-- Возвращает true если pending action был выполнен (caller должен вернуться).
function M.TickPending()
    if not BotControllerState.pending_action then return false end
    if BotControllerState.tick < BotControllerState.pending_reaction_tick then
        return false
    end
    -- Ещё micro-shift внутри tick'а — разные боты в одной jitter bucket
    -- исполняются с небольшим сдвигом.
    local subtick = BotControllerState.pending_subtick or 0
    if subtick > 0 then
        BotControllerState.pending_subtick = subtick - 1
        return false
    end
    local fn = BotControllerState.pending_fn
    local act = BotControllerState.pending_action
    BotControllerState.pending_action = nil
    BotControllerState.pending_fn = nil
    BotControllerState.pending_subtick = nil
    if fn then
        local ok, err = pcall(fn)
        if not ok then
            print("[anti_ban] pending action error: " .. tostring(err))
        end
    end
    BotControllerState.last_executed_action = act
    return true
end

-- Бот в CRITICAL_RETREAT должен игнорировать pending (человек тоже реагирует
-- моментально на "сейчас умру").
function M.ClearPending()
    BotControllerState.pending_action = nil
    BotControllerState.pending_fn = nil
    BotControllerState.pending_subtick = nil
end

function M.HasPending()
    return BotControllerState.pending_action ~= nil
end

-- Полный сброс per-match state. Вызывается из bot_controller при detection
-- перехода GAME_STATE (POST_GAME → HERO_SELECTION). Без этого last_cast и
-- chat_sent_count копятся между катками.
function M.Reset()
    BotControllerState.last_cast = {}
    BotControllerState.chat_sent_count = 0
    BotControllerState.chat_next_tick = 0
    BotControllerState.pending_action = nil
    BotControllerState.pending_fn = nil
    BotControllerState.pending_subtick = nil
    BotControllerState.pending_reaction_tick = 0
    print("[anti_ban] Reset — last_cast/chat/pending cleared")
end

-- ── A.2: Cast cooldown ──────────────────────────────────────────
-- Нельзя кастовать один и тот же скилл по одной и той же цели чаще чем
-- rand(cast_cooldown_min, cast_cooldown_max) секунд.

-- Ключ = имя способности + (playerID/entity hash цели).
function M.CastKey(ability_name, target)
    local tid = 0
    if target and target.GetPlayerID then
        tid = target:GetPlayerID() or 0
    end
    if tid == 0 and target and target.GetUnitName then
        local n = target:GetUnitName() or ""
        tid = (string.byte(n, 1) or 0)
              + (string.byte(n, 2) or 0) * 256
              + (string.byte(n, 3) or 0) * 65536
    end
    return ability_name .. "_" .. tostring(tid)
end

-- Возвращает true если касту разрешён (cooldown истёк).
function M.CanCast(ability_name, target, gameTime)
    local C = BotControllerConfig
    local key = M.CastKey(ability_name, target)
    local last = (BotControllerState.last_cast or {})[key] or -999
    local req = RandomFloat(C.cast_cooldown_min, C.cast_cooldown_max)
    return (gameTime - last) >= req
end

function M.MarkCast(ability_name, target, gameTime)
    local key = M.CastKey(ability_name, target)
    BotControllerState.last_cast = BotControllerState.last_cast or {}
    BotControllerState.last_cast[key] = gameTime
end

-- ── A.3: Chat ──────────────────────────────────────────────────

local function InitChatSchedule()
    local C = BotControllerConfig
    BotControllerState.chat_next_tick =
        BotControllerState.tick + RandomInt(C.chat_first_min_ticks, C.chat_first_max_ticks)
end

function M.MaybeChat(bot)
    local C = BotControllerConfig
    if not C.chat_enabled then return end
    if (BotControllerState.chat_sent_count or 0) >= C.chat_max_per_match then return end
    if (BotControllerState.chat_next_tick or 0) == 0 then InitChatSchedule() end
    if BotControllerState.tick < BotControllerState.chat_next_tick then return end

    local pool = BotControllerChatPool or {"gg"}
    local idx = RandomInt(1, #pool)
    local msg = pool[idx] or "gg"
    if ActionImmediate_Chat then
        ActionImmediate_Chat(msg, false)  -- team chat
    end

    BotControllerState.chat_sent_count = (BotControllerState.chat_sent_count or 0) + 1
    BotControllerState.chat_next_tick =
        BotControllerState.tick + RandomInt(C.chat_next_min_ticks, C.chat_next_max_ticks)
end

return M
