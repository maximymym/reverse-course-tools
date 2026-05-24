"""
Diff human controller (slot 0) vs bot controllers (slot 1, 2).
Find the flag that makes a bot a bot — then flip it on our hero.
"""
import sys, os, time, struct, math
sys.path.insert(0, os.path.dirname(__file__))
from cheat.memory import DotaMemory
from cheat.game_state import DotaGame
from cheat.offsets import (GameState, BaseEntity, GameSceneNode, BaseNPC,
                           EntitySystem, EntityIdentity)


def scan_all_entities_raw(mem, entity_system):
    results = []
    seen = set()
    for list_off in EntitySystem.CHUNK_LISTS:
        chunk_list = mem.read_ptr(entity_system + list_off)
        if not chunk_list or chunk_list < 0x10000:
            continue
        for ci in range(EntitySystem.MAX_CHUNKS):
            try:
                chunk_ptr = mem.read_ptr(chunk_list + ci * 8)
            except:
                continue
            if not chunk_ptr or chunk_ptr < 0x10000:
                continue
            try:
                chunk_data = mem.pm.read_bytes(chunk_ptr, EntitySystem.SLOTS_PER_CHUNK * EntityIdentity.STRIDE)
            except:
                continue
            for si in range(EntitySystem.SLOTS_PER_CHUNK):
                off = si * EntityIdentity.STRIDE
                ent_ptr = struct.unpack("<Q", chunk_data[off:off+8])[0]
                if ent_ptr and ent_ptr > 0x10000 and ent_ptr not in seen:
                    seen.add(ent_ptr)
                    results.append((ent_ptr, chunk_ptr + off))
    return results


def main():
    mem = DotaMemory()
    print(f"[+] PID {mem.pid}")
    game = DotaGame(mem)
    game.init()
    print(f"[+] State: {game.get_game_state()}")

    all_ents = scan_all_entities_raw(mem, game.entity_system)
    print(f"[+] Unique entities: {len(all_ents)}")

    # Find all controllers (deduplicated)
    controllers = []
    for ent_ptr, ident_addr in all_ents:
        try:
            dname_ptr = mem.read_ptr(ident_addr + EntityIdentity.DESIGNER_NAME)
            if not dname_ptr or dname_ptr < 0x10000:
                continue
            dname = mem.read_string(dname_ptr, 64)
            if dname != "dota_player_controller":
                continue
            slot = mem.read_i32(ent_ptr + 0x908)
            pname = mem.read_string(ent_ptr + 0x6E0, 32)
            controllers.append({"ptr": ent_ptr, "slot": slot, "name": pname})
        except:
            continue

    # Deduplicate by slot
    seen_slots = {}
    for c in controllers:
        if c["slot"] not in seen_slots:
            seen_slots[c["slot"]] = c
    controllers = list(seen_slots.values())

    print(f"\n=== CONTROLLERS ({len(controllers)}) ===")
    for c in sorted(controllers, key=lambda x: x["slot"]):
        print(f"  slot={c['slot']:3d} name='{c['name']}' ptr={hex(c['ptr'])}")

    # Find human (slot 0) and first bot
    human = None
    bot = None
    for c in controllers:
        if c["slot"] == 0:
            human = c
        elif c["slot"] > 0 and bot is None:
            bot = c

    if not human or not bot:
        print("[!] Need both human and bot controllers")
        return

    print(f"\n=== DIFF: human (slot {human['slot']}, {human['name']}) vs bot (slot {bot['slot']}, {bot['name']}) ===")

    # Read large blocks from both
    dump_size = 0x1000  # 4KB should cover the controller
    try:
        human_data = mem.pm.read_bytes(human["ptr"], dump_size)
        bot_data = mem.pm.read_bytes(bot["ptr"], dump_size)
    except Exception as e:
        print(f"[!] Read error: {e}")
        return

    # Find differences
    print(f"\n  Differences (byte-level):")
    diffs = []
    for off in range(dump_size):
        if human_data[off] != bot_data[off]:
            diffs.append(off)

    # Group nearby diffs
    print(f"  Total different bytes: {len(diffs)}")
    print(f"\n  {'Offset':>8s}  {'Human':>20s}  {'Bot':>20s}  Notes")
    print("  " + "-" * 75)

    # Show diffs with context, focusing on single-byte flags
    i = 0
    while i < len(diffs):
        off = diffs[i]
        # Check if this is part of a larger diff region
        region_end = off
        while i + 1 < len(diffs) and diffs[i+1] <= region_end + 4:
            i += 1
            region_end = diffs[i]

        region_size = region_end - off + 1

        if region_size <= 8:
            # Small diff — likely a flag or small value
            h_val = human_data[off:off+region_size]
            b_val = bot_data[off:off+region_size]

            h_hex = ' '.join(f'{b:02X}' for b in h_val)
            b_hex = ' '.join(f'{b:02X}' for b in b_val)

            note = ""
            # Check for bool-like diffs (0 vs 1)
            if region_size == 1 and h_val[0] in (0, 1) and b_val[0] in (0, 1):
                if h_val[0] == 0 and b_val[0] == 1:
                    note = "<<< BOOL: human=0, bot=1 (CANDIDATE m_bIsBot?)"
                elif h_val[0] == 1 and b_val[0] == 0:
                    note = "<<< BOOL: human=1, bot=0"
            elif region_size <= 4:
                h_i32 = struct.unpack("<i", human_data[off:off+4])[0] if off + 4 <= dump_size else 0
                b_i32 = struct.unpack("<i", bot_data[off:off+4])[0] if off + 4 <= dump_size else 0
                note = f"i32: {h_i32} vs {b_i32}"

            print(f"  +0x{off:04X}    {h_hex:>20s}  {b_hex:>20s}  {note}")
        else:
            # Large diff region — probably pointer or string
            h_hex = ' '.join(f'{human_data[off+j]:02X}' for j in range(min(8, region_size)))
            b_hex = ' '.join(f'{bot_data[off+j]:02X}' for j in range(min(8, region_size)))
            print(f"  +0x{off:04X}    {h_hex+'...':>20s}  {b_hex+'...':>20s}  ({region_size} bytes differ)")

        i += 1

    # === Check specific known offsets ===
    print(f"\n=== KEY FIELDS COMPARISON ===")

    key_fields = [
        (0x6E0, "player_name", "str"),
        (0x708, "steam_id?", "u64"),
        (0x768, "steam_id2?", "u64"),
        (0x810, "h_pawn?", "u32"),
        (0x878, "is_local?", "u8"),
        (0x908, "player_slot", "i32"),
    ]

    for off, name, typ in key_fields:
        if off + 8 > dump_size:
            continue
        if typ == "str":
            h_val = mem.read_string(human["ptr"] + off, 32)
            b_val = mem.read_string(bot["ptr"] + off, 32)
            print(f"  +0x{off:04X} {name:20s}: human='{h_val}', bot='{b_val}'")
        elif typ == "u64":
            h_val = struct.unpack("<Q", human_data[off:off+8])[0]
            b_val = struct.unpack("<Q", bot_data[off:off+8])[0]
            print(f"  +0x{off:04X} {name:20s}: human={h_val}, bot={b_val}")
        elif typ == "u32":
            h_val = struct.unpack("<I", human_data[off:off+4])[0]
            b_val = struct.unpack("<I", bot_data[off:off+4])[0]
            print(f"  +0x{off:04X} {name:20s}: human=0x{h_val:08X}, bot=0x{b_val:08X}")
        elif typ == "u8":
            print(f"  +0x{off:04X} {name:20s}: human={human_data[off]}, bot={bot_data[off]}")
        elif typ == "i32":
            h_val = struct.unpack("<i", human_data[off:off+4])[0]
            b_val = struct.unpack("<i", bot_data[off:off+4])[0]
            print(f"  +0x{off:04X} {name:20s}: human={h_val}, bot={b_val}")

    # === Find heroes belonging to each controller ===
    print(f"\n=== HERO ASSIGNMENT ===")
    heroes = []
    for ent_ptr, ident_addr in all_ents:
        try:
            hp = mem.read_i32(ent_ptr + BaseEntity.HEALTH)
            team = mem.read_u8(ent_ptr + BaseEntity.TEAM_NUM)
            if team not in (2, 3) or hp <= 0 or hp > 10000:
                continue
            uname_ptr = mem.read_ptr(ent_ptr + BaseNPC.UNIT_NAME)
            if not uname_ptr or uname_ptr < 0x10000:
                continue
            uname = mem.read_string(uname_ptr, 64)
            if "hero" not in uname:
                continue
            scene = mem.read_ptr(ent_ptr + BaseEntity.GAME_SCENE_NODE)
            x = mem.read_f32(scene + GameSceneNode.VEC_ABS_ORIGIN)
            y = mem.read_f32(scene + GameSceneNode.VEC_ABS_ORIGIN + 4)
            heroes.append({"e": ent_ptr, "name": uname, "hp": hp, "team": team, "x": x, "y": y})
        except:
            continue

    # Deduplicate heroes
    seen_heroes = {}
    for h in heroes:
        if h["e"] not in seen_heroes:
            seen_heroes[h["e"]] = h
    heroes = list(seen_heroes.values())

    print(f"  Unique heroes: {len(heroes)}")
    for h in heroes:
        t = {2: "Rad", 3: "Dire"}.get(h["team"], "?")
        print(f"    {h['name']:40s} {t} hp={h['hp']} entity={hex(h['e'])}")

    # Check how many total heroes (including Dire) exist
    print(f"\n  All heroes (including those we might miss):")
    # Try checking more entities by HP/team only (skip unit name check)
    hero_like = 0
    for ent_ptr, ident_addr in all_ents:
        try:
            hp = mem.read_i32(ent_ptr + BaseEntity.HEALTH)
            team = mem.read_u8(ent_ptr + BaseEntity.TEAM_NUM)
            if team not in (2, 3) or hp <= 0 or hp > 10000:
                continue
            level = mem.read_i32(ent_ptr + BaseNPC.CURRENT_LEVEL)
            if level < 0 or level > 30:
                continue
            uname_ptr = mem.read_ptr(ent_ptr + BaseNPC.UNIT_NAME)
            if not uname_ptr or uname_ptr < 0x10000:
                continue
            uname = mem.read_string(uname_ptr, 64)
            if "hero" in uname:
                hero_like += 1
            elif "npc_dota" in uname and "creep" not in uname and "tower" not in uname:
                scene = mem.read_ptr(ent_ptr + BaseEntity.GAME_SCENE_NODE)
                x = mem.read_f32(scene + GameSceneNode.VEC_ABS_ORIGIN)
                y = mem.read_f32(scene + GameSceneNode.VEC_ABS_ORIGIN + 4)
                print(f"    non-hero NPC: {uname:40s} team={team} hp={hp} lvl={level} pos=({x:.0f},{y:.0f})")
        except:
            continue
    print(f"  Total hero-like entities: {hero_like}")

    print("\nDone!")


if __name__ == "__main__":
    main()
