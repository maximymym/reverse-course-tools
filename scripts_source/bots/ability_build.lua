-- ability_build.lua — level-up sequences для каждого героя.
-- Вызывается C++ как AbilityLevelUpThink() каждый тик если abilityPoints > 0.
--
-- Формат sequences[heroName] = {slot1, slot2, ..., slot25} — 1-based slot numbers.
--   Слот 1 = первая способность (Q), 2 = W, 3 = E, 6 = R (ульт), 4/5 = таланты.
--   Длина array = 25 (максимальный level).
--
-- Default sequence: Q-W-E-Q-R-Q-W-W-E-E-R-E-W-talent-talent-R-talent-talent...
--
-- Hero key = internal name без префикса npc_dota_hero_, lowercase.
--   Пример: npc_dota_hero_antimage → "antimage" (БЕЗ подчёркивания).

local M = {}

M.sequences = {
    -- Default fallback (random hero / неизвестный). НЕ ТРОГАТЬ.
    default = { 1, 2, 3, 1, 6, 1, 2, 2, 3, 3, 6, 3, 2, 4, 4, 6, 4, 4, 1, 5, 5, 2, 3, 4, 6 },

    -- ─── Существующие (сохранены) ─────────────────────────────────────────

    -- Pudge — приоритет Rot (W) + Dismember (R)
    pudge      = { 2, 1, 2, 3, 2, 6, 2, 1, 1, 1, 6, 3, 3, 3, 4, 6, 4, 4, 4, 5, 5, 6, 5, 5, 5 },

    -- Lina — Dragon Slave (Q) + LSA spam
    lina       = { 1, 2, 1, 3, 1, 6, 1, 2, 2, 2, 6, 3, 3, 3, 4, 6, 4, 4, 4, 5, 5, 5, 5, 6, 6 },

    -- Sven — Cleave (W) priority
    sven       = { 2, 3, 2, 3, 2, 6, 2, 3, 3, 1, 6, 1, 1, 1, 4, 6, 4, 4, 4, 5, 5, 5, 5, 6, 6 },

    -- Juggernaut — Bladefury (Q) + Omnislash (R)
    juggernaut = { 1, 3, 1, 2, 1, 6, 1, 3, 3, 3, 6, 2, 2, 2, 4, 6, 4, 4, 4, 5, 5, 5, 5, 6, 6 },

    -- ─── Carry / Right-clickers ───────────────────────────────────────────

    -- Anti-Mage — Counterspell (W) max для magic immunity, Mana Break passive
    antimage         = { 2, 1, 2, 3, 2, 6, 2, 1, 1, 1, 6, 3, 3, 3, 4, 6, 4, 4, 4, 5, 5, 5, 5, 6, 6 },

    -- Wraith King — Wraithfire Blast (Q stun) max → Vampiric Spirit (W)
    skeleton_king    = { 1, 2, 1, 3, 1, 6, 1, 2, 2, 2, 6, 3, 3, 3, 4, 6, 4, 4, 4, 5, 5, 5, 5, 6, 6 },

    -- Sniper — Shrapnel (Q AoE) max → Take Aim (E range)
    sniper           = { 1, 3, 1, 2, 1, 6, 1, 3, 3, 3, 6, 2, 2, 2, 4, 6, 4, 4, 4, 5, 5, 5, 5, 6, 6 },

    -- Drow Ranger — Multishot (W) max, Frost Arrows (Q) lvl 1 для harass
    drow_ranger      = { 1, 2, 2, 1, 2, 6, 2, 1, 1, 1, 6, 3, 3, 3, 4, 6, 4, 4, 4, 5, 5, 5, 5, 6, 6 },

    -- Viper — Nethertoxin (W) max, Poison Attack (Q) lvl 1
    viper            = { 1, 2, 2, 3, 2, 6, 2, 3, 3, 3, 6, 1, 1, 1, 4, 6, 4, 4, 4, 5, 5, 5, 5, 6, 6 },

    -- Weaver — Shukuchi (W) main mobility max → The Swarm (Q)
    weaver           = { 2, 1, 2, 3, 2, 6, 2, 1, 1, 1, 6, 3, 3, 3, 4, 6, 4, 4, 4, 5, 5, 5, 5, 6, 6 },

    -- Troll Warlord — Whirling Axes Ranged (Q) harass → Berserker's Rage (W)
    troll_warlord    = { 1, 3, 1, 2, 1, 6, 1, 3, 3, 3, 6, 2, 2, 2, 4, 6, 4, 4, 4, 5, 5, 5, 5, 6, 6 },

    -- Phantom Assassin — Stifling Dagger (Q) → Phantom Strike (W)
    phantom_assassin = { 1, 2, 1, 3, 1, 6, 1, 2, 2, 2, 6, 3, 3, 3, 4, 6, 4, 4, 4, 5, 5, 5, 5, 6, 6 },

    -- Gyrocopter — Rocket Barrage (Q) max → Homing Missile (W)
    gyrocopter       = { 1, 3, 1, 2, 1, 6, 1, 3, 3, 3, 6, 2, 2, 2, 4, 6, 4, 4, 4, 5, 5, 5, 5, 6, 6 },

    -- Luna — Moon Glaives (E) lvl 1, Lunar Blessing (W) early, Lucent Beam (Q)
    luna             = { 3, 1, 1, 2, 1, 6, 1, 2, 2, 2, 6, 3, 3, 3, 4, 6, 4, 4, 4, 5, 5, 5, 5, 6, 6 },

    -- Terrorblade — Conjure Image (E) max push, Reflection (W) early, Meta (Q), Sunder (R)
    terrorblade      = { 3, 2, 3, 1, 3, 6, 3, 2, 2, 2, 6, 1, 1, 1, 4, 6, 4, 4, 4, 5, 5, 5, 5, 6, 6 },

    -- Medusa — Mystic Snake (Q) max → Mana Shield (W toggle) → Split Shot (E)
    medusa           = { 1, 3, 1, 3, 1, 6, 1, 3, 3, 3, 6, 2, 2, 2, 4, 6, 4, 4, 4, 5, 5, 5, 5, 6, 6 },

    -- Slark — Pounce (W) max → Essence Shift (E) → Dark Pact (Q)
    slark            = { 2, 1, 2, 3, 2, 6, 2, 1, 1, 1, 6, 3, 3, 3, 4, 6, 4, 4, 4, 5, 5, 5, 5, 6, 6 },

    -- Ursa — Earthshock (Q) → Overpower (W) → Fury Swipes (E passive) → Enrage (R)
    ursa             = { 1, 2, 1, 3, 1, 6, 1, 2, 2, 2, 6, 3, 3, 3, 4, 6, 4, 4, 4, 5, 5, 5, 5, 6, 6 },

    -- Lifestealer — Open Wounds (E) max → Feast (W passive) → Rage (Q)
    lifestealer      = { 3, 1, 3, 2, 3, 6, 3, 1, 1, 1, 6, 2, 2, 2, 4, 6, 4, 4, 4, 5, 5, 5, 5, 6, 6 },

    -- Riki — Smoke Screen (W) max → Blink Strike (E) → Permanent Invisibility (Q)
    riki             = { 2, 3, 2, 3, 2, 6, 2, 3, 3, 1, 6, 1, 1, 1, 4, 6, 4, 4, 4, 5, 5, 5, 5, 6, 6 },

    -- Bloodseeker — Bloodrage (Q) → Blood Rite (W silence) → Thirst (E passive)
    bloodseeker      = { 1, 2, 1, 2, 1, 6, 1, 2, 2, 2, 6, 3, 3, 3, 4, 6, 4, 4, 4, 5, 5, 5, 5, 6, 6 },

    -- ─── Casters / Supports ───────────────────────────────────────────────

    -- Lich — Frost Blast (Q) max → Sinister Gaze (W) → Frost Shield (E)
    lich             = { 1, 2, 1, 3, 1, 6, 1, 2, 2, 2, 6, 3, 3, 3, 4, 6, 4, 4, 4, 5, 5, 5, 5, 6, 6 },

    -- Ogre Magi — Fireblast (Q) max → Ignite (W) → Bloodlust (E)
    ogre_magi        = { 1, 2, 1, 3, 1, 6, 1, 2, 2, 2, 6, 3, 3, 3, 4, 6, 4, 4, 4, 5, 5, 5, 5, 6, 6 },

    -- Jakiro — Dual Breath (Q) max → Ice Path (W stun) → Liquid Fire (E)
    jakiro           = { 1, 2, 1, 3, 1, 6, 1, 2, 2, 2, 6, 3, 3, 3, 4, 6, 4, 4, 4, 5, 5, 5, 5, 6, 6 },

    -- Witch Doctor — Maledict (E) max → Paralyzing Cask (Q stun) → Voodoo Restoration (W)
    witch_doctor     = { 3, 1, 3, 2, 3, 6, 3, 1, 1, 1, 6, 2, 2, 2, 4, 6, 4, 4, 4, 5, 5, 5, 5, 6, 6 },

    -- Lion — Earth Spike (Q) max → Hex (W) → Mana Drain (E)
    lion             = { 1, 2, 1, 3, 1, 6, 1, 2, 2, 2, 6, 3, 3, 3, 4, 6, 4, 4, 4, 5, 5, 5, 5, 6, 6 },

    -- Shadow Shaman — Ether Shock (E) max → Shackles (W) → Hex (Q)
    shadow_shaman    = { 3, 2, 3, 1, 3, 6, 3, 2, 2, 2, 6, 1, 1, 1, 4, 6, 4, 4, 4, 5, 5, 5, 5, 6, 6 },

    -- Warlock — Shadow Word (W heal/dmg) max → Fatal Bonds (Q) → Upheaval (E)
    warlock          = { 2, 1, 2, 3, 2, 6, 2, 1, 1, 1, 6, 3, 3, 3, 4, 6, 4, 4, 4, 5, 5, 5, 5, 6, 6 },

    -- Vengeful Spirit — Magic Missile (Q stun) max → Wave of Terror (W) → Aura (E)
    vengefulspirit   = { 1, 2, 1, 3, 1, 6, 1, 2, 2, 2, 6, 3, 3, 3, 4, 6, 4, 4, 4, 5, 5, 5, 5, 6, 6 },

    -- Undying — Decay (Q) max → Soul Rip (W) → Tombstone (E)
    undying          = { 1, 2, 1, 3, 1, 6, 1, 2, 2, 2, 6, 3, 3, 3, 4, 6, 4, 4, 4, 5, 5, 5, 5, 6, 6 },

    -- Zeus — Lightning Bolt (W) max → Arc Lightning (Q) → Static Field (E)
    zuus             = { 1, 2, 2, 1, 2, 6, 2, 1, 1, 1, 6, 3, 3, 3, 4, 6, 4, 4, 4, 5, 5, 5, 5, 6, 6 },
}

-- Archetype-based fallback (Phase 2): если героя нет в M.sequences, выбираем
-- carry-default или caster-default по AttackCapability.
--   Carry-default (right-clicker): max W early для passive/active scaling, R по уровню.
--   Caster-default: max Q для nuke spam.
M.sequences.carry_default  = { 2, 1, 2, 3, 2, 6, 2, 1, 1, 1, 6, 3, 3, 3, 4, 6, 4, 4, 4, 5, 5, 5, 5, 6, 6 }
M.sequences.caster_default = { 1, 2, 1, 3, 1, 6, 1, 2, 2, 2, 6, 3, 3, 3, 4, 6, 4, 4, 4, 5, 5, 5, 5, 6, 6 }

function M.GetDefaultSequenceByArchetype(bot)
    if not bot or bot:IsNull() then return M.sequences.default end
    -- DOTA_UNIT_CAP_MELEE_ATTACK = 1, DOTA_UNIT_CAP_RANGED_ATTACK = 2
    local cap = bot.GetAttackCapability and bot:GetAttackCapability() or nil
    if cap == 2 then
        -- Ranged: чаще caster (но не всегда; Drow/Sniper/Viper уже в таблице)
        return M.sequences.caster_default
    elseif cap == 1 then
        -- Melee: чаще carry/initiator
        return M.sequences.carry_default
    end
    return M.sequences.default
end

function M.GetSequenceForHero(heroName, bot)
    heroName = heroName or ""
    heroName = heroName:gsub("^npc_dota_hero_", ""):lower()
    local seq = M.sequences[heroName]
    if seq then return seq end
    -- Miss: пробуем archetype-based fallback если есть bot, иначе default.
    if bot then
        return M.GetDefaultSequenceByArchetype(bot)
    end
    return M.sequences.default
end

function M.LevelUp(bot)
    if not bot or bot:IsNull() or not bot:IsAlive() then return end
    local pts = bot:GetAbilityPoints()
    if not pts or pts <= 0 then return end

    local heroName = bot:GetUnitName() or ""
    local seq = M.GetSequenceForHero(heroName, bot)

    local currLevel = bot:GetLevel() or 1
    -- Индекс в sequence = текущий level (т.к. следующая способность для этого level'а)
    -- currLevel=1 → seq[1] (первая).
    local nextSlotIdx = seq[currLevel]
    if not nextSlotIdx then return end

    -- Lua: 1-based sequence slot → 0-based C++ slot
    local ab = bot:GetAbilityInSlot(nextSlotIdx - 1)
    if ab and not ab:IsNull() then
        local name = ab:GetName() or ""
        if name == "" or name == "generic_hidden" then return end

        -- Race condition guard: на спавне Source 2 может вернуть ability handle
        -- ДО того как schema fully resolved → GetMaxLevel()==0 несмотря на наличие
        -- ability points. Без этого гарда у Viper'a (все abil max=0 на spawn)
        -- OrderTrainAbility шлётся 100+ раз/сек, отменяя AttackMove queue → бот
        -- никогда не выходит из фонтан-bubble. Sniper не пострадал т.к. у него
        -- abilPts=0 на старте, гейт `pts > 0` отрезал loop сам.
        local ok_max, maxL = pcall(ab.GetMaxLevel, ab)
        if not ok_max or not maxL or maxL == 0 then return end
        local ok_cur, curL = pcall(ab.GetLevel, ab)
        if ok_cur and curL and curL >= maxL then return end

        -- Throttle: даже при валидном max=N не спамим LevelAbility чаще раза в
        -- ~1 сек. C++ Execute() блокирует Think-thread на 100мс — если ability
        -- ещё не "проявилась" в engine, мы крадём 100мс×4 = 400мс/сек из бота.
        BotControllerState.ab_levelup_last = BotControllerState.ab_levelup_last or {}
        local last_t = BotControllerState.ab_levelup_last[name] or -999
        local now = (BotControllerState and BotControllerState.tick) or 0
        if (now - last_t) < 60 then return end
        BotControllerState.ab_levelup_last[name] = now

        bot:ActionImmediate_LevelAbility(name)
        print("[ability_build] level " .. name .. " (slot " .. nextSlotIdx .. ") at hero_level=" .. currLevel)
    end
end

function AbilityLevelUpThink()
    local bot = GetBot()
    if bot then M.LevelUp(bot) end
end

print("[ability_build] loaded " .. (function()
    local n = 0; for _ in pairs(M.sequences) do n = n + 1 end; return n
end)() .. " hero sequences")

return M
