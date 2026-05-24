-- item_build.lua — minimal QuickBuy-style purchases.
--
-- Engine handles component-resolution: bot:ActionImmediate_PurchaseItem(target)
-- with the FINAL item name. If gold is enough for the whole item, engine buys it
-- and courier delivers; if not, engine picks the next missing component matching
-- available gold from the build tree. Equivalent to in-game QuickBuy slot.
--
-- Pre-req: DLL must map item name → defId via items.json (CGameDataDB.LoadItems
-- now stores ID), CPanoramaJS::OrderPurchaseItem reads from CGameDataDB. Without
-- this rewrite the call would silently no-op for items missing from the legacy
-- hardcoded list (meteor, kaya, BKB, butterfly, ...).
--
-- Order: PT → Meteor Hammer → per-hero late items.

local M = {}

-- ============================================================
-- Starting consumables — bot покупает ПЕРВЫМ делом до PT.
-- Стандартный starting kit ~280g: tango + 2 branches + salve.
-- ============================================================
M.STARTING_ITEMS = {
    "item_tango",      -- 90g, 4 charges регена
    "item_branches",   -- 50g, +6 stats
    "item_flask",      -- 110g, salve heal
}

-- ============================================================
-- Power Treads attribute mapping (post-purchase toggle target stat).
-- Engine default after PT purchase = STR. Cycle: STR → AGI → INT → STR.
-- ============================================================
M.PT_DESIRED_ATTR = {
    -- INT
    lina            = "int",
    zuus            = "int",
    lich            = "int",
    jakiro          = "int",
    witch_doctor    = "int",
    lion            = "int",
    shadow_shaman   = "int",
    warlock         = "int",
    ogre_magi       = "int",
    medusa          = "int",
    -- AGI
    juggernaut      = "agi",
    sniper          = "agi",
    drow_ranger     = "agi",
    weaver          = "agi",
    troll_warlord   = "agi",
    antimage        = "agi",
    phantom_assassin= "agi",
    gyrocopter      = "agi",
    luna            = "agi",
    terrorblade     = "agi",
    slark           = "agi",
    riki            = "agi",
    bloodseeker     = "agi",
    viper           = "agi",
    -- STR (default fallback) — sven, skeleton_king, ursa, lifestealer, pudge etc.
}

M.PT_CYCLE = { str = 1, agi = 2, int = 3 }
M.PT_NEXT  = { str = "agi", agi = "int", int = "str" }

-- ============================================================
-- Per-hero late builds (cycled in order, after PT + Meteor are owned).
-- ============================================================
M.LATE_BUILDS = {
    default          = { "item_black_king_bar", "item_blink", "item_aghanims_scepter", "item_octarine_core" },

    -- Carries
    sven             = { "item_echo_sabre", "item_black_king_bar", "item_daedalus", "item_satanic", "item_assault" },
    juggernaut       = { "item_maelstrom", "item_manta", "item_butterfly", "item_abyssal_blade", "item_satanic" },
    skeleton_king    = { "item_mask_of_madness", "item_armlet", "item_assault", "item_abyssal_blade", "item_satanic" },
    sniper           = { "item_dragon_lance", "item_maelstrom", "item_hurricane_pike", "item_mjollnir", "item_butterfly" },
    drow_ranger      = { "item_dragon_lance", "item_hurricane_pike", "item_butterfly", "item_daedalus", "item_satanic" },
    viper            = { "item_dragon_lance", "item_lesser_crit", "item_bloodthorn", "item_butterfly", "item_assault" },
    weaver           = { "item_maelstrom", "item_diffusal_blade", "item_linkens_sphere", "item_butterfly", "item_daedalus" },
    troll_warlord    = { "item_mask_of_madness", "item_sange_and_yasha", "item_black_king_bar", "item_satanic", "item_butterfly" },
    antimage         = { "item_battle_fury", "item_manta", "item_abyssal_blade", "item_butterfly", "item_heart" },
    phantom_assassin = { "item_battle_fury", "item_desolator", "item_black_king_bar", "item_abyssal_blade", "item_satanic" },
    gyrocopter       = { "item_mask_of_madness", "item_sange_and_yasha", "item_black_king_bar", "item_satanic", "item_butterfly" },
    luna             = { "item_mask_of_madness", "item_manta", "item_black_king_bar", "item_butterfly", "item_hurricane_pike" },
    terrorblade      = { "item_manta", "item_dragon_lance", "item_eye_of_skadi", "item_butterfly", "item_satanic" },
    medusa           = { "item_mystic_staff", "item_manta", "item_eye_of_skadi", "item_butterfly", "item_monkey_king_bar" },
    slark            = { "item_echo_sabre", "item_diffusal_blade", "item_eye_of_skadi", "item_abyssal_blade", "item_satanic" },
    riki             = { "item_diffusal_blade", "item_manta", "item_abyssal_blade", "item_butterfly", "item_satanic" },
    bloodseeker      = { "item_mask_of_madness", "item_sange_and_yasha", "item_black_king_bar", "item_abyssal_blade", "item_satanic" },
    ursa             = { "item_blink", "item_black_king_bar", "item_skull_basher", "item_abyssal_blade", "item_satanic" },
    lifestealer      = { "item_armlet", "item_sange_and_yasha", "item_black_king_bar", "item_abyssal_blade", "item_satanic" },

    -- Supports
    pudge            = { "item_blink", "item_blade_mail", "item_aghanims_scepter", "item_ultimate_scepter", "item_octarine_core" },
    lina             = { "item_aether_lens", "item_black_king_bar", "item_aghanims_scepter", "item_octarine_core", "item_refresher" },
    lich             = { "item_aether_lens", "item_glimmer_cape", "item_force_staff", "item_aghanims_scepter", "item_refresher" },
    ogre_magi        = { "item_aether_lens", "item_glimmer_cape", "item_force_staff", "item_aghanims_scepter", "item_refresher" },
    jakiro           = { "item_aether_lens", "item_glimmer_cape", "item_force_staff", "item_aghanims_scepter", "item_refresher" },
    witch_doctor     = { "item_aether_lens", "item_glimmer_cape", "item_force_staff", "item_aghanims_scepter", "item_refresher" },
    lion             = { "item_aether_lens", "item_blink", "item_force_staff", "item_aghanims_scepter", "item_ethereal_blade" },
    shadow_shaman    = { "item_aether_lens", "item_aghanims_scepter", "item_blink", "item_octarine_core", "item_refresher" },
    warlock          = { "item_aether_lens", "item_glimmer_cape", "item_force_staff", "item_aghanims_scepter", "item_refresher" },
    vengefulspirit   = { "item_aether_lens", "item_force_staff", "item_aghanims_scepter", "item_octarine_core", "item_assault" },
    undying          = { "item_vladmir", "item_blade_mail", "item_aghanims_scepter", "item_shivas_guard", "item_lotus_orb" },
    zuus             = { "item_aether_lens", "item_kaya", "item_aghanims_scepter", "item_octarine_core", "item_refresher" },
}

-- ============================================================
-- State (survives match end → reset on backwards gt jump).
-- ============================================================
local state = {
    last_gt = 0,
}

-- ============================================================
-- Helpers
-- ============================================================
local function HasItemInInventory(bot, name)
    if not bot or bot:IsNull() then return false end
    if not bot.FindItemSlot then return false end
    local slot = bot:FindItemSlot(name)
    return slot and slot >= 0
end

local function HeroKey(bot)
    local n = bot:GetUnitName() or ""
    return n:gsub("^npc_dota_hero_", "")
end

-- Pick next late item not yet in inventory.
function M.GetLateItem(bot)
    local late = M.LATE_BUILDS[HeroKey(bot)] or M.LATE_BUILDS.default
    for _, item in ipairs(late) do
        if not HasItemInInventory(bot, item) then
            return item
        end
    end
    return nil
end

-- ============================================================
-- Main think
-- ============================================================
function M.Think(bot)
    if not bot or bot:IsNull() or not bot:IsAlive() then return end

    local gt = DotaTime() or 0

    if gt < state.last_gt - 60 then
        print(string.format("[item_build] new match detected (gt %.1f → %.1f), reset",
            state.last_gt, gt))
        M.Reset()
    end
    state.last_gt = gt

    -- Throttle: 1 attempt per 30 ticks (~3s at 10Hz controller tick).
    BotControllerState.tick = BotControllerState.tick or 0
    if (BotControllerState.tick % 30) ~= 0 then return end

    local target

    -- 1) Starting consumables (tango / branches / salve) — ПЕРВЫМ делом.
    -- HasItemInInventory true даже при 1 charge, поэтому каждый item
    -- покупаем максимум один раз пока он в инвенторе.
    for _, startItem in ipairs(M.STARTING_ITEMS) do
        if not HasItemInInventory(bot, startItem) then
            target = startItem
            break
        end
    end

    -- 2) Core build только после стартовых расходников
    if not target then
        if not HasItemInInventory(bot, "item_power_treads") then
            target = "item_power_treads"
        elseif not HasItemInInventory(bot, "item_meteor_hammer") then
            target = "item_meteor_hammer"
        else
            target = M.GetLateItem(bot)
        end
    end

    if not target then return end

    bot:ActionImmediate_PurchaseItem(target)
    print(string.format("[item_build] quickbuy %s gold=%d hero=%s",
        target, bot:GetGold() or 0, HeroKey(bot)))

    if target == "item_power_treads" then
        BotControllerState.pt_pending_attr_switch = true
        BotControllerState.pt_current_attr = "str"
    end
end

-- ============================================================
-- Power Treads attribute switch
-- Engine cycles STR → AGI → INT → STR per Action_UseAbility(item).
-- ============================================================
function M.MaybeSwitchTreadsAttribute(bot)
    if not bot or bot:IsNull() then return end
    if not BotControllerState.pt_pending_attr_switch then return end

    local slot = bot:FindItemSlot("item_power_treads")
    if not slot or slot < 0 then return end
    local item = bot:GetItemInSlot(slot)
    if not item or item:IsNull() then return end

    local heroName = HeroKey(bot)
    local desired = M.PT_DESIRED_ATTR[heroName] or "str"
    local cur = BotControllerState.pt_current_attr or "str"

    if cur == desired then
        BotControllerState.pt_pending_attr_switch = false
        print(string.format("[item_build] power_treads attr=%s for %s (done)",
            desired, heroName))
        return
    end

    -- Engine has ~0.4s CD on attr switch — 1.5s with jitter is safe.
    local gt = DotaTime() or 0
    local lastTog = BotControllerState.pt_last_toggle_gt or -999
    if gt - lastTog < 1.5 then return end
    BotControllerState.pt_last_toggle_gt = gt

    bot:Action_UseAbility(item)
    BotControllerState.pt_current_attr = M.PT_NEXT[cur] or "str"
    print(string.format("[item_build] power_treads toggle %s → %s (target=%s, hero=%s)",
        cur, BotControllerState.pt_current_attr, desired, heroName))
end

-- ============================================================
-- Entry point — C++ engine calls every tick.
-- ============================================================
function ItemPurchaseThink()
    local bot = GetBot()
    if not bot then return end
    M.Think(bot)
    pcall(M.MaybeSwitchTreadsAttribute, bot)
end

-- ============================================================
-- Reset — called from bot_controller on new match / hot reload.
-- ============================================================
function M.Reset()
    -- last_gt не обнуляем: Reset зовётся из Think после detection
    -- нового матча, gt уже свежий → обнуление зациклит detect.
    BotControllerState.pt_pending_attr_switch = false
    BotControllerState.pt_current_attr = "str"
    BotControllerState.pt_last_toggle_gt = -999

    print("[item_build] quickbuy mode: PT -> Meteor Hammer -> per-hero late")
end

print(string.format("[item_build] loaded (quickbuy, %d hero late builds)",
    (function() local n = 0; for _ in pairs(M.LATE_BUILDS) do n = n + 1 end; return n end)()))

return M
