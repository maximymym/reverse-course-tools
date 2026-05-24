"""
Phase 1.5 Integration Test — PrepareUnitOrders movement.

Requires: Dota 2 running in demo mode with a hero spawned.
"""
import sys, os, time
sys.path.insert(0, os.path.dirname(__file__))

from cheat.memory import DotaMemory
from cheat.game_state import DotaGame
from cheat.commands import DotaCommands, UnitOrder

def test_move_to():
    """Hero moves 500 units via PrepareUnitOrders."""
    mem = DotaMemory()
    game = DotaGame(mem)
    game.init()

    cmd = DotaCommands(mem)
    cmd.init(game)

    assert cmd.local_controller != 0, "Local controller not found"
    assert cmd.prepare_orders_addr != 0, "PrepareUnitOrders not resolved"

    hero = game.find_local_hero()
    assert hero, "No hero found"
    hero_addr, hero_name = hero
    print(f"Hero: {hero_name}")

    pos_before = game.read_position(hero_addr)
    target_x = pos_before[0] + 500
    target_y = pos_before[1]

    ok = cmd.move_to(target_x, target_y)
    assert ok, "move_to() failed"

    time.sleep(2.5)
    pos_after = game.read_position(hero_addr)
    dx = pos_after[0] - pos_before[0]
    dy = pos_after[1] - pos_before[1]
    dist = (dx*dx + dy*dy) ** 0.5
    print(f"Moved {dist:.1f} units (expected ~500)")
    assert dist > 400, f"Hero barely moved: {dist:.1f} units"
    print("PASS: move_to")
    mem.close()


def test_stop():
    """Hero stops after receiving stop order."""
    mem = DotaMemory()
    game = DotaGame(mem)
    game.init()

    cmd = DotaCommands(mem)
    cmd.init(game)

    hero = game.find_local_hero()
    assert hero
    hero_addr, _ = hero

    # Start moving far away
    pos = game.read_position(hero_addr)
    cmd.move_to(pos[0] + 2000, pos[1])
    time.sleep(0.5)

    # Stop
    ok = cmd.stop()
    assert ok, "stop() failed"

    time.sleep(0.3)
    pos_stopped = game.read_position(hero_addr)
    time.sleep(1.0)
    pos_check = game.read_position(hero_addr)

    dx = pos_check[0] - pos_stopped[0]
    dy = pos_check[1] - pos_stopped[1]
    drift = (dx*dx + dy*dy) ** 0.5
    print(f"Drift after stop: {drift:.1f} units (should be ~0)")
    assert drift < 50, f"Hero still moving after stop: {drift:.1f}"
    print("PASS: stop")
    mem.close()


def test_attack_move():
    """Hero a-moves to position."""
    mem = DotaMemory()
    game = DotaGame(mem)
    game.init()

    cmd = DotaCommands(mem)
    cmd.init(game)

    hero = game.find_local_hero()
    assert hero
    hero_addr, _ = hero

    pos = game.read_position(hero_addr)
    ok = cmd.attack_move(pos[0] - 500, pos[1])
    assert ok, "attack_move() failed"

    time.sleep(2.5)
    pos_after = game.read_position(hero_addr)
    dx = pos_after[0] - pos[0]
    dy = pos_after[1] - pos[1]
    dist = (dx*dx + dy*dy) ** 0.5
    print(f"A-moved {dist:.1f} units (expected ~500)")
    assert dist > 300, f"Hero barely moved: {dist:.1f}"
    print("PASS: attack_move")
    mem.close()


if __name__ == "__main__":
    tests = [test_move_to, test_stop, test_attack_move]
    passed = 0
    for t in tests:
        print(f"\n--- {t.__name__} ---")
        try:
            t()
            passed += 1
        except Exception as e:
            print(f"FAIL: {e}")
    print(f"\n=== {passed}/{len(tests)} tests passed ===")
