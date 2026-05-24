"""
Research Bot Takeover — Phase 2: Test commands and strategies.

PLAN:
1. Try starting a local bot practice game via console
2. Once in game, test bot AI activation
3. Analyze player controllers for bot flags

Run from menu — script will try to start a practice game automatically.
"""
import sys, os, time, struct
sys.path.insert(0, os.path.dirname(__file__))

from cheat.memory import DotaMemory
from cheat.commands import DotaCommands, ConVarSystem
from cheat.game_state import DotaGame
from cheat.offsets import GameState

def wait_for_game_state(game, target_state, timeout=60):
    """Wait for game state to reach target. Returns True if reached."""
    start = time.time()
    while time.time() - start < timeout:
        try:
            game._find_gamerules()
            state = game.get_game_state()
            state_name = GameState.NAMES.get(state, f"UNKNOWN({state})")
            print(f"  game_state = {state} ({state_name})", end='\r')
            if state == target_state:
                print()
                return True
        except:
            print(f"  waiting for gamerules...", end='\r')
        time.sleep(1)
    print()
    return False


def scan_convars_matching(convar, keyword):
    """Quick scan returning list of (name, value_int, flags)."""
    results = []
    batch = min(convar.arr3_count, 8000)
    data = convar.mem.pm.read_bytes(convar.arr3_ptr, batch * 0x10)
    for i in range(batch):
        entry_ptr = struct.unpack("<Q", data[i*0x10:i*0x10+8])[0]
        if not entry_ptr or entry_ptr < 0x10000:
            continue
        try:
            name_ptr = convar.mem.read_ptr(entry_ptr + ConVarSystem.CV_NAME)
            if not name_ptr or name_ptr < 0x10000:
                continue
            name = convar.mem.read_string(name_ptr, 128)
            if keyword.lower() in name.lower():
                val = convar.mem.read_i32(entry_ptr + ConVarSystem.CV_VALUE)
                flags = convar.mem.read_u32(entry_ptr + ConVarSystem.CV_FLAGS)
                results.append((name, val, flags, entry_ptr))
        except:
            continue
    return results


def analyze_controllers(game, mem):
    """Dump all dota_player_controller entities with key fields."""
    controllers = []
    for entity, ident, name in game.iter_entities():
        if name == "dota_player_controller":
            slot = mem.read_i32(entity + 0x908)
            player_name = ""
            try:
                player_name = mem.read_string(entity + 0x6E0, 64)
            except:
                pass

            # Read hPawn (handle to controlled hero)
            h_pawn = mem.read_u32(entity + 0x810)

            info = {
                "addr": entity,
                "slot": slot,
                "name": player_name,
                "h_pawn": h_pawn,
            }

            # Scan for potential bot flag — dump bytes around known offsets
            # In Source 2, CBasePlayerController typically has:
            #   m_bIsHLTV, m_bIsLocalPlayerController
            # CDOTAPlayerController may have bot-related fields
            # Let's scan a range and look for interesting byte patterns

            # Read large block for analysis
            try:
                block = mem.pm.read_bytes(entity + 0x700, 0x300)
                info["block_700"] = block
            except:
                info["block_700"] = None

            controllers.append(info)
    return controllers


def main():
    print("=" * 60)
    print("Bot Takeover Research — Phase 2")
    print("=" * 60)

    mem = DotaMemory()
    print(f"[+] Attached PID {mem.pid}")

    game = DotaGame(mem)
    game.init()

    cmd = DotaCommands(mem)
    cmd.init(game)

    # Check current game state
    try:
        game._find_gamerules()
        state = game.get_game_state()
        print(f"[+] Current game state: {state} ({GameState.NAMES.get(state, '?')})")
        in_game = state >= GameState.HERO_SELECTION
    except:
        print("[*] No gamerules found — we're in menu")
        in_game = False

    if not in_game:
        print("\n" + "=" * 60)
        print("STEP 1: Start local bot practice game")
        print("=" * 60)

        # Set practice game ConVars
        print("[>] Setting bot practice ConVars...")
        cmd.convar.set_int("dota_bot_practice_difficulty", 1)  # easy
        cmd.convar.set_int("dota_bot_practice_gamemode", 1)    # All Pick
        cmd.convar.set_int("dota_bot_practice_team", 0)        # Radiant

        # Try to start AI game via console command
        print("[>] Trying: dota_start_ai_game 1")
        cmd.execute("dota_start_ai_game 1")
        time.sleep(3)

        # Check if loading started
        game._find_gamerules()
        try:
            state = game.get_game_state()
            print(f"[*] After dota_start_ai_game: state = {state}")
        except:
            print("[*] No gamerules yet — checking if map is loading...")

        # Wait for game to load
        print("[>] Waiting for game to load (up to 90s)...")
        if not wait_for_game_state(game, GameState.HERO_SELECTION, timeout=90):
            # Maybe already past hero selection
            try:
                state = game.get_game_state()
                if state >= GameState.PRE_GAME:
                    print(f"[+] Already past hero select: state = {state}")
                else:
                    print("[!] Game didn't start. Try manually creating a lobby with bots.")
                    print("[!] Or try: dota_bot_practice_start 1")
                    cmd.execute("dota_bot_practice_start 1")
                    time.sleep(5)

                    print("[>] Trying map load via console...")
                    cmd.execute("map dota")
                    print("[>] Waiting 90s for map load...")
                    if not wait_for_game_state(game, GameState.HERO_SELECTION, timeout=90):
                        print("[!] Still no game. Exiting.")
                        return
            except:
                print("[!] Game didn't load. Please start a local bot game manually.")
                return

    # === IN GAME ===
    print("\n" + "=" * 60)
    print("STEP 2: In-Game Analysis")
    print("=" * 60)

    # Re-init to find entities
    game.init()
    state = game.get_game_state()
    print(f"[+] Game state: {state} ({GameState.NAMES.get(state, '?')})")

    # If hero selection, pick a hero first
    if state == GameState.HERO_SELECTION:
        print("[>] Hero selection — picking Wraith King")
        cmd.execute("dota_select_hero npc_dota_hero_wraith_king")
        time.sleep(5)
        state = game.get_game_state()
        print(f"[+] Game state after pick: {state}")

    # Wait for game to be in progress
    if state < GameState.GAME_IN_PROGRESS:
        print("[>] Waiting for GAME_IN_PROGRESS...")
        wait_for_game_state(game, GameState.GAME_IN_PROGRESS, timeout=120)

    # Analyze controllers
    print("\n--- Player Controllers ---")
    controllers = analyze_controllers(game, mem)
    print(f"Found {len(controllers)} controllers")

    for c in controllers:
        is_local = " [LOCAL]" if c["slot"] == 0 else ""
        print(f"\n  Controller {hex(c['addr'])}: slot={c['slot']}, name='{c['name']}', hPawn=0x{c['h_pawn']:08X}{is_local}")

        if c["block_700"]:
            block = c["block_700"]
            # Look for patterns: bytes that differ between local player and bots
            # Print key offsets in the 0x700-0xA00 range
            for rel_off in range(0, min(len(block), 0x200), 8):
                abs_off = 0x700 + rel_off
                val = struct.unpack("<Q", block[rel_off:rel_off+8])[0]
                if val != 0:  # Only print non-zero
                    val_lo = val & 0xFFFFFFFF
                    val_hi = (val >> 32) & 0xFFFFFFFF
                    # Highlight potential bool flags (0 or 1)
                    marker = ""
                    byte_val = block[rel_off]
                    if byte_val in (0, 1) and val < 0x100:
                        marker = f" ← possible bool ({byte_val})"
                    print(f"    +0x{abs_off:03X}: 0x{val:016X} ({val_lo:10d} | {val_hi:10d}){marker}")

    # Test bot-related commands
    print("\n" + "=" * 60)
    print("STEP 3: Testing bot commands")
    print("=" * 60)

    # Check current bot_disable state
    bd = cmd.convar.get_int("dota_bot_disable")
    print(f"  dota_bot_disable = {bd}")

    # Try setting dota_bot_disable to 0 (enable bot AI)
    print("\n[>] Setting dota_bot_disable = 0 (via direct memory write)")
    cmd.convar.set_int("dota_bot_disable", 0)
    time.sleep(1)
    bd2 = cmd.convar.get_int("dota_bot_disable")
    print(f"  dota_bot_disable = {bd2} (after write)")

    # Try bot_mimic (makes bots mimic player input)
    print("\n[>] Testing bot_mimic = 1")
    cmd.convar.set_int("bot_mimic", 1)

    # Try dota_bot_takeover_disconnected
    print("[>] Setting dota_bot_takeover_disconnected = 1")
    cmd.convar.set_int("dota_bot_takeover_disconnected", 1)

    # Try dota_bot_allow_human_control
    print("[>] Setting dota_bot_allow_human_control = 1")
    cmd.convar.set_int("dota_bot_allow_human_control", 1)

    # Try console commands
    print("\n[>] Testing console commands:")

    test_cmds = [
        "dota_bot_populate",
        "bot_add",
        "dota_bot_takeover",
    ]

    for tc in test_cmds:
        print(f"  Executing: {tc}")
        result = cmd.execute(tc)
        print(f"    Result: {'OK' if result else 'FAIL'}")
        time.sleep(1)

    print("\n[>] Waiting 5s for bot AI to potentially activate...")
    time.sleep(5)

    # Re-check controllers — any new bot entities?
    print("\n--- Controllers after bot commands ---")
    game.init()
    controllers2 = analyze_controllers(game, mem)
    print(f"Found {len(controllers2)} controllers (was {len(controllers)})")
    for c in controllers2:
        print(f"  slot={c['slot']}, name='{c['name']}', hPawn=0x{c['h_pawn']:08X}")

    # Scan for entities with "bot" in designerName
    print("\n--- Bot-related entities ---")
    for entity, ident, name in game.iter_entities():
        if name and ("bot" in name.lower() or "ai" in name.lower()):
            print(f"  {name}: entity={hex(entity)}")

    # Final summary
    print("\n" + "=" * 60)
    print("STEP 4: Check hero movement (is bot AI controlling?)")
    print("=" * 60)

    # Find our hero entity and read position twice
    local_ctrl = None
    for c in controllers2:
        if c["slot"] == 0:
            local_ctrl = c
            break

    if local_ctrl and local_ctrl["h_pawn"]:
        # Resolve pawn from handle
        handle = local_ctrl["h_pawn"]
        ent_idx = handle & 0x7FFF

        # Read hero position
        from cheat.offsets import GameSceneNode, BaseEntity

        for entity, ident, name in game.iter_entities():
            if name and "hero" in name:
                try:
                    scene = mem.read_ptr(entity + BaseEntity.GAME_SCENE_NODE)
                    if scene:
                        x = mem.read_f32(scene + GameSceneNode.VEC_ABS_ORIGIN)
                        y = mem.read_f32(scene + GameSceneNode.VEC_ABS_ORIGIN + 4)
                        z = mem.read_f32(scene + GameSceneNode.VEC_ABS_ORIGIN + 8)
                        hp = mem.read_i32(entity + BaseEntity.HEALTH)
                        team = mem.read_u8(entity + BaseEntity.TEAM_NUM)
                        print(f"  {name}: pos=({x:.0f},{y:.0f},{z:.0f}) hp={hp} team={team}")
                except:
                    pass

        print("\n[>] Sampling local hero position for 10s to detect bot movement...")
        # Find any hero
        hero_entity = None
        for entity, ident, name in game.iter_entities():
            if name and "hero" in name:
                team = mem.read_u8(entity + BaseEntity.TEAM_NUM)
                hp = mem.read_i32(entity + BaseEntity.HEALTH)
                if team == 2 and hp > 0:  # Radiant, alive
                    hero_entity = entity
                    hero_name = name
                    break

        if hero_entity:
            positions = []
            for i in range(10):
                try:
                    scene = mem.read_ptr(hero_entity + BaseEntity.GAME_SCENE_NODE)
                    x = mem.read_f32(scene + GameSceneNode.VEC_ABS_ORIGIN)
                    y = mem.read_f32(scene + GameSceneNode.VEC_ABS_ORIGIN + 4)
                    positions.append((x, y))
                except:
                    pass
                time.sleep(1)

            if len(positions) >= 2:
                import math
                total_dist = 0
                for i in range(1, len(positions)):
                    dx = positions[i][0] - positions[i-1][0]
                    dy = positions[i][1] - positions[i-1][1]
                    total_dist += math.sqrt(dx*dx + dy*dy)
                print(f"  Hero '{hero_name}' moved {total_dist:.1f} units in {len(positions)-1}s")
                if total_dist > 50:
                    print("  >>> HERO IS MOVING — bot AI may be active!")
                else:
                    print("  >>> Hero stationary — bot AI NOT active on this hero")

    print("\n" + "=" * 60)
    print("RESEARCH COMPLETE")
    print("=" * 60)


if __name__ == "__main__":
    main()
