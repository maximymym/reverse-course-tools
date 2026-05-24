"""
GameDataDB — static game data lookup from JSON files.

Loads npc_heroes.json, npc_abilities.json, items.json at startup.
Provides hero stats, ability data by level, item costs, etc.

All data is static (from game files), no memory reading needed.
"""
import json
import os
import re


class GameDataDB:
    def __init__(self, data_dir: str = None):
        if data_dir is None:
            data_dir = os.path.join(os.path.dirname(os.path.dirname(__file__)), "data")
        self._heroes = {}       # "npc_dota_hero_sven" -> dict
        self._abilities = {}    # "sven_storm_bolt" -> dict
        self._items = {}        # "item_blink" -> dict
        self._hero_base = {}    # base hero template (npc_dota_hero_base)
        self._load(data_dir)

    def _load(self, data_dir: str):
        # Heroes
        heroes_path = os.path.join(data_dir, "npc_heroes.json")
        if os.path.exists(heroes_path):
            with open(heroes_path, 'r', encoding='utf-8') as f:
                raw = json.load(f)
            heroes_root = raw.get("DOTAHeroes", raw)
            self._hero_base = heroes_root.get("npc_dota_hero_base", {})
            for key, val in heroes_root.items():
                if key.startswith("npc_dota_hero_") and key != "npc_dota_hero_base":
                    self._heroes[key] = val

        # Abilities
        abilities_path = os.path.join(data_dir, "npc_abilities.json")
        if os.path.exists(abilities_path):
            with open(abilities_path, 'r', encoding='utf-8') as f:
                raw = json.load(f)
            abilities_root = raw.get("DOTAAbilities", raw)
            for key, val in abilities_root.items():
                if isinstance(val, dict) and key != "Version":
                    self._abilities[key] = val

        # Items
        items_path = os.path.join(data_dir, "items.json")
        if os.path.exists(items_path):
            with open(items_path, 'r', encoding='utf-8') as f:
                raw = json.load(f)
            items_root = raw.get("DOTAAbilities", raw)
            for key, val in items_root.items():
                if isinstance(val, dict) and key.startswith("item_"):
                    self._items[key] = val

        print(f"[+] GameDataDB: {len(self._heroes)} heroes, "
              f"{len(self._abilities)} abilities, {len(self._items)} items")

    # ─── Heroes ─────────────────────────────────────────────

    def get_hero(self, name: str) -> dict | None:
        """Get hero data by full name (e.g. 'npc_dota_hero_sven')."""
        return self._heroes.get(name)

    def get_hero_merged(self, name: str) -> dict:
        """Get hero data merged with base template."""
        base = dict(self._hero_base)
        hero = self._heroes.get(name, {})
        base.update(hero)
        return base

    def hero_abilities(self, hero_name: str) -> list[str]:
        """Get list of ability names for a hero."""
        hero = self.get_hero_merged(hero_name)
        abilities = []
        for i in range(1, 30):
            key = f"Ability{i}"
            ab = hero.get(key, "")
            if ab and ab != "generic_hidden":
                abilities.append(ab)
        return abilities

    def hero_stat(self, hero_name: str, stat: str, default=None):
        """Get a hero stat, falling back to base template."""
        hero = self.get_hero_merged(hero_name)
        return hero.get(stat, default)

    def hero_base_damage(self, hero_name: str) -> tuple[int, int]:
        """Return (min_damage, max_damage) base attack damage."""
        hero = self.get_hero_merged(hero_name)
        return (int(hero.get("AttackDamageMin", 0)),
                int(hero.get("AttackDamageMax", 0)))

    def hero_attack_range(self, hero_name: str) -> int:
        return int(self.get_hero_merged(hero_name).get("AttackRange", 150))

    def hero_move_speed(self, hero_name: str) -> int:
        return int(self.get_hero_merged(hero_name).get("MovementSpeed", 300))

    def hero_primary_attr(self, hero_name: str) -> str:
        """Returns 'DOTA_ATTRIBUTE_STRENGTH' / 'AGILITY' / 'INTELLECT' / 'ALL'."""
        return self.get_hero_merged(hero_name).get("AttributePrimary", "")

    def hero_base_stats(self, hero_name: str) -> dict:
        """Return base STR/AGI/INT and gains."""
        h = self.get_hero_merged(hero_name)
        return {
            "str": float(h.get("AttributeBaseStrength", 0)),
            "str_gain": float(h.get("AttributeStrengthGain", 0)),
            "agi": float(h.get("AttributeBaseAgility", 0)),
            "agi_gain": float(h.get("AttributeAgilityGain", 0)),
            "int": float(h.get("AttributeBaseIntelligence", 0)),
            "int_gain": float(h.get("AttributeIntelligenceGain", 0)),
        }

    def hero_base_armor(self, hero_name: str) -> float:
        return float(self.get_hero_merged(hero_name).get("ArmorPhysical", 0))

    def hero_base_hp(self, hero_name: str) -> int:
        return int(self.get_hero_merged(hero_name).get("StatusHealth", 120))

    def hero_base_mana(self, hero_name: str) -> int:
        return int(self.get_hero_merged(hero_name).get("StatusMana", 75))

    # ─── Abilities ──────────────────────────────────────────

    def get_ability(self, name: str) -> dict | None:
        """Get ability data by name (e.g. 'sven_storm_bolt')."""
        return self._abilities.get(name)

    def ability_at_level(self, name: str, level: int) -> dict:
        """Get ability stats resolved for a specific level (1-indexed).

        Parses space-separated values like "21 18 15 12" and picks
        the value at the given level index. Also resolves AbilityValues.
        """
        ab = self._abilities.get(name, {})
        result = {}

        # Standard top-level fields
        for key in ["AbilityCooldown", "AbilityManaCost", "AbilityDamage",
                     "AbilityCastRange", "AbilityCastPoint", "AbilityDuration",
                     "AbilityChannelTime", "AbilityCharges", "AbilityChargeRestoreTime"]:
            val = ab.get(key, "")
            if val:
                result[key] = self._pick_level_value(val, level)

        # AbilityValues (nested dict)
        ability_values = ab.get("AbilityValues", {})
        if isinstance(ability_values, dict):
            for vk, vv in ability_values.items():
                if isinstance(vv, dict):
                    # Nested: {"value": "...", "affected_by_aoe_increase": "1"}
                    raw = vv.get("value", "0")
                else:
                    raw = vv
                result[vk] = self._pick_level_value(str(raw), level)

        # Behavior flags
        result["behavior"] = self._parse_behavior(ab.get("AbilityBehavior", ""))
        result["damage_type"] = ab.get("AbilityUnitDamageType", "")
        result["target_team"] = ab.get("AbilityUnitTargetTeam", "")
        result["max_level"] = int(ab.get("MaxLevel", 4))

        return result

    def ability_behavior_flags(self, name: str) -> int:
        """Parse AbilityBehavior string into integer flags."""
        ab = self._abilities.get(name, {})
        return self._parse_behavior(ab.get("AbilityBehavior", ""))

    def ability_cooldown(self, name: str, level: int) -> float:
        """Get ability cooldown at level."""
        ab = self._abilities.get(name, {})
        return self._pick_level_value(ab.get("AbilityCooldown", "0"), level)

    def ability_mana_cost(self, name: str, level: int) -> float:
        """Get ability mana cost at level."""
        ab = self._abilities.get(name, {})
        return self._pick_level_value(ab.get("AbilityManaCost", "0"), level)

    def ability_damage(self, name: str, level: int) -> float:
        """Get ability damage at level."""
        ab = self._abilities.get(name, {})
        return self._pick_level_value(ab.get("AbilityDamage", "0"), level)

    def ability_cast_range(self, name: str, level: int) -> float:
        ab = self._abilities.get(name, {})
        return self._pick_level_value(ab.get("AbilityCastRange", "0"), level)

    # ─── Items ──────────────────────────────────────────────

    def get_item(self, name: str) -> dict | None:
        """Get item data by name (e.g. 'item_blink')."""
        return self._items.get(name)

    def item_cost(self, name: str) -> int:
        item = self._items.get(name, {})
        return int(item.get("ItemCost", 0))

    def item_cooldown(self, name: str) -> float:
        item = self._items.get(name, {})
        return self._pick_level_value(item.get("AbilityCooldown", "0"), 1)

    def is_secret_shop_item(self, name: str) -> bool:
        item = self._items.get(name, {})
        tags = item.get("ItemShopTags", "")
        return "secret_shop" in tags

    # ─── Helpers ────────────────────────────────────────────

    @staticmethod
    def _pick_level_value(raw: str, level: int) -> float:
        """Pick value at level from space-separated string like '21 18 15 12'."""
        if not raw or raw == "0":
            return 0.0
        parts = raw.strip().split()
        try:
            idx = max(0, min(level - 1, len(parts) - 1))
            return float(parts[idx])
        except (ValueError, IndexError):
            return 0.0

    # Behavior flag name -> bit value mapping
    _BEHAVIOR_FLAGS = {
        "DOTA_ABILITY_BEHAVIOR_HIDDEN": 1,
        "DOTA_ABILITY_BEHAVIOR_PASSIVE": 2,
        "DOTA_ABILITY_BEHAVIOR_NO_TARGET": 4,
        "DOTA_ABILITY_BEHAVIOR_UNIT_TARGET": 8,
        "DOTA_ABILITY_BEHAVIOR_POINT": 16,
        "DOTA_ABILITY_BEHAVIOR_AOE": 32,
        "DOTA_ABILITY_BEHAVIOR_CHANNELLED": 128,
        "DOTA_ABILITY_BEHAVIOR_TOGGLE": 256,
        "DOTA_ABILITY_BEHAVIOR_DIRECTIONAL": 512,
        "DOTA_ABILITY_BEHAVIOR_IMMEDIATE": 1024,
        "DOTA_ABILITY_BEHAVIOR_AUTOCAST": 2048,
        "DOTA_ABILITY_BEHAVIOR_NOT_LEARNABLE": 4096,
        "DOTA_ABILITY_BEHAVIOR_AURA": 8192,
        "DOTA_ABILITY_BEHAVIOR_ATTACK": 16384,
        "DOTA_ABILITY_BEHAVIOR_ROOT_DISABLES": 32768,
        "DOTA_ABILITY_BEHAVIOR_UNRESTRICTED": 65536,
        "DOTA_ABILITY_BEHAVIOR_DONT_RESUME_MOVEMENT": 131072,
        "DOTA_ABILITY_BEHAVIOR_DONT_RESUME_ATTACK": 262144,
        "DOTA_ABILITY_BEHAVIOR_IGNORE_CHANNEL": 1048576,
        "DOTA_ABILITY_BEHAVIOR_IGNORE_PSEUDO_QUEUE": 2097152,
    }

    # Behavior bit constants for cast type detection
    BEHAVIOR_NO_TARGET = 4
    BEHAVIOR_UNIT_TARGET = 8
    BEHAVIOR_POINT = 16
    BEHAVIOR_PASSIVE = 2
    BEHAVIOR_HIDDEN = 1
    BEHAVIOR_TOGGLE = 256

    def ability_cast_type(self, name: str) -> str | None:
        """Determine cast type from AbilityBehavior flags.

        Returns: "target", "no_target", "position", or None (passive/hidden/unknown).
        """
        ab = self._abilities.get(name, {})
        flags = self._parse_behavior(ab.get("AbilityBehavior", ""))
        if not flags:
            return None
        # Passive/hidden abilities can't be cast
        if flags & self.BEHAVIOR_PASSIVE or flags & self.BEHAVIOR_HIDDEN:
            return None
        # Priority: unit_target > point > no_target (some abilities have multiple flags)
        if flags & self.BEHAVIOR_UNIT_TARGET:
            return "target"
        if flags & self.BEHAVIOR_POINT:
            return "position"
        if flags & self.BEHAVIOR_NO_TARGET:
            return "no_target"
        if flags & self.BEHAVIOR_TOGGLE:
            return "no_target"
        return None

    @classmethod
    def _parse_behavior(cls, behavior_str: str) -> int:
        """Parse 'DOTA_ABILITY_BEHAVIOR_UNIT_TARGET | AOE' into int flags."""
        if not behavior_str:
            return 0
        flags = 0
        for part in behavior_str.split("|"):
            part = part.strip()
            if part in cls._BEHAVIOR_FLAGS:
                flags |= cls._BEHAVIOR_FLAGS[part]
        return flags
