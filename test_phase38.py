"""
Phase 3.8 verification + Gold offset research.
Run while in a Dota 2 match (demo mode or bot match).
"""
import sys, os, struct, time
sys.path.insert(0, os.path.dirname(__file__))
from cheat.memory import DotaMemory
from cheat.game_state import DotaGame
from cheat.bot_brain import EntityCache
from cheat.offsets import (BaseEntity, GameSceneNode, BaseNPC, BaseHero,
                           Gamerules, EntitySystem, EntityIdentity, BaseAbility)

def main():
    print("=" * 60)
    print("Phase 3.8 Verification + Gold Research")
    print("=" * 60)

    mem = DotaMemory()
    game = DotaGame(mem)
    if not game.init():
        print("[!] Failed to init game state. Are you in a match?")
        return

    # ── Test 1: Game Time from Gamerules ──
    print("\n-- Test 1: Game Time --")
    gt = game.get_game_time()
    print(f"  Gamerules addr: {hex(game.gamerules)}")
    print(f"  m_flGameTime (offset 0x30): {gt:.2f}")
    # Also read game start time
    try:
        gst = mem.read_f32(game.gamerules + Gamerules.GAME_START_TIME)
        print(f"  m_flGameStartTime (offset 0x34): {gst:.2f}")
    except:
        print(f"  m_flGameStartTime: read failed")
    game_state = game.get_game_state()
    print(f"  Game state: {game.get_game_state_name()} ({game_state})")
    if -200 < gt < 10000:
        print(f"  OK Game time looks valid (Dota clock)")
    else:
        print(f"  FAIL Game time out of range, falling back to system clock")

    # ── Test 2: EntityCache with local hero ──
    print("\n-- Test 2: EntityCache + Local Hero --")
    cache = EntityCache()
    t0 = time.time()
    cache.update(game, mem)
    dt = time.time() - t0
    print(f"  Cache update took {dt*1000:.0f}ms")
    print(f"  Heroes: {len(cache.heroes)}, Creeps: {len(cache.creeps)}, "
          f"Towers: {len(cache.towers)}, Buildings: {len(cache.buildings)}")
    print(f"  Handle index entries: {len(cache.handle_index)}")
    print(f"  Game time in cache: {cache.game_time}")
    if -200 < cache.game_time < 10000:
        print(f"  OK Cache game_time is Dota clock")
    else:
        print(f"  FAIL Cache game_time is system clock fallback")

    # Show heroes
    print(f"\n  Heroes in cache:")
    for h in cache.heroes:
        flags = []
        if h.get("unit_state", 0) & (1 << 5): flags.append("STUNNED")
        if h.get("unit_state", 0) & (1 << 3): flags.append("SILENCED")
        state_str = f" [{','.join(flags)}]" if flags else ""
        print(f"    {h['name']}: HP={h['hp']}/{h.get('max_hp','?')} "
              f"Mana={h.get('mana',0):.0f}/{h.get('max_mana',0):.0f} "
              f"Lv={h.get('level','?')} Team={h['team']} "
              f"Pos=({h['x']:.0f},{h['y']:.0f}){state_str}")

    # ── Test 3: Local hero detection ──
    print("\n-- Test 3: Local Hero Detection --")
    hero_result = game.find_local_hero()
    if hero_result:
        hero_ent, hero_name = hero_result
        print(f"  OK Local hero: {hero_name} at {hex(hero_ent)}")
        # Check if it's in cache
        in_cache = any(h["entity"] == hero_ent for h in cache.heroes)
        print(f"  In EntityCache: {'OK YES' if in_cache else 'FAIL NO'}")

        # Read detailed stats
        hp = mem.read_i32(hero_ent + BaseEntity.HEALTH)
        max_hp = mem.read_i32(hero_ent + BaseEntity.MAX_HEALTH)
        mana = mem.read_f32(hero_ent + BaseNPC.MANA)
        max_mana = mem.read_f32(hero_ent + BaseNPC.MAX_MANA)
        level = mem.read_i32(hero_ent + BaseNPC.CURRENT_LEVEL)
        ap = mem.read_i32(hero_ent + BaseHero.ABILITY_POINTS)
        strength = mem.read_f32(hero_ent + BaseHero.STRENGTH)
        agi = mem.read_f32(hero_ent + BaseHero.AGILITY)
        intel = mem.read_f32(hero_ent + BaseHero.INTELLECT)
        print(f"  HP: {hp}/{max_hp}, Mana: {mana:.0f}/{max_mana:.0f}, Level: {level}")
        print(f"  STR/AGI/INT: {strength:.1f}/{agi:.1f}/{intel:.1f}, AbilityPoints: {ap}")

        # Abilities
        abilities = game.get_hero_abilities(hero_ent, cache.handle_index)
        resolved = [a for a in abilities if a.get("name")]
        print(f"  Abilities: {len(resolved)} resolved")
        for ab in resolved[:8]:  # show first 8
            cd_str = f" CD={ab['cooldown']:.1f}" if ab['cooldown'] > 0 else ""
            print(f"    [{ab['slot']:2d}] {ab['name']} Lv={ab['level']}{cd_str}")

        # Items
        items = game.get_hero_items(hero_ent, cache.handle_index)
        print(f"  Items: {len(items)}")
        for it in items:
            print(f"    [{it['slot']:2d}] {it['name']}")
    else:
        print(f"  FAIL Local hero NOT found")

    # ── Test 4: Gold research ──
    print("\n-- Test 4: Gold Research --")
    print(f"  data_radiant: {hex(cache.data_radiant) if cache.data_radiant else 'not found'}")
    print(f"  data_dire: {hex(cache.data_dire) if cache.data_dire else 'not found'}")

    # Try to find gold by scanning DataNonSpectator entities
    for team_name, data_ent in [("radiant", cache.data_radiant), ("dire", cache.data_dire)]:
        if not data_ent:
            print(f"  [{team_name}] Entity not found, skipping")
            continue
        print(f"\n  [{team_name}] Entity at {hex(data_ent)}")

        # Dump first 0x100 bytes to find structure
        try:
            header = mem.pm.read_bytes(data_ent, 0x100)
            print(f"    First 256 bytes (looking for pointers/counts):")
            for off in range(0, 0x100, 8):
                val = struct.unpack("<Q", header[off:off+8])[0]
                i32_lo = struct.unpack("<I", header[off:off+4])[0]
                i32_hi = struct.unpack("<I", header[off+4:off+8])[0]
                # Show interesting values (pointers, small ints, etc.)
                if 0x10000 < val < 0x7FFF00000000:
                    print(f"    +{off:04X}: ptr {hex(val)}")
                elif 0 < i32_lo <= 10 and off > 0x50:
                    print(f"    +{off:04X}: i32={i32_lo} (possible count?)")
        except Exception as e:
            print(f"    Read failed: {e}")

        # Try known offsets: DATA_TEAM_PTR=0x5F0
        try:
            vec_ptr = mem.read_ptr(data_ent + 0x5F0)
            count = mem.read_i32(data_ent + 0x5F8)
            print(f"\n    +0x5F0 (DATA_TEAM_PTR): {hex(vec_ptr) if vec_ptr else 'NULL'}")
            print(f"    +0x5F8 (DATA_TEAM_COUNT): {count}")
            if vec_ptr and 0x10000 < vec_ptr < 0x7FFF00000000 and 0 < count <= 10:
                print(f"    Looks like a valid vector! Trying gold reads...")
                for slot in range(min(count, 5)):
                    base = vec_ptr + slot * 0x1220
                    try:
                        rel_gold = mem.read_i32(base + 0x30)
                        unrel_gold = mem.read_i32(base + 0x34)
                        total_earned = mem.read_i32(base + 0x3C)
                        net_worth = mem.read_i32(base + 0x8C)
                        last_hits = mem.read_i32(base + 0x94)
                        denies = mem.read_i32(base + 0x98)
                        print(f"    Slot {slot}: gold={rel_gold}+{unrel_gold}={rel_gold+unrel_gold}, "
                              f"earned={total_earned}, nw={net_worth}, lh={last_hits}, dn={denies}")
                    except:
                        print(f"    Slot {slot}: read failed")
        except Exception as e:
            print(f"    +0x5F0 read failed: {e}")

        # Brute-force scan for gold-like values in entity
        # Gold in early game is typically 500-700
        print(f"\n    Scanning entity for gold-like i32 values (500-1000)...")
        try:
            scan_size = 0x2000
            data = mem.pm.read_bytes(data_ent, scan_size)
            hits = []
            for off in range(0, scan_size - 4, 4):
                val = struct.unpack("<i", data[off:off+4])[0]
                if 500 <= val <= 1000:
                    hits.append((off, val))
            print(f"    Found {len(hits)} values in 500-1000 range")
            for off, val in hits[:20]:  # show first 20
                print(f"      +{off:04X}: {val}")
        except Exception as e:
            print(f"    Scan failed: {e}")

    # ── Test 5: iter_entities generator cleanup ──
    print("\n-- Test 5: Generator Cleanup --")
    gen = game.iter_entities(max_count=10)
    count = 0
    try:
        for ent, ident, dname in gen:
            count += 1
            if count >= 3:
                break
    finally:
        gen.close()
    print(f"  OK Generator cleanly closed after {count} entities (no RuntimeError)")

    print("\n" + "=" * 60)
    print("Done!")

if __name__ == "__main__":
    main()
