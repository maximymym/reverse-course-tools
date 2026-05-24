"""
Debug: why VScript doesn't work? Test various command formats.
Also try running commands via different methods.
"""
import sys, os, time
sys.path.insert(0, os.path.dirname(__file__))
from cheat.memory import DotaMemory
from cheat.commands import DotaCommands
from cheat.game_state import DotaGame
from cheat.offsets import GameState, BaseEntity, BaseNPC


def main():
    mem = DotaMemory()
    print(f"[+] PID {mem.pid}")
    game = DotaGame(mem)
    game.init()
    cmd = DotaCommands(mem)
    cmd.init(game)

    state = game.get_game_state()
    print(f"[+] Game state: {state}")

    # === Test: verify basic commands still work ===
    print("\n=== VERIFY: basic console commands ===")

    # This should create a unit — we know it worked before
    print("  [>] dota_create_unit npc_dota_hero_sven")
    cmd.execute("dota_create_unit npc_dota_hero_sven")
    time.sleep(3)

    # Count heroes
    hero_count = 0
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
            if "hero" in uname:
                hero_count += 1
                t = {2: "Rad", 3: "Dire"}.get(team, "?")
                print(f"    {uname:40s} {t} hp={hp}")
        except:
            continue
    print(f"  Total heroes: {hero_count}")

    # === Test: sv_cheats via console command (not ConVar write) ===
    print("\n=== TEST: sv_cheats via console ===")
    print("  [>] sv_cheats 1")
    cmd.execute("sv_cheats 1")
    time.sleep(1)

    # Read sv_cheats value
    sv_cheats = cmd.convar.get_int("sv_cheats")
    print(f"  sv_cheats = {sv_cheats}")

    # === Test: script with various formats ===
    print("\n=== TEST: script command variants ===")

    tests = [
        # Simple print
        ('script print("HELLO")', "basic print"),
        # Semicolon separation
        ('script 1+1', "minimal script"),
        # script_execute
        ('script_execute test', "script_execute"),
        # SendToConsole
        ('script SendToConsole("echo SCRIPT_WORKS")', "SendToConsole"),
        # SendToServerConsole
        ('script SendToServerConsole("echo SERVER_WORKS")', "SendToServerConsole"),
        # Check if script is even a registered command
        ('help script', "help script"),
        # Try scriptdata
        ('script_help', "script_help"),
        # Try creating entity via script
        ('script CreateUnitByName("npc_dota_hero_drow_ranger", Vector(0,0,0), true, nil, nil, 3)', "CreateUnit via script"),
    ]

    for tcmd, desc in tests:
        print(f"  [{desc}] {tcmd}")
        cmd.execute(tcmd)
        time.sleep(1)

    # === Verify dota_create_unit works (control test) ===
    print("\n=== CONTROL: dota_create_unit (known working) ===")
    print("  [>] dota_create_unit npc_dota_hero_drow_ranger")
    cmd.execute("dota_create_unit npc_dota_hero_drow_ranger")
    time.sleep(3)

    game.init()
    print("  Heroes after create_unit:")
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
            if "hero" in uname:
                t = {2: "Rad", 3: "Dire"}.get(team, "?")
                print(f"    {uname:40s} {t} hp={hp}")
        except:
            continue

    # === Test: dota_bot_populate variants ===
    print("\n=== TEST: bot populate variants ===")
    populate_cmds = [
        "sv_cheats 1",
        "dota_bot_disable 0",
        "dota_bot_populate",
        "dota_bot_populate_seeded",  # found this string in server.dll!
    ]
    for c in populate_cmds:
        print(f"  [>] {c}")
        cmd.execute(c)
        time.sleep(2)

    # Check controllers
    print("\n  Controllers:")
    game.init()
    controllers = []
    for entity, ident, name in game.iter_entities():
        if name == "dota_player_controller":
            slot = mem.read_i32(entity + 0x908)
            pname = ""
            try:
                pname = mem.read_string(entity + 0x6E0, 32)
            except:
                pass
            controllers.append((slot, pname))
            print(f"    slot={slot:3d} name='{pname}'")
    print(f"  Total controllers: {len(controllers)}")

    print("\nDone! Check Dota window for visual changes.")


if __name__ == "__main__":
    main()
