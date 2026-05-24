"""
Research Bot Takeover — Phase 3: Analyze running bot practice game.
Game is already loaded (dota_start_ai_game worked).
"""
import sys, os, time, struct, math
sys.path.insert(0, os.path.dirname(__file__))

from cheat.memory import DotaMemory
from cheat.commands import DotaCommands, ConVarSystem
from cheat.game_state import DotaGame
from cheat.offsets import GameState, BaseEntity, GameSceneNode, BaseNPC

def main():
    mem = DotaMemory()
    print(f"[+] PID {mem.pid}")

    game = DotaGame(mem)
    game.init()

    cmd = DotaCommands(mem)
    cmd.init(game)

    state = game.get_game_state()
    print(f"[+] Game state: {state} ({GameState.NAMES.get(state, '?')})")

    # 1. List ALL entity types
    print("\n=== ALL ENTITIES ===")
    type_counts = {}
    all_ents = []
    for entity, ident, name in game.iter_entities():
        all_ents.append((entity, ident, name))
        type_counts[name] = type_counts.get(name, 0) + 1

    print(f"Total: {len(all_ents)} entities, {len(type_counts)} types")
    for name, cnt in sorted(type_counts.items(), key=lambda x: -x[1])[:40]:
        print(f"  {cnt:4d}x {name}")

    # 2. Player controllers
    print("\n=== PLAYER CONTROLLERS ===")
    controllers = []
    for entity, ident, name in all_ents:
        if name == "dota_player_controller":
            slot = mem.read_i32(entity + 0x908)
            pname = ""
            try:
                pname = mem.read_string(entity + 0x6E0, 64)
            except:
                pass

            # Try multiple possible hPawn offsets
            h_pawn_candidates = {}
            for off in [0x808, 0x810, 0x818, 0x820, 0x828]:
                try:
                    v = mem.read_u32(entity + off)
                    if v != 0 and v != 0xFFFFFFFF:
                        h_pawn_candidates[off] = v
                except:
                    pass

            # SteamID at +0x708 or nearby
            steam_id = 0
            try:
                steam_id = mem.read_u64(entity + 0x768)
            except:
                pass

            info = {"addr": entity, "slot": slot, "name": pname, "steam_id": steam_id,
                    "h_pawn": h_pawn_candidates}
            controllers.append(info)
            is_local = " [LOCAL]" if slot == 0 else ""
            print(f"  slot={slot:2d} name='{pname:20s}' steamid={steam_id} hPawn={h_pawn_candidates}{is_local}")

    # 3. Heroes
    print("\n=== HEROES ===")
    heroes = []
    for entity, ident, name in all_ents:
        if name and "hero" in name.lower() and "selection" not in name.lower():
            try:
                scene = mem.read_ptr(entity + BaseEntity.GAME_SCENE_NODE)
                x = mem.read_f32(scene + GameSceneNode.VEC_ABS_ORIGIN)
                y = mem.read_f32(scene + GameSceneNode.VEC_ABS_ORIGIN + 4)
                z = mem.read_f32(scene + GameSceneNode.VEC_ABS_ORIGIN + 8)
                hp = mem.read_i32(entity + BaseEntity.HEALTH)
                team = mem.read_u8(entity + BaseEntity.TEAM_NUM)
                level = mem.read_i32(entity + BaseNPC.CURRENT_LEVEL)
                team_name = {2: "Radiant", 3: "Dire"}.get(team, f"T{team}")
                heroes.append({"entity": entity, "name": name, "x": x, "y": y, "team": team})
                print(f"  {name:45s} team={team_name:8s} hp={hp:5d} lvl={level} pos=({x:.0f},{y:.0f},{z:.0f})")
            except Exception as e:
                print(f"  {name}: ERROR {e}")

    # 4. Test bot AI commands
    print("\n=== BOT AI TESTS ===")

    # Set convars via direct memory (bypasses flag check)
    print("[>] dota_bot_disable = 0")
    cmd.convar.set_int("dota_bot_disable", 0)

    print("[>] dota_bot_takeover_disconnected = 1")
    cmd.convar.set_int("dota_bot_takeover_disconnected", 1)

    print("[>] dota_bot_allow_human_control = 1")
    cmd.convar.set_int("dota_bot_allow_human_control", 1)

    # Try console commands
    for c in ["dota_bot_populate", "dota_bot_takeover", "bot_add"]:
        print(f"[>] cmd: {c}")
        cmd.execute(c)
        time.sleep(0.5)

    # 5. Track ALL hero positions for 15s to see who moves (= has bot AI)
    print("\n=== HERO MOVEMENT TRACKING (15s) ===")
    if heroes:
        initial_pos = {}
        for h in heroes:
            initial_pos[h["name"]] = (h["x"], h["y"])

        time.sleep(15)

        print(f"\n{'Hero':45s} {'Team':8s} {'Distance':>10s} {'Status'}")
        print("-" * 80)
        for h in heroes:
            try:
                scene = mem.read_ptr(h["entity"] + BaseEntity.GAME_SCENE_NODE)
                x2 = mem.read_f32(scene + GameSceneNode.VEC_ABS_ORIGIN)
                y2 = mem.read_f32(scene + GameSceneNode.VEC_ABS_ORIGIN + 4)
                x1, y1 = initial_pos[h["name"]]
                dist = math.sqrt((x2-x1)**2 + (y2-y1)**2)
                team_name = {2: "Radiant", 3: "Dire"}.get(h["team"], "?")
                status = "MOVING (bot AI)" if dist > 50 else "STATIONARY"
                print(f"  {h['name']:43s} {team_name:8s} {dist:10.1f}  {status}")
            except:
                print(f"  {h['name']:43s} ERROR")

    # 6. Check if our local hero moved (the key question)
    print("\n=== KEY QUESTION: Is local hero controlled by bot AI? ===")
    # Find local hero (Radiant, slot 0 — likely the one we control)
    # In bot practice, our hero gets auto-picked if we didn't pick
    # The bots should be moving to lanes. If OUR hero also moves → bot AI is active on it

    print("\nDone!")


if __name__ == "__main__":
    main()
