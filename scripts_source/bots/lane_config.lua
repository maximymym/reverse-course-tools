-- lane_config.lua — waypoints и lane-геометрия.
-- Грузится первым (до modes/*). Экспортирует GetLaneWaypointLua, GetLaneAmountForBot,
-- GetCreepFrontLocation — глобальные функции (Valve-API-style).

LaneWaypoints = {
    [1] = { -- LANE_TOP
        Vector(-6700, -6400, 128), Vector(-6700, -3800, 128), Vector(-6700,  -200, 128),
        Vector(-6400,  2600, 128), Vector(-5600,  4600, 128), Vector(-3600,  5800, 128),
        Vector(-1400,  6200, 128), Vector( 1000,  6300, 128), Vector( 3400,  6300, 128),
        Vector( 5800,  6200, 128), Vector( 6600,  6200, 128),
    },
    [2] = { -- LANE_MID
        Vector(-6400, -6200, 128), Vector(-4800, -4600, 128), Vector(-3200, -3000, 128),
        Vector(-1600, -1400, 128), Vector(    0,     0, 128), Vector( 1600,  1400, 128),
        Vector( 3200,  3000, 128), Vector( 4800,  4600, 128), Vector( 6400,  6200, 128),
    },
    [3] = { -- LANE_BOT
        Vector(-6400, -6200, 128), Vector(-3400, -6400, 128), Vector(-1000, -6400, 128),
        Vector( 1400, -6200, 128), Vector( 3600, -5800, 128), Vector( 5600, -4600, 128),
        Vector( 6400, -2600, 128), Vector( 6700,   200, 128), Vector( 6700,  3800, 128),
        Vector( 6700,  6400, 128),
    },
}

-- Интерполяция waypoint-а по параметру. 0.0 = начало (Radiant-side), 1.0 = конец.
function GetLaneWaypointLua(lane, amount)
    local pts = LaneWaypoints[lane]
    if not pts then return Vector(0, 0, 128) end
    local count = #pts
    if count < 2 then return pts[1] or Vector(0, 0, 128) end

    if amount < 0 then amount = 0 end
    if amount > 1 then amount = 1 end

    local idx = amount * (count - 1)
    local lo = math.floor(idx)
    if lo >= count - 1 then return pts[count] end

    local t = idx - lo
    local a = pts[lo + 1]
    local b = pts[lo + 2]
    return Vector(
        a.x + t * (b.x - a.x),
        a.y + t * (b.y - a.y),
        128
    )
end

-- Team-relative amount для позиции на линии.
-- Radiant (team=2): safe ≈ 0.2-0.35, lane ≈ 0.5, push ≈ 0.65.
-- Dire (team=3): симметрично.
function GetLaneAmountForBot(lane, team, phase)
    phase = phase or "lane"
    if lane == 2 then
        if phase == "push" then return (team == 2) and 0.65 or 0.35 end
        -- Mid safe: 0.25/0.75 — между T1 и T2 (раньше 0.35 = почти T1, retreat
        -- ставил бота прямо где его и били). idx=2.0 → пт между own fountain
        -- и T2 ≈ safe.
        if phase == "safe" then return (team == 2) and 0.25 or 0.75 end
        return 0.5
    end
    if phase == "push" then return (team == 2) and 0.55 or 0.45 end
    -- Top/Bot safe: 0.10/0.90 — раньше 0.20/0.80 ставил radiant'а В waypoint
    -- (-6700,-200) который ЕСТЬ radiant top T1. Бот retreat'ил В свою же
    -- позицию → 530 тиков STUCK под собственной T1 пока крипы добивают.
    -- 0.10 = pts[2] область ≈ (-6700,-3800), за T1 в сторону фонтана.
    if phase == "safe" then return (team == 2) and 0.10 or 0.90 end
    return (team == 2) and 0.35 or 0.65
end

-- Центр массы союзных крипов на линии (живой фронт). Fallback — waypoint.
function GetCreepFrontLocation(bot, lane, radius)
    radius = radius or 2000
    if not bot or bot:IsNull() then
        return GetLaneWaypointLua(lane, GetLaneAmountForBot(lane, 2, "lane"))
    end

    local team = bot:GetTeam()
    local creeps = bot:GetNearbyLaneCreeps(radius, false)
    if not creeps or #creeps == 0 then
        return GetLaneWaypointLua(lane, GetLaneAmountForBot(lane, team, "lane"))
    end

    local sx, sy, n = 0, 0, 0
    for _, c in ipairs(creeps) do
        if c:IsAlive() then
            local p = c:GetLocation()
            sx = sx + p.x
            sy = sy + p.y
            n = n + 1
        end
    end
    if n == 0 then
        return GetLaneWaypointLua(lane, GetLaneAmountForBot(lane, team, "lane"))
    end
    return Vector(sx / n, sy / n, 128)
end

print("[lane_config] loaded " ..
    (#LaneWaypoints[1]) .. " TOP, " ..
    (#LaneWaypoints[2]) .. " MID, " ..
    (#LaneWaypoints[3]) .. " BOT waypoints")
