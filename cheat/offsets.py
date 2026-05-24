"""
Dota 2 Offsets - AUTO-GENERATED from SchemaSystem runtime dump.
Do NOT edit manually. Run dump_schema_offsets.py --apply to regenerate.
"""

# === Patterns (for runtime scanning) ===
class Patterns:
    CREATE_INTERFACE = "4C 8B ?? ?? ?? ?? ?? 4C 8B ?? 4C 8B ?? 4D 85 ?? 74 ?? 49 8B ?? ?? 4D 8B"

# === Interface names ===
class Interfaces:
    GAME_RESOURCE_SERVICE = "GameResourceServiceClientV001"
    SOURCE2_CLIENT = "Source2Client002"
    SCHEMA_SYSTEM = "SchemaSystem_001"
    CVAR = "VEngineCvar007"

# === SchemaSystem layout (runtime verified) ===
class SchemaSystem:
    SCOPES_LIST = 0x198
    SCOPE_NAME = 0x8
    CONTAINERS_ARRAY = 0x580
    CONTAINERS_ARRAY_STRIDE = 0x28
    CONTAINERS_MAX = 256
    CONTAINER_PTR = 0x18
    CLASS_DESC_STRIDE = 0x20
    CLASS_DESC_PTR = 0x10

# === CGameResourceService ===
class GameResourceService:
    ENTITY_SYSTEM = 0x58

# === CGameEntitySystem ===
class EntitySystem:
    CHUNK_LISTS = [0x10, 0x18, 0x20]
    FIRST_IDENTITY = 0x210
    MAX_CHUNKS = 64
    SLOTS_PER_CHUNK = 512

# === CEntityIdentity ===
# Two access patterns coexist:
# 1. Chunk-based scan (entity discovery): stride 0x78, designer_name at +0x20
# 2. Flat index lookup (resolve_handle): list_base + eidx * 0x70, name at +0x18
class EntityIdentity:
    STRIDE = 0x78          # chunk-based iteration stride (entity scan)
    FLAT_STRIDE = 0x70     # flat index stride (resolve_handle)
    ENTITY_PTR = 0x00
    SCHEMA_CLASS_BINDING = 0x08
    HANDLE = 0x10
    NAME = 0x18
    DESIGNER_NAME = 0x20   # chunk-based scan
    DESIGNER_NAME_FLAT = 0x18  # flat index layout (resolve_handle)
    FLAGS = 0x30
    PREV = 0x58
    NEXT = 0x60

# === CEntityInstance ===
class EntityInstance:
    IDENTITY = 0x10

# === C_DOTAGamerulesProxy (runtime verified 2026-03-25) ===
# Schema reports 0x5F8 but runtime probing confirms 0x5F0 (state=2 at +0x7C)
class GamerulesProxy:
    GAMERULES = 0x5F0  # m_pGameRules

# === CDOTAGamerules (schema dump) ===
class Gamerules:
    PAUSED = 0x38  # m_bGamePaused
    SERVER_GAME_STATE = 0x74  # m_nServerGameState
    GAME_STATE = 0x7C  # m_nGameState
    GAME_MODE = 0xE4  # m_nGameMode
    GAME_TIME = 0x30  # m_flGameTime
    GAME_START_TIME = 0x34  # m_flGameStartTime
    HERO_PICK_STATE = 0x78  # m_nHeroPickState — verified via Captain's Draft probe (2026-03-25)

# === Hero Pick Sub-Phases (m_nHeroPickState) ===
# Values are mode-dependent. Observed:
#   All Pick (lobby): constant 1 (no ban phase in bot lobby)
#   Captain's Draft: 37→38→42→43→44→45 (increments per ban/pick round)
# For All Pick matchmaking (ranked): expected to change between ban→pick rounds.
class HeroPickPhase:
    NONE = 0
    # All Pick: value 1 = entire hero selection (no sub-phases in lobby)
    # Captain's Draft/Mode: values increment per round (30+)
    # Safe heuristic: if value changed since last read → phase transition happened
    AP_BAN_NOMINAL = 1    # All Pick ban nomination (or only phase in lobby)
    AP_PICK = 2           # All Pick pick phase (expected in ranked AP)
    # For is_pick_phase(): compare against mode-specific threshold

# === C_BaseEntity (runtime probed 2026-03-25, -8 shift from prev build) ===
class BaseEntity:
    GAME_SCENE_NODE = 0x330  # m_pGameSceneNode
    MAX_HEALTH = 0x348  # m_iMaxHealth
    HEALTH = 0x34C  # m_iHealth
    LIFE_STATE = 0x350  # m_lifeState
    TEAM_NUM = 0x3EB  # m_iTeamNum

# === CGameSceneNode (runtime probed 2026-03-25) ===
class GameSceneNode:
    VEC_ABS_ORIGIN = 0xD8  # Vector3 (float x,y,z) — was 0xD0, shifted +8

# === C_DOTA_BaseNPC (schema dump) ===
class BaseNPC:
    CURRENT_LEVEL = 0xB94  # m_iCurrentLevel
    MANA = 0xBEC  # m_flMana
    MAX_MANA = 0xBF0  # m_flMaxMana
    MANA_REGEN = 0x1628  # m_flManaRegen
    HEALTH_REGEN = 0x162C  # m_flHealthRegen
    DAMAGE_MIN = 0xD08  # m_iDamageMin
    DAMAGE_MAX = 0xD0C  # m_iDamageMax
    UNIT_NAME = 0xC60  # m_iszUnitName
    MODIFIER_MANAGER = 0xD20  # m_ModifierManager
    UNIT_STATE_64 = 0x1190  # m_nUnitState64
    UNIT_DEBUFF_STATE = 0x11A0  # m_nUnitDebuffState
    ATTACK_RANGE = 0xBC0  # m_iAttackRange
    MOVE_SPEED = 0xBD4  # m_iMoveSpeed
    BASE_ATTACK_TIME = 0xBDC  # m_flBaseAttackTime
    BASE_ATTACK_SPEED = 0xBD8  # m_iBaseAttackSpeed
    IS_ILLUSION = 0xC14  # m_bIsIllusion
    IS_PHANTOM = 0xB78  # m_bIsPhantom
    DAY_VISION_RANGE = 0xD00  # m_iDayTimeVisionRange
    NIGHT_VISION_RANGE = 0xD04  # m_iNightTimeVisionRange
    PHYSICAL_ARMOR = 0x1510  # m_flPhysicalArmorValue
    MAGIC_RESIST = 0x1514  # m_flMagicalResistanceValue
    HAS_INVENTORY = 0x11A8  # m_bHasInventory
    # m_vecAbilities: CNetworkUtlVectorBase at schema 0xC18
    # Layout: [count(4)][pad(4)][data_ptr(8)][count(4)]
    ABILITIES_COUNT = 0xC18  # int32 count
    ABILITIES_PTR = 0xC20  # ptr to CHandle[] array
    # m_Inventory: C_DOTA_UnitInventory at schema 0x1098
    INVENTORY = 0x1098
    # m_hItems: INLINE CHandle[25] inside inventory
    ITEMS_COUNT = 0x10B8  # int32 (inventory+0x20)
    ITEMS_INLINE = 0x10BC  # CHandle[25] inline (inventory+0x24)

# === C_DOTABaseAbility (schema offsets, NO +8 shift post-2026-03-25 update) ===
# Post-update: schema offsets = runtime offsets (verified on axe abilities)
class BaseAbility:
    LEVEL = 0x628  # m_iLevel
    COOLDOWN = 0x638  # m_fCooldown
    COOLDOWN_LENGTH = 0x63C  # m_flCooldownLength
    MANA_COST = 0x640  # m_iManaCost
    HIDDEN = 0x617  # m_bHidden
    ACTIVATED = 0x619  # m_bActivated
    TOGGLE_STATE = 0x62D  # m_bToggleState
    AUTOCAST_STATE = 0x644  # m_bAutoCastState
    CHARGES = 0x660  # m_nAbilityCurrentCharges
    CHARGE_RESTORE_TIME = 0x664  # m_fAbilityChargeRestoreTimeRemaining

# === C_DOTA_BaseNPC_Hero (schema dump) ===
class BaseHero:
    CURRENT_XP = 0x19A4  # m_iCurrentXP
    ABILITY_POINTS = 0x19A8  # m_iAbilityPoints
    STRENGTH = 0x19C0  # m_flStrength
    AGILITY = 0x19C4  # m_flAgility
    INTELLECT = 0x19C8  # m_flIntellect
    STRENGTH_TOTAL = 0x19CC  # m_flStrengthTotal
    AGILITY_TOTAL = 0x19D0  # m_flAgilityTotal
    INTELLECT_TOTAL = 0x19D4  # m_flIntellectTotal
    PLAYER_ID = 0x1A48  # m_iPlayerID
    RESPAWN_TIME = 0x19B8  # m_flRespawnTime

# === m_nUnitState64 bit flags (MODIFIER_STATE_* enum) ===
class UnitState:
    ROOTED                                     = 1 << 0
    DISARMED                                   = 1 << 1
    ATTACK_IMMUNE                              = 1 << 2
    SILENCED                                   = 1 << 3
    MUTED                                      = 1 << 4
    STUNNED                                    = 1 << 5
    HEXED                                      = 1 << 6
    INVISIBLE                                  = 1 << 7
    INVULNERABLE                               = 1 << 8
    MAGIC_IMMUNE                               = 1 << 9
    PROVIDES_VISION                            = 1 << 10
    NIGHTMARED                                 = 1 << 11
    BLOCK_DISABLED                             = 1 << 12
    EVADE_DISABLED                             = 1 << 13
    UNSELECTABLE                               = 1 << 14
    CANNOT_MISS                                = 1 << 15
    SPECIALLY_DENIABLE                         = 1 << 16
    FROZEN                                     = 1 << 17
    COMMAND_RESTRICTED                         = 1 << 18
    NOT_ON_MINIMAP                             = 1 << 19
    LOW_ATTACK_PRIORITY                        = 1 << 20
    NO_HEALTH_BAR                              = 1 << 21
    FLYING                                     = 1 << 22
    NO_UNIT_COLLISION                          = 1 << 23
    NO_TEAM_MOVE_TO                            = 1 << 24
    NO_TEAM_SELECT                             = 1 << 25
    PASSIVES_DISABLED                          = 1 << 26
    DOMINATED                                  = 1 << 27
    BLIND                                      = 1 << 28
    OUT_OF_GAME                                = 1 << 29
    FAKE_ALLY                                  = 1 << 30
    FLYING_FOR_PATHING_PURPOSES_ONLY           = 1 << 31
    TRUESIGHT_IMMUNE                           = 1 << 32
    UNTARGETABLE                               = 1 << 33
    IGNORING_MOVE_AND_ATTACK_ORDERS            = 1 << 34
    ALLOW_PATHING_THROUGH_TREES                = 1 << 35
    NOT_ON_MINIMAP_FOR_ENEMIES                 = 1 << 36
    DEBUFF_IMMUNE                              = 1 << 37

# === Game State Enum ===
class GameState:
    INIT = 0
    WAIT_FOR_PLAYERS = 1
    HERO_SELECTION = 2
    STRATEGY_TIME = 3
    PRE_GAME = 4
    GAME_IN_PROGRESS = 5
    POST_GAME = 6
    DISCONNECT = 7
    NAMES = {
        0: "INIT",
        1: "WAIT_FOR_PLAYERS",
        2: "HERO_SELECTION",
        3: "STRATEGY_TIME",
        4: "PRE_GAME",
        5: "GAME_IN_PROGRESS",
        6: "POST_GAME",
        7: "DISCONNECT",
    }

# === Teams ===
class Team:
    SPECTATOR = 1
    RADIANT = 2
    DIRE = 3

# === PrepareUnitOrders (client.dll) ===
PREPARE_UNIT_ORDERS_RVA = 0x1E05970  # AOB: 4C 89 4C 24 20 (verified 2026-03-25)

# === CDOTAPlayerController (schema dump) ===
class PlayerController:
    PLAYER_SLOT = 0x900  # m_nPlayerID
    ASSIGNED_HERO = 0x904  # m_hAssignedHero
    LAST_ASSIGNED_HERO = 0x908  # m_hLastAssignedHero
    PLAYER_NAME = 0x6E0  # inline string (not in schema)

# === C_DOTA_DataNonSpectator (gold, per-team player data) ===
# NOTE: This is an embedded entity, may need special handling
class DataNonSpectator:
    DATA_TEAM_PTR = 0x5F0   # TODO: verify via runtime scan
    DATA_TEAM_COUNT = 0x5F8
    DATA_TEAM_PLAYER_STRIDE = 0x1220  # TODO: verify

class DataTeamPlayer:
    RELIABLE_GOLD = 0x30
    UNRELIABLE_GOLD = 0x34
    TOTAL_EARNED_GOLD = 0x3C
    NET_WORTH = 0x8C
    LAST_HITS = 0x94
    DENIES = 0x98

