"""
Dota 2 Game State Reader — finds interfaces, reads game state, entities.
"""
import struct
from .memory import DotaMemory
from .offsets import (
    Interfaces, GameResourceService, EntitySystem, EntityIdentity,
    GamerulesProxy, Gamerules, BaseEntity, GameSceneNode, BaseNPC, GameState, Team,
    BaseAbility, BaseHero, UnitState, DataNonSpectator, DataTeamPlayer,
    PlayerController, EntityInstance, HeroPickPhase
)


class DotaGame:
    def __init__(self, mem: DotaMemory = None):
        self.mem = mem or DotaMemory()
        self.game_resource = 0
        self.entity_system = 0
        self.gamerules = 0

    def init(self) -> bool:
        """Find all runtime pointers. Returns True if successful."""
        # 1. Find GameResourceServiceClientV001
        self.game_resource = self.mem.find_interface("engine2.dll", Interfaces.GAME_RESOURCE_SERVICE)
        if not self.game_resource:
            print("[!] GameResourceService not found")
            return False
        print(f"[+] GameResourceService: {hex(self.game_resource)}")

        # 2. CGameEntitySystem = GameResourceService + 0x58
        self.entity_system = self.mem.read_ptr(self.game_resource + GameResourceService.ENTITY_SYSTEM)
        if not self.entity_system:
            print("[!] CGameEntitySystem is null")
            return False
        print(f"[+] CGameEntitySystem: {hex(self.entity_system)}")

        # 3. Try to find CDOTAGamerules
        self._find_gamerules()
        return True

    def _find_gamerules(self) -> bool:
        """Find dota_gamerules entity (C_DOTAGamerulesProxy), read gamerules ptr."""
        proxy = self._find_entity_by_designer_name("dota_gamerules")
        if not proxy:
            self.gamerules = 0
            return False

        self.gamerules = self.mem.read_ptr(proxy + GamerulesProxy.GAMERULES)
        if self.gamerules:
            print(f"[+] CDOTAGamerules: {hex(self.gamerules)}")
            return True
        return False

    def _find_entity_by_designer_name(self, name: str) -> int | None:
        """Find entity via chunk-based entity system scan (all 3 chunk lists)."""
        for list_off in EntitySystem.CHUNK_LISTS:
            chunk_list = self.mem.read_ptr(self.entity_system + list_off)
            if not chunk_list or chunk_list < 0x10000:
                continue
            for chunk_idx in range(EntitySystem.MAX_CHUNKS):
                try:
                    chunk_ptr = self.mem.read_ptr(chunk_list + chunk_idx * 8)
                except:
                    continue
                if not chunk_ptr or chunk_ptr < 0x10000:
                    continue
                for slot in range(EntitySystem.SLOTS_PER_CHUNK):
                    ident_addr = chunk_ptr + slot * EntityIdentity.STRIDE
                    try:
                        name_ptr = self.mem.read_ptr(ident_addr + EntityIdentity.DESIGNER_NAME)
                        if not name_ptr or name_ptr < 0x10000:
                            continue
                        ent_name = self.mem.read_string(name_ptr, 64)
                        if ent_name == name:
                            return self.mem.read_ptr(ident_addr + EntityIdentity.ENTITY_PTR)
                    except:
                        pass
        return None

    # --- Game State ---

    def get_game_state(self) -> int:
        if not self.gamerules:
            self._find_gamerules()
        if not self.gamerules:
            return -1
        return self.mem.read_i32(self.gamerules + Gamerules.GAME_STATE)

    def get_game_state_name(self) -> str:
        state = self.get_game_state()
        return GameState.NAMES.get(state, f"UNKNOWN({state})")

    def is_in_game(self) -> bool:
        state = self.get_game_state()
        return GameState.HERO_SELECTION <= state <= GameState.GAME_IN_PROGRESS

    def is_in_menu(self) -> bool:
        return self.gamerules == 0 or self.get_game_state() < 0

    def get_game_time(self) -> float:
        """Read m_flGameTime from CDOTAGamerules. Returns Dota clock (negative in pre-game)."""
        if not self.gamerules:
            self._find_gamerules()
        if not self.gamerules:
            return 0.0
        try:
            return self.mem.read_f32(self.gamerules + Gamerules.GAME_TIME)
        except:
            return 0.0

    def get_game_mode(self) -> int:
        if not self.gamerules:
            return -1
        return self.mem.read_i32(self.gamerules + Gamerules.GAME_MODE)

    def is_paused(self) -> bool:
        if not self.gamerules:
            return False
        return self.mem.read_u8(self.gamerules + Gamerules.PAUSED) != 0

    # --- Hero Pick Phase Detection ---

    def get_hero_pick_state(self) -> int:
        """Read m_nHeroPickState from CDOTAGamerules. Returns -1 if offset unknown."""
        if not Gamerules.HERO_PICK_STATE:
            return -1  # offset unknown
        if not self.gamerules:
            self._find_gamerules()
        if not self.gamerules:
            return -1
        return self.mem.read_i32(self.gamerules + Gamerules.HERO_PICK_STATE)

    def check_hero_assigned(self) -> int:
        """Check if local player (slot 0) has an assigned hero.

        Returns: hero handle (>0 = assigned), 0 = not assigned, -1 = controller not found.
        """
        gen = self.iter_entities(max_count=2000)
        try:
            for entity, ident, dname in gen:
                if dname == "dota_player_controller":
                    slot = self.mem.read_i32(entity + PlayerController.PLAYER_SLOT)
                    if slot == 0:
                        handle = self.mem.read_u32(entity + PlayerController.ASSIGNED_HERO)
                        if handle and handle != 0xFFFFFFFF:
                            return handle
                        return 0
        finally:
            gen.close()
        return -1  # controller not found

    _last_hero_pick_state = 0  # track changes for transition detection

    def is_pick_phase(self) -> bool | None:
        """Detect if we're in pick phase (safe to dota_select_hero).

        Returns: True = pick phase, False = ban phase, None = can't determine.

        Heuristic: m_nHeroPickState values are mode-dependent.
        - All Pick lobby: constant 1 (always pick phase, no bans)
        - All Pick ranked: increments through ban→pick rounds
        - Captain's Draft/Mode: 30+ values, increments per round
        We use timing + state change detection rather than absolute thresholds.
        """
        hps = self.get_hero_pick_state()
        if hps < 0:
            return None  # offset unknown

        # Track changes — if state changed, a phase transition happened
        if hps != self._last_hero_pick_state:
            self._last_hero_pick_state = hps

        # All Pick (mode 1, 22): value >= 2 means past ban nomination
        mode = self.get_game_mode()
        if mode in (1, 22):  # All Pick / Ranked All Pick
            return hps >= HeroPickPhase.AP_PICK

        # Other modes: can't determine from value alone, return None for timing fallback
        return None

    # --- Entity Iteration ---

    def iter_entities(self, max_count: int = 10000):
        """Yield (entity_ptr, identity_addr, designer_name) via chunk scan (all lists)."""
        count = 0
        seen = set()
        for list_off in EntitySystem.CHUNK_LISTS:
            chunk_list = self.mem.read_ptr(self.entity_system + list_off)
            if not chunk_list or chunk_list < 0x10000:
                continue
            for chunk_idx in range(EntitySystem.MAX_CHUNKS):
                try:
                    chunk_ptr = self.mem.read_ptr(chunk_list + chunk_idx * 8)
                except Exception:
                    continue
                if not chunk_ptr or chunk_ptr < 0x10000:
                    continue
                for slot in range(EntitySystem.SLOTS_PER_CHUNK):
                    if count >= max_count:
                        return
                    ident_addr = chunk_ptr + slot * EntityIdentity.STRIDE
                    if ident_addr in seen:
                        continue
                    try:
                        name_ptr = self.mem.read_ptr(ident_addr + EntityIdentity.DESIGNER_NAME)
                        if not name_ptr or name_ptr < 0x10000:
                            continue
                        designer_name = self.mem.read_string(name_ptr, 64)
                        if not designer_name or len(designer_name) < 2:
                            continue
                        entity = self.mem.read_ptr(ident_addr + EntityIdentity.ENTITY_PTR)
                        if entity and entity > 0x10000:
                            seen.add(ident_addr)
                            yield entity, ident_addr, designer_name
                            count += 1
                    except GeneratorExit:
                        return
                    except Exception:
                        pass

    def find_all_heroes(self) -> list:
        """Returns [(entity_addr, hero_name), ...] — deduplicated, valid only."""
        heroes = []
        seen = set()
        for entity, ident, name in self.iter_entities():
            if name.startswith("npc_dota_hero_") and "announcer" not in name:
                if entity not in seen:
                    # Filter: must have valid HP and team
                    hp = self.read_health(entity)
                    team = self.read_team(entity)
                    if 0 < hp < 10000 and team in (2, 3):
                        seen.add(entity)
                        heroes.append((entity, name))
        return heroes

    def find_local_hero(self) -> tuple | None:
        """Find local player's hero. Returns (entity_addr, hero_name) or None.

        Strategy:
        1. Find CDOTAPlayerController with slot 0 (local player)
        2. Read m_hAssignedHero CHandle
        3. Resolve via handle_index (chunk scan) or identity->entity_ptr
        4. Fallback: return first hero from find_all_heroes()
        """
        # Step 1: Find local controller (slot 0)
        controller = None
        hero_handle = 0
        gen = self.iter_entities(max_count=2000)
        try:
            for entity, ident, dname in gen:
                if dname == "dota_player_controller":
                    slot = self.mem.read_i32(entity + PlayerController.PLAYER_SLOT)
                    if slot == 0:
                        controller = entity
                        hero_handle = self.mem.read_u32(entity + PlayerController.ASSIGNED_HERO)
                        break
        finally:
            gen.close()

        if hero_handle and hero_handle != 0xFFFFFFFF:
            # Step 2: Try resolving via entity identity
            # The hero's identity stores its handle; scan identities for matching handle
            entity_index = hero_handle & 0x7FFF
            hero_ent = self._resolve_handle_via_identity(entity_index)
            if hero_ent:
                hero_name = self.read_unit_name(hero_ent)
                if hero_name:
                    return (hero_ent, hero_name)

        # Fallback: first hero from chunk scan
        heroes = self.find_all_heroes()
        return heroes[0] if heroes else None

    def _resolve_handle_via_identity(self, entity_index: int) -> int | None:
        """Resolve entity_index by scanning ALL identity regions for matching handle.

        This works even for predicted entities not in standard chunk lists.
        Searches chunk list bases as flat identity arrays.
        """
        target_slot = entity_index & 0x1FF  # slot within chunk (0-511)

        # Try each chunk list as a flat identity region
        for list_off in EntitySystem.CHUNK_LISTS:
            try:
                list_base = self.mem.read_ptr(self.entity_system + list_off)
                if not list_base or list_base < 0x10000:
                    continue
                # Walk chunks: each chunk_list entry at stride 8
                for ci in range(EntitySystem.MAX_CHUNKS):
                    try:
                        chunk_raw = self.mem.read_ptr(list_base + ci * 8)
                    except:
                        continue
                    if not chunk_raw or chunk_raw < 0x100000:
                        continue
                    # Skip if clearly invalid pointer
                    if chunk_raw > 0x7FFF00000000:
                        continue
                    # Try reading identity at target_slot
                    ident_addr = chunk_raw + target_slot * EntityIdentity.STRIDE
                    try:
                        h = self.mem.read_u32(ident_addr + EntityIdentity.HANDLE)
                        if (h & 0x7FFF) == entity_index:
                            ep = self.mem.read_ptr(ident_addr + EntityIdentity.ENTITY_PTR)
                            if ep and ep > 0x10000:
                                return ep
                    except:
                        continue
            except:
                continue

        # Last resort: scan identity memory regions near chunk list bases
        # Some entities (local predicted hero) have identities in regions
        # not indexed by chunk pointers but ARE in the same memory area.
        # Scan a range of identities around the expected position.
        chunk_idx = entity_index >> 9   # which chunk (0-63)
        slot_in_chunk = entity_index & 0x1FF  # 0-511

        for list_off in EntitySystem.CHUNK_LISTS:
            try:
                list_base = self.mem.read_ptr(self.entity_system + list_off)
                if not list_base or list_base < 0x10000:
                    continue
                # Scan a wide memory range in the identity region
                # Predicted entities can be at unexpected offsets
                scan_start = 0
                scan_end = min(0x20000, 512 * 64 * EntityIdentity.STRIDE)  # up to 128KB
                try:
                    data = self.mem.pm.read_bytes(list_base + scan_start, scan_end - scan_start)
                except:
                    continue
                # Search for our handle value in the data
                for off in range(0, len(data) - EntityIdentity.STRIDE, 4):
                    h = struct.unpack("<I", data[off + EntityIdentity.HANDLE:off + EntityIdentity.HANDLE + 4])[0]
                    if (h & 0x7FFF) == entity_index:
                        ep = struct.unpack("<Q", data[off:off + 8])[0]
                        if ep and 0x10000 < ep < 0x7FFF00000000:
                            # Validate: must be aligned and have readable identity backlink
                            if (ep & 0xF) != 0:
                                continue  # not aligned — false positive
                            try:
                                ident_back = self.mem.read_ptr(ep + 0x10)
                                if ident_back and ident_back > 0x10000:
                                    return ep
                            except:
                                continue
            except:
                continue

        return None

    # --- Entity Data ---

    def read_health(self, entity: int) -> int:
        return self.mem.read_i32(entity + BaseEntity.HEALTH)

    def read_team(self, entity: int) -> int:
        return self.mem.read_u8(entity + BaseEntity.TEAM_NUM)

    def read_alive(self, entity: int) -> bool:
        return self.mem.read_u8(entity + BaseEntity.LIFE_STATE) == 0

    def read_position(self, entity: int) -> tuple:
        """Read world position (x, y, z)."""
        scene_node = self.mem.read_ptr(entity + BaseEntity.GAME_SCENE_NODE)
        if not scene_node or scene_node < 0x10000:
            return (0.0, 0.0, 0.0)
        return self.mem.read_vec3(scene_node + GameSceneNode.VEC_ABS_ORIGIN)

    def read_level(self, entity: int) -> int:
        return self.mem.read_i32(entity + BaseNPC.CURRENT_LEVEL)

    def read_mana(self, entity: int) -> tuple:
        """Returns (mana, max_mana)."""
        mana = self.mem.read_f32(entity + BaseNPC.MANA)
        max_mana = self.mem.read_f32(entity + BaseNPC.MAX_MANA)
        return (mana, max_mana)

    def read_unit_name(self, entity: int) -> str:
        """Read m_iszUnitName (CUtlSymbolLarge = char*)."""
        ptr = self.mem.read_ptr(entity + BaseNPC.UNIT_NAME)
        if not ptr or ptr < 0x10000:
            return ""
        try:
            return self.mem.read_string(ptr, 64)
        except:
            return ""

    def read_damage_min(self, entity: int) -> int:
        return self.mem.read_i32(entity + BaseNPC.DAMAGE_MIN)

    def read_damage_max(self, entity: int) -> int:
        return self.mem.read_i32(entity + BaseNPC.DAMAGE_MAX)

    def read_mana_regen(self, entity: int) -> float:
        return self.mem.read_f32(entity + BaseNPC.MANA_REGEN)

    # --- Hero-specific ---

    def read_hero_xp(self, entity: int) -> int:
        return self.mem.read_i32(entity + BaseHero.CURRENT_XP)

    def read_ability_points(self, entity: int) -> int:
        return self.mem.read_i32(entity + BaseHero.ABILITY_POINTS)

    def read_strength(self, entity: int) -> float:
        return self.mem.read_f32(entity + BaseHero.STRENGTH)

    def read_agility(self, entity: int) -> float:
        return self.mem.read_f32(entity + BaseHero.AGILITY)

    def read_intellect(self, entity: int) -> float:
        return self.mem.read_f32(entity + BaseHero.INTELLECT)

    # --- Ability System ---

    def read_ability_handles(self, entity: int) -> list[int]:
        """Read CHandle array from m_vecAbilities (CNetworkUtlVectorBase).

        Layout: entity+ABILITIES_PTR → pointer to CHandle[], entity+ABILITIES_COUNT → count.
        Returns list of CHandle values (0 = empty).
        """
        try:
            vec_ptr = self.mem.read_ptr(entity + BaseNPC.ABILITIES_PTR)
            count = self.mem.read_i32(entity + BaseNPC.ABILITIES_COUNT)
            if not vec_ptr or vec_ptr < 0x10000 or count <= 0 or count > 35:
                return []
            data = self.mem.pm.read_bytes(vec_ptr, count * 4)
            handles = []
            for i in range(count):
                handle = struct.unpack("<I", data[i*4:(i+1)*4])[0]
                handles.append(handle if (handle and handle != 0xFFFFFFFF) else 0)
            return handles
        except:
            return []

    def resolve_handle(self, handle: int, handle_index: dict = None) -> int | None:
        """Resolve a CHandle (entity index + serial) to entity pointer.
        CHandle: bits [0:14] = entity index, [15:31] = serial number.

        Uses handle_index (fast path), then direct index lookup via
        list_base + entity_index * stride (medium path).
        """
        if handle == 0 or handle == 0xFFFFFFFF:
            return None
        entity_index = handle & 0x7FFF  # low 15 bits

        # Fast path: use pre-built index from entity cache
        if handle_index:
            result = handle_index.get(entity_index)
            if result:
                return result

        # Medium path: flat index lookup (list_base + eidx * FLAT_STRIDE)
        # Each entity list is a flat array: identity[eidx] = list_base + eidx * 0x70
        # Verified: boots(eidx=483) and TP(eidx=198) resolve correctly.
        flat_stride = EntityIdentity.FLAT_STRIDE  # 0x70
        for list_off in EntitySystem.CHUNK_LISTS:
            try:
                list_base = self.mem.read_ptr(self.entity_system + list_off)
                if not list_base or list_base < 0x10000:
                    continue
                ident_addr = list_base + entity_index * flat_stride
                # Verify handle matches (lower 32 bits at +0x10)
                h = self.mem.read_u32(ident_addr + EntityIdentity.HANDLE)
                if (h & 0x7FFF) == entity_index:
                    ent_ptr = self.mem.read_ptr(ident_addr + EntityIdentity.ENTITY_PTR)
                    if ent_ptr and 0x10000 < ent_ptr < 0x7FFF00000000 and (ent_ptr & 0x7) == 0:
                        return ent_ptr
            except:
                continue

        # Slow path: identity region scan (for entities not in any list)
        return self._resolve_handle_via_identity(entity_index)

    def read_ability_data(self, ability_entity: int) -> dict | None:
        """Read ability fields from an ability entity pointer."""
        if not ability_entity or ability_entity < 0x10000:
            return None
        # Filter out code/module addresses (0x7FF...) — only heap entities are valid
        if ability_entity > 0x7FF000000000:
            return None
        try:
            data = {}
            data["level"] = self.mem.read_i32(ability_entity + BaseAbility.LEVEL)
            data["cooldown"] = self.mem.read_f32(ability_entity + BaseAbility.COOLDOWN)
            data["cooldown_length"] = self.mem.read_f32(ability_entity + BaseAbility.COOLDOWN_LENGTH)
            data["mana_cost"] = self.mem.read_i32(ability_entity + BaseAbility.MANA_COST)
            data["hidden"] = self.mem.read_u8(ability_entity + BaseAbility.HIDDEN) != 0
            data["activated"] = self.mem.read_u8(ability_entity + BaseAbility.ACTIVATED) != 0
            data["toggle_state"] = self.mem.read_u8(ability_entity + BaseAbility.TOGGLE_STATE) != 0
            data["charges"] = self.mem.read_i32(ability_entity + BaseAbility.CHARGES)
            data["charge_restore"] = self.mem.read_f32(ability_entity + BaseAbility.CHARGE_RESTORE_TIME)
            # Ability identity → designer_name for ability name
            ident = self.mem.read_ptr(ability_entity + 0x10)  # EntityInstance::IDENTITY
            if ident and ident > 0x10000:
                name_ptr = self.mem.read_ptr(ident + EntityIdentity.DESIGNER_NAME)
                if name_ptr and name_ptr > 0x10000:
                    data["name"] = self.mem.read_string(name_ptr, 64)
            return data
        except:
            return None

    def get_hero_abilities(self, hero_entity: int, handle_index: dict = None) -> list[dict]:
        """Read all abilities for a hero. Returns list of ability data dicts.

        Args:
            handle_index: entity_index -> entity_ptr map from EntityCache.
                          Dramatically speeds up CHandle resolution.
        """
        handles = self.read_ability_handles(hero_entity)
        abilities = []
        for i, handle in enumerate(handles):
            if handle == 0:
                continue
            ent_ptr = self.resolve_handle(handle, handle_index)
            if not ent_ptr:
                continue
            ab_data = self.read_ability_data(ent_ptr)
            if ab_data and ab_data.get("name"):
                ab_data["slot"] = i
                ab_data["entity"] = ent_ptr
                ab_data["handle"] = handle
                abilities.append(ab_data)
        return abilities

    # --- Max Health ---

    def read_max_health(self, entity: int) -> int:
        return self.mem.read_i32(entity + BaseEntity.MAX_HEALTH)

    # --- Unit State Flags ---

    def read_unit_state(self, entity: int) -> int:
        """Read m_nUnitState64 (64-bit bitmask of MODIFIER_STATE_* flags)."""
        try:
            data = self.mem.pm.read_bytes(entity + BaseNPC.UNIT_STATE_64, 8)
            return struct.unpack("<Q", data)[0]
        except:
            return 0

    def is_stunned(self, entity: int) -> bool:
        return bool(self.read_unit_state(entity) & UnitState.STUNNED)

    def is_silenced(self, entity: int) -> bool:
        return bool(self.read_unit_state(entity) & UnitState.SILENCED)

    def is_magic_immune(self, entity: int) -> bool:
        return bool(self.read_unit_state(entity) & UnitState.MAGIC_IMMUNE)

    def is_invisible(self, entity: int) -> bool:
        return bool(self.read_unit_state(entity) & UnitState.INVISIBLE)

    def is_invulnerable(self, entity: int) -> bool:
        return bool(self.read_unit_state(entity) & UnitState.INVULNERABLE)

    def is_hexed(self, entity: int) -> bool:
        return bool(self.read_unit_state(entity) & UnitState.HEXED)

    def is_disarmed(self, entity: int) -> bool:
        return bool(self.read_unit_state(entity) & UnitState.DISARMED)

    def is_rooted(self, entity: int) -> bool:
        return bool(self.read_unit_state(entity) & UnitState.ROOTED)

    def is_muted(self, entity: int) -> bool:
        return bool(self.read_unit_state(entity) & UnitState.MUTED)

    # --- Inventory System ---

    def read_item_handles(self, entity: int) -> list[int]:
        """Read CHandle array from m_hItems (inline inside m_Inventory).

        Layout: count at ITEMS_COUNT, inline CHandle[] at ITEMS_INLINE.
        Returns list of CHandle values. Slots: 0-5 main, 6-8 backpack, 9-14 stash, 15+ TP/neutral.
        """
        try:
            count = self.mem.read_i32(entity + BaseNPC.ITEMS_COUNT)
            if count <= 0 or count > 30:
                return []
            data = self.mem.pm.read_bytes(entity + BaseNPC.ITEMS_INLINE, count * 4)
            handles = []
            for i in range(count):
                handle = struct.unpack("<I", data[i*4:(i+1)*4])[0]
                handles.append(handle if (handle and handle != 0xFFFFFFFF) else 0)
            return handles
        except:
            return []

    def read_item_data(self, item_entity: int) -> dict | None:
        """Read item fields from an item entity pointer."""
        if not item_entity or item_entity < 0x10000:
            return None
        try:
            data = {}
            data["level"] = self.mem.read_i32(item_entity + BaseAbility.LEVEL)
            data["cooldown"] = self.mem.read_f32(item_entity + BaseAbility.COOLDOWN)
            data["cooldown_length"] = self.mem.read_f32(item_entity + BaseAbility.COOLDOWN_LENGTH)
            data["charges"] = self.mem.read_i32(item_entity + BaseAbility.CHARGES)
            # Item identity → designer_name for item name
            ident = self.mem.read_ptr(item_entity + 0x10)  # EntityInstance::IDENTITY
            if ident and ident > 0x10000:
                name_ptr = self.mem.read_ptr(ident + EntityIdentity.DESIGNER_NAME)
                if name_ptr and name_ptr > 0x10000:
                    data["name"] = self.mem.read_string(name_ptr, 64)
            return data
        except:
            return None

    def get_hero_items(self, hero_entity: int, handle_index: dict = None) -> list[dict]:
        """Read all inventory items for a hero. Returns list of item data dicts.

        Each dict has: name, slot, level, cooldown, charges, entity.
        """
        handles = self.read_item_handles(hero_entity)
        items = []
        for i, handle in enumerate(handles):
            if handle == 0:
                continue
            ent_ptr = self.resolve_handle(handle, handle_index)
            if not ent_ptr:
                continue
            item_data = self.read_item_data(ent_ptr)
            if item_data and item_data.get("name"):
                item_data["slot"] = i
                item_data["entity"] = ent_ptr
                item_data["handle"] = handle
                items.append(item_data)
        return items

    # --- Gold (via C_DOTA_DataNonSpectator entity) ---

    def find_data_entity(self, team: int) -> int | None:
        """Find C_DOTA_DataNonSpectator entity for a team.
        designer_name: 'dota_data_radiant' (team=2) or 'dota_data_dire' (team=3).
        """
        name = "dota_data_radiant" if team == Team.RADIANT else "dota_data_dire"
        return self._find_entity_by_designer_name(name)

    def read_gold(self, data_entity: int, player_slot: int) -> tuple[int, int]:
        """Read (reliable_gold, unreliable_gold) for a player slot from DataNonSpectator.

        Returns (reliable, unreliable). Total gold = reliable + unreliable.
        """
        try:
            vec_ptr = self.mem.read_ptr(data_entity + DataNonSpectator.DATA_TEAM_PTR)
            count = self.mem.read_i32(data_entity + DataNonSpectator.DATA_TEAM_COUNT)
            if not vec_ptr or vec_ptr < 0x10000 or player_slot < 0 or player_slot >= count:
                return (0, 0)
            player_off = vec_ptr + player_slot * DataNonSpectator.DATA_TEAM_PLAYER_STRIDE
            reliable = self.mem.read_i32(player_off + DataTeamPlayer.RELIABLE_GOLD)
            unreliable = self.mem.read_i32(player_off + DataTeamPlayer.UNRELIABLE_GOLD)
            return (reliable, unreliable)
        except:
            return (0, 0)

    def read_net_worth(self, data_entity: int, player_slot: int) -> int:
        """Read net worth for a player slot from DataNonSpectator."""
        try:
            vec_ptr = self.mem.read_ptr(data_entity + DataNonSpectator.DATA_TEAM_PTR)
            count = self.mem.read_i32(data_entity + DataNonSpectator.DATA_TEAM_COUNT)
            if not vec_ptr or vec_ptr < 0x10000 or player_slot < 0 or player_slot >= count:
                return 0
            player_off = vec_ptr + player_slot * DataNonSpectator.DATA_TEAM_PLAYER_STRIDE
            return self.mem.read_i32(player_off + DataTeamPlayer.NET_WORTH)
        except:
            return 0

    def read_last_hits_denies(self, data_entity: int, player_slot: int) -> tuple[int, int]:
        """Read (last_hits, denies) for a player slot from DataNonSpectator."""
        try:
            vec_ptr = self.mem.read_ptr(data_entity + DataNonSpectator.DATA_TEAM_PTR)
            count = self.mem.read_i32(data_entity + DataNonSpectator.DATA_TEAM_COUNT)
            if not vec_ptr or vec_ptr < 0x10000 or player_slot < 0 or player_slot >= count:
                return (0, 0)
            player_off = vec_ptr + player_slot * DataNonSpectator.DATA_TEAM_PLAYER_STRIDE
            lh = self.mem.read_i32(player_off + DataTeamPlayer.LAST_HITS)
            dn = self.mem.read_i32(player_off + DataTeamPlayer.DENIES)
            return (lh, dn)
        except:
            return (0, 0)

    # --- Summary ---

    def print_hero_info(self, entity: int, name: str):
        """Print all available info about a hero entity."""
        pos = self.read_position(entity)
        hp = self.read_health(entity)
        alive = self.read_alive(entity)
        team = self.read_team(entity)
        level = self.read_level(entity)
        mana, max_mana = self.read_mana(entity)
        unit_name = self.read_unit_name(entity)
        dmg = self.read_damage_min(entity)

        team_name = {Team.RADIANT: "Radiant", Team.DIRE: "Dire"}.get(team, f"Team {team}")

        print(f"  {name}")
        print(f"    Unit: {unit_name}")
        print(f"    HP: {hp}, Alive: {alive}")
        print(f"    Team: {team_name} ({team})")
        print(f"    Position: ({pos[0]:.0f}, {pos[1]:.0f}, {pos[2]:.0f})")
        print(f"    Level: {level}, Mana: {mana:.0f}/{max_mana:.0f}")
        print(f"    Base Damage: {dmg}")

    def close(self):
        self.mem.close()
