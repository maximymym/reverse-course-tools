"""
Bot Takeover Research — Phase 5:
User is in a local lobby. Populate bots, test bot AI, try takeover.
"""
import sys, os, time, struct, math
sys.path.insert(0, os.path.dirname(__file__))

from cheat.memory import DotaMemory
from cheat.game_state import DotaGame
from cheat.commands import DotaCommands
from cheat.offsets import GameState, BaseEntity, GameSceneNode, BaseNPC


def find_heroes(game, mem):
    heroes = []
    for entity, ident, name in game.iter_entities():
        try:
            hp = mem.read_i32(entity + BaseEntity.HEALTH)
            team = mem.read_u8(entity + BaseEntity.TEAM_NUM)
            if team not in (2, 3) or hp <= 0 or hp > 10000:
                continue
            uname_ptr = mem.read_ptr(entity + BaseNPC.UNIT_NAME)
            if not uname_ptr or uname_ptr < 0x10000:
                continue
            uname = mem.read_string(uname_ptr, 64)
            if "hero" not in uname:
                continue
            scene = mem.read_ptr(entity + BaseEntity.GAME_SCENE_NODE)
            if not scene:
                continue
            x = mem.read_f32(scene + GameSceneNode.VEC_ABS_ORIGIN)
            y = mem.read_f32(scene + GameSceneNode.VEC_ABS_ORIGIN + 4)
            heroes.append({"entity": entity, "name": uname, "hp": hp, "team": team, "x": x, "y": y})
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

    # Step 1: Try adding bots
    print("\n=== STEP 1: POPULATE BOTS ===")

    # Enable cheats (should already be on in local)
    cmd.execute("sv_cheats 1")
    time.sleep(0.5)

    # Set bot difficulty
    cmd.convar.set_int("dota_bot_practice_difficulty", 2)  # medium

    # Try various bot commands
    bot_cmds = [
        "dota_bot_populate",
        "dota_create_fake_clients",
        "sv_cheats 1",
        "dota_bot_disable 0",
    ]
    for c in bot_cmds:
        print(f"  [>] {c}")
        cmd.execute(c)
        time.sleep(1)

    # Wait for bots to spawn
    print("[>] Waiting 10s for bots to join...")
    time.sleep(10)

    # Re-init and check
    game.init()
    heroes = find_heroes(game, mem)
    print(f"\nFound {len(heroes)} heroes:")
    for h in heroes:
        t = {2: "Rad", 3: "Dire"}.get(h["team"], "?")
        print(f"  {h['name']:40s} {t} hp={h['hp']} pos=({h['x']:.0f},{h['y']:.0f})")

    if len(heroes) < 2:
        print("\n[!] Bots didn't join. Trying more commands...")

        more_cmds = [
            "dota_bot_populate",
            "bot_add",
            "bot_add_ct",
            "dota_create_unit npc_dota_hero_axe",
        ]
        for c in more_cmds:
            print(f"  [>] {c}")
            cmd.execute(c)
            time.sleep(2)

        time.sleep(10)
        game.init()
        heroes = find_heroes(game, mem)
        print(f"\nAfter retry: {len(heroes)} heroes")
        for h in heroes:
            t = {2: "Rad", 3: "Dire"}.get(h["team"], "?")
            print(f"  {h['name']:40s} {t} hp={h['hp']} pos=({h['x']:.0f},{h['y']:.0f})")

    # Step 2: Track movement
    if len(heroes) > 0:
        print(f"\n=== STEP 2: TRACKING MOVEMENT (20s) ===")
        initial = {h["entity"]: (h["x"], h["y"]) for h in heroes}
        time.sleep(20)
        heroes2 = find_heroes(game, mem)

        for h in heroes2:
            e = h["entity"]
            if e in initial:
                dx = h["x"] - initial[e][0]
                dy = h["y"] - initial[e][1]
                dist = math.sqrt(dx*dx + dy*dy)
            else:
                dist = -1
            t = {2: "Rad", 3: "Dire"}.get(h["team"], "?")
            status = "MOVING" if dist > 50 else "STATIC"
            print(f"  {h['name']:40s} {t} dist={dist:8.1f} {status}")

    # Step 3: List all dota_player_controller entities
    print(f"\n=== STEP 3: ALL CONTROLLERS ===")
    for entity, ident, name in game.iter_entities():
        if name and "controller" in name.lower():
            slot = mem.read_i32(entity + 0x908)
            pname = ""
            try:
                pname = mem.read_string(entity + 0x6E0, 32)
            except:
                pass
            print(f"  {name:40s} slot={slot:3d} name='{pname}'")

    # Step 4: Try dota_bot_takeover on our hero
    print(f"\n=== STEP 4: BOT TAKEOVER ATTEMPTS ===")

    takeover_cmds = [
        # Try various forms
        "dota_bot_takeover",
        "dota_bot_takeover 0",      # slot 0
        "bot_takeover",
        "bot_zombie 1",             # bot zombie mode
        "dota_bot_disable 0",
        "dota_bot_mode 1",
        "dota_bot_practice_start 1",
    ]
    for c in takeover_cmds:
        print(f"  [>] {c}")
        cmd.execute(c)
        time.sleep(0.5)

    # Check if our hero started moving
    our = None
    for h in find_heroes(game, mem):
        if "lion" in h["name"]:
            our = h
            break

    if our:
        print(f"\n  Our hero at ({our['x']:.0f}, {our['y']:.0f})")
        time.sleep(10)
        heroes3 = find_heroes(game, mem)
        for h in heroes3:
            if "lion" in h["name"]:
                dist = math.sqrt((h["x"]-our["x"])**2 + (h["y"]-our["y"])**2)
                if dist > 50:
                    print(f"  >>> HERO MOVED {dist:.0f} units! Bot AI active!")
                else:
                    print(f"  >>> Hero stationary ({dist:.1f} units). Bot AI not active.")

    print("\nDone!")


if __name__ == "__main__":
    main()
