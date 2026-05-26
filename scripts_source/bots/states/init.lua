-- states/init.lua — регистрация state handlers.
-- Возвращает таблицу [FSM.State.<NAME>] = handler module.
-- DEAD не имеет handler'а (bot_controller возвращает раньше).

local FSM = require("fsm")

local function safeRequire(name)
    local ok, mod = pcall(require, name)
    if ok and type(mod) == "table" and mod.Run then return mod end
    print("[states.init] FAILED to load " .. name .. ": " .. tostring(mod))
    return nil
end

local handlers = {
    [FSM.State.RESPAWN]      = safeRequire("states.respawn"),
    [FSM.State.LANE_FARM]    = safeRequire("states.lane_farm"),
    [FSM.State.LANE_WAIT]    = safeRequire("states.lane_wait"),
    [FSM.State.JUNGLE_FARM]  = safeRequire("states.jungle_farm"),
    [FSM.State.PUSH]         = safeRequire("states.push"),
    [FSM.State.RETREAT]      = safeRequire("states.retreat"),
    [FSM.State.METEOR_SQUAD] = safeRequire("states.meteor_squad"),
    [FSM.State.SIDE_BAIT]    = safeRequire("states.side_bait"),
}

local count = 0
for _ in pairs(handlers) do count = count + 1 end
print("[states.init] loaded " .. count .. " state handlers")

return handlers
