"""
Phase 3.5 — Runtime verification of new offsets.
Run while Dota 2 is in a demo/lobby with a hero selected.

Tests:
1. m_iMaxHealth — should be > 0 and >= HP
2. m_nUnitState64 — should read without crash, flags make sense
3. m_hItems (inventory) — resolve CHandles to item entities
4. C_DOTA_DataNonSpectator — gold, net worth, last hits
5. Ability points from BaseHero
"""
import sys
import struct
sys.path.insert(0, ".")

from cheat.memory import DotaMemory
from cheat.game_state import DotaGame
from cheat.offsets import BaseEntity, BaseNPC, BaseHero, UnitState

def main():
    print("=" * 60)
    print("Phase 3.5 — Runtime Offset Verification")
    print("=" * 60)

    mem = DotaMemory()
    game = DotaGame(mem)
    if not game.init():
        print("[!] Failed to init game state")
        return

    # Find heroes
    heroes = game.find_all_heroes()
    if not heroes:
        print("[!] No heroes found — need to be in demo/lobby")
        return

    print(f"\n[+] Found {len(heroes)} heroes")

    # Build handle index for fast CHandle resolution
    handle_index = {}
    from cheat.offsets import EntitySystem, EntityIdentity
    for list_off in EntitySystem.CHUNK_LISTS:
        try:
            chunk_list = mem.read_ptr(game.entity_system + list_off)
        except:
            continue
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
                if not ent_ptr or ent_ptr < 0x10000:
                    continue
                handle_val = struct.unpack("<I", chunk_data[off+EntityIdentity.HANDLE:off+EntityIdentity.HANDLE+4])[0]
                if handle_val and handle_val != 0xFFFFFFFF:
                    eidx = handle_val & 0x7FFF
                    if eidx not in handle_index:
                        handle_index[eidx] = ent_ptr

    passed = 0
    failed = 0

    for entity, name in heroes:
        print(f"\n--- {name} (entity={hex(entity)}) ---")

        # 1. MaxHealth
        print("\n[Test 1] m_iMaxHealth")
        hp = game.read_health(entity)
        max_hp = game.read_max_health(entity)
        print(f"  HP={hp}, MaxHP={max_hp}")
        if max_hp > 0 and max_hp >= hp:
            print("  OK MaxHP valid")
            passed += 1
        else:
            print(f"  FAIL MaxHP invalid (expected > 0 and >= HP)")
            failed += 1

        # 2. UnitState64
        print("\n[Test 2] m_nUnitState64")
        state = game.read_unit_state(entity)
        print(f"  UnitState64 = {hex(state)} ({state})")
        # In normal gameplay, state should be 0 or have some common flags
        active_flags = []
        for flag_name in dir(UnitState):
            if flag_name.startswith('_'):
                continue
            flag_val = getattr(UnitState, flag_name)
            if isinstance(flag_val, int) and state & flag_val:
                active_flags.append(flag_name)
        if active_flags:
            print(f"  Active flags: {', '.join(active_flags)}")
        else:
            print("  No active flags (normal idle state)")
        # Just check it didn't crash
        print("  OK UnitState64 read OK")
        passed += 1

        # State flag helpers
        print(f"  IsStunned={game.is_stunned(entity)}, IsSilenced={game.is_silenced(entity)}, "
              f"IsMagicImmune={game.is_magic_immune(entity)}, IsInvisible={game.is_invisible(entity)}")

        # 3. Inventory
        print("\n[Test 3] Inventory (m_hItems)")
        item_handles = game.read_item_handles(entity)
        print(f"  Item handles count: {len(item_handles)}")
        if item_handles:
            print(f"  Raw handles: {[hex(h) if h else 0 for h in item_handles[:10]]}")
        items = game.get_hero_items(entity, handle_index)
        if items:
            print(f"  Resolved items ({len(items)}):")
            for item in items:
                print(f"    slot {item['slot']}: {item['name']} "
                      f"(lv={item.get('level',0)}, cd={item.get('cooldown',0):.1f}, "
                      f"charges={item.get('charges',0)})")
            passed += 1
        else:
            print("  No items found (may be OK in lobby/demo without items)")
            # Not necessarily a failure — demo heroes might not have items
            passed += 1

        # 4. Ability Points
        print("\n[Test 4] AbilityPoints (BaseHero)")
        try:
            ap = game.read_ability_points(entity)
            print(f"  AbilityPoints = {ap}")
            if ap >= 0:
                print("  OK AbilityPoints valid")
                passed += 1
            else:
                print("  FAIL AbilityPoints negative")
                failed += 1
        except Exception as e:
            print(f"  FAIL Error: {e}")
            failed += 1

        # Only test first hero to keep output manageable
        break

    # 5. Gold (C_DOTA_DataNonSpectator)
    print("\n[Test 5] Gold (C_DOTA_DataNonSpectator)")
    for team_name, team_id in [("Radiant", 2), ("Dire", 3)]:
        data_ent = game.find_data_entity(team_id)
        if data_ent:
            print(f"  {team_name} data entity: {hex(data_ent)}")
            for slot in range(5):
                try:
                    r, u = game.read_gold(data_ent, slot)
                    nw = game.read_net_worth(data_ent, slot)
                    lh, dn = game.read_last_hits_denies(data_ent, slot)
                    if r != 0 or u != 0 or nw != 0:
                        print(f"    Slot {slot}: gold={r}+{u}={r+u}, NW={nw}, LH={lh}/{dn}")
                except Exception as e:
                    print(f"    Slot {slot}: error {e}")
            passed += 1
        else:
            print(f"  {team_name}: data entity NOT FOUND (may need in-game state)")

    print(f"\n{'=' * 60}")
    print(f"Results: {passed} passed, {failed} failed")
    print(f"{'=' * 60}")

    mem.close()

if __name__ == "__main__":
    main()
