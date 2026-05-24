"""
Research Bot Takeover — Phase 4:
1. Find ALL heroes by scanning entity fields (not names)
2. Check if bots are playing (moving heroes)
3. Decide next steps
"""
import sys, os, time, struct, math
sys.path.insert(0, os.path.dirname(__file__))

from cheat.memory import DotaMemory
from cheat.game_state import DotaGame
from cheat.commands import DotaCommands
from cheat.offsets import (GameState, BaseEntity, GameSceneNode, BaseNPC,
                           EntitySystem, EntityIdentity)


def find_all_heroes(game, mem):
    """Find heroes by checking if entity has valid m_iHealth + m_iTeamNum + m_iszUnitName with 'hero'."""
    heroes = []
    for entity, ident, name in game.iter_entities():
        try:
            hp = mem.read_i32(entity + BaseEntity.HEALTH)
            team = mem.read_u8(entity + BaseEntity.TEAM_NUM)
            if team not in (2, 3):
                continue
            if hp <= 0 or hp > 10000:
                continue
            # Try reading unit name
            unit_name_ptr = mem.read_ptr(entity + BaseNPC.UNIT_NAME)
            if not unit_name_ptr or unit_name_ptr < 0x10000:
                continue
            unit_name = mem.read_string(unit_name_ptr, 64)
            if "hero" not in unit_name:
                continue
            # Read position
            scene = mem.read_ptr(entity + BaseEntity.GAME_SCENE_NODE)
            if not scene or scene < 0x10000:
                continue
            x = mem.read_f32(scene + GameSceneNode.VEC_ABS_ORIGIN)
            y = mem.read_f32(scene + GameSceneNode.VEC_ABS_ORIGIN + 4)
            z = mem.read_f32(scene + GameSceneNode.VEC_ABS_ORIGIN + 8)
            level = mem.read_i32(entity + BaseNPC.CURRENT_LEVEL)

            heroes.append({
                "entity": entity, "unit_name": unit_name,
                "hp": hp, "team": team, "level": level,
                "x": x, "y": y, "z": z,
                "designer_name": name,
            })
        except:
            continue
    return heroes


def main():
    mem = DotaMemory()
    print(f"[+] PID {mem.pid}")
    game = DotaGame(mem)
    game.init()
    cmd = DotaCommands(mem)
    cmd.init(game)

    state = game.get_game_state()
    print(f"[+] Game state: {state} ({GameState.NAMES.get(state, '?')})")

    if state != GameState.GAME_IN_PROGRESS:
        print("[!] Not in game. Exiting.")
        return

    # 1. Find all heroes via unit_name field
    print("\n=== FINDING HEROES VIA UNIT NAME ===")
    heroes = find_all_heroes(game, mem)
    print(f"Found {len(heroes)} heroes:")
    for h in heroes:
        team = {2: "Rad", 3: "Dire"}.get(h["team"], "?")
        print(f"  {h['unit_name']:45s} {team} hp={h['hp']:5d} lvl={h['level']} "
              f"pos=({h['x']:.0f}, {h['y']:.0f}, {h['z']:.0f})")

    # 2. Track movement for 20s
    print(f"\n=== TRACKING HERO MOVEMENT (20s) ===")
    initial = {h["entity"]: (h["x"], h["y"]) for h in heroes}
    time.sleep(20)

    print(f"\n{'Hero':45s} {'Team':5s} {'Dist':>8s} {'Status'}")
    print("-" * 75)
    heroes2 = find_all_heroes(game, mem)
    for h in heroes2:
        e = h["entity"]
        if e in initial:
            x0, y0 = initial[e]
            dist = math.sqrt((h["x"]-x0)**2 + (h["y"]-y0)**2)
        else:
            dist = -1
        team = {2: "Rad", 3: "Dire"}.get(h["team"], "?")
        status = "MOVING" if dist > 50 else ("STATIC" if dist >= 0 else "NEW")
        is_lion = " <<< OUR HERO" if "lion" in h["unit_name"] else ""
        print(f"  {h['unit_name']:43s} {team:5s} {dist:8.1f}  {status}{is_lion}")

    # 3. Summary
    moving = sum(1 for h in heroes2 if h["entity"] in initial and
                 math.sqrt((h["x"]-initial[h["entity"]][0])**2 +
                           (h["y"]-initial[h["entity"]][1])**2) > 50)
    print(f"\n{moving}/{len(heroes2)} heroes are moving (have bot AI)")

    # 4. Check: is our hero (lion) moving?
    our_hero = None
    for h in heroes2:
        if "lion" in h["unit_name"] and h["team"] == 2:
            our_hero = h
            break
    if our_hero and our_hero["entity"] in initial:
        x0, y0 = initial[our_hero["entity"]]
        dist = math.sqrt((our_hero["x"]-x0)**2 + (our_hero["y"]-y0)**2)
        if dist > 50:
            print(f"\n>>> OUR HERO IS MOVING ({dist:.0f} units)! Bot AI is active!")
        else:
            print(f"\n>>> Our hero is STATIONARY ({dist:.1f} units). Bot AI NOT active.")
            print("    Fallback: use PrepareUnitOrders for scripted behavior.")

    print("\nDone!")


if __name__ == "__main__":
    main()
