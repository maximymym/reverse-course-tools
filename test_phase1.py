"""
Phase 1 Integration Test: Console command injection + movement.

Run with Dota 2 open in demo mode (hero on map).
Tests:
1. echo command
2. ConVar read/write (direct memory)
3. ConVar via console command
4. Item purchase
5. Hero movement (demo mode commands)
"""
import sys, time
sys.path.insert(0, ".")
from cheat.memory import DotaMemory
from cheat.game_state import DotaGame
from cheat.commands import DotaCommands

def test_echo(cmd: DotaCommands):
    print("\n--- Test 1: echo command ---")
    ok = cmd.execute("echo [DOTA2_BOT] Phase1 command injection working!")
    print(f"  Result: {'OK' if ok else 'FAIL'}")
    return ok

def test_convar_direct(cmd: DotaCommands):
    print("\n--- Test 2: ConVar direct R/W ---")
    name = "dota_player_units_auto_attack_mode"
    old = cmd.convar_get_int(name)
    print(f"  {name} = {old}")

    cmd.convar_set_int(name, 2)
    check = cmd.convar_get_int(name)
    print(f"  Set to 2, read back: {check}")
    ok = check == 2

    cmd.convar_set_int(name, old if old is not None else 1)
    print(f"  Restored to: {cmd.convar_get_int(name)}")
    print(f"  Result: {'OK' if ok else 'FAIL'}")
    return ok

def test_convar_via_command(cmd: DotaCommands):
    print("\n--- Test 3: ConVar via console command ---")
    name = "dota_player_units_auto_attack_mode"
    old = cmd.convar_get_int(name)

    cmd.execute(f"{name} 3")
    time.sleep(0.3)
    check = cmd.convar_get_int(name)
    print(f"  Sent '{name} 3', read back: {check}")
    ok = check == 3

    cmd.convar_set_int(name, old if old is not None else 1)
    print(f"  Result: {'OK' if ok else 'FAIL'}")
    return ok

def test_purchase(cmd: DotaCommands):
    print("\n--- Test 4: Item purchase ---")
    # In demo mode, we have gold. Try buying tangos.
    ok = cmd.purchase_item("item_tango")
    time.sleep(0.3)
    print(f"  Sent dota_purchase_item item_tango: {'OK' if ok else 'FAIL'}")
    # Can't easily verify purchase from memory, but command was sent
    return ok

def test_movement(cmd: DotaCommands, game: DotaGame):
    print("\n--- Test 5: Hero movement ---")
    # Read hero position before
    heroes = game.find_all_heroes()
    if not heroes:
        print("  [!] No heroes found, skipping")
        return False

    hero_addr, hero_name = heroes[0]
    pos_before = game.read_position(hero_addr)
    print(f"  Hero: {hero_name}")
    print(f"  Position before: ({pos_before[0]:.0f}, {pos_before[1]:.0f}, {pos_before[2]:.0f})")

    # Try various movement commands
    # In demo mode with sv_cheats 1:
    # 1. dota_dev hero_move_to_point X Y Z
    # 2. noclip
    # 3. dota_camera_set_lookatpos X Y Z

    # First check sv_cheats
    sv_cheats = cmd.convar_get_int("sv_cheats")
    print(f"  sv_cheats = {sv_cheats}")

    # Try movement: pick a target ~500 units from current pos
    target_x = pos_before[0] + 500
    target_y = pos_before[1] + 500

    # Method 1: dota_dev hero_move_to_point (if available in demo)
    print(f"  Sending movement to ({target_x:.0f}, {target_y:.0f})")
    cmd.execute(f"dota_dev hero_move_to_point {target_x:.0f} {target_y:.0f} 128")
    time.sleep(2.0)

    pos_after1 = game.read_position(hero_addr)
    moved1 = abs(pos_after1[0] - pos_before[0]) > 10 or abs(pos_after1[1] - pos_before[1]) > 10
    print(f"  After move cmd: ({pos_after1[0]:.0f}, {pos_after1[1]:.0f}, {pos_after1[2]:.0f}) moved={moved1}")

    if moved1:
        print("  [OK] Movement via dota_dev hero_move_to_point works!")
        return True

    # Method 2: dota_bot_give_order (for bots in demo)
    # dota_create_unit npc_dota_hero_axe -> maybe we can order the existing hero

    # Method 3: Try direct order via protobuf-style command
    # cmd.execute("dota_unit_order_move_to_position ...")

    # Method 4: Try +forward / cl_dota_testmove
    print("  Trying alternative: cl_forwardspeed + +forward")
    cmd.execute("cl_forwardspeed 500")
    cmd.execute("+forward")
    time.sleep(2.0)
    cmd.execute("-forward")

    pos_after2 = game.read_position(hero_addr)
    moved2 = abs(pos_after2[0] - pos_before[0]) > 10 or abs(pos_after2[1] - pos_before[1]) > 10
    print(f"  After +forward: ({pos_after2[0]:.0f}, {pos_after2[1]:.0f}, {pos_after2[2]:.0f}) moved={moved2}")

    if moved2:
        print("  [OK] Movement via +forward works!")
        return True

    # Method 5: Camera test (just to prove commands work)
    print("  Movement didn't work via console. Trying camera command...")
    cmd.execute("dota_camera_set_lookatpos 0 0 128")
    time.sleep(0.5)
    print("  Camera command sent (check if camera moved)")

    return False

def main():
    mem = DotaMemory()
    game = DotaGame(mem)
    cmd = DotaCommands(mem)

    print("=== Phase 1 Integration Test ===")

    # Initialize
    if not game.init():
        print("[!] Game init failed")
        return
    if not cmd.init():
        print("[!] Commands init failed")
        return

    print(f"[+] Game state: {game.get_game_state_name()}")

    results = {}
    results["echo"] = test_echo(cmd)
    results["convar_direct"] = test_convar_direct(cmd)
    results["convar_cmd"] = test_convar_via_command(cmd)
    results["purchase"] = test_purchase(cmd)
    results["movement"] = test_movement(cmd, game)

    # Summary
    print("\n=== Results ===")
    for name, ok in results.items():
        status = "PASS" if ok else "FAIL"
        print(f"  {name}: {status}")

    passed = sum(1 for ok in results.values() if ok)
    total = len(results)
    print(f"\n  {passed}/{total} tests passed")

    mem.close()

if __name__ == "__main__":
    main()
