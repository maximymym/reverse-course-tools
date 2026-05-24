"""
Full hero scan — find ALL heroes by brute-force checking HP+team+unitname on ALL entities.
Don't rely on designerName (often garbage). Check every entity in all 3 chunk lists.
"""
import sys, os, time, math, struct
sys.path.insert(0, os.path.dirname(__file__))
from cheat.memory import DotaMemory
from cheat.game_state import DotaGame
from cheat.offsets import (GameState, BaseEntity, GameSceneNode, BaseNPC,
                           EntitySystem, EntityIdentity)


def scan_all_entities_raw(mem, entity_system):
    """Scan ALL entities across all 3 chunk lists, return (entity_ptr, ident_addr) pairs."""
    results = []
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
            # Read whole chunk at once for speed (512 * 0x78 = 0xF000)
            try:
                chunk_data = mem.pm.read_bytes(chunk_ptr, EntitySystem.SLOTS_PER_CHUNK * EntityIdentity.STRIDE)
            except:
                continue
            for si in range(EntitySystem.SLOTS_PER_CHUNK):
                off = si * EntityIdentity.STRIDE
                ent_ptr = struct.unpack("<Q", chunk_data[off:off+8])[0]
                if ent_ptr and ent_ptr > 0x10000:
                    results.append((ent_ptr, chunk_ptr + off))
    return results


def main():
    mem = DotaMemory()
    print(f"[+] PID {mem.pid}")
    game = DotaGame(mem)
    game.init()

    state = game.get_game_state()
    print(f"[+] State: {state}")

    # Scan ALL entities
    print("\n=== SCANNING ALL ENTITIES ===")
    all_ents = scan_all_entities_raw(mem, game.entity_system)
    print(f"Total entity pointers: {len(all_ents)}")

    # Find heroes: check each entity for HP + team + unit_name containing "hero"
    print("\n=== FINDING HEROES ===")
    heroes = []
    for ent_ptr, ident_addr in all_ents:
        try:
            hp = mem.read_i32(ent_ptr + BaseEntity.HEALTH)
            team = mem.read_u8(ent_ptr + BaseEntity.TEAM_NUM)
            if team not in (2, 3):
                continue
            if hp <= 0 or hp > 10000:
                continue
            # Read unit name
            uname_ptr = mem.read_ptr(ent_ptr + BaseNPC.UNIT_NAME)
            if not uname_ptr or uname_ptr < 0x10000:
                continue
            uname = mem.read_string(uname_ptr, 64)
            if "hero" not in uname:
                continue
            # Read position
            scene = mem.read_ptr(ent_ptr + BaseEntity.GAME_SCENE_NODE)
            if not scene or scene < 0x10000:
                continue
            x = mem.read_f32(scene + GameSceneNode.VEC_ABS_ORIGIN)
            y = mem.read_f32(scene + GameSceneNode.VEC_ABS_ORIGIN + 4)
            z = mem.read_f32(scene + GameSceneNode.VEC_ABS_ORIGIN + 8)
            level = mem.read_i32(ent_ptr + BaseNPC.CURRENT_LEVEL)

            heroes.append({
                "e": ent_ptr, "name": uname, "hp": hp, "team": team,
                "x": x, "y": y, "z": z, "level": level
            })
        except:
            continue

    print(f"Found {len(heroes)} heroes:")
    for h in heroes:
        t = {2: "Rad", 3: "Dire"}.get(h["team"], "?")
        print(f"  {h['name']:45s} {t} hp={h['hp']:5d} lvl={h['level']} pos=({h['x']:.0f},{h['y']:.0f},{h['z']:.0f})")

    # Controllers
    print(f"\n=== CONTROLLERS ===")
    for ent_ptr, ident_addr in all_ents:
        try:
            # Check designerName
            dname_ptr = mem.read_ptr(ident_addr + EntityIdentity.DESIGNER_NAME)
            if not dname_ptr or dname_ptr < 0x10000:
                continue
            dname = mem.read_string(dname_ptr, 64)
            if dname != "dota_player_controller":
                continue
            slot = mem.read_i32(ent_ptr + 0x908)
            pname = mem.read_string(ent_ptr + 0x6E0, 32)
            print(f"  slot={slot:3d} name='{pname}' entity={hex(ent_ptr)}")
        except:
            continue

    # Track movement (20s)
    if heroes:
        print(f"\n=== MOVEMENT TRACKING (20s) ===")
        init = {h["e"]: (h["name"], h["x"], h["y"], h["team"]) for h in heroes}
        time.sleep(20)

        # Re-scan heroes
        heroes2 = []
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
                heroes2.append({"e": ent_ptr, "name": uname, "x": x, "y": y, "team": team, "hp": hp})
            except:
                continue

        print(f"\n{'Hero':45s} {'Team':5s} {'HP':>5s} {'Dist':>8s} {'Status'}")
        print("-" * 80)
        for h in heroes2:
            if h["e"] in init:
                _, x0, y0, _ = init[h["e"]]
                dist = math.sqrt((h["x"]-x0)**2 + (h["y"]-y0)**2)
            else:
                dist = -1
            t = {2: "Rad", 3: "Dire"}.get(h["team"], "?")
            status = ">>> MOVING (BOT AI)" if dist > 50 else "STATIC"
            print(f"  {h['name']:43s} {t:5s} {h['hp']:5d} {dist:8.1f} {status}")

        moving = sum(1 for h in heroes2 if h["e"] in init and
                     math.sqrt((h["x"]-init[h["e"]][1])**2 + (h["y"]-init[h["e"]][2])**2) > 50)
        total = len(heroes2)
        print(f"\n  RESULT: {moving}/{total} heroes moving with bot AI")

    print("\nDone!")


if __name__ == "__main__":
    main()
