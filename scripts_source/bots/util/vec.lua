-- util/vec.lua — общие Vector helpers для FSM и state handlers.
-- Inspired by UC Zone Vector class (Dist2D/Extend2D/Lerp). Заменяет дубликаты
-- inline Dist2D/centroid-расчёта в 7 файлах.
--
-- ВАЖНО: Action_* binding в C++ требует Vector userdata (Valve native), не plain
-- Lua table — поэтому Extend2D/Lerp/Centroid возвращают именно Vector(...).

local M = {}

-- Расстояние в 2D. Semantic: если любой аргумент nil → 99999 (очень далеко,
-- не triggers "dist < threshold" проверки у callers).
function M.Dist2D(a, b)
    if not a or not b then return 99999 end
    local dx = (a.x or 0) - (b.x or 0)
    local dy = (a.y or 0) - (b.y or 0)
    return math.sqrt(dx * dx + dy * dy)
end

-- Квадрат расстояния (без sqrt) для сравнений — быстрее Dist2D в 2-3 раза.
function M.DistSqr2D(a, b)
    if not a or not b then return 99999 * 99999 end
    local dx = (a.x or 0) - (b.x or 0)
    local dy = (a.y or 0) - (b.y or 0)
    return dx * dx + dy * dy
end

-- Проверка "a в радиусе range от b". Использует DistSqr2D, избегает sqrt.
function M.IsInRange2D(a, b, range)
    return M.DistSqr2D(a, b) < range * range
end

-- Возвращает Vector userdata, на `distance` unit ahead от `from` в сторону `to`.
-- Если векторы совпадают (len<1) — возвращает сам from как Vector.
function M.Extend2D(from, to, distance)
    local dx = (to.x or 0) - (from.x or 0)
    local dy = (to.y or 0) - (from.y or 0)
    local len = math.sqrt(dx * dx + dy * dy)
    local z = from.z or 128
    if len < 1 then
        return Vector(from.x or 0, from.y or 0, z)
    end
    return Vector((from.x or 0) + distance * dx / len,
                  (from.y or 0) + distance * dy / len,
                  z)
end

-- Linear interpolate a→b по параметру t (0..1). Возвращает Vector userdata.
function M.Lerp(a, b, t)
    local ax, ay = (a.x or 0), (a.y or 0)
    local bx, by = (b.x or 0), (b.y or 0)
    return Vector(ax + (bx - ax) * t,
                  ay + (by - ay) * t,
                  a.z or 128)
end

-- Центроид списка юнитов (фильтрует мёртвых/null). Второе значение — сколько
-- живых юнитов посчитано. Если пусто → (nil, 0).
function M.Centroid(units)
    local cx, cy, n = 0, 0, 0
    for _, u in ipairs(units or {}) do
        if u and not u:IsNull() and u:IsAlive() then
            local p = u:GetLocation()
            cx = cx + p.x
            cy = cy + p.y
            n = n + 1
        end
    end
    if n == 0 then return nil, 0 end
    return Vector(cx / n, cy / n, 128), n
end

return M
