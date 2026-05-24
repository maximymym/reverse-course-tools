"""
Dota 2 Bot Brain — runs original Lua bot scripts via lupa,
mocking the Dota 2 Bot API with real game data from memory.

Architecture:
  Python (memory read) → Lua API mock → bot_generic.lua Think() → Action_* → PrepareUnitOrders

Usage:
    from cheat.bot_brain import BotBrain
    brain = BotBrain(mem, game, cmd, scripts_dir="C:/Users/aleks/Downloads/dota2bot-OpenHyperAI")
    brain.init()
    brain.run()  # blocking loop at ~10Hz
"""
import time
import math
import os
import json
import struct
from typing import Optional
from .memory import DotaMemory
from .game_state import DotaGame
from .commands import DotaCommands, UnitOrder, OrderIssuer
from .offsets import (GameState, BaseEntity, GameSceneNode, BaseNPC,
                      EntitySystem, EntityIdentity, Gamerules, BaseAbility, BaseHero,
                      UnitState)
from .game_data import GameDataDB

try:
    from lupa import LuaRuntime
except ImportError:
    raise ImportError("pip install lupa")

from collections import defaultdict


class BotTrace:
    """Per-tick trace collector for debugging bot behavior.
    Enable via BotBrain(..., trace=True). Collects events per tick,
    prints summary at tick end."""

    def __init__(self, enabled=False, print_every=1):
        self.enabled = enabled
        self.print_every = print_every  # print every N ticks (1=all)
        self._events = []
        self._tick = 0

    def event(self, category: str, msg: str):
        if self.enabled:
            self._events.append(f"[{category}] {msg}")

    def tick_done(self):
        self._tick += 1
        if self.enabled and self._events and (self._tick % self.print_every == 0):
            print(f"  === TRACE tick {self._tick} ({len(self._events)} events) ===")
            for e in self._events:
                print(f"    {e}")
        self._events.clear()


class BotLogger:
    """Deduplicating error logger for bot brain. Collects all errors,
    prints first occurrence, and shows summary every N ticks."""

    def __init__(self, summary_interval: int = 50):
        self.summary_interval = summary_interval
        self.errors = defaultdict(int)       # error_key -> count
        self.first_msg = {}                  # error_key -> full message
        self.actions = defaultdict(int)       # action_name -> count
        self.tick = 0
        self._printed_keys = set()

    def error(self, category: str, msg: str, key: str = None):
        """Log an error. key=dedup key (defaults to category+first_line)."""
        if key is None:
            first_line = str(msg).split('\n')[0][:120]
            key = f"{category}:{first_line}"
        self.errors[key] += 1
        if key not in self._printed_keys:
            self._printed_keys.add(key)
            print(f"  [!] {category}: {msg}")
            self.first_msg[key] = f"{category}: {msg}"

    def action(self, name: str):
        """Log a successful action."""
        self.actions[name] += 1

    def tick_done(self):
        """Called after each tick. Prints summary periodically."""
        self.tick += 1
        if self.tick % self.summary_interval == 0:
            self.print_summary()

    def print_summary(self):
        """Print error/action summary."""
        if self.errors:
            # Show top errors by count
            sorted_errs = sorted(self.errors.items(), key=lambda x: -x[1])
            top = sorted_errs[:10]
            err_lines = [f"{k.split(':',1)[-1][:60]}(x{v})" for k, v in top]
            print(f"  [errors] {len(self.errors)} unique, top: {', '.join(err_lines)}")
        if self.actions:
            act_str = ', '.join(f"{k}={v}" for k, v in sorted(self.actions.items()))
            print(f"  [actions] {act_str}")
        # Reset action counts per summary period
        self.actions.clear()


class BotAlgoLog:
    """Structured per-tick algorithm logging.
    Writes JSONL to file + prints one-liner per tick to console.
    Enable via BotBrain(..., algo_log=True) or algo_log="/path/to/file.jsonl".
    """

    MODE_NAMES = {
        0: "NONE", 1: "LANING", 2: "ATTACK", 3: "ROAM", 4: "RETREAT",
        7: "PUSH_TOP", 8: "PUSH_MID", 9: "PUSH_BOT",
        10: "DEF_TOP", 11: "DEF_MID", 12: "DEF_BOT",
        13: "ASSEMBLE", 14: "TEAM_ROAM", 15: "FARM",
        16: "DEF_ALLY", 18: "ROSHAN",
    }

    def __init__(self, filepath="C:/temp/bot_trace.jsonl", enabled=True):
        self.enabled = enabled
        self._tick = 0
        self._events = []  # events this tick
        self._f = None
        if enabled and filepath:
            try:
                self._f = open(filepath, "w", encoding="utf-8")
            except:
                pass

    def event(self, category: str, data):
        """Add structured event for current tick."""
        if not self.enabled:
            return
        self._events.append({"cat": category, "d": data})

    def mode_eval(self, desires):
        """Log mode evaluation results. desires=[(name, float), ...]"""
        if not self.enabled:
            return
        self._events.append({
            "cat": "MODE",
            "d": {n: round(d, 3) for n, d in desires}
        })

    def ability_trace(self, name, desire, motive=""):
        """Log ConsiderX result."""
        if not self.enabled:
            return
        self._events.append({
            "cat": "ABILITY",
            "d": {"fn": name, "desire": desire, "motive": motive}
        })

    def tick_done(self, state_summary=""):
        """Flush tick events to file + print console one-liner."""
        self._tick += 1
        if not self.enabled:
            return

        # Build one-liner for console
        mode_str = ""
        ability_strs = []
        cast_str = ""
        for ev in self._events:
            cat = ev["cat"]
            d = ev["d"]
            if cat == "MODE" and isinstance(d, dict):
                top = sorted(d.items(), key=lambda x: -x[1])[:3]
                mode_str = " ".join(f"{n}={v:.2f}" for n, v in top)
            elif cat == "ABILITY" and isinstance(d, dict) and "fn" in d:
                desire = d.get("desire", 0)
                fn = d["fn"]
                if desire and desire > 0:
                    ability_strs.append(f"{fn}={desire:.2f}({d.get('motive','')})")
                else:
                    ability_strs.append(f"{fn}=0")
            elif cat == "CAST":
                cast_str = str(d)
            elif cat == "OVERRIDE":
                mode_str += f" [->{d}]"

        parts = [f"[T{self._tick}]"]
        if mode_str:
            parts.append(f"MODE: {mode_str}")
        if ability_strs:
            parts.append(" ".join(ability_strs))
        if cast_str:
            parts.append(f"CAST: {cast_str}")
        if state_summary:
            parts.append(state_summary)

        # Print every tick for first 20, then every 5th
        if self._tick <= 20 or self._tick % 5 == 0:
            print("  " + " | ".join(parts))

        # Write JSONL
        if self._f:
            try:
                record = {"tick": self._tick, "events": self._events}
                self._f.write(json.dumps(record, ensure_ascii=False, default=str) + "\n")
                if self._tick % 10 == 0:
                    self._f.flush()
            except:
                pass

        self._events = []

    def close(self):
        if self._f:
            self._f.close()
            self._f = None


# ─── Constants matching Dota 2 Bot API ─────────────────────
TEAM_RADIANT = 2
TEAM_DIRE = 3
TEAM_NONE = 0

# BOT_MODE constants
BOT_MODE_NONE = 0
BOT_MODE_LANING = 1
BOT_MODE_ATTACK = 2
BOT_MODE_ROAM = 3
BOT_MODE_RETREAT = 4
BOT_MODE_SECRET_SHOP = 5
BOT_MODE_SIDE_SHOP = 6
BOT_MODE_PUSH_TOWER_TOP = 7
BOT_MODE_PUSH_TOWER_MID = 8
BOT_MODE_PUSH_TOWER_BOT = 9
BOT_MODE_DEFEND_TOWER_TOP = 10
BOT_MODE_DEFEND_TOWER_MID = 11
BOT_MODE_DEFEND_TOWER_BOT = 12
BOT_MODE_ASSEMBLE = 13
BOT_MODE_TEAM_ROAM = 14
BOT_MODE_FARM = 15
BOT_MODE_DEFEND_ALLY = 16
BOT_MODE_EVASIVE_MANEUVERS = 17
BOT_MODE_ROSHAN = 18
BOT_MODE_ITEM = 19
BOT_MODE_WARD = 20
BOT_MODE_COMPANION = 21

# LANE constants
LANE_NONE = 0
LANE_TOP = 1
LANE_MID = 2
LANE_BOT = 3

# DIFFICULTY
DIFFICULTY_EASY = 0
DIFFICULTY_MEDIUM = 1
DIFFICULTY_HARD = 2
DIFFICULTY_UNFAIR = 3

# UNIT_LIST
UNIT_LIST_ALLIED_HEROES = 1
UNIT_LIST_ENEMY_HEROES = 2
UNIT_LIST_ALLIED_CREEPS = 3
UNIT_LIST_ENEMY_CREEPS = 4
UNIT_LIST_ALLIED_BUILDINGS = 5
UNIT_LIST_ENEMY_BUILDINGS = 6

# Fountain positions
FOUNTAIN_RADIANT = (-7200, -6700, 128)
FOUNTAIN_DIRE = (7200, 6700, 128)

# Lane front positions (approximate mid-game defaults)
LANE_WAYPOINTS = {
    LANE_TOP: [(-6200, -3600), (-6200, 2000), (-6200, 5800), (-3200, 6000), (0, 6000), (3200, 6000), (6000, 6000)],
    LANE_MID: [(-5000, -4800), (-3000, -3000), (0, 0), (3000, 3000), (5000, 4800)],
    LANE_BOT: [(-6000, -6000), (-3200, -6000), (0, -6000), (3200, -6000), (6200, -5800), (6200, -2000), (6200, 3600)],
}


class EntityCache:
    """Cached entity data refreshed each tick."""

    def __init__(self):
        self.heroes = []      # [{entity, name, hp, max_hp, team, x, y, z, level, alive}]
        self.creeps = []      # [{entity, name, hp, team, x, y, z, is_lane}]
        self.towers = []      # [{entity, name, hp, team, x, y, z}]
        self.buildings = []
        self.handle_index = {}  # entity_index -> entity_ptr (built during scan)
        self.game_time = 0.0
        self.game_state = 0
        self.last_update = 0
        self.data_radiant = 0  # C_DOTA_DataNonSpectator entity ptr
        self.data_dire = 0
        # HP damage tracking: entity_ptr -> {last_hp, damage_time}
        self._hp_track = {}    # entity_ptr -> last known HP
        self._damage_time = {} # entity_ptr -> game_time when HP decreased

    def update(self, game: DotaGame, mem: DotaMemory):
        """Refresh all entity data from memory."""
        self.heroes.clear()
        self.creeps.clear()
        self.towers.clear()
        self.buildings.clear()

        # Game time from gamerules
        if game.gamerules:
            try:
                self.game_state = mem.read_i32(game.gamerules + Gamerules.GAME_STATE)
                gt = mem.read_f32(game.gamerules + Gamerules.GAME_TIME)
                # Dota game time: negative in pre-game (-90..-0), 0+ in game
                # gt == 0.0 means "not replicated yet" (server-side only)
                if gt != 0.0 and -200 < gt < 10000:
                    self.game_time = gt
                # else: keep previous game_time (don't overwrite with garbage)
            except:
                pass  # keep previous game_time
        # game_time stays 0.0 until first valid read

        # Scan entities + build handle index
        seen = set()
        self.handle_index.clear()
        for list_off in EntitySystem.CHUNK_LISTS:
            try:
                chunk_list = mem.read_ptr(game.entity_system + list_off)
            except:
                continue
            if not chunk_list or chunk_list < 0x10000:
                continue

            # Phase A: standard chunk scan (valid chunk pointers)
            for ci in range(EntitySystem.MAX_CHUNKS):
                try:
                    chunk_ptr = mem.read_ptr(chunk_list + ci * 8)
                except:
                    continue
                if not chunk_ptr or chunk_ptr < 0x100000:
                    continue
                if chunk_ptr > 0x7FFF00000000 or (chunk_ptr >> 32) == 0xFFFFFFFF:
                    continue  # sentinel chunk — handled in Phase B
                try:
                    chunk_data = mem.pm.read_bytes(chunk_ptr, EntitySystem.SLOTS_PER_CHUNK * EntityIdentity.STRIDE)
                except:
                    continue
                for si in range(EntitySystem.SLOTS_PER_CHUNK):
                    off = si * EntityIdentity.STRIDE
                    ent_ptr = struct.unpack("<Q", chunk_data[off:off+8])[0]
                    if not ent_ptr or ent_ptr < 0x10000:
                        continue
                    # Validate entity pointer: must be aligned and in heap range
                    # Garbage pointers like 0x51300000509 fail alignment check
                    if (ent_ptr & 0xFF) != 0 or ent_ptr > 0x7FFF00000000:
                        continue
                    handle_val = struct.unpack("<I", chunk_data[off+EntityIdentity.HANDLE:off+EntityIdentity.HANDLE+4])[0]
                    if handle_val and handle_val != 0xFFFFFFFF:
                        eidx = handle_val & 0x7FFF
                        if eidx not in self.handle_index:
                            self.handle_index[eidx] = ent_ptr
                    if ent_ptr in seen:
                        continue
                    seen.add(ent_ptr)
                    self._classify_entity(mem, ent_ptr)

            # Phase B: identity region scan for entities in sentinel chunks
            # Some entities (local hero, predicted) live in identity memory regions
            # but their chunk pointers are sentinel (0xFFFFFFFF...).
            # Scan 128KB from chunk_list base for additional identities.
            try:
                region_data = mem.pm.read_bytes(chunk_list, 0x20000)
                for off in range(0, len(region_data) - EntityIdentity.STRIDE, 4):
                    handle_val = struct.unpack("<I", region_data[off+EntityIdentity.HANDLE:off+EntityIdentity.HANDLE+4])[0]
                    if not handle_val or handle_val == 0xFFFFFFFF:
                        continue
                    eidx = handle_val & 0x7FFF
                    if eidx in self.handle_index:
                        continue  # already found in Phase A
                    ent_ptr = struct.unpack("<Q", region_data[off:off+8])[0]
                    if not ent_ptr or ent_ptr < 0x100000 or ent_ptr > 0x7FFF00000000:
                        continue
                    # Validate: entity ptr must be readable (quick 8-byte test)
                    try:
                        mem.pm.read_bytes(ent_ptr, 8)
                    except:
                        continue
                    self.handle_index[eidx] = ent_ptr
                    if ent_ptr not in seen:
                        seen.add(ent_ptr)
                        self._classify_entity(mem, ent_ptr)
            except:
                pass

        # Find gold data entities (C_DOTA_DataNonSpectator)
        self.data_radiant = 0
        self.data_dire = 0
        gen = game.iter_entities(max_count=200)
        try:
            for ent_ptr, ident, dname in gen:
                if dname == "dota_data_radiant":
                    self.data_radiant = ent_ptr
                elif dname == "dota_data_dire":
                    self.data_dire = ent_ptr
                if self.data_radiant and self.data_dire:
                    break
        finally:
            gen.close()

        # Add local hero to cache if not already found via chunk scan
        # Local hero can be in a sentinel chunk, only findable via identity region scan
        hero_result = game.find_local_hero()
        if hero_result:
            hero_ent, hero_name = hero_result
            already_in = any(h["entity"] == hero_ent for h in self.heroes)
            if not already_in:
                try:
                    hp = mem.read_i32(hero_ent + BaseEntity.HEALTH)
                    team = mem.read_u8(hero_ent + BaseEntity.TEAM_NUM)
                    if hp > 0 and hp <= 50000 and team in (2, 3):
                        scene = mem.read_ptr(hero_ent + BaseEntity.GAME_SCENE_NODE)
                        x = mem.read_f32(scene + GameSceneNode.VEC_ABS_ORIGIN) if scene else 0.0
                        y = mem.read_f32(scene + GameSceneNode.VEC_ABS_ORIGIN + 4) if scene else 0.0
                        z = mem.read_f32(scene + GameSceneNode.VEC_ABS_ORIGIN + 8) if scene else 0.0
                        max_hp = mem.read_i32(hero_ent + BaseEntity.MAX_HEALTH)
                        if max_hp <= 0 or max_hp > 50000:
                            max_hp = hp
                        level = mem.read_i32(hero_ent + BaseNPC.CURRENT_LEVEL)
                        if level < 1 or level > 30:
                            level = 1
                        entry = {
                            "entity": hero_ent, "name": hero_name, "hp": hp, "team": team,
                            "x": x, "y": y, "z": z, "max_hp": max_hp, "level": level,
                            "alive": hp > 0,
                        }
                        try:
                            entry["mana"] = mem.read_f32(hero_ent + BaseNPC.MANA)
                            entry["max_mana"] = mem.read_f32(hero_ent + BaseNPC.MAX_MANA)
                            entry["mana_regen"] = mem.read_f32(hero_ent + BaseNPC.MANA_REGEN)
                            entry["health_regen"] = mem.read_f32(hero_ent + BaseNPC.HEALTH_REGEN)
                            entry["damage_min"] = mem.read_i32(hero_ent + BaseNPC.DAMAGE_MIN)
                            entry["damage_max"] = mem.read_i32(hero_ent + BaseNPC.DAMAGE_MAX)
                            entry["move_speed"] = mem.read_i32(hero_ent + BaseNPC.MOVE_SPEED)
                            entry["attack_range"] = mem.read_i32(hero_ent + BaseNPC.ATTACK_RANGE)
                            entry["physical_armor"] = mem.read_f32(hero_ent + BaseNPC.PHYSICAL_ARMOR)
                            entry["magic_resist"] = mem.read_f32(hero_ent + BaseNPC.MAGIC_RESIST)
                            entry["day_vision"] = mem.read_i32(hero_ent + BaseNPC.DAY_VISION_RANGE)
                            entry["night_vision"] = mem.read_i32(hero_ent + BaseNPC.NIGHT_VISION_RANGE)
                            entry["is_illusion"] = bool(mem.read_u8(hero_ent + BaseNPC.IS_ILLUSION))
                        except:
                            entry.setdefault("mana", 0.0)
                            entry.setdefault("max_mana", 0.0)
                            entry.setdefault("mana_regen", 0.0)
                            entry.setdefault("health_regen", 0.0)
                            entry.setdefault("damage_min", 0)
                        try:
                            state_data = mem.pm.read_bytes(hero_ent + BaseNPC.UNIT_STATE_64, 8)
                            entry["unit_state"] = struct.unpack("<Q", state_data)[0]
                        except:
                            entry["unit_state"] = 0
                        self.heroes.append(entry)
                except:
                    pass

        # Track HP deltas for WasRecentlyDamaged detection
        for h in self.heroes:
            ent = h["entity"]
            hp = h.get("hp", 0)
            prev_hp = self._hp_track.get(ent, hp)
            if hp < prev_hp and prev_hp - hp >= 3:  # took at least 3 damage (filter regen noise)
                self._damage_time[ent] = self.game_time
            self._hp_track[ent] = hp

        self.last_update = time.time()

    def was_recently_damaged(self, entity_ptr, threshold=2.0):
        """Check if entity took damage within threshold seconds."""
        dmg_time = self._damage_time.get(entity_ptr, -999)
        return (self.game_time - dmg_time) < threshold

    def time_since_damaged(self, entity_ptr):
        """Seconds since entity last took damage."""
        dmg_time = self._damage_time.get(entity_ptr, -999)
        if dmg_time < -900:
            return 999.0
        return max(0.0, self.game_time - dmg_time)

    def _classify_entity(self, mem, ent_ptr):
        try:
            hp = mem.read_i32(ent_ptr + BaseEntity.HEALTH)
            team = mem.read_u8(ent_ptr + BaseEntity.TEAM_NUM)
            if team not in (2, 3):
                return
            if hp <= 0 or hp > 50000:
                return

            uname_ptr = mem.read_ptr(ent_ptr + BaseNPC.UNIT_NAME)
            if not uname_ptr or uname_ptr < 0x10000:
                return
            uname = mem.read_string(uname_ptr, 64)
            if not uname or len(uname) < 4:
                return

            scene = mem.read_ptr(ent_ptr + BaseEntity.GAME_SCENE_NODE)
            if not scene or scene < 0x10000:
                return
            x = mem.read_f32(scene + GameSceneNode.VEC_ABS_ORIGIN)
            y = mem.read_f32(scene + GameSceneNode.VEC_ABS_ORIGIN + 4)
            z = mem.read_f32(scene + GameSceneNode.VEC_ABS_ORIGIN + 8)

            entry = {
                "entity": ent_ptr, "name": uname, "hp": hp, "team": team,
                "x": x, "y": y, "z": z,
            }

            if "hero" in uname and "announcer" not in uname and "selection" not in uname:
                try:
                    max_hp = mem.read_i32(ent_ptr + BaseEntity.MAX_HEALTH)
                    if max_hp <= 0 or max_hp > 50000:
                        max_hp = hp
                except:
                    max_hp = hp
                level = 1
                try:
                    level = mem.read_i32(ent_ptr + BaseNPC.CURRENT_LEVEL)
                    if level < 1 or level > 30:
                        level = 1
                except:
                    pass
                entry["max_hp"] = max_hp
                entry["level"] = level
                entry["alive"] = hp > 0
                try:
                    entry["mana"] = mem.read_f32(ent_ptr + BaseNPC.MANA)
                    entry["max_mana"] = mem.read_f32(ent_ptr + BaseNPC.MAX_MANA)
                    entry["mana_regen"] = mem.read_f32(ent_ptr + BaseNPC.MANA_REGEN)
                    entry["health_regen"] = mem.read_f32(ent_ptr + BaseNPC.HEALTH_REGEN)
                    entry["damage_min"] = mem.read_i32(ent_ptr + BaseNPC.DAMAGE_MIN)
                    entry["damage_max"] = mem.read_i32(ent_ptr + BaseNPC.DAMAGE_MAX)
                    entry["move_speed"] = mem.read_i32(ent_ptr + BaseNPC.MOVE_SPEED)
                    entry["attack_range"] = mem.read_i32(ent_ptr + BaseNPC.ATTACK_RANGE)
                    entry["physical_armor"] = mem.read_f32(ent_ptr + BaseNPC.PHYSICAL_ARMOR)
                    entry["magic_resist"] = mem.read_f32(ent_ptr + BaseNPC.MAGIC_RESIST)
                    entry["day_vision"] = mem.read_i32(ent_ptr + BaseNPC.DAY_VISION_RANGE)
                    entry["night_vision"] = mem.read_i32(ent_ptr + BaseNPC.NIGHT_VISION_RANGE)
                    entry["is_illusion"] = bool(mem.read_u8(ent_ptr + BaseNPC.IS_ILLUSION))
                except:
                    entry.setdefault("mana", 0.0)
                    entry.setdefault("max_mana", 0.0)
                    entry.setdefault("mana_regen", 0.0)
                    entry.setdefault("health_regen", 0.0)
                    entry.setdefault("damage_min", 0)
                # Unit state flags (64-bit bitmask)
                try:
                    state_data = mem.pm.read_bytes(ent_ptr + BaseNPC.UNIT_STATE_64, 8)
                    entry["unit_state"] = struct.unpack("<Q", state_data)[0]
                except:
                    entry["unit_state"] = 0
                self.heroes.append(entry)

            elif "creep" in uname:
                entry["is_lane"] = "lane" in uname
                self.creeps.append(entry)

            elif "tower" in uname:
                self.towers.append(entry)

            elif "rax" in uname or "fort" in uname or "filler" in uname:
                self.buildings.append(entry)

        except:
            pass


class BotBrain:
    """Runs Lua bot scripts with mocked Dota 2 API, reading game state from memory."""

    def __init__(self, mem: DotaMemory, game: DotaGame, cmd: DotaCommands,
                 scripts_dir: str = None, hero_name: str = None, trace: bool = False,
                 algo_log: bool = False):
        self.mem = mem
        self.game = game
        self.cmd = cmd
        self.scripts_dir = scripts_dir or "C:/Users/aleks/Downloads/dota2bot-OpenHyperAI"
        self.hero_name = hero_name  # auto-detect if None
        self.cache = EntityCache()
        self.db = GameDataDB()
        self.log = BotLogger(summary_interval=50)
        self.trace = BotTrace(enabled=trace)
        self.algo_log = BotAlgoLog(enabled=algo_log)
        self.lua = None
        self.bot_think_fn = None
        self._start_time = 0
        self._our_hero_entity = 0
        self._our_team = TEAM_RADIANT
        self._assigned_lane = LANE_MID
        self._active_mode = BOT_MODE_LANING
        self._active_mode_desire = 0.5
        self._target = None
        self._last_action = ""
        self._last_action_time = 0
        self._ability_cache = {}   # entity_ptr -> [ability_data, ...]
        self._ability_cache_time = 0
        self._item_cache = {}      # entity_ptr -> [item_data, ...]
        self._item_cache_time = 0
        self._player_slot = 0      # our player slot index (0 in most cases)

    def init(self) -> bool:
        """Initialize Lua runtime, register API, load bot scripts."""
        # Refresh game state
        self.game.init()
        self.cache.update(self.game, self.mem)
        self._start_time = time.time()

        # Find our hero
        if not self._find_our_hero():
            print("[!] Could not find our hero")
            return False

        print(f"[+] Bot Brain: hero={self.hero_name}, team={self._our_team}")

        # Fix local controller to match OUR hero (not hardcoded slot 0)
        if self._our_hero_entity and self.cmd:
            self.cmd._find_local_controller(self.game, hero_entity=self._our_hero_entity)

        # Create Lua runtime
        self.lua = LuaRuntime(unpack_returned_tuples=True)

        # Register all API
        self._register_globals()
        self._register_constants()

        # Load bot scripts
        ok = self._load_scripts()
        if ok:
            self._inject_algo_trace()
        return ok

    def _find_our_hero(self) -> bool:
        """Find the local player's hero from entity cache."""
        # Debug: show available heroes
        if not hasattr(self, '_hero_debug_done'):
            print(f"  [debug] Heroes in cache: {[h['name'] for h in self.cache.heroes]}")
            self._hero_debug_done = True

        old_entity = self._our_hero_entity

        # Match by hero_name (substring match)
        if self.hero_name:
            for h in self.cache.heroes:
                if self.hero_name in h["name"] or h["name"] in self.hero_name:
                    self._our_hero_entity = h["entity"]
                    self._our_team = h["team"]
                    break
            else:
                # Auto-detect: take first hero
                if self.cache.heroes:
                    h = self.cache.heroes[0]
                    self._our_hero_entity = h["entity"]
                    self._our_team = h["team"]
                    if not self.hero_name:
                        self.hero_name = h["name"]
                else:
                    return False
        elif self.cache.heroes:
            h = self.cache.heroes[0]
            self._our_hero_entity = h["entity"]
            self._our_team = h["team"]
            if not self.hero_name:
                self.hero_name = h["name"]
        else:
            return False

        # Invalidate cached bot handle if entity changed
        if self._our_hero_entity != old_entity and hasattr(self, '_cached_bot_handle'):
            self._cached_bot_handle = None

        return True

    def _register_constants(self):
        """Register Dota 2 constants in Lua."""
        g = self.lua.globals()
        # Teams
        g.TEAM_RADIANT = TEAM_RADIANT
        g.TEAM_DIRE = TEAM_DIRE
        g.TEAM_NONE = TEAM_NONE
        g.TEAM_NEUTRAL = 4

        # Bot modes
        for name, val in [
            ("BOT_MODE_NONE", 0), ("BOT_MODE_LANING", 1), ("BOT_MODE_ATTACK", 2),
            ("BOT_MODE_ROAM", 3), ("BOT_MODE_RETREAT", 4), ("BOT_MODE_SECRET_SHOP", 5),
            ("BOT_MODE_SIDE_SHOP", 6), ("BOT_MODE_PUSH_TOWER_TOP", 7),
            ("BOT_MODE_PUSH_TOWER_MID", 8), ("BOT_MODE_PUSH_TOWER_BOT", 9),
            ("BOT_MODE_DEFEND_TOWER_TOP", 10), ("BOT_MODE_DEFEND_TOWER_MID", 11),
            ("BOT_MODE_DEFEND_TOWER_BOT", 12), ("BOT_MODE_ASSEMBLE", 13),
            ("BOT_MODE_TEAM_ROAM", 14), ("BOT_MODE_FARM", 15),
            ("BOT_MODE_DEFEND_ALLY", 16), ("BOT_MODE_EVASIVE_MANEUVERS", 17),
            ("BOT_MODE_ROSHAN", 18), ("BOT_MODE_ITEM", 19), ("BOT_MODE_WARD", 20),
        ]:
            setattr(g, name, val)

        # Lanes
        g.LANE_NONE = 0; g.LANE_TOP = 1; g.LANE_MID = 2; g.LANE_BOT = 3

        # Difficulties
        g.DIFFICULTY_EASY = 0; g.DIFFICULTY_MEDIUM = 1
        g.DIFFICULTY_HARD = 2; g.DIFFICULTY_UNFAIR = 3

        # BOT_MODE_DESIRE_* (used for mode desire return values)
        g.BOT_MODE_DESIRE_NONE = 0.0
        g.BOT_MODE_DESIRE_VERYLOW = 0.1
        g.BOT_MODE_DESIRE_LOW = 0.25
        g.BOT_MODE_DESIRE_MODERATE = 0.5
        g.BOT_MODE_DESIRE_HIGH = 0.75
        g.BOT_MODE_DESIRE_VERYHIGH = 0.9
        g.BOT_MODE_DESIRE_ABSOLUTE = 1.0

        # BOT_ACTION_DESIRE_* (same values, different name prefix used by some scripts)
        g.BOT_ACTION_DESIRE_NONE = 0.0
        g.BOT_ACTION_DESIRE_VERYLOW = 0.1
        g.BOT_ACTION_DESIRE_LOW = 0.25
        g.BOT_ACTION_DESIRE_MODERATE = 0.5
        g.BOT_ACTION_DESIRE_HIGH = 0.75
        g.BOT_ACTION_DESIRE_VERYHIGH = 0.9
        g.BOT_ACTION_DESIRE_ABSOLUTE = 1.0

        # Unit lists
        g.UNIT_LIST_ALLIED_HEROES = 1; g.UNIT_LIST_ENEMY_HEROES = 2
        g.UNIT_LIST_ALLIED_CREEPS = 3; g.UNIT_LIST_ENEMY_CREEPS = 4
        g.UNIT_LIST_ALLIED_BUILDINGS = 5; g.UNIT_LIST_ENEMY_BUILDINGS = 6
        g.UNIT_LIST_ALLIED_WARDS = 7; g.UNIT_LIST_ENEMY_WARDS = 8

        # Ability behavior flags
        g.ABILITY_BEHAVIOR_NONE = 0
        g.ABILITY_BEHAVIOR_PASSIVE = 1
        g.ABILITY_BEHAVIOR_UNIT_TARGET = 8
        g.ABILITY_BEHAVIOR_POINT = 16
        g.ABILITY_BEHAVIOR_NO_TARGET = 4
        g.ABILITY_BEHAVIOR_CHANNELLED = 128
        g.ABILITY_BEHAVIOR_TOGGLE = 2
        g.ABILITY_BEHAVIOR_AOE = 32
        g.ABILITY_BEHAVIOR_HIDDEN = 64
        g.DOTA_ABILITY_BEHAVIOR_NOT_LEARNABLE = 64  # used by aba_skill.lua

        # String constants
        g.generic_hidden = "generic_hidden"

        # Damage types
        g.DAMAGE_TYPE_PHYSICAL = 1
        g.DAMAGE_TYPE_MAGICAL = 2
        g.DAMAGE_TYPE_PURE = 4
        g.DAMAGE_TYPE_ALL = 7

        # Game states
        g.GAME_STATE_INIT = 0
        g.GAME_STATE_HERO_SELECTION = 2
        g.GAME_STATE_STRATEGY_TIME = 3
        g.GAME_STATE_PRE_GAME = 4
        g.GAME_STATE_GAME_IN_PROGRESS = 5
        g.GAME_STATE_POST_GAME = 6

        # Game modes
        g.GAMEMODE_AP = 1; g.GAMEMODE_CM = 2; g.GAMEMODE_RD = 3
        g.GAMEMODE_SD = 4; g.GAMEMODE_AR = 5; g.GAMEMODE_MO = 15
        g.GAMEMODE_TURBO = 23

        # Runes
        g.RUNE_DOUBLEDAMAGE = 0; g.RUNE_HASTE = 1; g.RUNE_ILLUSION = 2
        g.RUNE_INVISIBILITY = 3; g.RUNE_REGENERATION = 4; g.RUNE_BOUNTY = 5
        g.RUNE_ARCANE = 6; g.RUNE_WATER = 7
        g.RUNE_STATUS_UNKNOWN = 0; g.RUNE_STATUS_AVAILABLE = 1; g.RUNE_STATUS_MISSING = 2

        # Purchase results
        g.PURCHASE_ITEM_SUCCESS = 0
        g.PURCHASE_ITEM_OUT_OF_STOCK = 1
        g.PURCHASE_ITEM_NOT_AT_SECRET_SHOP = 2
        g.PURCHASE_ITEM_NOT_AT_SIDE_SHOP = 3
        g.PURCHASE_ITEM_INSUFFICIENT_GOLD = 4
        g.PURCHASE_ITEM_NOT_AT_HOME_SHOP = 5

        # Item slot types
        g.ITEM_SLOT_TYPE_MAIN = 0
        g.ITEM_SLOT_TYPE_BACKPACK = 1
        g.ITEM_SLOT_TYPE_STASH = 2
        g.ITEM_SLOT_TYPE_INVALID = -1

        # Action types
        g.BOT_ACTION_TYPE_IDLE = 0
        g.BOT_ACTION_TYPE_ATTACK = 1
        g.BOT_ACTION_TYPE_MOVE_TO = 2
        g.BOT_ACTION_TYPE_CAST_ABILITY = 3
        g.BOT_ACTION_TYPE_PICK_UP_RUNE = 4
        g.BOT_ACTION_TYPE_PICK_UP_ITEM = 5
        g.BOT_ACTION_TYPE_USE_SHRINE = 6

        # Misc
        g.DOTA_ModifyXP_Unspecified = 0

    def _register_globals(self):
        """Register global functions in Lua."""
        g = self.lua.globals()
        brain = self  # capture for closures

        # ─── GetBot() ─────────────────────────────────────
        g.GetBot = lambda: brain._make_bot_handle()

        # ─── Time ─────────────────────────────────────────
        # game_time from Gamerules: negative in pre-game (-90..), 0 at horn, increasing in game.
        # 0.0 = not replicated yet → fallback to elapsed time since bot start.
        def _dota_time():
            gt = brain.cache.game_time
            if gt != 0.0 and -200 < gt < 10000:
                return gt
            return time.time() - brain._start_time
        g.DotaTime = _dota_time
        g.GameTime = _dota_time
        g.RealTime = lambda: time.time()

        # ─── Game State ───────────────────────────────────
        g.GetGameState = lambda: brain.cache.game_state
        g.GetGameMode = lambda: 1  # All Pick

        # ─── Teams ────────────────────────────────────────
        g.GetTeam = lambda: brain._our_team
        g.GetEnemyTeam = lambda: TEAM_DIRE if brain._our_team == TEAM_RADIANT else TEAM_RADIANT

        # ─── Team Players ─────────────────────────────────
        def get_team_players(team):
            t = self.lua.table_from([0, 1, 2, 3, 4])
            return t
        g.GetTeamPlayers = get_team_players
        g.GetTeamMember = lambda pid: brain._make_unit_handle_by_pid(pid)
        g.IsPlayerBot = lambda pid: pid != 0  # slot 0 is "us" (controlled by brain)
        g.IsHeroAlive = lambda pid: True  # TODO
        g.GetSelectedHeroName = lambda pid: brain.hero_name or ""
        g.GetHeroName = lambda pid: brain.hero_name or ""

        # ─── Distance ─────────────────────────────────────
        def get_unit_distance(u1, u2):
            try:
                loc1 = u1.GetLocation()
                loc2 = u2.GetLocation()
                dx = loc1.x - loc2.x
                dy = loc1.y - loc2.y
                return math.sqrt(dx*dx + dy*dy)
            except:
                return 99999
        g.GetUnitToUnitDistance = get_unit_distance

        def get_unit_loc_distance(u, loc):
            try:
                ul = u.GetLocation()
                dx = ul.x - loc.x
                dy = ul.y - loc.y
                return math.sqrt(dx*dx + dy*dy)
            except:
                return 99999
        g.GetUnitToLocationDistance = get_unit_loc_distance

        def get_loc_loc_distance(loc1, loc2):
            try:
                dx = loc1.x - loc2.x
                dy = loc1.y - loc2.y
                return math.sqrt(dx*dx + dy*dy)
            except:
                return 99999
        g.GetLocationToLocationDistance = get_loc_loc_distance

        # ─── Map (dynamic lane front from creep positions) ─────
        # Lane bounding boxes: creeps within these regions belong to that lane
        _LANE_BOUNDS = {
            LANE_TOP: (-7500, -1000, -1000, 7500),  # x_min, y_min, x_max, y_max
            LANE_MID: (-6000, -6000, 6000, 6000),    # diagonal — use distance to line
            LANE_BOT: (-1000, -7500, 7500, 1000),
        }

        def _classify_lane(cx, cy):
            """Classify a creep position to a lane."""
            # Distance to mid diagonal (y = x)
            dist_mid = abs(cy - cx) / 1.414
            # Distance to top path (x ~ -6200 or y ~ 6000)
            dist_top = min(abs(cx + 6200), abs(cy - 6000))
            # Distance to bot path (y ~ -6000 or x ~ 6200)
            dist_bot = min(abs(cy + 6000), abs(cx - 6200))
            best = min(dist_mid, dist_top, dist_bot)
            if best == dist_mid:
                return LANE_MID
            elif best == dist_top:
                return LANE_TOP
            else:
                return LANE_BOT

        def get_lane_front(team, lane, offset=0):
            """Dynamic lane front: average position of allied creeps on this lane.
            Falls back to waypoints if no creeps visible."""
            team = int(team)
            lane = int(lane) if lane else LANE_MID
            offset = float(offset) if offset else 0.0

            # Find allied creeps on this lane
            creep_positions = []
            for c in brain.cache.creeps:
                if c["team"] != team:
                    continue
                cl = _classify_lane(c["x"], c["y"])
                if cl == lane:
                    creep_positions.append((c["x"], c["y"]))

            if creep_positions:
                # Average position of allied creeps = lane front
                avg_x = sum(p[0] for p in creep_positions) / len(creep_positions)
                avg_y = sum(p[1] for p in creep_positions) / len(creep_positions)
                # Apply offset along lane direction (positive = towards enemy)
                if offset != 0:
                    wps = LANE_WAYPOINTS.get(lane, LANE_WAYPOINTS[LANE_MID])
                    # Direction from Radiant to Dire along lane
                    dx = wps[-1][0] - wps[0][0]
                    dy = wps[-1][1] - wps[0][1]
                    l = math.sqrt(dx*dx + dy*dy)
                    if l > 0:
                        # For Dire team, offset is reversed
                        sign = 1 if team == TEAM_RADIANT else -1
                        avg_x += sign * (dx / l) * offset
                        avg_y += sign * (dy / l) * offset
                return brain._make_vector(avg_x, avg_y, 128)

            # No creeps visible — use static waypoints based on game time
            wps = LANE_WAYPOINTS.get(lane, LANE_WAYPOINTS[LANE_MID])
            gt = time.time() - brain._start_time
            # Early game: front is near our T1. Progress towards mid over time.
            progress = min(gt / 300.0, 0.5)  # 0→0.5 over 5 minutes
            if team == TEAM_DIRE:
                progress = 1.0 - progress
            idx = int(progress * (len(wps) - 1))
            idx = max(0, min(idx, len(wps) - 1))
            return brain._make_vector(wps[idx][0], wps[idx][1], 128)
        g.GetLaneFrontLocation = get_lane_front

        def _get_lane_front_amount(team, lane, ignore=False):
            """How far along the lane the front is (0=our base, 1=enemy base)."""
            team = int(team) if team else brain._our_team
            lane = int(lane) if lane else LANE_MID
            # Find allied creeps on lane
            for c in brain.cache.creeps:
                if c["team"] != team:
                    continue
                cl = _classify_lane(c["x"], c["y"])
                if cl == lane:
                    # Project creep position onto lane waypoints
                    wps = LANE_WAYPOINTS.get(lane, LANE_WAYPOINTS[LANE_MID])
                    total_len = 0
                    proj_len = 0
                    best_dist = 99999
                    for i in range(len(wps) - 1):
                        seg_dx = wps[i+1][0] - wps[i][0]
                        seg_dy = wps[i+1][1] - wps[i][1]
                        seg_len = math.sqrt(seg_dx**2 + seg_dy**2)
                        # Distance from creep to this segment midpoint
                        mid_x = (wps[i][0] + wps[i+1][0]) / 2
                        mid_y = (wps[i][1] + wps[i+1][1]) / 2
                        d = math.sqrt((c["x"]-mid_x)**2 + (c["y"]-mid_y)**2)
                        if d < best_dist:
                            best_dist = d
                            proj_len = total_len + seg_len / 2
                        total_len += seg_len
                    if total_len > 0:
                        return proj_len / total_len
            return 0.3  # default early game
        g.GetLaneFrontAmount = _get_lane_front_amount

        def get_loc_along_lane(lane, amount):
            lane = int(lane) if lane else LANE_MID
            amount = float(amount) if amount else 0.5
            wps = LANE_WAYPOINTS.get(lane, LANE_WAYPOINTS[LANE_MID])
            idx = int(amount * (len(wps) - 1))
            idx = max(0, min(idx, len(wps) - 1))
            wp = wps[idx]
            return brain._make_vector(wp[0], wp[1], 128)
        g.GetLocationAlongLane = get_loc_along_lane

        def _get_amount_along_lane(lane, loc):
            """Returns {amount=0-1, distance=float}."""
            lane = int(lane) if lane else LANE_MID
            result = self.lua.table()
            wps = LANE_WAYPOINTS.get(lane, LANE_WAYPOINTS[LANE_MID])
            # Find closest waypoint
            best_dist = 99999
            best_idx = 0
            try:
                lx, ly = float(loc.x), float(loc.y)
            except:
                lx, ly = 0, 0
            for i, wp in enumerate(wps):
                d = math.sqrt((lx - wp[0])**2 + (ly - wp[1])**2)
                if d < best_dist:
                    best_dist = d
                    best_idx = i
            result.amount = best_idx / max(len(wps) - 1, 1)
            result.distance = best_dist
            return result
        g.GetAmountAlongLane = _get_amount_along_lane

        # ─── Buildings ────────────────────────────────────
        def _get_tower(team, tid=0):
            # Return a mock building handle for towers in cache
            for t in brain.cache.towers:
                if t["team"] == team:
                    return brain._make_unit_handle(t["entity"])
            return brain._make_building_handle(team)
        g.GetTower = _get_tower
        g.GetBarracks = lambda team, bid=0: brain._make_building_handle(team)
        g.GetAncient = lambda team: brain._make_building_handle(team, is_ancient=True)
        g.GetShrine = lambda team, sid=0: None
        g.GetTowerAttackTarget = lambda tower: None

        # ─── Runes ────────────────────────────────────────
        g.GetRuneStatus = lambda rid: 0  # UNKNOWN
        g.GetRuneType = lambda rid: 0
        g.GetRuneSpawnLocation = lambda rid: brain._make_vector(0, 0, 128)
        g.GetRuneTimeSinceSeen = lambda rid: 999

        # ─── Items (from JSON DB) ────────────────────────────
        g.GetItemCost = lambda name: brain.db.item_cost(name)
        g.GetItemStockCount = lambda name: 1
        g.IsItemPurchasedFromSecretShop = lambda name: brain.db.is_secret_shop_item(name)
        g.IsItemPurchasedFromSideShop = lambda name: False

        # ─── Combat ───────────────────────────────────────
        def find_aoe_location(enemies, heroes, loc, radius, min_count, time_ahead, team=None):
            t = self.lua.table()
            t.targetloc = loc
            t.count = 0
            return t
        g.FindAoELocation = find_aoe_location
        g.GetHeroLastSeenInfo = lambda pid: self.lua.table()

        # ─── Roshan / Glyph ──────────────────────────────
        g.GetRoshanKillTime = lambda: -999
        g.GetGlyphCooldown = lambda: 0
        g.GetScanCooldown = lambda: 0

        # ─── Desires ──────────────────────────────────────
        g.GetPushLaneDesire = lambda lane: 0.0
        g.GetDefendLaneDesire = lambda lane: 0.0
        g.GetFarmLaneDesire = lambda lane: 0.0
        g.GetRoshanDesire = lambda: 0.0

        # ─── Unit Lists (from entity cache) ──────────────
        def _get_unit_list(unit_type):
            result = []
            enemy_team = TEAM_DIRE if brain._our_team == TEAM_RADIANT else TEAM_RADIANT
            if unit_type == UNIT_LIST_ALLIED_HEROES:
                for h in brain.cache.heroes:
                    if h["team"] == brain._our_team:
                        result.append(brain._make_unit_handle(h["entity"]))
            elif unit_type == UNIT_LIST_ENEMY_HEROES:
                for h in brain.cache.heroes:
                    if h["team"] == enemy_team:
                        result.append(brain._make_unit_handle(h["entity"]))
            elif unit_type == UNIT_LIST_ALLIED_CREEPS:
                for c in brain.cache.creeps:
                    if c["team"] == brain._our_team:
                        result.append(brain._make_unit_handle(c["entity"]))
            elif unit_type == UNIT_LIST_ENEMY_CREEPS:
                for c in brain.cache.creeps:
                    if c["team"] == enemy_team:
                        result.append(brain._make_unit_handle(c["entity"]))
            elif unit_type == UNIT_LIST_ALLIED_BUILDINGS:
                for b in brain.cache.towers + brain.cache.buildings:
                    if b["team"] == brain._our_team:
                        result.append(brain._make_unit_handle(b["entity"]))
            elif unit_type == UNIT_LIST_ENEMY_BUILDINGS:
                for b in brain.cache.towers + brain.cache.buildings:
                    if b["team"] == enemy_team:
                        result.append(brain._make_unit_handle(b["entity"]))
            return brain.lua.table_from(result)
        g.GetUnitList = _get_unit_list

        # ─── Courier ──────────────────────────────────────
        g.GetCourier = lambda idx: None
        g.GetNumCouriers = lambda: 0
        g.GetCourierState = lambda c: 0
        g.IsCourierAvailable = lambda: False
        g.IsFlyingCourier = lambda: True

        # ─── Misc ─────────────────────────────────────────
        g.GetScriptDirectory = lambda: self.scripts_dir + "/bots"
        g.RandomInt = lambda a, b: __import__('random').randint(a, b)
        g.RandomFloat = lambda a, b: __import__('random').uniform(a, b)
        g.Min = lambda *a: min(a) if a else 0
        g.Max = lambda *a: max(a) if a else 0
        g.Clamp = lambda v, lo, hi: max(lo, min(v, hi))
        g.RemapVal = lambda v, imin, imax, omin, omax: omin + (v - imin) / max(imax - imin, 0.001) * (omax - omin)
        g.RemapValClamped = lambda v, imin, imax, omin, omax: max(omin, min(omax, omin + (v - imin) / max(imax - imin, 0.001) * (omax - omin)))
        g.GetWorldBounds = lambda: (brain._make_vector(-8288, -8288, 0), brain._make_vector(8288, 8288, 0))
        g.GetNeutralSpawners = lambda: self.lua.table()
        g.GetHeightLevel = lambda loc: 0
        g.GetTreeLocation = lambda tid: brain._make_vector(0, 0, 128)

        # ─── Item Components ──────────────────────────────
        def _get_item_components(item_name):
            # TODO: parse item recipe tree from items.json
            return self.lua.table()
        g.GetItemComponents = _get_item_components

        # ─── Hero Pick State ─────────────────────────────
        def _get_hero_pick_state():
            try:
                hps = brain._game.get_hero_pick_state()
                return hps if hps >= 0 else 0
            except:
                return 0
        g.GetHeroPickState = _get_hero_pick_state
        g.SelectHero = lambda pid, name: None
        g.IsPlayerInHeroSelectionControl = lambda pid: False

        # ─── Missing globals that scripts might call ─────
        g.GetOpposingTeam = lambda: TEAM_DIRE if brain._our_team == TEAM_RADIANT else TEAM_RADIANT
        g.GetDefendLaneDesire = lambda lane: 0.0
        g.GetPushLaneDesire = lambda lane: 0.0
        g.GetFarmLaneDesire = lambda lane: 0.0
        g.IsModeActive = lambda mode: False
        g.IsLocationVisible = lambda loc: False
        g.IsLocationPassable = lambda loc: True
        g.GetLinearProjectiles = lambda: self.lua.table()
        g.GetAvoidanceZones = lambda: self.lua.table()
        g.GetIncomingTrackingProjectiles = lambda *a: self.lua.table()
        g.GetGameModeDesire = lambda *a: 0.0
        g.GetDefendLaneDesireAll = lambda: self.lua.table_from([0.0, 0.0, 0.0, 0.0])
        g.GetPushLaneDesireAll = lambda: self.lua.table_from([0.0, 0.0, 0.0, 0.0])
        g.GetFarmLaneDesireAll = lambda: self.lua.table_from([0.0, 0.0, 0.0, 0.0])
        g.SetPowerLevelOfBot = lambda *a: None
        g.GetPowerOfTeam = lambda *a: 1.0
        g.GetTeamAssignedLane = lambda *a: LANE_MID
        g.GetBarracksHealthPercent = lambda *a: 1.0
        g.GetTowerHealthPercent = lambda *a: 1.0

        # ─── Dropped items ───────────────────────────────
        g.GetDroppedItemList = lambda: self.lua.table()
        g.GetIncomingTeleports = lambda: self.lua.table()
        g.GetNearbyFilledRune = lambda *a: None
        g.GetNearbyUnfilledRune = lambda *a: None
        g.GetItemStockCount = lambda *a: 1
        g.GetTimeOfDay = lambda: 0.5  # 0=night, 0.25=dawn, 0.5=day, 0.75=dusk
        g.IsDaytime = lambda: True
        g.IsNightstalkerNight = lambda: False
        g.GetUnitPotentialValue = lambda *a: 0
        g.GetTreeLocation = lambda *a: brain._make_vector(0, 0, 128)
        g.GetNeutralSpawners = lambda: self.lua.table()
        g.GetGoldMineLocation = lambda *a: brain._make_vector(0, 0, 128)
        g.GetRoshanSpawnLocation = lambda: brain._make_vector(-2000, 1600, 128)
        g.GetShopLocation = lambda *a: brain._make_vector(-7200, -6700, 128)
        g.GetSecretShopLocation = lambda *a: brain._make_vector(-4700, 1400, 128)
        g.GetSideShopLocation = lambda *a: brain._make_vector(0, 0, 128)
        g.GetAncient = lambda team: brain._make_building_handle(team, is_ancient=True)
        g.GetItemPurchaseCost = lambda name: brain.db.item_cost(name)
        g.GetHeroPickState = _get_hero_pick_state
        g.GetHeroDayTimeVisionRange = lambda *a: 1800
        g.GetHeroNightTimeVisionRange = lambda *a: 800

        # ─── Vector metatable (arithmetic + tostring) ────
        self.lua.execute(f'''
            _VectorMT = {{}}
            _VectorMT.__index = _VectorMT
            function _VectorMT.__add(a, b)
                return Vector(a.x + b.x, a.y + b.y, a.z + b.z)
            end
            function _VectorMT.__sub(a, b)
                return Vector(a.x - b.x, a.y - b.y, a.z - b.z)
            end
            function _VectorMT.__mul(a, b)
                if type(a) == "number" then return Vector(a * b.x, a * b.y, a * b.z) end
                if type(b) == "number" then return Vector(a.x * b, a.y * b, a.z * b) end
                return Vector(a.x * b.x, a.y * b.y, a.z * b.z)
            end
            function _VectorMT.__div(a, b)
                if type(b) == "number" then return Vector(a.x / b, a.y / b, a.z / b) end
                return Vector(a.x / b.x, a.y / b.y, a.z / b.z)
            end
            function _VectorMT.__unm(a)
                return Vector(-a.x, -a.y, -a.z)
            end
            function _VectorMT.__eq(a, b)
                return a.x == b.x and a.y == b.y and a.z == b.z
            end
            function _VectorMT.__tostring(v)
                return string.format("Vector(%.1f, %.1f, %.1f)", v.x, v.y, v.z)
            end
            function _VectorMT:Length()
                return math.sqrt(self.x^2 + self.y^2 + self.z^2)
            end
            function _VectorMT:Length2D()
                return math.sqrt(self.x^2 + self.y^2)
            end
            function _VectorMT:Normalized()
                local l = self:Length()
                if l < 0.001 then return Vector(0, 0, 0) end
                return Vector(self.x/l, self.y/l, self.z/l)
            end
            function _VectorMT:Dot(other)
                return self.x * other.x + self.y * other.y + self.z * other.z
            end
            function _VectorMT:Cross(other)
                return Vector(
                    self.y * other.z - self.z * other.y,
                    self.z * other.x - self.x * other.z,
                    self.x * other.y - self.y * other.x)
            end
        ''')

        # ─── Lua-side error/trace logger ────────────────────
        def _lua_trace(msg):
            brain.log.action(f"lua:{msg}")
        g._py_trace = _lua_trace

        def _lua_log_error(cat, msg):
            brain.log.error(f"lua:{cat}", str(msg))
        g._py_log_error = _lua_log_error

        # Wrap pcall to capture errors from Lua scripts
        self.lua.execute('''
            local _orig_pcall = pcall
            local _orig_xpcall = xpcall
            -- Don't override pcall/xpcall globally — too noisy
            -- Instead, provide a helper the scripts can use
            function _safe_call(fn, ...)
                local ok, result = _orig_pcall(fn, ...)
                if not ok then
                    _py_log_error("lua_pcall", tostring(result))
                end
                return ok, result
            end
        ''')

        # ─── Debug ────────────────────────────────────────
        # ─── Missing globals discovered via error logging ──
        def _get_hero_level(pid):
            pid = int(pid) if pid is not None else 0
            if 0 <= pid < len(brain.cache.heroes):
                return brain.cache.heroes[pid].get("level", 1)
            return (brain._get_hero_data() or {}).get("level", 1)
        g.GetHeroLevel = _get_hero_level

        g.DebugDrawText = lambda *a: None
        g.DebugDrawCircle = lambda *a: None
        g.DebugDrawLine = lambda *a: None
        g.DebugPause = lambda: None

        # ─── Print / Msg ──────────────────────────────────
        g.Msg = lambda *args: print("[LuaBot]", *args)

        # ─── Vector constructor ───────────────────────────
        def _vector_ctor(*args):
            # Handle: Vector(x,y,z), Vector(x,y), Vector(x), Vector()
            x = float(args[0]) if len(args) > 0 else 0.0
            y = float(args[1]) if len(args) > 1 else 0.0
            z = float(args[2]) if len(args) > 2 else 0.0
            return brain._make_vector(x, y, z)
        g.Vector = _vector_ctor

    def _make_vector(self, x=0, y=0, z=0):
        """Create a Lua-compatible Vector with metatable for arithmetic."""
        # Use Lua-side Vector constructor for proper metatable support
        if not hasattr(self, '_lua_make_vector'):
            self._lua_make_vector = self.lua.eval('''
                function(x, y, z)
                    local v = {x = x, y = y, z = z}
                    return setmetatable(v, _VectorMT)
                end
            ''')
        return self._lua_make_vector(float(x), float(y), float(z))

    def _make_building_handle(self, team, is_ancient=False):
        """Create a mock building handle (for GetAncient, GetTower, GetBarracks)."""
        handle = self.lua.table()
        brain = self
        m = lambda fn: (lambda *a: fn())

        # Position: ancient at fountain area
        if team == TEAM_RADIANT:
            bx, by = -6200, -6200
        else:
            bx, by = 6200, 6200

        handle.GetLocation = m(lambda: brain._make_vector(bx, by, 128))
        handle.GetAbsOrigin = m(lambda: brain._make_vector(bx, by, 128))
        handle.GetTeam = m(lambda: team)
        handle.GetHealth = m(lambda: 4250)
        handle.GetMaxHealth = m(lambda: 4250)
        handle.OriginalGetHealth = m(lambda: 4250)
        handle.OriginalGetMaxHealth = m(lambda: 4250)
        handle.GetUnitName = m(lambda: "npc_dota_fort" if is_ancient else "npc_dota_tower")
        handle.IsAlive = m(lambda: True)
        handle.IsNull = m(lambda: False)
        handle.IsHero = m(lambda: False)
        handle.IsBuilding = m(lambda: True)
        handle.IsTower = m(lambda: not is_ancient)
        handle.IsAncient = m(lambda: is_ancient)
        handle.IsInvulnerable = m(lambda: False)
        handle.CanBeSeen = m(lambda: True)
        handle.GetAttackRange = m(lambda: 700.0)
        handle.GetAttackDamage = m(lambda: 100)
        handle.GetArmor = m(lambda: 20.0)
        handle.HasModifier = lambda *a: False
        handle.NumModifiers = m(lambda: 0)
        handle.GetAnimActivity = m(lambda: -1)
        return handle

    def _get_hero_data(self, entity_ptr=None):
        """Get cached hero data for entity_ptr, or our hero if None."""
        if entity_ptr is None:
            entity_ptr = self._our_hero_entity
        for h in self.cache.heroes:
            if h["entity"] == entity_ptr:
                return h
        return None

    def _make_bot_handle(self):
        """Create/return the cached bot handle for GetBot().
        In original Dota 2, GetBot() always returns the same userdata,
        so scripts can store properties on it (bot.frameProcessTime, etc.).
        """
        if not hasattr(self, '_cached_bot_handle') or self._cached_bot_handle is None:
            self._cached_bot_handle = self._make_unit_handle(self._our_hero_entity)
        return self._cached_bot_handle

    def _make_unit_handle(self, entity_ptr):
        """Create a unit handle with all API methods.
        NOTE: All methods accept *args because Lua ':' syntax passes self as first arg.
        Works for heroes, creeps, towers, and buildings.
        """
        handle = self.lua.table()
        brain = self

        # Identity — search ALL caches (heroes, creeps, towers, buildings)
        def get_data():
            for h in brain.cache.heroes:
                if h["entity"] == entity_ptr:
                    return h
            for c in brain.cache.creeps:
                if c["entity"] == entity_ptr:
                    return c
            for t in brain.cache.towers:
                if t["entity"] == entity_ptr:
                    return t
            for b in brain.cache.buildings:
                if b["entity"] == entity_ptr:
                    return b
            return None

        def _unit_type():
            """Determine unit type from cached name."""
            d = get_data()
            if not d:
                return "unknown"
            n = d.get("name", "")
            if "hero" in n:
                return "hero"
            if "creep" in n:
                return "creep"
            if "tower" in n:
                return "tower"
            if "rax" in n or "fort" in n or "filler" in n:
                return "building"
            return "unit"

        # Helper: wrap lambda to accept and ignore Lua self
        def m(fn):
            """Wrap a no-arg function to accept Lua self."""
            return lambda *a: fn()

        handle.GetUnitName = m(lambda: (get_data() or {}).get("name", "npc_dota_hero_unknown"))
        # PlayerID: use entity index in heroes cache as stable unique ID
        def _get_player_id():
            for i, h in enumerate(brain.cache.heroes):
                if h["entity"] == entity_ptr:
                    return i
            return 0
        handle.GetPlayerID = m(_get_player_id)
        handle.IsBot = m(lambda: True)
        handle.IsNull = m(lambda: False)
        handle.IsAlive = m(lambda: (get_data() or {}).get("hp", 0) > 0)
        handle.IsIllusion = m(lambda: (get_data() or {}).get("is_illusion", False))
        handle.IsHero = m(lambda: _unit_type() == "hero")
        handle.IsInvulnerable = m(lambda: bool(_us() & UnitState.INVULNERABLE))
        handle.GetTeam = m(lambda: (get_data() or {}).get("team", brain._our_team))
        handle.GetLevel = m(lambda: (get_data() or {}).get("level", 1))
        handle.GetDifficulty = m(lambda: DIFFICULTY_HARD)
        handle.GetActiveMode = m(lambda: brain._active_mode)
        handle.GetActiveModeDesire = m(lambda: brain._active_mode_desire)
        handle.GetAssignedLane = m(lambda: brain._assigned_lane)

        # Position — all methods use m() wrapper for Lua self compatibility
        def _get_location():
            d = get_data()
            if d:
                return brain._make_vector(d["x"], d["y"], d["z"])
            return brain._make_vector(0, 0, 128)
        handle.GetLocation = m(_get_location)
        handle.GetAbsOrigin = m(_get_location)
        handle.GetFacing = m(lambda: 0.0)
        handle.GetVelocity = m(lambda: brain._make_vector(0, 0, 0))
        # GetCurrentMovementSpeed set below with GetBaseMovementSpeed (from memory)
        handle.GetFront = m(_get_location)
        handle.IsFacingLocation = lambda *a: True
        handle.GetBoundingRadius = m(lambda: 24.0)

        def _dist_fountain():
            d = get_data()
            if not d: return 0
            fx, fy, fz = FOUNTAIN_RADIANT if brain._our_team == TEAM_RADIANT else FOUNTAIN_DIRE
            return math.sqrt((d["x"]-fx)**2 + (d["y"]-fy)**2)
        handle.DistanceFromFountain = m(_dist_fountain)
        handle.DistanceFromSecretShop = m(lambda: 3000)
        handle.DistanceFromSideShop = m(lambda: 3000)

        def _get_x_towards(*args):
            # args: (self_table, loc, dist) or (loc, dist)
            a = [x for x in args if not isinstance(x, (int, float)) or args.index(x) > 0]
            loc = args[-2] if len(args) >= 2 else args[0]
            dist = float(args[-1]) if len(args) >= 2 else 100
            d = get_data()
            if not d: return brain._make_vector(0, 0, 128)
            dx = loc.x - d["x"]; dy = loc.y - d["y"]
            l = math.sqrt(dx*dx + dy*dy)
            if l < 1: return _get_location()
            return brain._make_vector(d["x"] + dx/l*dist, d["y"] + dy/l*dist, d.get("z", 128))
        handle.GetXUnitsTowardsLocation = _get_x_towards

        # Health & Mana (from memory via EntityCache)
        handle.GetHealth = m(lambda: (get_data() or {}).get("hp", 1))
        handle.GetMaxHealth = m(lambda: (get_data() or {}).get("max_hp", (get_data() or {}).get("hp", 1)))
        handle.OriginalGetHealth = m(lambda: (get_data() or {}).get("hp", 1))
        handle.OriginalGetMaxHealth = m(lambda: (get_data() or {}).get("max_hp", 1))
        handle.GetHealthRegen = m(lambda: (get_data() or {}).get("health_regen", 2.0))
        handle.GetMana = m(lambda: (get_data() or {}).get("mana", 0.0))
        handle.GetMaxMana = m(lambda: (get_data() or {}).get("max_mana", 1.0))
        handle.GetManaRegen = m(lambda: (get_data() or {}).get("mana_regen", 1.5))

        # Combat stats (from JSON + memory)
        def _hero_name():
            d = get_data()
            return d["name"] if d else brain.hero_name or ""

        # Combat stats: prefer live memory values, fallback to JSON DB
        def _attack_range():
            d = get_data()
            ar = (d or {}).get("attack_range", 0)
            return float(ar) if ar > 0 else float(brain.db.hero_attack_range(_hero_name()))
        handle.GetAttackRange = m(_attack_range)
        handle.GetAttackDamage = m(lambda: (get_data() or {}).get("damage_min", 50))
        handle.GetBaseDamage = m(lambda: (get_data() or {}).get("damage_min", 50))
        handle.GetAttackSpeed = m(lambda: float(brain.db.hero_stat(_hero_name(), "BaseAttackSpeed", 100)))
        handle.GetSecondsPerAttack = m(lambda: float(brain.db.hero_stat(_hero_name(), "AttackRate", "1.7")))
        handle.GetAttackPoint = m(lambda: float(brain.db.hero_stat(_hero_name(), "AttackAnimationPoint", "0.3")))
        handle.GetAttackProjectileSpeed = m(lambda: int(brain.db.hero_stat(_hero_name(), "ProjectileSpeed", 0)))
        handle.GetLastAttackTime = m(lambda: 0)
        def _armor():
            d = get_data()
            a = (d or {}).get("physical_armor")
            return float(a) if a is not None else float(brain.db.hero_base_armor(_hero_name()))
        handle.GetArmor = m(_armor)
        def _magic_resist():
            d = get_data()
            mr = (d or {}).get("magic_resist")
            # memory stores as 0.25 (25%), Dota bot API expects 0.25 too
            return float(mr) if mr is not None else float(brain.db.hero_stat(_hero_name(), "MagicalResistance", 25)) / 100.0
        handle.GetMagicResist = m(_magic_resist)
        handle.GetEvasion = m(lambda: 0.0)
        handle.GetRawOffensivePower = m(lambda: float((get_data() or {}).get("damage_min", 50)))
        handle.GetOffensivePower = m(lambda: float((get_data() or {}).get("damage_min", 50)))
        handle.GetEstimatedDamageToTarget = lambda *a: float((get_data() or {}).get("damage_min", 50))
        handle.GetActualIncomingDamage = lambda *a: float(a[-2]) * 0.8 if len(a) >= 2 else 40.0

        # Targeting — dynamic: return nearest enemy hero within 1600 range
        def _get_target():
            if brain._target:
                return brain._target
            d = get_data()
            if not d:
                return None
            enemy_team = TEAM_DIRE if brain._our_team == TEAM_RADIANT else TEAM_RADIANT
            best = None
            best_dist = 99999
            for h in brain.cache.heroes:
                if h["team"] != enemy_team or h.get("hp", 0) <= 0:
                    continue
                dist = math.sqrt((h["x"] - d["x"])**2 + (h["y"] - d["y"])**2)
                if dist < 1600 and dist < best_dist:
                    best = h
                    best_dist = dist
            if best:
                return brain._make_unit_handle(best["entity"])
            return None
        handle.GetTarget = m(_get_target)
        handle.GetAttackTarget = m(_get_target)
        handle.SetTarget = lambda *a: setattr(brain, '_target', a[-1] if a else None)

        # CC & Status — from m_nUnitState64 bitmask
        def _us():
            return (get_data() or {}).get("unit_state", 0)
        handle.IsStunned = m(lambda: bool(_us() & UnitState.STUNNED))
        handle.IsRooted = m(lambda: bool(_us() & UnitState.ROOTED))
        handle.IsSilenced = m(lambda: bool(_us() & UnitState.SILENCED))
        handle.IsHexed = m(lambda: bool(_us() & UnitState.HEXED))
        handle.IsNightmared = m(lambda: bool(_us() & UnitState.NIGHTMARED))
        handle.IsDisarmed = m(lambda: bool(_us() & UnitState.DISARMED))
        handle.IsBlind = m(lambda: bool(_us() & UnitState.BLIND))
        handle.IsMuted = m(lambda: bool(_us() & UnitState.MUTED))
        handle.IsInvisible = m(lambda: bool(_us() & UnitState.INVISIBLE))
        handle.IsMagicImmune = m(lambda: bool(_us() & UnitState.MAGIC_IMMUNE))
        handle.IsAttackImmune = m(lambda: bool(_us() & UnitState.ATTACK_IMMUNE))
        handle.IsChanneling = m(lambda: bool(_us() & UnitState.COMMAND_RESTRICTED))
        handle.IsCastingAbility = m(lambda: False)  # no simple flag for this
        handle.IsUsingAbility = m(lambda: False)
        handle.IsUnableToMiss = m(lambda: bool(_us() & UnitState.CANNOT_MISS))
        handle.IsBuilding = m(lambda: _unit_type() in ("tower", "building"))
        handle.IsTower = m(lambda: _unit_type() == "tower")
        handle.IsAncient = m(lambda: "fort" in (get_data() or {}).get("name", ""))
        handle.IsAncientCreep = m(lambda: "ancient" in (get_data() or {}).get("name", "").lower()
                                           and _unit_type() == "creep")
        handle.IsCreep = m(lambda: _unit_type() == "creep")
        handle.GetAnimActivity = m(lambda: -1)
        handle.GetAnimCycle = m(lambda: 0.0)
        handle.GetDayTimeVisionRange = m(lambda: (get_data() or {}).get("day_vision", 1800))
        handle.GetNightTimeVisionRange = m(lambda: (get_data() or {}).get("night_vision", 800))
        handle.GetIncomingTrackingProjectiles = m(lambda: brain.lua.table())
        handle.GetCurrentVisionRange = m(lambda: (get_data() or {}).get("day_vision", 1800))
        handle.GetHealthPercent = m(lambda: (get_data() or {}).get("hp", 1) / max((get_data() or {}).get("max_hp", 1), 1))
        handle.GetManaPercent = m(lambda: (get_data() or {}).get("mana", 0) / max((get_data() or {}).get("max_mana", 1), 1))
        def _move_speed():
            d = get_data()
            ms = (d or {}).get("move_speed", 0)
            return ms if ms > 0 else 300
        handle.GetBaseMovementSpeed = m(_move_speed)
        handle.GetCurrentMovementSpeed = m(_move_speed)
        handle.HasBotMovementMode = m(lambda: False)
        handle.GetPrimaryAttribute = m(lambda: 0)
        handle.GetAttributeValue = lambda *a: 0.0  # attribute stat value (str/agi/int)
        handle.IsFort = m(lambda: "fort" in (get_data() or {}).get("name", ""))
        handle.IsPhantom = m(lambda: False)
        handle.IsPhantomBlocker = m(lambda: False)
        handle.CanBeSeen = m(lambda: True)
        handle.HasSilence = m(lambda: bool(_us() & UnitState.SILENCED))
        handle.GetStunDuration = lambda *a: 0.0  # TODO: modifier duration
        handle.GetSlowDuration = lambda *a: 0.0

        # Damage history — from HP delta tracking in EntityCache
        # We can't distinguish damage source (hero/creep/tower) without modifiers,
        # so WasRecentlyDamagedByHero/ByAnyHero both use the same HP delta check.
        handle.WasRecentlyDamagedByHero = m(lambda: brain.cache.was_recently_damaged(entity_ptr, 2.0))
        handle.WasRecentlyDamagedByAnyHero = m(lambda: brain.cache.was_recently_damaged(entity_ptr, 2.0))
        handle.WasRecentlyDamagedByCreep = m(lambda: brain.cache.was_recently_damaged(entity_ptr, 2.0))
        handle.WasRecentlyDamagedByTower = m(lambda: brain.cache.was_recently_damaged(entity_ptr, 3.0))
        handle.TimeSinceDamagedByAnyHero = m(lambda: brain.cache.time_since_damaged(entity_ptr))
        handle.TimeSinceDamagedByCreep = m(lambda: brain.cache.time_since_damaged(entity_ptr))
        handle.TimeSinceDamagedByTower = m(lambda: brain.cache.time_since_damaged(entity_ptr))

        # Nearby units — accept *args for Lua self
        def _nearby_heroes(*args):
            # args: (self, radius, enemy, mode) or (radius, enemy, mode)
            radius = float(args[1]) if len(args) > 2 else float(args[0]) if args else 1600
            enemy = bool(args[2]) if len(args) > 2 else bool(args[1]) if len(args) > 1 else True
            result = []
            enemy_team = TEAM_DIRE if brain._our_team == TEAM_RADIANT else TEAM_RADIANT
            target_team = enemy_team if enemy else brain._our_team
            d = get_data()
            if not d: return brain.lua.table_from(result)
            for h in brain.cache.heroes:
                if h["entity"] == entity_ptr: continue
                if h["team"] != target_team: continue
                dist = math.sqrt((h["x"]-d["x"])**2 + (h["y"]-d["y"])**2)
                if dist <= radius:
                    result.append(brain._make_unit_handle(h["entity"]))
            brain.trace.event("GetNearbyHeroes", f"radius={radius} enemy={enemy} found={len(result)}")
            return brain.lua.table_from(result)
        handle.GetNearbyHeroes = _nearby_heroes

        def _nearby_creeps(*args):
            radius = float(args[1]) if len(args) > 2 else float(args[0]) if args else 1600
            enemy = bool(args[2]) if len(args) > 2 else bool(args[1]) if len(args) > 1 else True
            result = []
            enemy_team = TEAM_DIRE if brain._our_team == TEAM_RADIANT else TEAM_RADIANT
            target_team = enemy_team if enemy else brain._our_team
            d = get_data()
            if not d: return brain.lua.table_from(result)
            for c in brain.cache.creeps:
                if c["team"] != target_team: continue
                dist = math.sqrt((c["x"]-d["x"])**2 + (c["y"]-d["y"])**2)
                if dist <= radius:
                    result.append(brain._make_unit_handle(c["entity"]))
            return brain.lua.table_from(result)
        handle.GetNearbyCreeps = _nearby_creeps
        handle.GetNearbyLaneCreeps = _nearby_creeps
        handle.GetNearbyNeutralCreeps = lambda *a: brain.lua.table()

        def _nearby_towers(*args):
            radius = float(args[1]) if len(args) > 2 else float(args[0]) if args else 1600
            enemy = bool(args[2]) if len(args) > 2 else bool(args[1]) if len(args) > 1 else True
            result = []
            enemy_team = TEAM_DIRE if brain._our_team == TEAM_RADIANT else TEAM_RADIANT
            target_team = enemy_team if enemy else brain._our_team
            d = get_data()
            if not d: return brain.lua.table_from(result)
            for t in brain.cache.towers:
                if t["team"] != target_team: continue
                dist = math.sqrt((t["x"]-d["x"])**2 + (t["y"]-d["y"])**2)
                if dist <= radius:
                    result.append(brain._make_unit_handle(t["entity"]))
            return brain.lua.table_from(result)
        handle.GetNearbyTowers = _nearby_towers
        def _nearby_barracks(*args):
            radius = float(args[1]) if len(args) > 2 else float(args[0]) if args else 1600
            enemy = bool(args[2]) if len(args) > 2 else bool(args[1]) if len(args) > 1 else True
            result = []
            enemy_team = TEAM_DIRE if brain._our_team == TEAM_RADIANT else TEAM_RADIANT
            target_team = enemy_team if enemy else brain._our_team
            d = get_data()
            if not d: return brain.lua.table_from(result)
            for b in brain.cache.buildings:
                if b["team"] != target_team: continue
                if "rax" not in b.get("name", ""): continue
                dist = math.sqrt((b["x"]-d["x"])**2 + (b["y"]-d["y"])**2)
                if dist <= radius:
                    result.append(brain._make_unit_handle(b["entity"]))
            return brain.lua.table_from(result)
        handle.GetNearbyBarracks = _nearby_barracks
        handle.GetNearbyTrees = lambda *a: brain.lua.table()

        # FindAoELocation — find best AoE position hitting most enemies
        def _find_aoe_location(*args):
            # args: (self, enemies_bool, heroes_bool, loc, cast_range, radius, ?, ?)
            # Simplified: count enemies within radius of our location
            try:
                loc_arg = args[3] if len(args) > 3 else (args[1] if len(args) > 1 else None)
                radius = float(args[4]) if len(args) > 4 else 300
                d = get_data()
                if not d:
                    t = brain.lua.table()
                    t.count = 0
                    t.targetloc = brain._make_vector(0, 0, 128)
                    return t
                enemy_team = TEAM_DIRE if brain._our_team == TEAM_RADIANT else TEAM_RADIANT
                best_count = 0
                best_loc = brain._make_vector(d["x"], d["y"], d["z"])
                # Check each enemy hero as potential center
                for h in brain.cache.heroes:
                    if h["team"] != enemy_team or h.get("hp", 0) <= 0:
                        continue
                    cx, cy = h["x"], h["y"]
                    dist_to_us = math.sqrt((cx - d["x"])**2 + (cy - d["y"])**2)
                    cast_range = float(args[4]) if len(args) > 4 else 800
                    if dist_to_us > cast_range + radius:
                        continue
                    # Count enemies near this hero
                    cnt = 0
                    for h2 in brain.cache.heroes:
                        if h2["team"] != enemy_team or h2.get("hp", 0) <= 0:
                            continue
                        dd = math.sqrt((h2["x"] - cx)**2 + (h2["y"] - cy)**2)
                        if dd <= radius:
                            cnt += 1
                    if cnt > best_count:
                        best_count = cnt
                        best_loc = brain._make_vector(cx, cy, h.get("z", 128))
                t = brain.lua.table()
                t.count = best_count
                t.targetloc = best_loc
                return t
            except Exception as e:
                brain.log.error("FindAoELocation", str(e))
                t = brain.lua.table()
                t.count = 0
                t.targetloc = brain._make_vector(0, 0, 128)
                return t
        handle.FindAoELocation = _find_aoe_location

        # Modifiers
        handle.NumModifiers = m(lambda: 0)
        handle.GetModifierName = lambda *a: ""
        handle.HasModifier = lambda *a: False
        handle.GetModifierByName = lambda *a: None
        handle.GetModifierRemainingDuration = lambda *a: 0
        handle.GetModifierStackCount = lambda *a: 0

        # Economy (from C_DOTA_DataNonSpectator)
        handle.GetGold = m(lambda: brain._get_gold())
        handle.GetNetWorth = m(lambda: brain._get_net_worth())
        handle.GetLastHits = m(lambda: brain._get_last_hits_denies()[0])
        handle.GetDenies = m(lambda: brain._get_last_hits_denies()[1])
        handle.HasBuyback = m(lambda: False)
        handle.GetBuybackCooldown = m(lambda: 0)
        handle.GetBuybackCost = m(lambda: 0)
        handle.GetStashValue = m(lambda: 0)
        handle.GetCourierValue = m(lambda: 0)

        # Inventory (items from memory)
        def _get_item_in_slot(*args):
            slot = int(args[-1]) if args else 0
            items = brain._refresh_items(entity_ptr)
            for item in items:
                if item.get("slot", -1) == slot:
                    return brain._make_item_handle(item["name"], slot)
            return None
        handle.GetItemInSlot = _get_item_in_slot

        def _find_item_slot(*args):
            item_name = args[-1] if args else ""
            if not isinstance(item_name, str):
                return -1
            items = brain._refresh_items(entity_ptr)
            for item in items:
                if item.get("name", "") == item_name:
                    return item.get("slot", -1)
            return -1
        handle.FindItemSlot = _find_item_slot

        def _get_item_slot_type(*args):
            slot = int(args[-1]) if args else -1
            if 0 <= slot <= 5:
                return 0  # MAIN
            elif 6 <= slot <= 8:
                return 1  # BACKPACK
            elif 9 <= slot <= 14:
                return 2  # STASH
            return -1
        handle.GetItemSlotType = _get_item_slot_type
        handle.SetNextItemPurchaseValue = lambda *a: None

        # HasScepter — scan inventory for aghs or consumed buff
        def _has_scepter():
            items = brain._refresh_items(entity_ptr)
            for item in items:
                n = item.get("name", "")
                if "item_ultimate_scepter" in n:
                    return True
            return False
        handle.HasScepter = m(_has_scepter)

        # Abilities (REAL: from memory + JSON)
        def _get_ability_by_name(*args):
            ab_name = args[-1] if args else ""
            if not isinstance(ab_name, str):
                return None
            abilities = brain._refresh_abilities(entity_ptr)
            for ab in abilities:
                if ab.get("name", "") == ab_name:
                    return brain._make_ability_handle(ab, _hero_name())
            # Fallback: create stub from JSON only (level=0, not learned)
            if brain.db.get_ability(ab_name):
                stub = {"name": ab_name, "level": 0, "cooldown": 0, "cooldown_length": 0,
                        "mana_cost": 0, "hidden": True, "activated": False, "toggle_state": False,
                        "charges": 0, "slot": -1}
                return brain._make_ability_handle(stub, _hero_name())
            return None
        handle.GetAbilityByName = _get_ability_by_name

        def _get_ability_in_slot(*args):
            slot = int(args[-1]) if args else 0
            abilities = brain._refresh_abilities(entity_ptr)
            for ab in abilities:
                if ab.get("slot", -1) == slot:
                    return brain._make_ability_handle(ab, _hero_name())
            return None
        handle.GetAbilityInSlot = _get_ability_in_slot

        handle.GetAbilityCount = m(lambda: len(brain._refresh_abilities(entity_ptr)))
        def _get_ability_points():
            try:
                return brain.game.read_ability_points(entity_ptr)
            except:
                return 0
        handle.GetAbilityPoints = m(_get_ability_points)

        # Action queue
        handle.NumQueuedActions = m(lambda: 0)
        handle.GetCurrentActionType = m(lambda: 0)  # BOT_ACTION_TYPE_IDLE
        def _action_clear(*args):
            brain.cmd.stop()
            brain._last_action = "STOP"
        handle.Action_ClearActions = _action_clear

        # Extrapolated location (pos + velocity * dt)
        def _get_extrap_location(*args):
            dt = float(args[-1]) if args and isinstance(args[-1], (int, float)) else 0.0
            d = get_data()
            if d:
                return brain._make_vector(d["x"], d["y"], d["z"])  # no velocity yet
            return brain._make_vector(0, 0, 128)
        handle.GetExtrapolatedLocation = _get_extrap_location

        # ─── ACTIONS (these actually DO things) ───────────
        # All accept *args for Lua self compatibility
        # All logged via brain.log for debugging

        def _action_move_to(*args):
            try:
                loc = args[-1]
                brain._execute_move(loc.x, loc.y, getattr(loc, 'z', 128))
                brain.log.action("Move")
            except Exception as e:
                brain.log.error("Action_Move", str(e))
        handle.Action_MoveToLocation = _action_move_to
        handle.ActionPush_MoveToLocation = _action_move_to
        handle.ActionQueue_MoveToLocation = _action_move_to
        handle.Action_MoveDirectly = _action_move_to

        def _action_move_to_unit(*args):
            try:
                unit = args[-1]
                loc = unit.GetLocation()
                brain._execute_move(loc.x, loc.y, getattr(loc, 'z', 128))
                brain.log.action("MoveToUnit")
            except Exception as e:
                brain.log.error("Action_MoveToUnit", str(e))
        handle.Action_MoveToUnit = _action_move_to_unit

        def _action_attack_move(*args):
            try:
                loc = args[-1]
                brain._execute_attack_move(loc.x, loc.y, getattr(loc, 'z', 128))
                brain.log.action("AttackMove")
            except Exception as e:
                brain.log.error("Action_AttackMove", str(e))
        handle.Action_AttackMove = _action_attack_move
        handle.ActionPush_AttackMove = _action_attack_move
        handle.ActionQueue_AttackMove = _action_attack_move

        def _action_attack_unit(*args):
            try:
                unit = args[1] if len(args) > 1 else args[0]
                loc = unit.GetLocation()
                brain._execute_attack_move(loc.x, loc.y, getattr(loc, 'z', 128))
                brain.log.action("AttackUnit")
            except Exception as e:
                brain.log.error("Action_AttackUnit", str(e))
        handle.Action_AttackUnit = _action_attack_unit
        handle.ActionPush_AttackUnit = _action_attack_unit
        handle.ActionQueue_AttackUnit = _action_attack_unit

        # Ability actions — resolve ability entity index for cast orders
        def _find_ability_info(ability_handle):
            """Find ability slot + entity index from handle."""
            try:
                ab_name = ability_handle.GetName()
                abilities = brain._refresh_abilities(entity_ptr)
                for ab in abilities:
                    if ab.get("name", "") == ab_name:
                        ent_idx = (ab.get("handle", 0) & 0x7FFF) if ab.get("handle") else ab.get("slot", 0)
                        brain.trace.event("FindAbility", f"{ab_name} → slot={ab.get('slot',0)} ent_idx={ent_idx}")
                        return ab.get("slot", 0), ent_idx, ab_name
                brain.trace.event("FindAbility", f"{ab_name} NOT FOUND in {len(abilities)} abilities")
            except Exception as e:
                brain.log.error("FindAbility", str(e))
                brain.trace.event("FindAbility", f"ERROR: {e}")
            return 0, 0, "?"

        def _action_use_ability(*args):
            try:
                ability = args[1] if len(args) > 1 else args[0]
                slot, ent_idx, name = _find_ability_info(ability)
                if not ent_idx:
                    brain.trace.event("CastNT", f"{name}: SKIP ent_idx=0")
                    return  # invalid ability — don't crash the game
                brain.cmd.cast_no_target(ability_index=ent_idx)
                brain._last_action = f"CAST_NT({name})"
                brain._last_action_time = time.time()
                brain.log.action(f"CastNT:{name}")
                brain.trace.event("CastNT", f"{name} ent_idx={ent_idx} OK")
            except Exception as e:
                brain.log.error("Action_UseAbility", str(e))
        handle.Action_UseAbility = _action_use_ability
        handle.ActionPush_UseAbility = _action_use_ability
        handle.ActionQueue_UseAbility = _action_use_ability

        def _action_use_ability_on_entity(*args):
            try:
                ability = args[1] if len(args) > 2 else args[0]
                target = args[2] if len(args) > 2 else args[1]
                slot, ent_idx, name = _find_ability_info(ability)
                if not ent_idx:
                    return  # invalid ability
                # Resolve target entity index from handle
                target_eidx = 0
                try:
                    t_ent = getattr(target, '_entity_ptr', 0)
                    if t_ent:
                        t_ident = brain.mem.read_ptr(t_ent + 0x10)
                        if t_ident and t_ident > 0x10000:
                            t_handle = brain.mem.read_u32(t_ident + 0x10)
                            target_eidx = t_handle & 0x7FFF
                except:
                    pass
                brain.cmd.cast_target(target_index=target_eidx, ability_index=ent_idx)
                brain._last_action = f"CAST_ENT({name})"
                brain._last_action_time = time.time()
                brain.log.action(f"CastEnt:{name}")
                brain.trace.event("CastEnt", f"{name} ent_idx={ent_idx} target={target_eidx} OK")
            except Exception as e:
                brain.log.error("Action_UseAbilityOnEntity", str(e))
        handle.Action_UseAbilityOnEntity = _action_use_ability_on_entity
        handle.ActionPush_UseAbilityOnEntity = _action_use_ability_on_entity
        handle.ActionQueue_UseAbilityOnEntity = _action_use_ability_on_entity

        def _action_use_ability_on_location(*args):
            try:
                ability = args[1] if len(args) > 2 else args[0]
                loc = args[2] if len(args) > 2 else args[1]
                slot, ent_idx, name = _find_ability_info(ability)
                if not ent_idx:
                    return  # invalid ability
                brain.cmd.cast_position(ent_idx, float(loc.x), float(loc.y),
                                        float(getattr(loc, 'z', 128)))
                brain._last_action = f"CAST_POS({name},{loc.x:.0f},{loc.y:.0f})"
                brain._last_action_time = time.time()
                brain.log.action(f"CastPos:{name}")
                brain.trace.event("CastPos", f"{name} ent_idx={ent_idx} pos=({loc.x:.0f},{loc.y:.0f}) OK")
            except Exception as e:
                brain.log.error("Action_UseAbilityOnLocation", str(e))
        handle.Action_UseAbilityOnLocation = _action_use_ability_on_location
        handle.ActionPush_UseAbilityOnLocation = _action_use_ability_on_location
        handle.ActionQueue_UseAbilityOnLocation = _action_use_ability_on_location

        handle.Action_UseAbilityOnTree = lambda *a: None  # TODO: tree targeting

        # Immediate actions
        def _purchase(*args):
            try:
                item = args[-1] if args else ""
                if isinstance(item, str):
                    ok = brain.cmd.purchase_item(item)
                    if ok:
                        brain.log.action(f"Buy:{item}")
                    else:
                        brain.log.error("Purchase", f"purchase_item({item}) returned False")
            except Exception as e:
                brain.log.error("Purchase", str(e))
        handle.ActionImmediate_PurchaseItem = _purchase
        handle.ActionImmediate_SellItem = lambda *a: None
        handle.ActionImmediate_Buyback = lambda *a: brain.cmd.buyback()
        handle.ActionImmediate_Glyph = lambda *a: None
        def _level_ability(*args):
            try:
                ab_name = args[-1] if args else ""
                if hasattr(ab_name, 'GetName'):
                    ab_name = ab_name.GetName()
                if not isinstance(ab_name, str) or not ab_name:
                    return
                abilities = brain._refresh_abilities(entity_ptr)
                for ab in abilities:
                    if ab.get("name", "") == ab_name and ab.get("handle"):
                        # TRAIN_ABILITY needs entity index, not slot
                        ent_idx = ab["handle"] & 0x7FFF
                        brain.cmd.train_ability(ability_index=ent_idx)
                        brain._last_action = f"LEVEL({ab_name})"
                        brain._last_action_time = time.time()
                        return
            except Exception as e:
                if not hasattr(brain, '_level_err_logged'):
                    brain._level_err_logged = True
                    print(f"  [!] LevelAbility error: {e}")
        handle.ActionImmediate_LevelAbility = _level_ability
        handle.ActionImmediate_Chat = lambda *a: None
        handle.ActionImmediate_Ping = lambda *a: None
        handle.ActionImmediate_Courier = lambda *a: None
        handle.ActionImmediate_SwapItems = lambda *a: None
        handle.ActionImmediate_DisassembleItem = lambda *a: None
        handle.ActionImmediate_SetItemCombineLock = lambda *a: None

        # ─── Properties used by aba_global_overrides patches ──────
        # These get set/read by scripts on the bot table directly
        # (e.g., bot.frameProcessTime, bot.stuckLoc, bot.assignedRole)
        # Lua tables support arbitrary key assignment, so this works naturally.

        return handle

    def _make_unit_handle_by_pid(self, pid):
        """Create a unit handle for a player by their PlayerID."""
        pid = int(pid)
        if 0 <= pid < len(self.cache.heroes):
            return self._make_unit_handle(self.cache.heroes[pid]["entity"])
        return self._make_bot_handle()

    def _refresh_abilities(self, entity_ptr):
        """Refresh ability cache for an entity (rate-limited to 1Hz)."""
        now = time.time()
        if now - self._ability_cache_time < 1.0 and entity_ptr in self._ability_cache:
            return self._ability_cache.get(entity_ptr, [])
        try:
            abilities = self.game.get_hero_abilities(entity_ptr, self.cache.handle_index)
            self._ability_cache[entity_ptr] = abilities
            self._ability_cache_time = now
            if abilities:
                summary = ", ".join(f"{a.get('name','?')}(lv={a.get('level',0)})" for a in abilities[:6])
                self.trace.event("RefreshAbilities", f"{len(abilities)} abilities: {summary}")
        except Exception as e:
            abilities = self._ability_cache.get(entity_ptr, [])
            self.trace.event("RefreshAbilities", f"ERROR: {e}")
        return abilities

    def _refresh_items(self, entity_ptr):
        """Refresh item cache for an entity (rate-limited to 1Hz)."""
        now = time.time()
        if now - self._item_cache_time < 1.0 and entity_ptr in self._item_cache:
            return self._item_cache.get(entity_ptr, [])
        try:
            items = self.game.get_hero_items(entity_ptr, self.cache.handle_index)
            self._item_cache[entity_ptr] = items
            self._item_cache_time = now
        except:
            items = self._item_cache.get(entity_ptr, [])
        return items

    def _get_gold(self) -> int:
        """Read total gold (reliable + unreliable) for our hero.

        Uses DataNonSpectator entity (reliable + unreliable gold).
        Fallback: 625 (starting gold).
        """
        data_ent = (self.cache.data_radiant if self._our_team == TEAM_RADIANT
                    else self.cache.data_dire)
        if data_ent:
            try:
                r, u = self.game.read_gold(data_ent, self._player_slot)
                total = r + u
                if total > 0:
                    return total
            except:
                pass

        return 625  # starting gold fallback

    def _get_net_worth(self) -> int:
        """Read net worth for our hero."""
        data_ent = (self.cache.data_radiant if self._our_team == TEAM_RADIANT
                    else self.cache.data_dire)
        if not data_ent:
            return 0
        try:
            return self.game.read_net_worth(data_ent, self._player_slot)
        except:
            return 0

    def _get_last_hits_denies(self) -> tuple[int, int]:
        """Read (last_hits, denies) for our hero."""
        data_ent = (self.cache.data_radiant if self._our_team == TEAM_RADIANT
                    else self.cache.data_dire)
        if not data_ent:
            return (0, 0)
        try:
            return self.game.read_last_hits_denies(data_ent, self._player_slot)
        except:
            return (0, 0)

    def _make_ability_handle(self, ab_data: dict, hero_name: str):
        """Create a Lua-compatible ability handle from memory + JSON data.

        ab_data: from game.read_ability_data() — has level, cooldown, name, slot, etc.
        hero_name: for JSON lookup fallback.

        IMPORTANT: All dynamic fields (level, cooldown, hidden, mana_cost) are re-read
        from memory on each call. Handles are cached at Lua module load time, so stale
        snapshot values would break IsFullyCastable() permanently.
        """
        handle = self.lua.table()
        brain = self
        db = self.db

        ab_name = ab_data.get("name", "")
        ab_entity = ab_data.get("entity", 0)  # ability entity pointer for live reads
        slot = ab_data.get("slot", 0)

        brain.trace.event("MakeAbilityHandle", f"{ab_name} slot={slot} ent=0x{ab_entity:X} init_lv={ab_data.get('level', 0)}")

        def m(fn):
            return lambda *a: fn()

        # --- Dynamic reads from memory (NOT stale snapshots) ---

        def _get_level():
            """Re-read ability level from memory."""
            if ab_entity:
                try:
                    return brain.mem.read_i32(ab_entity + BaseAbility.LEVEL)
                except:
                    pass
            return ab_data.get("level", 0)

        def _get_cd_remaining():
            """Re-read cooldown from memory."""
            cd_expire = 0.0
            if ab_entity:
                try:
                    cd_expire = brain.mem.read_f32(ab_entity + BaseAbility.COOLDOWN)
                except:
                    cd_expire = ab_data.get("cooldown", 0.0)
            else:
                cd_expire = ab_data.get("cooldown", 0.0)
            if cd_expire <= 0:
                return 0.0
            gt = brain.cache.game_time
            if gt >= 1e9:
                gt = time.time() - brain._start_time
            remaining = cd_expire - gt
            return max(0.0, remaining)

        def _is_hidden():
            """Re-read hidden flag from memory."""
            if ab_entity:
                try:
                    return brain.mem.read_u8(ab_entity + BaseAbility.HIDDEN) != 0
                except:
                    pass
            return ab_data.get("hidden", False)

        def _get_mana_cost():
            """Re-read mana cost from memory, fallback to JSON."""
            if ab_entity:
                try:
                    mc = brain.mem.read_i32(ab_entity + BaseAbility.MANA_COST)
                    if mc > 0:
                        return mc
                except:
                    pass
            mc = ab_data.get("mana_cost", 0)
            if mc > 0:
                return mc
            return int(db.ability_mana_cost(ab_name, max(_get_level(), 1)))

        # Identity
        handle.GetName = m(lambda: ab_name)
        handle.GetAbilityName = m(lambda: ab_name)

        # Level (DYNAMIC from memory)
        handle.GetLevel = m(_get_level)

        # Cooldown (DYNAMIC from memory)
        handle.GetCooldownTimeRemaining = m(_get_cd_remaining)
        handle.GetCooldown = m(lambda: db.ability_cooldown(ab_name, max(_get_level(), 1)))
        handle.IsCooldownReady = m(lambda: _get_cd_remaining() <= 0)
        handle.IsInAbilityPhase = m(lambda: False)

        # Mana cost (DYNAMIC)
        handle.GetManaCost = m(_get_mana_cost)

        # Castable check (all dynamic)
        def _is_fully_castable():
            cur_level = _get_level()
            cd = _get_cd_remaining()
            hidden = _is_hidden()
            mana = 0
            cost = 0
            hero_data = brain._get_hero_data()
            if hero_data:
                mana = hero_data.get("mana", 0)
                cost = _get_mana_cost()

            castable = True
            reason = ""
            if cur_level <= 0:
                castable = False
                reason = "level=0"
            elif cd > 0:
                castable = False
                reason = f"cd={cd:.1f}"
            elif hidden:
                castable = False
                reason = "hidden"
            elif hero_data and mana < cost:
                castable = False
                reason = f"mana={mana:.0f}<cost={cost}"

            brain.trace.event("IsFullyCastable",
                              f"{ab_name}: lv={cur_level} cd={cd:.1f} hidden={hidden} "
                              f"mana={mana:.0f}/{cost} → {castable}" +
                              (f" ({reason})" if reason else ""))
            return castable
        handle.IsFullyCastable = m(_is_fully_castable)

        # Damage (from JSON, uses dynamic level)
        handle.GetDamage = m(lambda: db.ability_damage(ab_name, max(_get_level(), 1)))
        handle.GetAbilityDamage = m(lambda: db.ability_damage(ab_name, max(_get_level(), 1)))

        # Cast range (from JSON)
        handle.GetCastRange = m(lambda: db.ability_cast_range(ab_name, max(_get_level(), 1)))
        handle.GetCastPoint = m(lambda: float(db.get_ability(ab_name).get("AbilityCastPoint", "0.3").split()[0]) if db.get_ability(ab_name) else 0.3)

        # Behavior (from JSON)
        handle.GetBehavior = m(lambda: db.ability_behavior_flags(ab_name))

        # Target type
        handle.GetTargetTeam = m(lambda: 0)
        handle.GetTargetType = m(lambda: 0)
        handle.GetTargetFlags = m(lambda: 0)

        # Charges
        handle.GetCurrentCharges = m(lambda: ab_data.get("charges", 0))

        # State
        handle.IsHidden = m(lambda: ab_data.get("hidden", False))
        handle.IsActivated = m(lambda: ab_data.get("activated", True))
        handle.IsToggle = m(lambda: bool(db.ability_behavior_flags(ab_name) & 256))
        handle.GetToggleState = m(lambda: ab_data.get("toggle_state", False))
        handle.GetAutoCastState = m(lambda: ab_data.get("autocast", False))
        handle.IsStolen = m(lambda: False)
        handle.IsNull = m(lambda: False)
        handle.IsTalent = m(lambda: "special_bonus" in ab_name)
        handle.IsPassive = m(lambda: bool(db.ability_behavior_flags(ab_name) & 1))
        handle.IsUltimate = m(lambda: slot == 5 or (db.get_ability(ab_name) or {}).get("AbilityType", "") == "DOTA_ABILITY_TYPE_ULTIMATE")
        handle.IsItem = m(lambda: ab_name.startswith("item_"))
        handle.IsInnateAbility = m(lambda: "innate" in ab_name or slot >= 18)
        handle.ProcsMagicStick = m(lambda: not ("special_bonus" in ab_name or "innate" in ab_name))
        handle.GetAbilityType = m(lambda: 1 if slot == 5 else 0)  # 1=ultimate
        handle.IsTrained = m(lambda: _get_level() > 0)
        handle.CanAbilityBeUpgraded = m(lambda: True if _get_level() == 0 else False)
        handle.GetHeroLevelRequiredToUpgrade = m(lambda: 6 if slot == 5 else 1)
        handle.GetMaxLevel = m(lambda: 3 if slot == 5 else 4)
        handle.IsChanneling = m(lambda: False)
        handle.IsInAbilityPhase = m(lambda: False)
        handle.GetDamageType = m(lambda: 2)  # DAMAGE_TYPE_MAGICAL
        handle.GetImmunityType = m(lambda: 0)
        handle.GetDispellableType = m(lambda: 0)
        handle.GetAbilityTextureName = m(lambda: ab_name)
        handle.GetBackswingTime = m(lambda: 0.0)
        handle.GetChannelTime = m(lambda: 0.0)
        handle.GetChannelledManaCostPerSecond = m(lambda: 0)
        handle.IsOwnersManaEnough = m(lambda: True)
        handle.IsOwnersGoldEnough = m(lambda: True)

        # AOE radius
        def _get_aoe_radius():
            at_level = db.ability_at_level(ab_name, max(_get_level(), 1))
            return at_level.get("radius", at_level.get("aoe_radius", 0.0))
        handle.GetAOERadius = m(_get_aoe_radius)

        # Duration
        handle.GetDuration = m(lambda: float(
            (db.get_ability(ab_name) or {}).get("AbilityDuration", "0").split()[0]
        ) if db.get_ability(ab_name) else 0.0)

        # Specials / AbilityValues lookup
        def _get_special_value(*args):
            # args: (self, key) or (key)
            key = args[-1] if args else ""
            if isinstance(key, str):
                at_level = db.ability_at_level(ab_name, max(_get_level(), 1))
                return at_level.get(key, 0.0)
            return 0.0
        handle.GetSpecialValueInt = _get_special_value
        handle.GetSpecialValueFloat = _get_special_value
        handle.GetSpecialValueFor = _get_special_value
        handle.GetLevelSpecialValueFor = lambda *a: _get_special_value(*a[:-1]) if len(a) > 2 else _get_special_value(*a)

        return handle

    def _make_item_handle(self, item_name: str, slot: int = 0):
        """Create a Lua-compatible item handle from JSON data."""
        handle = self.lua.table()
        brain = self
        db = self.db

        def m(fn):
            return lambda *a: fn()

        handle.GetName = m(lambda: item_name)
        handle.IsNull = m(lambda: False)
        handle.IsHidden = m(lambda: False)
        handle.GetCooldownTimeRemaining = m(lambda: 0.0)
        handle.IsCooldownReady = m(lambda: True)
        handle.IsFullyCastable = m(lambda: True)
        handle.GetCurrentCharges = m(lambda: 1)
        handle.GetCastRange = m(lambda: float(
            (db.get_item(item_name) or {}).get("AbilityCastRange", "0").split()[0]
        ))
        handle.GetManaCost = m(lambda: float(
            (db.get_item(item_name) or {}).get("AbilityManaCost", "0").split()[0]
        ))
        handle.GetCooldown = m(lambda: db.item_cooldown(item_name))
        handle.GetToggleState = m(lambda: False)
        handle.GetLevel = m(lambda: 1)
        handle.IsTalent = m(lambda: False)
        handle.IsPassive = m(lambda: False)
        handle.IsUltimate = m(lambda: False)
        handle.IsItem = m(lambda: True)
        handle.IsInnateAbility = m(lambda: False)
        handle.ProcsMagicStick = m(lambda: False)
        handle.GetAbilityType = m(lambda: 0)
        handle.GetBehavior = m(lambda: 0)
        handle.GetTargetTeam = m(lambda: 0)
        handle.GetTargetType = m(lambda: 0)
        handle.GetTargetFlags = m(lambda: 0)
        handle.GetDamage = m(lambda: 0)
        handle.GetAbilityDamage = m(lambda: 0)
        handle.GetDuration = m(lambda: 0)
        handle.GetAOERadius = m(lambda: 0)
        handle.GetCastPoint = m(lambda: 0)
        handle.IsTrained = m(lambda: True)
        handle.CanAbilityBeUpgraded = m(lambda: False)
        handle.IsChanneling = m(lambda: False)
        handle.IsInAbilityPhase = m(lambda: False)
        handle.IsActivated = m(lambda: True)
        handle.GetDamageType = m(lambda: 0)
        handle.GetAbilityTextureName = m(lambda: item_name)
        handle.GetBackswingTime = m(lambda: 0.0)
        handle.GetChannelTime = m(lambda: 0.0)
        handle.IsOwnersManaEnough = m(lambda: True)
        handle.IsOwnersGoldEnough = m(lambda: True)

        # Specials
        def _get_special(*args):
            key = args[-1] if args else ""
            if isinstance(key, str):
                item_data = db.get_item(item_name) or {}
                vals = item_data.get("AbilityValues", {})
                raw = vals.get(key, 0)
                if isinstance(raw, dict):
                    raw = raw.get("value", 0)
                try:
                    return float(raw)
                except:
                    return 0.0
            return 0.0
        handle.GetSpecialValueInt = _get_special
        handle.GetSpecialValueFloat = _get_special
        handle.GetSpecialValueFor = _get_special

        return handle

    def _execute_move(self, x, y, z=128):
        """Send move order via PrepareUnitOrders."""
        self.cmd.move_to(float(x), float(y), float(z))
        self._last_action = f"MOVE({x:.0f},{y:.0f})"
        self._last_action_time = time.time()
        self.trace.event("Order", f"MOVE({x:.0f},{y:.0f})")

    def _execute_attack_move(self, x, y, z=128):
        """Send attack-move order via PrepareUnitOrders."""
        self.cmd.attack_move(float(x), float(y), float(z))
        self._last_action = f"AMOVE({x:.0f},{y:.0f})"
        self._last_action_time = time.time()
        self.trace.event("Order", f"AMOVE({x:.0f},{y:.0f})")

    def _execute_attack_target(self, entity_ptr):
        """Attack specific entity by resolving its entity index."""
        try:
            ident = self.mem.read_ptr(entity_ptr + 0x10)
            if ident and ident > 0x10000:
                eidx = self.mem.read_u32(ident + 0x10) & 0x7FFF
                if eidx > 0:
                    self.cmd.attack_target(target_index=eidx)
                    self._last_action_time = time.time()
                    self.trace.event("Order", f"ATK_TARGET(eidx={eidx})")
                    return True
        except:
            pass
        return False

    def _load_scripts(self) -> bool:
        """Load bot_generic.lua and mode files with sandboxed environments."""
        bots_dir = os.path.join(self.scripts_dir, "bots")
        bot_generic = os.path.join(bots_dir, "bot_generic.lua")

        if not os.path.exists(bot_generic):
            print(f"[!] bot_generic.lua not found at {bot_generic}")
            return False

        # Set up Lua package path — absolute paths first, then relative
        bots_dir_lua = bots_dir.replace("\\", "/")
        scripts_dir_lua = self.scripts_dir.replace("\\", "/")
        self.lua.execute(f'''
            package.path = "?.lua;?/init.lua;{bots_dir_lua}/?.lua;{scripts_dir_lua}/game/?.lua;" .. package.path

            -- Patch dofile to auto-add .lua extension (Dota engine does this)
            local _dofile = dofile
            dofile = function(path)
                if path and not string.find(path, "%.lua$") then
                    path = path .. ".lua"
                end
                return _dofile(path)
            end

            -- Mock CDOTA_Bot_Script class (used for monkey-patching in bot scripts)
            CDOTA_Bot_Script = {{}}
            setmetatable(CDOTA_Bot_Script, {{
                __index = function(t, k)
                    return function(...) end
                end
            }})

            -- Proper setfenv/getfenv shim for Lua 5.2+ (lupa uses 5.4)
            if not setfenv then
                function setfenv(level_or_fn, env)
                    local func
                    if type(level_or_fn) == "number" then
                        local info = debug.getinfo(level_or_fn + 1, "f")
                        if info then func = info.func end
                    else
                        func = level_or_fn
                    end
                    if func then
                        local i = 1
                        while true do
                            local name = debug.getupvalue(func, i)
                            if name == "_ENV" then
                                debug.setupvalue(func, i, env)
                                return func
                            elseif not name then
                                break
                            end
                            i = i + 1
                        end
                    end
                    return level_or_fn
                end
                function getfenv(level_or_fn)
                    local func
                    if type(level_or_fn) == "number" then
                        local info = debug.getinfo(level_or_fn + 1, "f")
                        if info then func = info.func end
                    else
                        func = level_or_fn
                    end
                    if func then
                        local i = 1
                        while true do
                            local name, val = debug.getupvalue(func, i)
                            if name == "_ENV" then
                                return val
                            elseif not name then
                                break
                            end
                            i = i + 1
                        end
                    end
                    return _G
                end
            end

            -- String helper that bot scripts expect
            if not string.starts then
                string.starts = function(str, start)
                    return str:sub(1, #start) == start
                end
            end
            if not string.ends then
                string.ends = function(str, ending)
                    return ending == "" or str:sub(-#ending) == ending
                end
            end

            -- ─── Lua 5.4 compat shims ───────────────────────────────
            if not unpack then
                unpack = table.unpack
            end

            -- ─── bit library shim (Lua 5.4 uses bitwise operators, not bit.* module) ───
            if not bit then
                bit = {{}}
                bit.band = function(a, b) return a & b end
                bit.bor = function(a, b) return a | b end
                bit.bxor = function(a, b) return a ~ b end
                bit.bnot = function(a) return ~a end
                bit.lshift = function(a, n) return a << n end
                bit.rshift = function(a, n) return a >> n end
                bit.arshift = function(a, n) return a >> n end  -- arithmetic = logical for positive
                bit.tobit = function(a) return a & 0xFFFFFFFF end
                bit.tohex = function(a) return string.format("%x", a & 0xFFFFFFFF) end
            end

            -- ─── math.pow shim (removed in Lua 5.4, use ^ operator) ───
            if not math.pow then
                math.pow = function(base, exp) return base ^ exp end
            end

            -- ─── Permissive require ─────────────────────────────────────
            -- Wraps the original require to catch errors and return a permissive
            -- mock table (auto-creates subtables/methods on access) instead of crashing.
            _original_require = _original_require or require
            _require_cache = _require_cache or {{}}
            _require_errors = _require_errors or {{}}

            -- Permissive mock: any key access returns a no-op or empty sub-mock
            function _make_permissive_mock(label)
                local mock = {{}}
                setmetatable(mock, {{
                    __index = function(t, k)
                        -- Return a callable that returns 0/false/empty
                        local fn = function(...)
                            return 0
                        end
                        rawset(t, k, fn)
                        return fn
                    end,
                    __tostring = function() return "PermissiveMock<" .. (label or "?") .. ">" end,
                    __call = function(t, ...) return t end,
                    __len = function() return 0 end,
                }})
                return mock
            end

            -- Paths where failure should propagate (caller uses xpcall to handle)
            _require_passthrough = _require_passthrough or {{
                "Customize", "game/Customize",
            }}

            function _is_passthrough_module(modname)
                for _, pat in ipairs(_require_passthrough) do
                    if string.find(modname, pat, 1, true) then
                        return true
                    end
                end
                return false
            end

            function require(modname)
                -- Check cache first
                if _require_cache[modname] then
                    return _require_cache[modname]
                end
                -- Already known to fail
                if _require_errors[modname] then
                    -- Passthrough modules: re-raise error so xpcall in caller handles it
                    if _is_passthrough_module(modname) then
                        error("module '" .. modname .. "' not found")
                    end
                    return _make_permissive_mock(modname)
                end
                -- Try real require
                local ok, result = pcall(_original_require, modname)
                if ok and result ~= nil then
                    _require_cache[modname] = result
                    return result
                end
                -- Failed — record error
                _require_errors[modname] = true
                -- Passthrough modules: propagate error
                if _is_passthrough_module(modname) then
                    error("module '" .. modname .. "' not found")
                end
                -- Other modules: return permissive mock
                local mock = _make_permissive_mock(modname)
                _require_cache[modname] = mock
                return mock
            end

            -- ─── Sandbox loader ────────────────────────────────────────
            -- Loads a Lua file in an isolated environment that inherits _G.
            -- All top-level function defs (GetDesire, Think, etc.) land in the
            -- returned table instead of polluting _G.
            function _sandbox_load(filepath)
                local chunk, err = loadfile(filepath)
                if not chunk then
                    return nil, err
                end
                -- Create sandboxed env: reads fall through to _G, writes go into env
                local env = {{}}
                setmetatable(env, {{
                    __index = _G,
                }})
                -- Set the chunk's _ENV
                -- In Lua 5.4, the first upvalue of a loaded chunk is always _ENV
                debug.setupvalue(chunk, 1, env)
                local ok, result = pcall(chunk)
                if not ok then
                    return nil, result
                end
                -- If chunk returned a table, merge it into env
                if type(result) == "table" then
                    for k, v in pairs(result) do
                        env[k] = v
                    end
                end
                return env, nil
            end
        ''')

        # Load bot_generic.lua (loads hero-specific module)
        try:
            with open(bot_generic, 'r', encoding='utf-8') as f:
                code = f.read()
            # Patch: make BotBuild global so ability_item_usage_generic.lua can access it.
            # In Dota 2 engine all scripts share one env; our sandbox isolation breaks this.
            code = code.replace("local BotBuild = dofile(", "BotBuild = dofile(", 1)
            print(f"[+] Loading bot_generic.lua ({len(code)} bytes)")
            self.lua.execute(code)
            print("[+] bot_generic.lua loaded OK")
            # Verify BotBuild is available
            g = self.lua.globals()
            try:
                bb = g.BotBuild
                if bb:
                    has_skill = bool(bb['sSkillList'])
                    print(f"  [+] BotBuild loaded, sSkillList={'YES' if has_skill else 'NO'}")
            except:
                print("  [!] BotBuild is nil after loading bot_generic.lua")
        except Exception as e:
            print(f"[!] Error loading bot_generic: {e}")

        # Load mode files in sandboxed environments
        self._modes = {}
        mode_files = [
            ("laning", "mode_laning_generic.lua"),
            ("attack", "mode_attack_generic.lua"),
            ("retreat", "mode_retreat_generic.lua"),
            ("farm", "mode_farm_generic.lua"),
            ("push_top", "mode_push_tower_top_generic.lua"),
            ("push_mid", "mode_push_tower_mid_generic.lua"),
            ("push_bot", "mode_push_tower_bot_generic.lua"),
            ("defend_top", "mode_defend_tower_top_generic.lua"),
            ("defend_mid", "mode_defend_tower_mid_generic.lua"),
            ("defend_bot", "mode_defend_tower_bot_generic.lua"),
            ("roam", "mode_roam_generic.lua"),
            ("team_roam", "mode_team_roam_generic.lua"),
        ]

        sandbox_load = self.lua.globals()._sandbox_load
        for mode_name, filename in mode_files:
            filepath = os.path.join(bots_dir, filename).replace("\\", "/")
            if not os.path.exists(filepath):
                continue
            try:
                env, err = sandbox_load(filepath)
                if err:
                    print(f"  [!] {filename}: {err}")
                    continue
                # Check if env has GetDesire (required) and optionally Think
                has_desire = False
                has_think = False
                try:
                    if env.GetDesire:
                        has_desire = True
                except:
                    pass
                try:
                    if env.Think:
                        has_think = True
                except:
                    pass
                if has_desire:
                    self._modes[mode_name] = env
                    status = "GetDesire+Think" if has_think else "GetDesire only"
                    print(f"  [+] {mode_name}: {status}")
            except Exception as e:
                print(f"  [!] {filename}: {e}")

        if self._modes:
            print(f"[+] Loaded {len(self._modes)} modes")
        else:
            print("[!] No modes loaded, will use fallback AI")

        # Check for global Think
        g = self.lua.globals()
        try:
            if g.Think:
                self.bot_think_fn = g.Think
                print("[+] Global Think() found")
        except:
            pass

        # Load ability_item_usage_generic.lua — contains AbilityUsageThink (hero skills),
        # AbilityLevelUpThink (auto level-up), ItemUsageThink (auto item usage)
        self._ability_usage_env = None
        ability_usage_file = os.path.join(bots_dir, "ability_item_usage_generic.lua").replace("\\", "/")
        if os.path.exists(ability_usage_file):
            try:
                env, err = sandbox_load(ability_usage_file)
                if err:
                    print(f"  [!] ability_item_usage_generic: {err}")
                else:
                    self._ability_usage_env = env
                    funcs = []
                    for fn_name in ("AbilityUsageThink", "AbilityLevelUpThink", "ItemUsageThink"):
                        try:
                            if getattr(env, fn_name, None):
                                funcs.append(fn_name)
                        except:
                            pass
                    print(f"[+] ability_item_usage_generic loaded: {', '.join(funcs)}")
            except Exception as e:
                print(f"  [!] ability_item_usage_generic: {e}")

        return True

    # ─── Python Brain: State Evaluation ──────────────────────

    def _evaluate_state(self):
        """Evaluate current game state for decision making.
        Returns dict with all relevant info, or None if no hero data."""
        d = self._get_hero_data()
        if not d:
            return None

        my_x, my_y, my_z = d['x'], d['y'], d.get('z', 128)
        my_hp = d.get('hp', 0)
        my_max_hp = max(d.get('max_hp', 1), 1)
        my_hp_pct = my_hp / my_max_hp
        my_mana = d.get('mana', 0)
        my_max_mana = max(d.get('max_mana', 1), 1)
        my_mana_pct = my_mana / my_max_mana
        my_level = d.get('level', 1)
        my_dmg = d.get('damage_min', 50)
        enemy_team = TEAM_DIRE if self._our_team == TEAM_RADIANT else TEAM_RADIANT

        state = {
            'x': my_x, 'y': my_y, 'z': my_z,
            'hp': my_hp, 'max_hp': my_max_hp, 'hp_pct': my_hp_pct,
            'mana': my_mana, 'max_mana': my_max_mana, 'mana_pct': my_mana_pct,
            'level': my_level, 'damage': my_dmg,
            'enemy_team': enemy_team,
            'alive': my_hp > 0,
        }

        # Nearby enemy heroes (sorted by distance)
        enemy_heroes = []
        for h in self.cache.heroes:
            if h.get('team') == self._our_team:
                continue
            hp = h.get('hp', 0)
            if hp <= 0 or hp > 50000:
                continue
            dx = h['x'] - my_x
            dy = h['y'] - my_y
            dist = math.sqrt(dx*dx + dy*dy)
            enemy_heroes.append({**h, 'dist': dist})
        enemy_heroes.sort(key=lambda e: e['dist'])
        state['enemy_heroes'] = enemy_heroes
        state['enemy_hero_near'] = enemy_heroes[0] if enemy_heroes and enemy_heroes[0]['dist'] < 1600 else None

        # Nearby allied heroes
        ally_heroes = []
        for h in self.cache.heroes:
            if h.get('team') != self._our_team or h['entity'] == self._our_hero_entity:
                continue
            hp = h.get('hp', 0)
            if hp <= 0 or hp > 50000:
                continue
            dx = h['x'] - my_x
            dy = h['y'] - my_y
            dist = math.sqrt(dx*dx + dy*dy)
            ally_heroes.append({**h, 'dist': dist})
        state['ally_heroes_nearby'] = [a for a in ally_heroes if a['dist'] < 1500]

        # Enemy/allied creeps
        enemy_creeps = []
        ally_creeps = []
        for c in self.cache.creeps:
            hp = c.get('hp', 0)
            if hp <= 0:
                continue
            dx = c['x'] - my_x
            dy = c['y'] - my_y
            dist = math.sqrt(dx*dx + dy*dy)
            entry = {**c, 'dist': dist}
            if c['team'] == enemy_team:
                enemy_creeps.append(entry)
            else:
                ally_creeps.append(entry)
        state['enemy_creeps'] = sorted(enemy_creeps, key=lambda c: c['dist'])
        state['ally_creeps'] = sorted(ally_creeps, key=lambda c: c['dist'])
        state['enemy_creeps_nearby'] = [c for c in state['enemy_creeps'] if c['dist'] < 900]
        state['ally_creeps_nearby'] = [c for c in state['ally_creeps'] if c['dist'] < 900]

        # Tower awareness
        enemy_towers = []
        ally_towers = []
        for t in self.cache.towers:
            hp = t.get('hp', 0)
            if hp <= 0:
                continue
            dx = t['x'] - my_x
            dy = t['y'] - my_y
            dist = math.sqrt(dx*dx + dy*dy)
            entry = {**t, 'dist': dist}
            if t['team'] == enemy_team:
                enemy_towers.append(entry)
            else:
                ally_towers.append(entry)
        state['enemy_towers'] = sorted(enemy_towers, key=lambda t: t['dist'])
        state['ally_towers'] = sorted(ally_towers, key=lambda t: t['dist'])
        state['under_enemy_tower'] = bool(enemy_towers and enemy_towers[0]['dist'] < 700)
        state['near_ally_tower'] = bool(ally_towers and ally_towers[0]['dist'] < 900)

        # Fountain distances
        fx, fy = FOUNTAIN_RADIANT[:2] if self._our_team == TEAM_RADIANT else FOUNTAIN_DIRE[:2]
        state['dist_fountain'] = math.sqrt((my_x - fx)**2 + (my_y - fy)**2)
        state['fountain'] = (fx, fy, 128)

        # Game time
        state['game_time'] = self.cache.game_time

        return state

    def _nearest_safe_pos(self, state):
        """Find safest retreat position: nearest ally tower or fountain."""
        # Prefer ally tower if one is nearby (< 3000 range)
        for t in state['ally_towers']:
            if t['dist'] < 3000:
                return (t['x'], t['y'], t.get('z', 128))
        # Otherwise fountain
        return state['fountain']

    # ─── Python Brain: Decision Tree ──────────────────────────

    def _python_brain_think(self, state):
        """Main Python brain decision tree. Returns action name for logging."""

        # === P0: DEAD — do nothing ===
        if not state['alive'] or state['hp'] <= 0:
            return "DEAD"

        # === P0: TP FROM FOUNTAIN — after respawn, TP to nearest tower ===
        if state['dist_fountain'] < 2000 and state['hp_pct'] > 0.9:
            # At fountain with full HP — just spawned. Try TP to lane.
            tp_eidx, _ = self._find_tp_in_inventory()
            if tp_eidx:
                # Cooldown check: don't spam TP (rate limit 1 per 10s)
                tp_cooldown_key = '_last_tp_time'
                last_tp = getattr(self, tp_cooldown_key, 0)
                if time.time() - last_tp > 10:
                    if self._tp_to_nearest_tower(state):
                        setattr(self, tp_cooldown_key, time.time())
                        return "TP_TO_LANE"
            else:
                # No TP — buy one (rate limit 1 per 15s)
                buy_key = '_last_tp_buy_time'
                last_buy = getattr(self, buy_key, 0)
                if time.time() - last_buy > 15:
                    self._buy_tp()
                    setattr(self, buy_key, time.time())
                    return "BUY_TP"

        # === P0: TP HOME — low HP, far from fountain ===
        if state['hp_pct'] < 0.25 and state['dist_fountain'] > 4000:
            # Dangerously low HP and far from base — TP home
            tp_eidx, _ = self._find_tp_in_inventory()
            if tp_eidx:
                last_tp = getattr(self, '_last_tp_time', 0)
                if time.time() - last_tp > 10:
                    if self._tp_home(state):
                        setattr(self, '_last_tp_time', time.time())
                        return f"TP_HOME(hp={state['hp_pct']:.0%})"

        # === P0: SURVIVAL — retreat if low HP ===
        if state['hp_pct'] < 0.35 and not state['under_enemy_tower']:
            safe = self._nearest_safe_pos(state)
            self._execute_move(safe[0], safe[1], safe[2])
            self._active_mode = BOT_MODE_RETREAT
            return f"RETREAT(hp={state['hp_pct']:.0%})"

        # Desperate retreat: under enemy tower with no allied creeps
        if state['under_enemy_tower'] and not state['ally_creeps_nearby']:
            safe = self._nearest_safe_pos(state)
            self._execute_move(safe[0], safe[1], safe[2])
            self._active_mode = BOT_MODE_RETREAT
            return "RETREAT(tower_no_creeps)"

        # === P0: COMBAT — enemy hero very close (< 600) or killable ===
        enemy = state['enemy_hero_near']
        if enemy:
            ehp_pct = enemy.get('hp', 0) / max(enemy.get('max_hp', 1), 1)
            # Commit to fight if enemy is very close or killable
            if enemy['dist'] < 600 or ehp_pct < 0.25:
                return self._combat_think(state, enemy)

        # === P1: LANING — farm creeps (LH priority over harassing distant hero) ===
        if state['enemy_creeps_nearby'] or state['ally_creeps_nearby']:
            result = self._laning_think(state)
            if result:
                return result

        # === P1.5: COMBAT — enemy hero in range, no creeps to farm ===
        if enemy:
            return self._combat_think(state, enemy)

        # === P1: ADVANCE — walk to lane ===
        return self._advance_think(state)

    def _combat_think(self, state, enemy):
        """Combat decision: harass, all-in, or kite."""
        ex, ey = enemy['x'], enemy['y']
        edist = enemy['dist']
        enemy_hp = enemy.get('hp', 0)
        enemy_max_hp = max(enemy.get('max_hp', 1), 1)
        enemy_hp_pct = enemy_hp / enemy_max_hp

        # Don't chase under enemy tower without allied creeps
        if self._pos_under_enemy_tower(ex, ey, state):
            if not state['ally_creeps_nearby']:
                # Kite back — move away from enemy
                safe = self._nearest_safe_pos(state)
                self._execute_move(safe[0], safe[1], safe[2])
                return f"KITE_TOWER(enemy_under_tower)"

        # Kill potential: enemy low HP, commit
        if enemy_hp_pct < 0.25 and state['hp_pct'] > 0.4:
            self._execute_attack_move(ex, ey, enemy.get('z', 128))
            self._active_mode = BOT_MODE_ATTACK
            return f"ALL_IN(ehp={enemy_hp_pct:.0%})"

        # We have advantage: more HP% or allies nearby
        n_allies = len(state['ally_heroes_nearby'])
        advantage = (state['hp_pct'] > enemy_hp_pct + 0.15) or n_allies >= 1

        if advantage and state['hp_pct'] > 0.5:
            # Aggressive: attack-move towards enemy
            self._execute_attack_move(ex, ey, enemy.get('z', 128))
            self._active_mode = BOT_MODE_ATTACK
            return f"AGGRO(ehp={enemy_hp_pct:.0%} allies={n_allies})"

        # Neutral: harass at max range with kiting
        # Skywrath attack range ~600, cast range ~600-1000
        # Ideal range: 500-700 (close enough to hit, far enough to kite)

        if edist > 800:
            # Move closer to get in cast/attack range
            self._execute_attack_move(ex, ey, enemy.get('z', 128))
            return f"APPROACH(d={edist:.0f})"

        if edist < 350:
            # Too close — kite back (move away from enemy)
            mx, my = state['x'], state['y']
            dx = mx - ex
            dy = my - ey
            d = math.sqrt(dx*dx + dy*dy) or 1.0
            # Move 400 units away from enemy
            kite_x = mx + (dx / d) * 400
            kite_y = my + (dy / d) * 400
            self._execute_move(kite_x, kite_y, 128)
            return f"KITE(d={edist:.0f})"

        # 350-800: good harass range — attack-move, abilities will cast on top
        self._execute_attack_move(ex, ey, enemy.get('z', 128))
        return f"HARASS(d={edist:.0f})"

    def _pos_under_enemy_tower(self, x, y, state):
        """Check if position (x,y) is within enemy tower range (700)."""
        for t in state['enemy_towers']:
            dx = t['x'] - x
            dy = t['y'] - y
            if math.sqrt(dx*dx + dy*dy) < 700:
                return True
        return False

    def _laning_think(self, state):
        """Laning: last hit, deny, approach low-HP creeps, or hold wave.

        Improvements over Phase 10:
        - Wider approach radius (1500) to walk towards creeps early
        - Higher approach HP threshold (350) to prepare for LH sooner
        - Ranged hero projectile compensation (approach closer before attacking)
        - Tighter LH window based on actual damage (1.3x for melee, 1.1x for ranged)
        - Prioritize lowest HP creep in kill range
        """
        attack_range = 600
        is_ranged = False
        try:
            attack_range = float(self.db.hero_attack_range(self.hero_name or ""))
            is_ranged = attack_range > 200
        except:
            pass
        my_dmg = max(state['damage'], 40)  # floor at 40 (hero base damage)

        # Ranged heroes need tighter threshold (projectile delay means less time window)
        # Melee heroes can use a wider threshold (instant hit)
        if is_ranged:
            lh_threshold = my_dmg * 1.2  # tighter for ranged — projectile travel
        else:
            lh_threshold = my_dmg * 1.5  # wider for melee — instant damage

        # === Phase 1: Find killable creeps (in attack range, HP < threshold) ===
        best_lh = None
        best_deny = None

        # For ranged: effective range slightly shorter to account for approach time
        effective_range = attack_range + 50 if is_ranged else attack_range + 100

        for c in state['enemy_creeps']:
            if c['dist'] > effective_range:
                continue
            if c['hp'] <= lh_threshold and c['hp'] > 0:
                if not best_lh or c['hp'] < best_lh['hp']:
                    best_lh = c

        for c in state['ally_creeps']:
            if c['dist'] > effective_range:
                continue
            # Deny threshold: allies can only be denied below 50% HP
            if c['hp'] <= lh_threshold and c['hp'] > 0:
                # Check deny eligibility: HP must be below ~50% of max creep HP (~275 for melee)
                if c['hp'] < 275:
                    if not best_deny or c['hp'] < best_deny['hp']:
                        best_deny = c

        # Last-hit: use attack_target for precision
        if best_lh:
            if self._execute_attack_target(best_lh['entity']):
                self._last_action = f"LH(hp={best_lh['hp']} d={best_lh['dist']:.0f})"
            else:
                self._execute_attack_move(best_lh['x'], best_lh['y'], best_lh.get('z', 128))
                self._last_action = f"LH_AM(hp={best_lh['hp']} d={best_lh['dist']:.0f})"
            self._active_mode = BOT_MODE_LANING
            return self._last_action

        if best_deny:
            if self._execute_attack_target(best_deny['entity']):
                self._last_action = f"DENY(hp={best_deny['hp']} d={best_deny['dist']:.0f})"
            else:
                self._execute_attack_move(best_deny['x'], best_deny['y'], best_deny.get('z', 128))
                self._last_action = f"DENY_AM(hp={best_deny['hp']} d={best_deny['dist']:.0f})"
            self._active_mode = BOT_MODE_LANING
            return self._last_action

        # === Phase 2: Approach low-HP creeps (getting ready for LH) ===
        # Wider radius (1500) and higher HP threshold (350) for earlier positioning
        approach_target = None
        for c in state['enemy_creeps']:
            if c['dist'] > 1500:
                continue
            if c['hp'] > 0 and c['hp'] < 350:  # getting low — position early
                if not approach_target or c['hp'] < approach_target['hp']:
                    approach_target = c

        if approach_target and approach_target['dist'] > attack_range - 100:
            # Move to get within attack range of the low-HP creep
            # For ranged heroes: get closer than max range for reliable LH
            target_range = attack_range - 150 if is_ranged else attack_range - 50
            if approach_target['dist'] > target_range:
                self._execute_move(approach_target['x'], approach_target['y'], 128)
                self._active_mode = BOT_MODE_LANING
                return f"LH_APPROACH(hp={approach_target['hp']} d={approach_target['dist']:.0f})"

        # === Phase 3: Hold position near creep wave ===
        # Stay close to the wave center for quick reaction
        if state['ally_creeps_nearby']:
            cx = sum(c['x'] for c in state['ally_creeps_nearby']) / len(state['ally_creeps_nearby'])
            cy = sum(c['y'] for c in state['ally_creeps_nearby']) / len(state['ally_creeps_nearby'])
            # Stand slightly behind wave (closer for melee, further for ranged)
            offset = 150 if is_ranged else 50  # ranged can stand further back safely
            fx, fy = state['fountain'][0], state['fountain'][1]
            dx = fx - cx
            dy = fy - cy
            d = math.sqrt(dx*dx + dy*dy) or 1.0
            hold_x = cx + (dx / d) * offset
            hold_y = cy + (dy / d) * offset
            self._execute_move(hold_x, hold_y, 128)
            self._active_mode = BOT_MODE_LANING
            return f"HOLD_WAVE({hold_x:.0f},{hold_y:.0f})"

        return None

    # ─── Item Purchasing Logic ─────────────────────────────────

    # Simple item build order (universal, works for any hero)
    _ITEM_BUILD = [
        ("item_tango", 90),
        ("item_branches", 50),
        ("item_branches", 50),
        ("item_magic_stick", 200),
        ("item_boots", 500),
        ("item_magic_wand", 450),   # recipe (needs stick + 2 branches)
        ("item_wind_lace", 250),
        ("item_ring_of_basilius", 425),
    ]

    def _try_buy_items(self, state=None):
        """Periodically buy next item in build order via dota_purchase_item.

        dota_purchase_item works from anywhere — items go to stash if not at shop,
        and are moved to inventory when hero visits fountain.
        Dota rejects silently if insufficient gold (no harm).
        Rate limited to once every 10s to avoid spam.
        """
        if not hasattr(self, '_last_buy_check'):
            self._last_buy_check = 0
            self._buy_index = 0

        if time.time() - self._last_buy_check < 10:
            return
        self._last_buy_check = time.time()

        if self._buy_index >= len(self._ITEM_BUILD):
            return

        item_name, cost = self._ITEM_BUILD[self._buy_index]
        self.cmd.purchase_item(item_name)
        self._buy_index += 1
        self.log.action(f"Buy:{item_name}")
        print(f"  [buy] {item_name}")

    def _advance_think(self, state):
        """Advance along assigned lane towards the front."""
        # Ensure we have a TP scroll for emergencies (buy every 30s if missing)
        if not hasattr(self, '_last_tp_check'):
            self._last_tp_check = 0
        if time.time() - self._last_tp_check > 30:
            self._last_tp_check = time.time()
            tp_eidx, _ = self._find_tp_in_inventory()
            if not tp_eidx:
                last_buy = getattr(self, '_last_tp_buy_time', 0)
                if time.time() - last_buy > 15:
                    self._buy_tp()
                    setattr(self, '_last_tp_buy_time', time.time())

        # Try buying items from build order
        self._try_buy_items(state)

        # If at fountain, walk to lane
        if state['dist_fountain'] < 2000:
            wps = LANE_WAYPOINTS[self._assigned_lane]
            wp = wps[len(wps) // 3]
            self._execute_move(wp[0], wp[1], 128)
            self._active_mode = BOT_MODE_LANING
            return f"WALK_TO_LANE({wp[0]:.0f},{wp[1]:.0f})"

        wps = LANE_WAYPOINTS[self._assigned_lane]
        if self._our_team == TEAM_DIRE:
            wps = list(reversed(wps))

        # Don't advance beyond the front — stop before enemy towers
        # Find which waypoint we're closest to
        my_x, my_y = state['x'], state['y']
        min_dist = 99999
        closest_idx = 0
        for idx, wp in enumerate(wps):
            dx = wp[0] - my_x
            dy = wp[1] - my_y
            d2 = math.sqrt(dx*dx + dy*dy)
            if d2 < min_dist:
                min_dist = d2
                closest_idx = idx

        # Target = next waypoint, but don't walk into enemy tower range
        next_idx = min(closest_idx + 1, len(wps) - 1)
        target = wps[next_idx]

        # Safety: if next waypoint is under enemy tower, stay at current
        if self._pos_under_enemy_tower(target[0], target[1], state):
            target = wps[closest_idx]

        self._execute_attack_move(target[0], target[1], 128)
        self._active_mode = BOT_MODE_LANING
        return f"ADVANCE({target[0]:.0f},{target[1]:.0f})"

    # ─── think_once: Python Brain replaces Lua ────────────────

    def think_once(self):
        """Execute one Think tick — PYTHON BRAIN (Lua brain disabled).

        Order:
        1. Evaluate game state
        2. Python brain decision tree (survival > combat > laning > advance)
        3. Cast abilities (Python fallback)
        4. Level up abilities (Lua AbilityLevelUpThink if available)
        """
        # Refresh entity cache
        self.cache.update(self.game, self.mem)
        self._find_our_hero()

        # Evaluate state
        state = self._evaluate_state()
        if not state:
            return

        # Trace: per-tick state summary
        if self.trace.enabled:
            self.trace.event("State",
                f"pos=({state['x']:.0f},{state['y']:.0f}) hp={state['hp']}/{state['max_hp']} "
                f"mana={state['mana']:.0f}/{state['max_mana']:.0f} lv={state['level']} "
                f"enemies={len(state['enemy_heroes'])} creeps={len(state['enemy_creeps'])}+{len(state['ally_creeps'])} "
                f"towers_enemy={len(state['enemy_towers'])} under_tower={state['under_enemy_tower']}")

        # Python brain decision tree
        action = "NONE"
        try:
            action = self._python_brain_think(state)
            self._last_action = action or "NONE"
        except Exception as e:
            self.log.error("python_brain", str(e))
            self.trace.event("Brain", f"ERROR: {e}")

        self.algo_log.event("BRAIN", action)

        # Set mode for ability casting context
        if state['enemy_hero_near']:
            self._active_mode = BOT_MODE_ATTACK
            self._active_mode_desire = 0.75

        # Buy items (works best near fountain/shop)
        self._try_buy_items(state)

        # Cast abilities — Python direct casting
        self._python_cast_fallback()

        # Level up abilities via Lua if available
        try:
            env = getattr(self, '_ability_usage_env', None)
            if env:
                fn = getattr(env, 'AbilityLevelUpThink', None)
                if fn:
                    fn()
        except Exception as e:
            self.log.error("LevelUp", str(e))

    def _inject_algo_trace(self):
        """Inject Lua-side monkey-patches for algorithm tracing.
        Wraps BotBuild.ConsiderQ/W/E/R to log return values,
        and adds deep diagnostic eval before AbilityUsageThink.
        """
        if not self.algo_log.enabled and not self.trace.enabled:
            return

        g = self.lua.globals()

        # Register _TRACE global that calls back into Python algo_log
        def _trace_fn(cat, name, desire, motive):
            cat_s = str(cat) if cat else "?"
            name_s = str(name) if name else "?"
            desire_f = float(desire) if desire is not None else 0
            motive_s = str(motive) if motive else ""
            self.algo_log.ability_trace(name_s, desire_f, motive_s)
        g._TRACE = _trace_fn

        # Register _TRACE_EVENT for generic events
        def _trace_event_fn(cat, msg):
            self.algo_log.event(str(cat) if cat else "?", str(msg) if msg else "")
        g._TRACE_EVENT = _trace_event_fn

        # Wrap BotBuild.ConsiderQ/W/E/R if BotBuild exists
        try:
            bb = g.BotBuild
            if not bb:
                print("  [algo_trace] BotBuild is nil, skipping ConsiderX wraps")
                return
        except:
            print("  [algo_trace] BotBuild not found, skipping ConsiderX wraps")
            return

        # Inject Lua wrappers
        try:
            self.lua.execute('''
                if BotBuild and not BotBuild._algo_wrapped then
                    BotBuild._algo_wrapped = true

                    -- Wrap ConsiderQ/W/E/R to log return values
                    local fns = {"ConsiderQ", "ConsiderW", "ConsiderE", "ConsiderR"}
                    for _, fname in ipairs(fns) do
                        local orig = BotBuild[fname]
                        if orig then
                            BotBuild["_orig_" .. fname] = orig
                            BotBuild[fname] = function(...)
                                local ok, d, loc, m = pcall(orig, ...)
                                if ok then
                                    _TRACE("ABILITY", fname, d or 0, m or "")
                                    return d, loc, m
                                else
                                    _TRACE("ABILITY", fname, -1, "ERROR:" .. tostring(d))
                                    return 0
                                end
                            end
                        end
                    end

                    -- Wrap SkillsComplement to log pre-call state
                    local origSC = BotBuild.SkillsComplement
                    if origSC then
                        BotBuild._orig_SkillsComplement = origSC
                        BotBuild.SkillsComplement = function(...)
                            local bot = GetBot()
                            local ok2, J = pcall(require, GetScriptDirectory().."/FunLib/jmz_func")
                            if ok2 and J then
                                local canNotUse = J.CanNotUseAbility(bot)
                                local isInvis = bot:IsInvisible()
                                local mode = bot:GetActiveMode()
                                local goingOn = false
                                pcall(function() goingOn = J.IsGoingOnSomeone(bot) end)
                                local nMP = bot:GetMana() / math.max(bot:GetMaxMana(), 1)
                                local nHP = bot:GetHealth() / math.max(bot:GetMaxHealth(), 1)
                                local target = J.GetProperTarget(bot)
                                local enemies = J.GetNearbyHeroes(bot, 1600, true, BOT_MODE_NONE)
                                local laningPhase = false
                                pcall(function() laningPhase = J.IsInLaningPhase() end)

                                _TRACE_EVENT("PRE_SKILLS", string.format(
                                    "canNotUse=%s invis=%s mode=%d goingOn=%s mp=%.0f%% hp=%.0f%% target=%s enemies=%d laning=%s",
                                    tostring(canNotUse), tostring(isInvis), mode,
                                    tostring(goingOn), nMP*100, nHP*100,
                                    tostring(target ~= nil), #enemies, tostring(laningPhase)
                                ))
                            end
                            return origSC(...)
                        end
                    end
                end
            ''')
            print("[+] Algo trace: ConsiderQ/W/E/R + SkillsComplement wrapped")
        except Exception as e:
            print(f"  [!] Algo trace injection failed: {e}")

        # Replace AbilityUsageThink in sandbox with traced version
        # The sandbox's copy may not see our _G.BotBuild wrappers due to Lua 5.4
        # env scoping. So we inject a new AbilityUsageThink that explicitly calls
        # the wrapped BotBuild from _G.
        if self._ability_usage_env:
            try:
                self.lua.execute('''
                    _aut_replacement = function()
                        local bot = GetBot()
                        local botName = bot:GetUnitName()

                        -- Guard 1: basic checks (same as original)
                        if bot:IsInvulnerable() or not bot:IsHero() or not bot:IsAlive()
                           or not string.find(botName, "hero") or bot:IsIllusion() then
                            return
                        end

                        -- Guard 2: frame rate limiting
                        if bot.lastAbilityFrameProcessTime == nil then
                            bot.lastAbilityFrameProcessTime = DotaTime()
                        end
                        local Customize = Customize or {}
                        local thinkLess = (Customize.Enable and Customize.ThinkLess) or 1
                        if (DotaTime() - bot.lastAbilityFrameProcessTime < ((bot.frameProcessTime or 0.1) * (1 + thinkLess)))
                           and bot.isBear == nil then
                            return
                        end
                        bot.lastAbilityFrameProcessTime = DotaTime()

                        -- Guard 3: illusion check
                        local J = require(GetScriptDirectory().."/FunLib/jmz_func")
                        if J.IsNoAbilityIllution(bot) then return end

                        -- Call SkillsComplement from _G.BotBuild (wrapped)
                        if BotBuild and BotBuild.SkillsComplement then
                            local ok, err = pcall(BotBuild.SkillsComplement)
                            if not ok then
                                _TRACE_EVENT("SC_ERROR", tostring(err))
                            end
                        end
                    end
                ''')
                # Replace in the sandbox env
                self._ability_usage_env.AbilityUsageThink = self.lua.eval('_aut_replacement')
                print("[+] Algo trace: AbilityUsageThink replaced with traced version")
            except Exception as e:
                print(f"  [!] AbilityUsageThink replacement failed: {e}")

    # ─── TP Scroll Logic ──────────────────────────────────────

    def _find_tp_in_inventory(self):
        """Find TP scroll in inventory. Returns (entity_index, slot) or (None, None).

        TP scroll is typically in slot 15 (dedicated TP slot), but may also be
        in main inventory slots 0-5. Checks slot 15 first, then main slots.
        """
        if not self._our_hero_entity:
            return None, None
        try:
            items = self.game.get_hero_items(
                self._our_hero_entity,
                handle_index=self.cache.handle_index
            )
            # Check slot 15 (TP slot) first, then main slots
            for item in sorted(items, key=lambda i: (0 if i['slot'] == 15 else 1, i['slot'])):
                if item.get('name') == 'item_tpscroll':
                    handle = item.get('handle', 0)
                    if handle:
                        ent_idx = handle & 0x7FFF
                        return ent_idx, item['slot']
            return None, None
        except:
            return None, None

    def _buy_tp(self):
        """Purchase TP scroll via console command."""
        self.cmd.execute("dota_purchase_item item_tpscroll")
        self.log.action("BuyTP")
        self.trace.event("TP", "bought TP scroll")

    def _tp_to_position(self, x, y, z=128):
        """Use TP scroll to teleport to position (usually a tower).

        Returns True if TP order was sent, False if no TP available.
        """
        tp_eidx, tp_slot = self._find_tp_in_inventory()
        if not tp_eidx:
            return False
        self.cmd.cast_position(ability_index=tp_eidx, x=float(x), y=float(y), z=float(z))
        self._last_action = f"TP({x:.0f},{y:.0f})"
        self._last_action_time = time.time()
        self.log.action("TP")
        self.trace.event("TP", f"TP to ({x:.0f},{y:.0f})")
        return True

    def _tp_to_nearest_tower(self, state):
        """TP to the nearest allied tower to the lane front (for getting back to lane).

        For respawn: TP to tower closest to mid lane front.
        Returns True if TP was sent.
        """
        # Find allied towers, prefer mid > top/bot
        best_tower = None
        best_dist_to_lane = 99999
        # Mid lane center as reference point
        lane_ref = (0, 0)
        for t in state.get('ally_towers', []):
            dx = t['x'] - lane_ref[0]
            dy = t['y'] - lane_ref[1]
            dist = math.sqrt(dx*dx + dy*dy)
            if dist < best_dist_to_lane:
                best_dist_to_lane = dist
                best_tower = t

        if best_tower:
            return self._tp_to_position(best_tower['x'], best_tower['y'], best_tower.get('z', 128))
        return False

    def _tp_home(self, state):
        """TP to fountain when low HP and far from base.

        Returns True if TP was sent.
        """
        fx, fy, fz = state['fountain']
        return self._tp_to_position(fx, fy, fz)

    def _precache_cast_types(self):
        """Pre-cache ability cast types from game_data JSON (no 300ms probing).

        Called once after hero abilities are known. Uses AbilityBehavior flags
        to determine target/no_target/position for each slot.
        """
        if not hasattr(self, '_ab_cast_type'):
            self._ab_cast_type = {}
        if not self._our_hero_entity:
            return

        try:
            ab_count = self.mem.read_i32(self._our_hero_entity + BaseNPC.ABILITIES_COUNT)
            ab_base = self.mem.read_ptr(self._our_hero_entity + BaseNPC.ABILITIES_PTR)
            if not ab_base or ab_count <= 0:
                return
        except:
            return

        for slot_idx in [0, 1, 2, 3, 4, 5]:  # Q, W, E, D, F, R
            if slot_idx in self._ab_cast_type:
                continue  # already cached
            if slot_idx >= ab_count:
                continue
            try:
                handle = self.mem.read_u32(ab_base + slot_idx * 4)
                if not handle or handle == 0xFFFFFFFF:
                    continue
                ab_ent = self.game.resolve_handle(handle)
                if not ab_ent:
                    continue
                # Read ability name from identity
                ident = self.mem.read_ptr(ab_ent + 0x10)
                if not ident or ident < 0x10000:
                    continue
                name_ptr = self.mem.read_ptr(ident + EntityIdentity.DESIGNER_NAME)
                if not name_ptr or name_ptr < 0x10000:
                    continue
                ab_name = self.mem.read_string(name_ptr, 64)
                if not ab_name:
                    continue
                # Look up cast type from JSON
                ct = self.db.ability_cast_type(ab_name)
                if ct:
                    self._ab_cast_type[slot_idx] = ct
                    self.trace.event("CastTypeCache", f"slot{slot_idx} {ab_name} → {ct}")
                else:
                    # Passive/hidden — mark as skip
                    self._ab_cast_type[slot_idx] = "passive"
            except:
                continue

        if self._ab_cast_type:
            cached = {k: v for k, v in self._ab_cast_type.items() if v != "passive"}
            if cached:
                print(f"  [+] Cast type pre-cache: {cached}")

    def _python_cast_fallback(self):
        """Python fallback: cast ALL ready abilities on nearby enemies.
        Uses PrepareUnitOrders directly. Cast type determined from game_data JSON
        (pre-cached), with mana probe as last-resort fallback."""
        hero_data = self._get_hero_data()
        if not hero_data:
            return
        my_x, my_y = hero_data['x'], hero_data['y']
        my_hp_pct = hero_data.get('hp', 0) / max(hero_data.get('max_hp', 1), 1)

        if my_hp_pct < 0.25:
            return

        # Find nearest enemy hero
        nearest_enemy = None
        nearest_dist = 99999
        for h in self.cache.heroes:
            if h.get('team') == self._our_team:
                continue
            hp = h.get('hp', 0)
            if hp <= 0 or hp > 50000:
                continue
            dx = h['x'] - my_x
            dy = h['y'] - my_y
            dist = math.sqrt(dx*dx + dy*dy)
            if dist < nearest_dist:
                nearest_dist = dist
                nearest_enemy = h

        if not nearest_enemy or nearest_dist > 1600:
            return

        # Resolve target entity index
        t_entity = nearest_enemy.get('entity', 0)
        target_eidx = 0
        if t_entity:
            try:
                t_ident = self.mem.read_ptr(t_entity + 0x10)
                if t_ident and t_ident > 0x10000:
                    target_eidx = self.mem.read_u32(t_ident + 0x10) & 0x7FFF
            except:
                pass
        if not target_eidx:
            return

        ex, ey, ez = nearest_enemy['x'], nearest_enemy['y'], nearest_enemy.get('z', 128)

        # Init per-ability cast type cache
        if not hasattr(self, '_ab_cast_type'):
            self._ab_cast_type = {}
        # Pre-cache on first call (or when empty)
        if not self._ab_cast_type:
            self._precache_cast_types()

        ab_count = self.mem.read_i32(self._our_hero_entity + BaseNPC.ABILITIES_COUNT)
        ab_base = self.mem.read_ptr(self._our_hero_entity + BaseNPC.ABILITIES_PTR)
        if not ab_base or ab_count <= 0:
            return

        cast_count = 0
        for slot_idx in [0, 1, 2, 5]:  # Q, W, E, R
            if slot_idx >= ab_count:
                continue
            try:
                handle = self.mem.read_u32(ab_base + slot_idx * 4)
                if not handle or handle == 0xFFFFFFFF:
                    continue
                ent_idx = handle & 0x7FFF
                ab_ent = self.game.resolve_handle(handle)
                if not ab_ent:
                    continue

                ab_level = self.mem.read_i32(ab_ent + BaseAbility.LEVEL)
                ab_cd = self.mem.read_f32(ab_ent + BaseAbility.COOLDOWN)
                ab_cost = self.mem.read_i32(ab_ent + BaseAbility.MANA_COST)
                ab_hidden = self.mem.read_u8(ab_ent + BaseAbility.HIDDEN)
                my_mana = self.mem.read_f32(self._our_hero_entity + BaseNPC.MANA)

                if ab_level <= 0 or ab_cd > 0.1 or ab_hidden or ab_cost == 0 or my_mana < ab_cost:
                    continue

                # Use pre-cached cast type
                ct = self._ab_cast_type.get(slot_idx)
                if ct in ("unknown", "passive"):
                    continue  # skip non-castable
                if ct == "target":
                    self.cmd.cast_target(ability_index=ent_idx, target_index=target_eidx)
                elif ct == "no_target":
                    self.cmd.cast_no_target(ability_index=ent_idx)
                elif ct == "position":
                    self.cmd.cast_position(ent_idx, ex, ey, ez)
                else:
                    # Fallback: mana probe discovery (only if game_data didn't have info)
                    mana_before = my_mana
                    self.cmd.cast_target(ability_index=ent_idx, target_index=target_eidx)
                    time.sleep(0.3)
                    m1 = self.mem.read_f32(self._our_hero_entity + BaseNPC.MANA)
                    if mana_before - m1 > ab_cost * 0.3:
                        self._ab_cast_type[slot_idx] = "target"
                    else:
                        self.cmd.cast_no_target(ability_index=ent_idx)
                        time.sleep(0.3)
                        m2 = self.mem.read_f32(self._our_hero_entity + BaseNPC.MANA)
                        if mana_before - m2 > ab_cost * 0.3:
                            self._ab_cast_type[slot_idx] = "no_target"
                        else:
                            self.cmd.cast_position(ent_idx, ex, ey, ez)
                            time.sleep(0.3)
                            m3 = self.mem.read_f32(self._our_hero_entity + BaseNPC.MANA)
                            if mana_before - m3 > ab_cost * 0.3:
                                self._ab_cast_type[slot_idx] = "position"
                            else:
                                self._ab_cast_type[slot_idx] = "unknown"
                                self.log.error("PyCast", f"slot{slot_idx}: all cast types failed")
                                continue

                cast_count += 1
                self.log.action(f"PyCast:slot{slot_idx}")
                self.algo_log.event("PY_CAST", f"slot={slot_idx} type={self._ab_cast_type.get(slot_idx,'?')} dist={nearest_dist:.0f}")
                time.sleep(0.05)  # small delay between abilities
            except Exception as e:
                self.log.error("PyCast", f"slot{slot_idx}: {e}")

        if cast_count > 0:
            self._last_action = f"PY_CAST(x{cast_count})"
            self._last_action_time = time.time()
            self.trace.event("PyCast", f"{cast_count} abilities on {nearest_enemy.get('name','?')} dist={nearest_dist:.0f}")

    @staticmethod
    def _mode_name_to_const(name: str) -> int:
        """Map mode name to BOT_MODE_* constant."""
        mapping = {
            "laning": BOT_MODE_LANING, "attack": BOT_MODE_ATTACK,
            "retreat": BOT_MODE_RETREAT, "farm": BOT_MODE_FARM,
            "roam": BOT_MODE_ROAM, "team_roam": BOT_MODE_TEAM_ROAM,
            "push_top": BOT_MODE_PUSH_TOWER_TOP, "push_mid": BOT_MODE_PUSH_TOWER_MID,
            "push_bot": BOT_MODE_PUSH_TOWER_BOT,
            "defend_top": BOT_MODE_DEFEND_TOWER_TOP, "defend_mid": BOT_MODE_DEFEND_TOWER_MID,
            "defend_bot": BOT_MODE_DEFEND_TOWER_BOT,
        }
        return mapping.get(name, BOT_MODE_NONE)

    def _fallback_think(self):
        """Legacy fallback — redirects to Python brain."""
        state = self._evaluate_state()
        if state:
            self._python_brain_think(state)

    def _run_ability_think(self):
        """Call AbilityUsageThink + AbilityLevelUpThink from ability_item_usage_generic.lua."""
        env = getattr(self, '_ability_usage_env', None)
        if not env:
            self.trace.event("AbilityThink", "SKIP: no _ability_usage_env")
            return

        # AbilityLevelUpThink — auto level-up
        try:
            fn = getattr(env, 'AbilityLevelUpThink', None)
            if fn:
                action_before = self._last_action_time
                fn()
                if self._last_action_time > action_before:
                    self.log.action("LevelUp")
                    self.trace.event("AbilityThink", "LevelUp action produced")
            else:
                self.trace.event("AbilityThink", "AbilityLevelUpThink fn=nil")
        except Exception as e:
            self.log.error("AbilityLevelUpThink", str(e))
            self.trace.event("AbilityThink", f"AbilityLevelUpThink ERROR: {e}")

        # AbilityUsageThink — hero skill usage (SkillsComplement)
        # Pre-call diagnostics: check what the Lua guard clause will see
        if self.trace.enabled:
            try:
                bh = self._make_bot_handle()
                d = self._get_hero_data()
                self.trace.event("AbilityThink:guard",
                    f"IsAlive={bh.IsAlive()} IsHero={bh.IsHero()} "
                    f"IsInvuln={bh.IsInvulnerable()} IsIllusion={bh.IsIllusion()} "
                    f"name={bh.GetUnitName()} mana={d.get('mana',0):.0f}/{d.get('max_mana',0):.0f}" if d else "NO_DATA")
            except Exception as e:
                self.trace.event("AbilityThink:guard", f"diagnostic error: {e}")
        try:
            fn = getattr(env, 'AbilityUsageThink', None)
            if fn:
                # Lua-side deep diagnostic: check what ConsiderQ will see
                if self.trace.enabled or self.algo_log.enabled:
                    try:
                        diag = self.lua.eval('''
                            (function()
                                local bot = GetBot()
                                local J = require(GetScriptDirectory().."/FunLib/jmz_func")
                                local canNotUse = J.CanNotUseAbility(bot)
                                local isInvis = bot:IsInvisible()
                                local mode = bot:GetActiveMode()
                                local target = J.GetProperTarget(bot)
                                local enemies = J.GetNearbyHeroes(bot, 1600, true, BOT_MODE_NONE)
                                local nearEn = J.GetNearbyHeroes(bot, 1200, true, BOT_MODE_NONE)
                                local goingOn = false
                                pcall(function() goingOn = J.IsGoingOnSomeone(bot) end)
                                local laningPhase = false
                                pcall(function() laningPhase = J.IsInLaningPhase() end)
                                local nMP = bot:GetMana()/math.max(bot:GetMaxMana(),1)
                                local nHP = bot:GetHealth()/math.max(bot:GetMaxHealth(),1)

                                -- Deep ConsiderQ check: which scenario would fire?
                                local qDiag = ""
                                if BotBuild then
                                    local abilityQ = bot:GetAbilityInSlot(0)
                                    if abilityQ and abilityQ:IsFullyCastable() then
                                        local castRange = abilityQ:GetCastRange() + 20
                                        local inRange = J.GetNearbyHeroes(bot, castRange, true, BOT_MODE_NONE)
                                        local bonus = J.GetNearbyHeroes(bot, castRange + 200, true, BOT_MODE_NONE)
                                        qDiag = string.format("Qcastable=Y range=%d inRange=%d bonus=%d",
                                            castRange, #inRange, #bonus)
                                        -- Check aggression path
                                        if target then
                                            local canCast = false
                                            pcall(function() canCast = J.CanCastOnNonMagicImmune(target) end)
                                            local inR = false
                                            pcall(function() inR = J.IsInRange(target, bot, castRange + 300) end)
                                            qDiag = qDiag .. string.format(" tgtCanCast=%s tgtInRange=%s",
                                                tostring(canCast), tostring(inR))
                                        end
                                    else
                                        qDiag = "Qcastable=N"
                                    end
                                end

                                return string.format(
                                    "canNotUse=%s invis=%s mode=%d goingOn=%s target=%s en1600=%d en1200=%d laning=%s mp=%.0f%% hp=%.0f%% | %s",
                                    tostring(canNotUse), tostring(isInvis), mode,
                                    tostring(goingOn), tostring(target ~= nil),
                                    #enemies, #nearEn, tostring(laningPhase),
                                    nMP * 100, nHP * 100, qDiag)
                            end)()
                        ''')
                        self.trace.event("LuaDiag", str(diag))
                        self.algo_log.event("DIAG", str(diag))
                    except Exception as e:
                        self.trace.event("LuaDiag", f"ERROR: {e}")

                action_before = self._last_action_time
                self.trace.event("AbilityThink", "calling AbilityUsageThink...")
                fn()
                if self._last_action_time > action_before:
                    self.log.action("AbilityUsage")
                    self.trace.event("AbilityThink", f"AbilityUsage CAST: {self._last_action}")
                    self.algo_log.event("CAST", self._last_action)
                else:
                    self.trace.event("AbilityThink", "AbilityUsageThink returned (no cast)")
            else:
                self.trace.event("AbilityThink", "AbilityUsageThink fn=nil")
        except Exception as e:
            self.log.error("AbilityUsageThink", str(e))
            self.trace.event("AbilityThink", f"AbilityUsageThink ERROR: {e}")

        # ItemUsageThink — auto item usage
        try:
            fn = getattr(env, 'ItemUsageThink', None)
            if fn:
                fn()
        except Exception as e:
            self.log.error("ItemUsageThink", str(e))

    def run(self, tick_rate: float = 0.1, duration: float = 0):
        """Run the bot Think loop.

        Args:
            tick_rate: seconds between Think calls (0.1 = 10Hz)
            duration: seconds to run (0 = forever)
        """
        print(f"[+] Bot Brain running at {1/tick_rate:.0f}Hz")
        start = time.time()
        tick = 0
        try:
            while True:
                if duration > 0 and time.time() - start > duration:
                    break
                try:
                    self.think_once()
                except Exception as e:
                    self.log.error("think_once", str(e))
                self.log.tick_done()
                self.trace.tick_done()
                # Algo log: per-tick state summary + flush
                if self.algo_log.enabled:
                    d = self._get_hero_data()
                    if d:
                        state_str = (f"hp={d.get('hp',0)}/{d.get('max_hp',0)} "
                                     f"mp={d.get('mana',0):.0f} "
                                     f"pos=({d['x']:.0f},{d['y']:.0f}) "
                                     f"act={self._last_action}")
                    else:
                        state_str = "no_data"
                    self.algo_log.tick_done(state_str)
                tick += 1
                if tick % 50 == 0 and not self.algo_log.enabled:
                    d = self._get_hero_data()
                    pos = f"({d['x']:.0f},{d['y']:.0f})" if d else "?"
                    hp = f"{d.get('hp',0)}/{d.get('max_hp',0)}" if d else "?"
                    mana = f"{d.get('mana',0):.0f}" if d else "?"
                    print(f"  [tick {tick}] pos={pos} hp={hp} mana={mana} mode={self._active_mode} action={self._last_action}")
                time.sleep(tick_rate)
        except KeyboardInterrupt:
            pass
        print(f"\n[+] Bot Brain stopped after {tick} ticks ({time.time()-start:.0f}s)")
        self.log.print_summary()
        self.algo_log.close()
