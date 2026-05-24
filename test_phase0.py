"""
Phase 0 Test — verify all game state reading works.
Run with Dota 2 open (menu or in-game).
"""
import sys
import os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from cheat.game_state import DotaGame


def main():
    print("=" * 50)
    print("  Dota 2 Phase 0 — Game State Reader Test")
    print("=" * 50)

    game = DotaGame()
    if not game.init():
        print("\n[!] Failed to initialize. Is Dota 2 running?")
        return

    # --- Game State ---
    state = game.get_game_state()
    state_name = game.get_game_state_name()
    print(f"\n--- Game State ---")
    if state < 0:
        print(f"  State: MENU (no GameRules)")
    else:
        print(f"  State: {state_name} ({state})")
        print(f"  Game Mode: {game.get_game_mode()}")
        print(f"  Paused: {game.is_paused()}")

    # --- Entity List ---
    print(f"\n--- Entities ---")
    entity_types = {}
    for entity, ident, name in game.iter_entities():
        entity_types.setdefault(name, []).append(entity)

    print(f"  Total entity types: {len(entity_types)}")
    total = sum(len(v) for v in entity_types.values())
    print(f"  Total entities: {total}")

    # Show some interesting types
    for key in sorted(entity_types.keys()):
        if any(x in key for x in ["hero", "player", "gamerule", "creep", "tower", "courier"]):
            print(f"    {key} x{len(entity_types[key])}")

    # --- Heroes ---
    print(f"\n--- Heroes ---")
    heroes = game.find_all_heroes()
    if not heroes:
        print("  No heroes found (expected in menu)")
    else:
        for entity, name in heroes:
            game.print_hero_info(entity, name)

    # --- Local Hero ---
    print(f"\n--- Local Hero ---")
    local = game.find_local_hero()
    if local:
        entity, name = local
        print(f"  Found: {name} at {hex(entity)}")
    else:
        print("  Not found")

    game.close()
    print("\n[+] Phase 0 test complete!")


if __name__ == "__main__":
    main()
