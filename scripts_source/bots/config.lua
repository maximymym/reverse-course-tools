-- config.lua — все настраиваемые параметры ботов.
-- Тестер правит здесь 80% случаев. F7 (Reload Lua) применяет изменения.
--
-- BotControllerConfig — глобальная таблица, доступна из любого mode/util модуля.
-- Если уже существует (после reload) — не перезаписываем, сохраняем runtime-правки.

BotControllerConfig = BotControllerConfig or {
    -- ── HP пороги ────────────────────────────────────────────────
    critical_hp     = 0.15,   -- <15% → фонтан (overrides всё, instant reaction)
    low_hp          = 0.30,   -- <30% → safe retreat
    tower_near_hp   = 0.50,   -- <50% рядом с вражеской башней

    -- ── Радиусы ─────────────────────────────────────────────────
    tower_radius          = 900.0,   -- радиус "рядом с вражеской башней"
    friendly_creep_radius = 900.0,   -- есть ли союзные крипы рядом
    attack_radius         = 1200.0,  -- искать цели для атаки
    cast_radius           = 800.0,   -- кастовать скиллы в радиусе
    jungle_radius         = 1200.0,  -- радиус поиска neutral camp
    gank_radius           = 2000.0,  -- радиус gank-рассмотрения

    -- ── Линия ───────────────────────────────────────────────────
    far_lane_dist    = 5000.0,  -- > → TP scroll
    walk_lane_dist   = 3000.0,  -- > → attack-move к линии
    hold_lane_dist   = 300.0,   -- < → hold position

    -- ── Tower avoidance ─────────────────────────────────────────
    tower_danger_radius       = 1400.0,  -- в этом радиусе от enemy tower без крипов: lane_farm=0, hold ведёт на safe_pos

    -- ── Retreat safety ──────────────────────────────────────────
    critical_fountain_maxdist = 3000.0,  -- если до фонтана >N И враг tower <1500 → CRITICAL retreat на safe_pos

    -- ── Анти-AFK ────────────────────────────────────────────────
    stuck_ticks      = 50,      -- 5s без движения → force move
    tp_cooldown      = 80.0,    -- секунд между TP scroll'ами

    -- ── Throttling ──────────────────────────────────────────────
    order_throttle   = 3,       -- tick%3 == 0 для move/attackmove
    combat_throttle  = 2,       -- tick%2 == 0 для combat

    -- ── Anti-ban (A.1 jitter) ───────────────────────────────────
    -- Задержка между "обнаружением" триггера и выдачей приказа.
    -- При ~10Hz tick rate: 8-25 ticks ≈ 80-250ms. Делаем реакцию нелинейной.
    jitter_min_ticks = 8,
    jitter_max_ticks = 25,

    -- ── Anti-ban (A.2 cast cooldown) ────────────────────────────
    -- Минимальная пауза между двумя кастами одного и того же скилла
    -- по одной и той же цели. Предотвращает spell-spam (как был в beta).
    cast_cooldown_min = 0.8,
    cast_cooldown_max = 1.5,

    -- ── Anti-ban (A.3 chat) ─────────────────────────────────────
    -- Случайный чат, ~2-3 сообщения/матч, по тайм-расписанию.
    chat_first_min_ticks = 300,     -- первый чат после 30с
    chat_first_max_ticks = 900,     -- или до 90с
    chat_next_min_ticks  = 3000,    -- следующие каждые 5-15 мин
    chat_next_max_ticks  = 9000,
    chat_max_per_match   = 3,
    chat_enabled         = true,    -- false → молча играет

    -- ── Farm phase semaphore (5/5 min cycle) ────────────────────
    -- Принудительная ротация линия ↔ лес: бот фиксированное время фармит
    -- одну зону, потом переключается. Reactive переходы внутри active phase
    -- подавляются (см. fsm.lua _semaphore_holds_us) — иначе крипы вернулись
    -- → flip обратно через 5 секунд → семафор смысла не имеет.
    -- safety override: RETREAT всегда сильнее семафора.
    farm_phase_seconds   = 300,     -- 5 мин активная фаза
    farm_phase_min_hp    = 0.50,    -- ниже — не свитчиться (дофармить здесь)

    -- ── Camp self-healing blacklist ─────────────────────────────
    -- Камп считается "пустым" если бот пришёл (dist<radius) и через
    -- check_ticks тиков ctx.neutrals всё ещё пуст. После N подряд таких
    -- промахов — blacklist на K секунд. Само-лечится, верификации руками
    -- не требует.
    camp_arrival_radius     = 600.0,
    -- 60 тиков ≈ 8с на 7.5 Hz. Было 100 (~13с) — нейтралы либо есть на
    -- arrival, либо мертвы; 13с ожидания только удлиняли STUCK эпизоды
    -- (Sniper 280 тиков на camp #4 -8000,-605, 3×100 тиков прежде чем
    -- BLACKLIST). 8с — достаточно чтобы learn-pulse игровой логики успел.
    camp_empty_check_ticks  = 60,
    camp_miss_threshold     = 3,      -- N misses → blacklist
    camp_blacklist_seconds  = 60,     -- K сек blacklist'а

    -- ── Co-op jungle farming ────────────────────────────────────
    -- Coordination 5 ботов в JUNGLE_FARM: 2 группы (default 3+2) делят кампы,
    -- shared focus fire на одного нейтрала. Без IPC — детерминированно через
    -- сортировку GetPlayerID и вычисление группы локально в каждом боте.
    -- Solo бот (group.size<2): требует hp_pct>=0.7 для входа в JUNGLE_FARM
    -- (vs 0.5 в группе) — solo лес опасен.
    jungle_coop_enabled         = true,    -- false → старая solo логика
    jungle_group_a_size         = 3,       -- размер группы A; B=total-A
    jungle_group_recompute_ticks= 15,      -- кеш группы (~2с), 1 раз пересобираем allies
    jungle_group_lock_seconds   = 30,      -- TTL group camp lock; пересчёт если group не дошла
    jungle_focus_fire           = true,    -- shared target ON (выкл → каждый weakest своему)
    jungle_solo_min_hp_pct      = 0.70,    -- solo bot не идёт в лес ниже этого hp
    jungle_follow_arrived_radius= 700,     -- radius "уже на camp'е" для HasArrivedMember
    jungle_coop_wait_ticks      = 70,      -- non-leader timeout ожидания лидера в JUNGLE (~9с)

    -- ── Mode desire boosts ──────────────────────────────────────
    -- Базовые desire каждого mode-файла можно масштабировать здесь.
    -- По умолчанию 1.0. Увеличь чтобы mode активировался чаще.
    desire_boost_retreat     = 1.0,
    desire_boost_jungle_farm = 1.0,
    desire_boost_lane_farm   = 1.0,
    desire_boost_push        = 1.0,
    desire_boost_gank        = 1.0,
    desire_boost_roshan      = 1.0,
    desire_boost_hold        = 1.0,
}

-- ── Team strategy (paired-orchestrators 5v5 self-play) ────────────────
-- После N секунд игры команда играет одну из ролей:
--   WIN     — 5 ботов агрессивно пушат mid (cond_strategy_win_push в fsm.lua).
--   LOSE    — боты избегают mid + ускоренно уходят в лес (cond_lose_mid_block).
--   DEBOOST — текущий фарм-цикл (default).
-- Значение читается из C:\temp\andromeda\strategy.txt каждые 50 тиков
-- (см. _read_strategy в bot_controller.lua). Объявлено через `or default`,
-- чтобы F7-reload не затирал runtime-значение, выставленное poll'ом.
BotControllerConfig.team_strategy         = BotControllerConfig.team_strategy         or "DEBOOST"
BotControllerConfig.strategy_enable_time  = BotControllerConfig.strategy_enable_time  or 1800   -- gt seconds, 30 мин
BotControllerConfig.lose_mid_block_radius = BotControllerConfig.lose_mid_block_radius or 1500   -- mid avoid radius для LOSE

-- Пул реплик для чата (короткие, обычные для dota playerspeak)
BotControllerChatPool = BotControllerChatPool or {
    "gg", "ss", "?", "...", "nice", "gj", "lol",
    "care", "mid ss", "miss", "wp", "hf",
}

print("[config] BotControllerConfig loaded (crit_hp=" .. tostring(BotControllerConfig.critical_hp) .. ")")
