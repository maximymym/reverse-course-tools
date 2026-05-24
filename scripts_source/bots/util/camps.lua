-- util/camps.lua — actual neutral camp coordinates dumped by user (2026-04-23).
-- Заменяет хардкод в LuaStubs::GetNeutralCampLocations который ссылался на
-- устаревшие точки (например (-3800,-300) для radiant hard — такого camp нет
-- в текущей карте → бот WK стоял АФК на пустом spot, neut=0 nstreak=1056).
--
-- Возвращаемая структура совместима с current consumers:
--   - states/jungle_farm.lua:GetAllCamps() — итерирует {radiant,dire}.<group>.<list>
--   - context.lua nearest_neutral_dist — той же итерацией
--
-- Группировка по level: easy=1 (малые), medium=4 (средние/bounty), hard=5 (большие),
-- ancient (требует level 6+ для WK low-hp). Bounty-runes spots тоже считаем
-- "camps" — они часто содержат neutral creeps ИЛИ дают gold pickup, оба
-- полезные target'ы для WALK_CAMP. Roshan отдельно.
--
-- Difficulty mapping для new ancient filter (jungle_farm.NearestCamp):
--   - easy: small camps (1 small unit, low XP) — safe для всех уровней
--   - medium: medium/bounty (mixed) — safe для l2+
--   - hard: large camps (1 large + 2 medium + 1 small) — l4+ recommended
--   - ancient: ancient camps (Black Dragon / Mud Golems / Granite) — l6+ ONLY
--
-- TODO confirmation: ancient точки в user dump'е отсутствовали. Стандартные
-- координаты dota2 7.x:
--   - radiant ancient: ~(-4675, -85) — у roshan top-side (Mud Golems)
--   - dire ancient: ~(4187, 256) — symmetric bot-side
-- Если эти координаты не совпадают с actual карта — bot просто пройдёт мимо
-- (camp пустой → blacklist через camp_miss_threshold).
--
-- TEAM convention: pos на стороне team-owner'а (radiant=2 — south-east jungle,
-- dire=3 — north-west jungle). Z указан там где явно отличается от 128.

local function V(x, y, z) return Vector(x, y, z or 128) end

local CAMPS = {
    radiant = {
        -- level 1 — small
        easy = {
            V(4062, -5101, 128),       -- radbotlinespotmalenkiy
            V(-7951, -1790, 256),      -- radtopmalenkiycampexp
        },
        -- level 4 — medium / bounty
        medium = {
            V(1894, -4077, 256),       -- radbotbountyspotsredniy
            V(259,  -5051, 134),       -- radbountyyawericispot
            V(-1935, -4806, 128),      -- radmidsredniycamp
            V(-8000, -605,  256),      -- radtopsredniycampexp
        },
        -- level 5 — large
        hard = {
            V(4749, -3750, 128),       -- radbotlinespotbolwoy
            V(-1489, -3319, 128),      -- radmidbolwoycamp
        },
        -- level 6+ — ancient (TODO confirm coords against current map)
        ancient = {
            V(-4675, -85, 128),        -- radiant ancient near roshan top-side
        },
    },
    dire = {
        easy = {
            V(-3901, 4903, 128),       -- diretoplinespotmalenkiy
            V(8000, 1252, 256),        -- direbotmalenkiycampexp
        },
        medium = {
            V(-2613, 3916, 256),       -- diretopbountyspotsredniy
            V(-946,  4760, 134),       -- diretopbountyyawericispot
            V(1256,  4072, 128),       -- diremidsredniycamp
            V(7935,  -31,  256),       -- direbotsredniycampexp
        },
        hard = {
            V(-4791, 4036, 128),       -- diretoplinespotbolwoy
            V(1119,  2551, 128),       -- diremidbolwoycamp
        },
        -- level 6+ — ancient (TODO confirm coords against current map)
        ancient = {
            V(4187, 256, 128),         -- dire ancient symmetric bot-side
        },
    },
    roshan = V(-2500, -2000, 128),     -- legacy pit; new pit ~(2400,2400) — TODO confirm
}

-- Build identity → difficulty lookup (Vector userdata can't have .difficulty
-- field assigned, so we maintain a side-map keyed by exact Vector reference).
-- jungle_farm.NearestCamp использует Camps.DifficultyOf(camp) для skip ancient
-- при level<6.
local _difficultyOf = {}
local function _registerSide(side)
    for groupName, list in pairs(side) do
        if type(list) == "table" then
            for _, p in ipairs(list) do
                _difficultyOf[p] = groupName
            end
        end
    end
end
_registerSide(CAMPS.radiant)
_registerSide(CAMPS.dire)

-- Public helper. Возвращает "easy"/"medium"/"hard"/"ancient" или "medium" по
-- умолчанию (graceful — TODO unknown camps трактуем как стандарт).
function CAMPS.DifficultyOf(pos)
    return _difficultyOf[pos] or "medium"
end

return CAMPS
