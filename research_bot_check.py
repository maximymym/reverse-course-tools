"""
Check if dota_bot_populate bots are playing.
Also try to enable sv_cheats and populate more bots.
"""
import sys, os, time, math
sys.path.insert(0, os.path.dirname(__file__))
from cheat.memory import DotaMemory
from cheat.commands import DotaCommands
from cheat.game_state import DotaGame
from cheat.offsets import GameState, BaseEntity, GameSceneNode, BaseNPC


def list_heroes(game, mem):
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
            x = mem.read_f32(scene + GameSceneNode.VEC_ABS_ORIGIN)
            y = mem.read_f32(scene + GameSceneNode.VEC_ABS_ORIGIN + 4)
            heroes.append({"e": entity, "name": uname, "hp": hp, "team": team, "x": x, "y": y})
        except:
            continue
    return heroes


def main():
    mem = DotaMemory()
    game = DotaGame(mem)
    game.init()
    cmd = DotaCommands(mem)
    cmd.init(game)
    print(f"[+] PID {mem.pid}, state={game.get_game_state()}")

    # Step 1: Force sv_cheats=1 via direct memory write (bypass command)
    print("\n=== FORCE sv_cheats = 1 via memory ===")
    cmd.convar.set_int("sv_cheats", 1)
    time.sleep(0.5)
    print(f"  sv_cheats after write: {cmd.convar.get_int('sv_cheats')}")

    # Also try the console command
    cmd.execute("sv_cheats 1")
    time.sleep(0.5)
    print(f"  sv_cheats after cmd: {cmd.convar.get_int('sv_cheats')}")

    # Step 2: Populate more bots
    print("\n=== POPULATE BOTS ===")
    cmd.execute("dota_bot_populate")
    time.sleep(5)

    # List controllers
    print("\n  Controllers:")
    game.init()
    for entity, ident, name in game.iter_entities():
        if name == "dota_player_controller":
            slot = mem.read_i32(entity + 0x908)
            pname = ""
            try:
                pname = mem.read_string(entity + 0x6E0, 32)
            except:
                pass
            print(f"    slot={slot:3d} name='{pname}'")

    # List heroes
    print("\n  Heroes:")
    heroes = list_heroes(game, mem)
    for h in heroes:
        t = {2: "Rad", 3: "Dire"}.get(h["team"], "?")
        print(f"    {h['name']:40s} {t} hp={h['hp']} pos=({h['x']:.0f},{h['y']:.0f})")

    # Step 3: Track movement (20s)
    print(f"\n=== MOVEMENT TRACKING (20s) — {len(heroes)} heroes ===")
    init = {h["e"]: (h["name"], h["x"], h["y"], h["team"]) for h in heroes}
    time.sleep(20)

    heroes2 = list_heroes(game, mem)
    for h in heroes2:
        if h["e"] in init:
            _, x0, y0, _ = init[h["e"]]
            dist = math.sqrt((h["x"]-x0)**2 + (h["y"]-y0)**2)
        else:
            dist = -1
        t = {2: "Rad", 3: "Dire"}.get(h["team"], "?")
        status = ">>> MOVING! BOT AI ACTIVE" if dist > 50 else "static"
        print(f"    {h['name']:40s} {t} moved={dist:8.1f} {status}")

    moving = sum(1 for h in heroes2 if h["e"] in init and
                 math.sqrt((h["x"]-init[h["e"]][1])**2 + (h["y"]-init[h["e"]][2])**2) > 50)
    print(f"\n  {moving}/{len(heroes2)} heroes moving")

    if moving > 0:
        print("\n  *** BOT AI IS WORKING! ***")
        print("  dota_bot_populate creates bots with active AI!")
    else:
        print("\n  Bots are not moving. Trying additional commands...")
        # Try enabling bot thinking
        cmd.convar.set_int("dota_bot_disable", 0)
        cmd.execute("dota_bot_disable 0")
        time.sleep(10)

        # Check again
        heroes3 = list_heroes(game, mem)
        for h in heroes3:
            if h["e"] in init:
                _, x0, y0, _ = init[h["e"]]
                dist = math.sqrt((h["x"]-x0)**2 + (h["y"]-y0)**2)
            else:
                dist = -1
            t = {2: "Rad", 3: "Dire"}.get(h["team"], "?")
            status = "MOVING!" if dist > 50 else "static"
            print(f"    {h['name']:40s} {t} moved={dist:8.1f} {status}")

    print("\nDone!")


if __name__ == "__main__":
    main()
