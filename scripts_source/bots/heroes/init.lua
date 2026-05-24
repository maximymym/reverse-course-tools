-- heroes/init.lua — регистрация hero-specific overrides.

local function safeRequire(name)
    local ok, mod = pcall(require, name)
    if ok and type(mod) == "table" then return mod end
    print("[heroes/init] " .. name .. " load failed: " .. tostring(mod))
    return nil
end

local heroes = {}

-- Carries
heroes["sniper"]        = safeRequire("heroes.sniper")
heroes["viper"]         = safeRequire("heroes.viper")
heroes["drow_ranger"]   = safeRequire("heroes.drow_ranger")
heroes["weaver"]        = safeRequire("heroes.weaver")
heroes["gyrocopter"]    = safeRequire("heroes.gyrocopter")
heroes["lifestealer"]   = safeRequire("heroes.lifestealer")

-- Casters / supports
heroes["lich"]          = safeRequire("heroes.lich")
heroes["ogre_magi"]     = safeRequire("heroes.ogre_magi")
heroes["jakiro"]        = safeRequire("heroes.jakiro")
heroes["lion"]          = safeRequire("heroes.lion")
heroes["witch_doctor"]  = safeRequire("heroes.witch_doctor")
heroes["lina"]          = safeRequire("heroes.lina")
heroes["zuus"]          = safeRequire("heroes.zuus")

-- Existing example
heroes["pudge"]         = safeRequire("heroes.pudge")

-- Removes nil entries (failed requires) для cleaner Heroes table:
local clean = {}
for k, v in pairs(heroes) do if v then clean[k] = v end end

local n = 0
for _ in pairs(clean) do n = n + 1 end
print(string.format("[heroes/init] %d hero overrides loaded", n))

return clean
