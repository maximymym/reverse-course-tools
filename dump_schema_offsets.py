"""
Dota 2 SchemaSystem Runtime Offset Dumper

Reads ALL needed class offsets directly from the running game's SchemaSystem.
Generates offsets.py automatically — no more manual "+8 shift" guessing.

Usage:
    python dump_schema_offsets.py          # dump + print
    python dump_schema_offsets.py --apply  # dump + overwrite offsets.py

Run after each Dota 2 update to get fresh offsets.
"""
import struct
import sys
import json
import os

sys.path.insert(0, os.path.dirname(__file__))
from cheat.memory import DotaMemory
from cheat.offsets import SchemaSystem as SS, Interfaces, GameResourceService

# Classes we need from SchemaSystem
TARGET_CLASSES = {
    "C_BaseEntity",
    "CGameSceneNode",
    "C_DOTA_BaseNPC",
    "C_DOTA_BaseNPC_Hero",
    "C_DOTABaseAbility",
    "C_DOTA_UnitInventory",
    "CDOTA_ModifierManager",
    "C_DOTAPlayerController",
    "CDOTAGamerules",
    "C_DOTAGamerulesProxy",
    "C_DOTA_DataNonSpectator",
    "DataTeamPlayer_t",
}

# Fields we MUST have (class -> [field_names])
REQUIRED_FIELDS = {
    "C_BaseEntity": [
        "m_pGameSceneNode", "m_iHealth", "m_iMaxHealth", "m_lifeState", "m_iTeamNum",
    ],
    "CGameSceneNode": [
        "m_vecAbsOrigin",
    ],
    "C_DOTA_BaseNPC": [
        "m_iCurrentLevel", "m_flMana", "m_flMaxMana", "m_flManaRegen", "m_flHealthRegen",
        "m_vecAbilities", "m_iszUnitName", "m_iDamageMin", "m_iDamageMax",
        "m_ModifierManager", "m_Inventory", "m_nUnitState64", "m_nUnitDebuffState",
        "m_iAttackRange", "m_iMoveSpeed", "m_flBaseAttackTime", "m_iBaseAttackSpeed",
        "m_bIsIllusion", "m_bIsPhantom", "m_iCurShop",
        "m_iDayTimeVisionRange", "m_iNightTimeVisionRange",
        "m_flPhysicalArmorValue", "m_flMagicalResistanceValue",
        "m_bHasInventory", "m_bCanUseAllItems",
    ],
    "C_DOTA_BaseNPC_Hero": [
        "m_iCurrentXP", "m_iAbilityPoints", "m_iTotalAbilityPoints",
        "m_flStrength", "m_flAgility", "m_flIntellect",
        "m_flStrengthTotal", "m_flAgilityTotal", "m_flIntellectTotal",
        "m_iPlayerID", "m_flRespawnTime",
    ],
    "C_DOTABaseAbility": [
        "m_iLevel", "m_fCooldown", "m_flCooldownLength", "m_iManaCost",
        "m_bHidden", "m_bActivated", "m_bToggleState", "m_bAutoCastState",
        "m_nAbilityCurrentCharges", "m_fAbilityChargeRestoreTimeRemaining",
    ],
    "C_DOTAPlayerController": [
        "m_nPlayerID", "m_hAssignedHero", "m_hLastAssignedHero",
    ],
    "CDOTAGamerules": [
        "m_nGameState", "m_nServerGameState", "m_nGameMode",
        "m_flGameTime", "m_flGameStartTime", "m_bGamePaused",
        "m_nHeroPickState", "m_flStateTransitionTime",
    ],
    "C_DOTAGamerulesProxy": [
        "m_pGameRules",
    ],
}


def walk_schema(mem, scope_name="client.dll"):
    """Walk SchemaSystem and dump all fields for target classes."""
    schema = mem.find_interface("schemasystem.dll", Interfaces.SCHEMA_SYSTEM)
    if not schema:
        print("[!] SchemaSystem not found")
        return {}

    scopes_base = mem.read_ptr(schema + SS.SCOPES_LIST)
    target_scope = None

    for i in range(25):
        sp = mem.read_ptr(scopes_base + i * 8)
        if not sp or sp < 0x10000:
            continue
        try:
            name = mem.read_string(sp + SS.SCOPE_NAME, 64)
            if name == scope_name:
                target_scope = sp
                break
        except:
            continue

    if not target_scope:
        print(f"[!] Scope '{scope_name}' not found")
        return {}

    # Walk hash map containers
    results = {}  # class_name -> {field_name: offset}
    found_classes = set()

    for ci in range(SS.CONTAINERS_MAX):
        co = target_scope + SS.CONTAINERS_ARRAY + ci * SS.CONTAINERS_ARRAY_STRIDE
        try:
            cp = mem.read_ptr(co + SS.CONTAINER_PTR)
        except:
            continue
        if not cp or cp < 0x10000:
            continue

        for di in range(128):
            do = cp + di * SS.CLASS_DESC_STRIDE
            try:
                dp = mem.read_ptr(do + SS.CLASS_DESC_PTR)
            except:
                break
            if not dp or dp < 0x10000:
                continue
            try:
                np = mem.read_ptr(dp + 0x8)
                cls_name = mem.read_string(np, 128)
            except:
                continue

            if cls_name not in TARGET_CLASSES or cls_name in found_classes:
                continue
            found_classes.add(cls_name)

            # Post-2026-03-25 update: class descriptor layout shifted +8
            # member_count moved from +0x1C to +0x24, members_base from +0x28 to +0x30
            member_count = struct.unpack("<H", mem.pm.read_bytes(dp + 0x24, 2))[0]
            members_base = mem.read_ptr(dp + 0x30)
            if not members_base or members_base < 0x10000:
                continue

            fields = {}
            for mi in range(member_count):
                mp = members_base + mi * 0x20
                try:
                    fnp = mem.read_ptr(mp + 0x0)
                    fn = mem.read_string(fnp, 64)
                    fo = struct.unpack("<H", mem.pm.read_bytes(mp + 0x10, 2))[0]
                    fields[fn] = fo
                except:
                    continue

            results[cls_name] = fields

    return results


def dump_all_scopes(mem):
    """Dump from both client.dll and server.dll scopes (some classes only in one)."""
    all_results = {}
    for scope in ["client.dll", "server.dll"]:
        r = walk_schema(mem, scope)
        for cls, fields in r.items():
            if cls not in all_results or len(fields) > len(all_results[cls]):
                all_results[cls] = fields

    # Base classes (C_BaseEntity, C_DOTABaseAbility, etc.) are NOT in the schema hash map.
    # Their offsets must be found via leaf class inheritance or runtime verification.
    # We infer them from known-good offsets verified against running game.
    # Strategy: read C_DOTA_BaseNPC (which inherits from C_BaseEntity) and check
    # if base class fields are accessible at their known offsets.

    if "C_BaseEntity" not in all_results:
        all_results["C_BaseEntity"] = _infer_base_entity(mem)
    if "C_DOTABaseAbility" not in all_results:
        # Base ability class is never in schema. Use verified fallback.
        # These were verified with shrapnel (level=7) in previous session.
        print("  [+] Using verified fallback for C_DOTABaseAbility")
        all_results["C_DOTABaseAbility"] = ABILITY_FALLBACK
    if "CGameSceneNode" not in all_results:
        all_results["CGameSceneNode"] = {"m_vecAbsOrigin": 0xD0}
    if "CDOTAGamerules" not in all_results:
        all_results["CDOTAGamerules"] = _infer_gamerules(mem)
    if "C_DOTAGamerulesProxy" not in all_results:
        all_results["C_DOTAGamerulesProxy"] = _infer_gamerules_proxy(mem)

    return all_results


def _infer_base_entity(mem):
    """Infer C_BaseEntity offsets by reading a known hero entity."""
    from cheat.game_state import DotaGame
    game = DotaGame(mem)
    game.game_resource = mem.find_interface("engine2.dll", Interfaces.GAME_RESOURCE_SERVICE)
    game.entity_system = mem.read_ptr(game.game_resource + GameResourceService.ENTITY_SYSTEM)

    # Find any hero with valid HP
    for entity, ident, dname in game.iter_entities(max_count=3000):
        if not dname.startswith("npc_dota_hero_") or "announcer" in dname:
            continue
        # Test known offsets (from previous verified values)
        # These are stable — C_BaseEntity rarely changes layout
        candidates = {
            "m_pGameSceneNode": [0x338, 0x330, 0x340],
            "m_iHealth": [0x354, 0x34C, 0x35C],
            "m_iMaxHealth": [0x350, 0x348, 0x358],
            "m_lifeState": [0x358, 0x350, 0x360],
            "m_iTeamNum": [0x3F3, 0x3EB, 0x3FB],
        }

        result = {}
        # Verify m_iHealth: should be positive, reasonable
        for off in candidates["m_iHealth"]:
            try:
                hp = mem.read_i32(entity + off)
                if 1 < hp < 50000:
                    result["m_iHealth"] = off
                    break
            except:
                continue

        # m_iMaxHealth should be at m_iHealth - 4
        if "m_iHealth" in result:
            hp_off = result["m_iHealth"]
            result["m_iMaxHealth"] = hp_off - 4
            result["m_lifeState"] = hp_off + 4

        # m_iTeamNum: should be 2 or 3
        for off in candidates["m_iTeamNum"]:
            try:
                team = mem.read_u8(entity + off)
                if team in (2, 3):
                    result["m_iTeamNum"] = off
                    break
            except:
                continue

        # m_pGameSceneNode: ptr that leads to valid position
        for off in candidates["m_pGameSceneNode"]:
            try:
                scene = mem.read_ptr(entity + off)
                if scene and 0x10000 < scene < 0x7FFF00000000:
                    # Check m_vecAbsOrigin at scene+0xD0
                    x = mem.read_f32(scene + 0xD0)
                    if -20000 < x < 20000:
                        result["m_pGameSceneNode"] = off
                        break
            except:
                continue

        if len(result) >= 3:
            print(f"  [+] Inferred C_BaseEntity from {dname}: {result}")
            return result
        break

    # Fallback: last known good
    print("  [!] Could not infer C_BaseEntity, using last known values")
    return {
        "m_pGameSceneNode": 0x338, "m_iHealth": 0x354, "m_iMaxHealth": 0x350,
        "m_lifeState": 0x358, "m_iTeamNum": 0x3F3,
    }


def _infer_base_ability(mem):
    """Infer C_DOTABaseAbility offsets from a known ability entity."""
    from cheat.game_state import DotaGame
    game = DotaGame(mem)
    game.game_resource = mem.find_interface("engine2.dll", Interfaces.GAME_RESOURCE_SERVICE)
    game.entity_system = mem.read_ptr(game.game_resource + GameResourceService.ENTITY_SYSTEM)

    # Find any ability entity (designer_name contains ability name)
    for entity, ident, dname in game.iter_entities(max_count=5000):
        if not dname.startswith(("lion_", "sniper_", "viper_", "sven_", "pudge_")):
            continue
        if "hero" in dname:
            continue

        # Test m_iLevel candidates — must be small int, NOT float 1.0 (0x3F800000)
        candidates_level = [0x628, 0x620, 0x630, 0x618]
        for off in candidates_level:
            try:
                raw = mem.pm.read_bytes(entity + off, 4)
                lv = struct.unpack("<i", raw)[0]
                # Reject float-like values (0x3F800000 = 1.0f reads as int 1065353216)
                if lv > 100 or lv < 0:
                    continue
                if 0 <= lv <= 30:
                    # Verify: at off+0x10 should be cooldown (float, typically 0.0 or gametime)
                    cd_off = off + 0x10
                    cd = mem.read_f32(entity + cd_off)
                    if -1 < cd < 999999:
                        result = {
                            "m_iLevel": off,
                            "m_fCooldown": cd_off,
                            "m_flCooldownLength": cd_off + 4,
                            "m_iManaCost": cd_off + 8,
                            "m_bHidden": off - 0x11,
                            "m_bActivated": off - 0xF,
                            "m_bToggleState": off + 0x5,
                            "m_bAutoCastState": cd_off + 0xC,
                            "m_nAbilityCurrentCharges": cd_off + 0x28,
                            "m_fAbilityChargeRestoreTimeRemaining": cd_off + 0x2C,
                        }
                        print(f"  [+] Inferred C_DOTABaseAbility from {dname}: level@{hex(off)}")
                        return result
            except:
                continue
        break

    # Fallback: last known good (verified via SchemaSystem query + runtime test, 2026-03-23)
    print("  [!] Could not infer C_DOTABaseAbility, using last known values")
    return ABILITY_FALLBACK


# Verified ability offsets — used as both fallback and validation reference
ABILITY_FALLBACK = {
    "m_iLevel": 0x628, "m_fCooldown": 0x638, "m_flCooldownLength": 0x63C,
    "m_iManaCost": 0x640, "m_bHidden": 0x617, "m_bActivated": 0x619,
    "m_bToggleState": 0x62D, "m_bAutoCastState": 0x644,
    "m_nAbilityCurrentCharges": 0x660, "m_fAbilityChargeRestoreTimeRemaining": 0x664,
}


def _infer_gamerules(mem):
    """Infer CDOTAGamerules offsets. These are from Dota2Patcher (not shifted)."""
    result = {
        "m_bGamePaused": 0x38, "m_nServerGameState": 0x74, "m_nGameState": 0x7C,
        "m_nGameMode": 0xE4,
        "m_flGameTime": 0x30,  # approximate
        "m_flGameStartTime": 0x34,  # approximate
    }

    # Probe m_nHeroPickState if in hero selection (game_state == 2)
    try:
        from cheat.game_state import DotaGame
        game = DotaGame(mem)
        game.game_resource = mem.find_interface("engine2.dll", Interfaces.GAME_RESOURCE_SERVICE)
        game.entity_system = mem.read_ptr(game.game_resource + GameResourceService.ENTITY_SYSTEM)
        game._find_gamerules()
        if game.gamerules:
            state = mem.read_i32(game.gamerules + 0x7C)
            if state == 2:  # HERO_SELECTION — good time to probe
                known = {0x30, 0x34, 0x38, 0x74, 0x7C, 0xE4}
                for off in [0x80, 0x84, 0x88, 0x8C, 0x90]:
                    if off in known:
                        continue
                    val = mem.read_i32(game.gamerules + off)
                    if 0 <= val <= 20 and val != state:
                        print(f"  [?] m_nHeroPickState candidate: {hex(off)} = {val}")
                        result["m_nHeroPickState"] = off
                        break
    except:
        pass

    return result


def _infer_gamerules_proxy(mem):
    """Infer GamerulesProxy offset by testing known values."""
    from cheat.game_state import DotaGame
    game = DotaGame(mem)
    game.game_resource = mem.find_interface("engine2.dll", Interfaces.GAME_RESOURCE_SERVICE)
    game.entity_system = mem.read_ptr(game.game_resource + GameResourceService.ENTITY_SYSTEM)

    proxy = game._find_entity_by_designer_name("dota_gamerules")
    if not proxy:
        print("  [!] dota_gamerules not found, using fallback")
        return {"m_pGameRules": 0x5F8}

    # Test candidates for m_pGameRules
    for off in [0x5F8, 0x5E8, 0x5F0, 0x608, 0x510, 0x518]:
        try:
            gr = mem.read_ptr(proxy + off)
            if not gr or gr < 0x10000:
                continue
            # Verify: gamerules+0x7C should be a valid game state (0-7)
            state = mem.read_i32(gr + 0x7C)
            if 0 <= state <= 7:
                print(f"  [+] Inferred GamerulesProxy.m_pGameRules = {hex(off)} (state={state})")
                return {"m_pGameRules": off}
        except:
            continue

    print("  [!] Could not infer GamerulesProxy, using fallback")
    return {"m_pGameRules": 0x5F8}


def generate_offsets_py(dump: dict) -> str:
    """Generate offsets.py source code from schema dump."""

    def get(cls, field, default="???"):
        fields = dump.get(cls, {})
        val = fields.get(field)
        if val is not None:
            return val
        return default

    def fmt(val):
        if isinstance(val, int):
            return f"0x{val:X}"
        return str(val)

    # Verify required fields
    missing = []
    for cls, field_list in REQUIRED_FIELDS.items():
        for f in field_list:
            if get(cls, f) == "???":
                missing.append(f"{cls}.{f}")
    if missing:
        print(f"[!] WARNING: {len(missing)} required fields NOT found in SchemaSystem:")
        for m in missing:
            print(f"    - {m}")
        print("    These classes may not be in schema (base classes, embedded structs).")
        print("    Will use previous known values where available.")

    # Build the source
    lines = []
    lines.append('"""')
    lines.append("Dota 2 Offsets - AUTO-GENERATED from SchemaSystem runtime dump.")
    lines.append("Do NOT edit manually. Run dump_schema_offsets.py --apply to regenerate.")
    lines.append('"""')
    lines.append("")

    # Patterns (not from schema, static)
    lines.append("# === Patterns (for runtime scanning) ===")
    lines.append("class Patterns:")
    lines.append('    CREATE_INTERFACE = "4C 8B ?? ?? ?? ?? ?? 4C 8B ?? 4C 8B ?? 4D 85 ?? 74 ?? 49 8B ?? ?? 4D 8B"')
    lines.append("")

    # Interfaces
    lines.append("# === Interface names ===")
    lines.append("class Interfaces:")
    lines.append('    GAME_RESOURCE_SERVICE = "GameResourceServiceClientV001"')
    lines.append('    SOURCE2_CLIENT = "Source2Client002"')
    lines.append('    SCHEMA_SYSTEM = "SchemaSystem_001"')
    lines.append('    CVAR = "VEngineCvar007"')
    lines.append("")

    # SchemaSystem layout (these are constants, not from dump)
    lines.append("# === SchemaSystem layout (runtime verified) ===")
    lines.append("class SchemaSystem:")
    lines.append("    SCOPES_LIST = 0x198")
    lines.append("    SCOPE_NAME = 0x8")
    lines.append("    CONTAINERS_ARRAY = 0x580")
    lines.append("    CONTAINERS_ARRAY_STRIDE = 0x28")
    lines.append("    CONTAINERS_MAX = 256")
    lines.append("    CONTAINER_PTR = 0x18")
    lines.append("    CLASS_DESC_STRIDE = 0x20")
    lines.append("    CLASS_DESC_PTR = 0x10")
    lines.append("")

    # GameResourceService
    lines.append("# === CGameResourceService ===")
    lines.append("class GameResourceService:")
    lines.append("    ENTITY_SYSTEM = 0x58")
    lines.append("")

    # EntitySystem (constants, not schema)
    lines.append("# === CGameEntitySystem ===")
    lines.append("class EntitySystem:")
    lines.append("    CHUNK_LISTS = [0x10, 0x18, 0x20]")
    lines.append("    FIRST_IDENTITY = 0x210")
    lines.append("    MAX_CHUNKS = 64")
    lines.append("    SLOTS_PER_CHUNK = 512")
    lines.append("")

    # EntityIdentity (constants)
    lines.append("# === CEntityIdentity (stride 0x78) ===")
    lines.append("class EntityIdentity:")
    lines.append("    STRIDE = 0x78")
    lines.append("    ENTITY_PTR = 0x00")
    lines.append("    SCHEMA_CLASS_BINDING = 0x08")
    lines.append("    HANDLE = 0x10")
    lines.append("    NAME = 0x18")
    lines.append("    DESIGNER_NAME = 0x20")
    lines.append("    FLAGS = 0x30")
    lines.append("    PREV = 0x58")
    lines.append("    NEXT = 0x60")
    lines.append("")

    # EntityInstance
    lines.append("# === CEntityInstance ===")
    lines.append("class EntityInstance:")
    lines.append("    IDENTITY = 0x10")
    lines.append("")

    # C_DOTAGamerulesProxy
    v = get("C_DOTAGamerulesProxy", "m_pGameRules")
    lines.append("# === C_DOTAGamerulesProxy (schema dump) ===")
    lines.append("class GamerulesProxy:")
    lines.append(f"    GAMERULES = {fmt(v)}  # m_pGameRules")
    lines.append("")

    # CDOTAGamerules
    lines.append("# === CDOTAGamerules (schema dump) ===")
    lines.append("class Gamerules:")
    gr = dump.get("CDOTAGamerules", {})
    for field, pyname in [
        ("m_bGamePaused", "PAUSED"), ("m_nServerGameState", "SERVER_GAME_STATE"),
        ("m_nGameState", "GAME_STATE"), ("m_nGameMode", "GAME_MODE"),
        ("m_flGameTime", "GAME_TIME"), ("m_flGameStartTime", "GAME_START_TIME"),
    ]:
        v = gr.get(field, "???")
        lines.append(f"    {pyname} = {fmt(v)}  # {field}")
    lines.append("")

    # C_BaseEntity
    lines.append("# === C_BaseEntity (schema dump) ===")
    lines.append("class BaseEntity:")
    be = dump.get("C_BaseEntity", {})
    for field, pyname in [
        ("m_pGameSceneNode", "GAME_SCENE_NODE"), ("m_iMaxHealth", "MAX_HEALTH"),
        ("m_iHealth", "HEALTH"), ("m_lifeState", "LIFE_STATE"), ("m_iTeamNum", "TEAM_NUM"),
    ]:
        v = be.get(field, "???")
        lines.append(f"    {pyname} = {fmt(v)}  # {field}")
    lines.append("")

    # CGameSceneNode
    lines.append("# === CGameSceneNode (schema dump) ===")
    lines.append("class GameSceneNode:")
    gsn = dump.get("CGameSceneNode", {})
    v = gsn.get("m_vecAbsOrigin", 0xD0)
    lines.append(f"    VEC_ABS_ORIGIN = {fmt(v)}  # Vector3 (float x,y,z)")
    lines.append("")

    # C_DOTA_BaseNPC
    lines.append("# === C_DOTA_BaseNPC (schema dump) ===")
    lines.append("class BaseNPC:")
    bnpc = dump.get("C_DOTA_BaseNPC", {})

    npc_fields = [
        ("m_iCurrentLevel", "CURRENT_LEVEL"),
        ("m_flMana", "MANA"), ("m_flMaxMana", "MAX_MANA"),
        ("m_flManaRegen", "MANA_REGEN"), ("m_flHealthRegen", "HEALTH_REGEN"),
        ("m_iDamageMin", "DAMAGE_MIN"), ("m_iDamageMax", "DAMAGE_MAX"),
        ("m_iszUnitName", "UNIT_NAME"),
        ("m_ModifierManager", "MODIFIER_MANAGER"),
        ("m_nUnitState64", "UNIT_STATE_64"),
        ("m_nUnitDebuffState", "UNIT_DEBUFF_STATE"),
        ("m_iAttackRange", "ATTACK_RANGE"),
        ("m_iMoveSpeed", "MOVE_SPEED"),
        ("m_flBaseAttackTime", "BASE_ATTACK_TIME"),
        ("m_iBaseAttackSpeed", "BASE_ATTACK_SPEED"),
        ("m_bIsIllusion", "IS_ILLUSION"),
        ("m_bIsPhantom", "IS_PHANTOM"),
        ("m_iDayTimeVisionRange", "DAY_VISION_RANGE"),
        ("m_iNightTimeVisionRange", "NIGHT_VISION_RANGE"),
        ("m_flPhysicalArmorValue", "PHYSICAL_ARMOR"),
        ("m_flMagicalResistanceValue", "MAGIC_RESIST"),
        ("m_bHasInventory", "HAS_INVENTORY"),
    ]
    for field, pyname in npc_fields:
        v = bnpc.get(field, "???")
        lines.append(f"    {pyname} = {fmt(v)}  # {field}")

    # m_vecAbilities special handling
    vec_ab = bnpc.get("m_vecAbilities", "???")
    lines.append(f"    # m_vecAbilities: CNetworkUtlVectorBase at schema {fmt(vec_ab)}")
    lines.append(f"    # Layout: [count(4)][pad(4)][data_ptr(8)][count(4)]")
    if isinstance(vec_ab, int):
        lines.append(f"    ABILITIES_COUNT = {fmt(vec_ab)}  # int32 count")
        lines.append(f"    ABILITIES_PTR = {fmt(vec_ab + 8)}  # ptr to CHandle[] array")
    else:
        lines.append(f"    ABILITIES_COUNT = ???")
        lines.append(f"    ABILITIES_PTR = ???")

    # m_Inventory special handling
    inv = bnpc.get("m_Inventory", "???")
    lines.append(f"    # m_Inventory: C_DOTA_UnitInventory at schema {fmt(inv)}")
    lines.append(f"    INVENTORY = {fmt(inv)}")
    if isinstance(inv, int):
        lines.append(f"    # m_hItems: INLINE CHandle[25] inside inventory")
        lines.append(f"    ITEMS_COUNT = {fmt(inv + 0x20)}  # int32 (inventory+0x20)")
        lines.append(f"    ITEMS_INLINE = {fmt(inv + 0x24)}  # CHandle[25] inline (inventory+0x24)")
    else:
        lines.append(f"    ITEMS_COUNT = ???")
        lines.append(f"    ITEMS_INLINE = ???")
    lines.append("")

    # C_DOTABaseAbility
    lines.append("# === C_DOTABaseAbility (schema dump) ===")
    lines.append("class BaseAbility:")
    ab = dump.get("C_DOTABaseAbility", {})
    ab_fields = [
        ("m_iLevel", "LEVEL"), ("m_fCooldown", "COOLDOWN"),
        ("m_flCooldownLength", "COOLDOWN_LENGTH"), ("m_iManaCost", "MANA_COST"),
        ("m_bHidden", "HIDDEN"), ("m_bActivated", "ACTIVATED"),
        ("m_bToggleState", "TOGGLE_STATE"), ("m_bAutoCastState", "AUTOCAST_STATE"),
        ("m_nAbilityCurrentCharges", "CHARGES"),
        ("m_fAbilityChargeRestoreTimeRemaining", "CHARGE_RESTORE_TIME"),
    ]
    for field, pyname in ab_fields:
        v = ab.get(field, "???")
        lines.append(f"    {pyname} = {fmt(v)}  # {field}")
    lines.append("")

    # C_DOTA_BaseNPC_Hero
    lines.append("# === C_DOTA_BaseNPC_Hero (schema dump) ===")
    lines.append("class BaseHero:")
    hero = dump.get("C_DOTA_BaseNPC_Hero", {})
    hero_fields = [
        ("m_iCurrentXP", "CURRENT_XP"), ("m_iAbilityPoints", "ABILITY_POINTS"),
        ("m_flStrength", "STRENGTH"), ("m_flAgility", "AGILITY"), ("m_flIntellect", "INTELLECT"),
        ("m_flStrengthTotal", "STRENGTH_TOTAL"), ("m_flAgilityTotal", "AGILITY_TOTAL"),
        ("m_flIntellectTotal", "INTELLECT_TOTAL"),
        ("m_iPlayerID", "PLAYER_ID"), ("m_flRespawnTime", "RESPAWN_TIME"),
    ]
    for field, pyname in hero_fields:
        v = hero.get(field, "???")
        lines.append(f"    {pyname} = {fmt(v)}  # {field}")
    lines.append("")

    # UnitState flags (constants, not from schema)
    lines.append("# === m_nUnitState64 bit flags (MODIFIER_STATE_* enum) ===")
    lines.append("class UnitState:")
    state_flags = [
        ("ROOTED", 0), ("DISARMED", 1), ("ATTACK_IMMUNE", 2), ("SILENCED", 3),
        ("MUTED", 4), ("STUNNED", 5), ("HEXED", 6), ("INVISIBLE", 7),
        ("INVULNERABLE", 8), ("MAGIC_IMMUNE", 9), ("PROVIDES_VISION", 10),
        ("NIGHTMARED", 11), ("BLOCK_DISABLED", 12), ("EVADE_DISABLED", 13),
        ("UNSELECTABLE", 14), ("CANNOT_MISS", 15), ("SPECIALLY_DENIABLE", 16),
        ("FROZEN", 17), ("COMMAND_RESTRICTED", 18), ("NOT_ON_MINIMAP", 19),
        ("LOW_ATTACK_PRIORITY", 20), ("NO_HEALTH_BAR", 21), ("FLYING", 22),
        ("NO_UNIT_COLLISION", 23), ("NO_TEAM_MOVE_TO", 24), ("NO_TEAM_SELECT", 25),
        ("PASSIVES_DISABLED", 26), ("DOMINATED", 27), ("BLIND", 28),
        ("OUT_OF_GAME", 29), ("FAKE_ALLY", 30),
        ("FLYING_FOR_PATHING_PURPOSES_ONLY", 31), ("TRUESIGHT_IMMUNE", 32),
        ("UNTARGETABLE", 33), ("IGNORING_MOVE_AND_ATTACK_ORDERS", 34),
        ("ALLOW_PATHING_THROUGH_TREES", 35), ("NOT_ON_MINIMAP_FOR_ENEMIES", 36),
        ("DEBUFF_IMMUNE", 37),
    ]
    for name, bit in state_flags:
        lines.append(f"    {name:42s} = 1 << {bit}")
    lines.append("")

    # Game State Enum
    lines.append("# === Game State Enum ===")
    lines.append("class GameState:")
    for name, val in [("INIT", 0), ("WAIT_FOR_PLAYERS", 1), ("HERO_SELECTION", 2),
                      ("STRATEGY_TIME", 3), ("PRE_GAME", 4), ("GAME_IN_PROGRESS", 5),
                      ("POST_GAME", 6), ("DISCONNECT", 7)]:
        lines.append(f"    {name} = {val}")
    lines.append("    NAMES = {")
    for val, name in [(0, "INIT"), (1, "WAIT_FOR_PLAYERS"), (2, "HERO_SELECTION"),
                      (3, "STRATEGY_TIME"), (4, "PRE_GAME"), (5, "GAME_IN_PROGRESS"),
                      (6, "POST_GAME"), (7, "DISCONNECT")]:
        lines.append(f'        {val}: "{name}",')
    lines.append("    }")
    lines.append("")

    # Teams
    lines.append("# === Teams ===")
    lines.append("class Team:")
    lines.append("    SPECTATOR = 1")
    lines.append("    RADIANT = 2")
    lines.append("    DIRE = 3")
    lines.append("")

    # PrepareUnitOrders
    lines.append("# === PrepareUnitOrders (client.dll) ===")
    lines.append("PREPARE_UNIT_ORDERS_RVA = 0x1D16120  # AOB-scanned, verify per build")
    lines.append("")

    # PlayerController
    lines.append("# === CDOTAPlayerController (schema dump) ===")
    lines.append("class PlayerController:")
    pc = dump.get("C_DOTAPlayerController", {})
    pc_fields = [
        ("m_nPlayerID", "PLAYER_SLOT"),
        ("m_hAssignedHero", "ASSIGNED_HERO"),
        ("m_hLastAssignedHero", "LAST_ASSIGNED_HERO"),
    ]
    for field, pyname in pc_fields:
        v = pc.get(field, "???")
        lines.append(f"    {pyname} = {fmt(v)}  # {field}")
    lines.append("    PLAYER_NAME = 0x6E0  # inline string (not in schema)")
    lines.append("")

    # DataNonSpectator (may not be in schema)
    lines.append("# === C_DOTA_DataNonSpectator (gold, per-team player data) ===")
    lines.append("# NOTE: This is an embedded entity, may need special handling")
    lines.append("class DataNonSpectator:")
    lines.append("    DATA_TEAM_PTR = 0x5F0   # TODO: verify via runtime scan")
    lines.append("    DATA_TEAM_COUNT = 0x5F8")
    lines.append("    DATA_TEAM_PLAYER_STRIDE = 0x1220  # TODO: verify")
    lines.append("")
    lines.append("class DataTeamPlayer:")
    lines.append("    RELIABLE_GOLD = 0x30")
    lines.append("    UNRELIABLE_GOLD = 0x34")
    lines.append("    TOTAL_EARNED_GOLD = 0x3C")
    lines.append("    NET_WORTH = 0x8C")
    lines.append("    LAST_HITS = 0x94")
    lines.append("    DENIES = 0x98")
    lines.append("")

    return "\n".join(lines) + "\n"


def main():
    apply = "--apply" in sys.argv

    print("[*] Connecting to Dota 2...")
    mem = DotaMemory()
    print(f"[+] PID: {mem.pid}")

    print("[*] Dumping SchemaSystem...")
    dump = dump_all_scopes(mem)

    print(f"[+] Found {len(dump)} classes:")
    for cls, fields in sorted(dump.items()):
        print(f"    {cls}: {len(fields)} fields")

    # Save raw dump
    dump_json = {}
    for cls, fields in dump.items():
        dump_json[cls] = {f: {"offset": o, "hex": f"0x{o:X}"} for f, o in fields.items()}

    json_path = os.path.join(os.path.dirname(__file__), "schema_dump.json")
    with open(json_path, "w") as f:
        json.dump(dump_json, f, indent=2)
    print(f"[+] Raw dump saved to {json_path}")

    # Generate offsets.py
    source = generate_offsets_py(dump)

    if apply:
        offsets_path = os.path.join(os.path.dirname(__file__), "cheat", "offsets.py")
        with open(offsets_path, "w", encoding="utf-8") as f:
            f.write(source)
        print(f"[+] offsets.py written to {offsets_path}")
    else:
        print("\n" + "=" * 60)
        print("Generated offsets.py (use --apply to write):")
        print("=" * 60)
        print(source)

    mem.close()


if __name__ == "__main__":
    main()
