"""
VScript execution test — verify it works with visible side effects.
Then use it to add bots properly.
"""
import sys, os, time
sys.path.insert(0, os.path.dirname(__file__))
from cheat.memory import DotaMemory
from cheat.commands import DotaCommands
from cheat.game_state import DotaGame
from cheat.offsets import GameState

def main():
    mem = DotaMemory()
    print(f"[+] PID {mem.pid}")
    game = DotaGame(mem)
    game.init()
    cmd = DotaCommands(mem)
    cmd.init(game)

    state = game.get_game_state()
    print(f"[+] Game state: {state} ({GameState.NAMES.get(state, '?')})")

    if state != GameState.GAME_IN_PROGRESS:
        print("[!] Need GAME_IN_PROGRESS. Exiting.")
        return

    # === Test 1: VScript with visible side effects ===
    print("\n=== TEST 1: VScript basic execution ===")
    print("  Testing if 'script' command works...")

    # Give gold to player 0 — should be visible in UI
    print("  [>] script PlayerResource:SetGold(0, 99999, true)")
    cmd.execute('script PlayerResource:SetGold(0, 99999, true)')
    time.sleep(2)

    # Set time of day — visible change
    print("  [>] script GameRules:SetTimeOfDay(0.0) [should make it night]")
    cmd.execute('script GameRules:SetTimeOfDay(0.0)')
    time.sleep(2)

    # Check: did gold change? Read via ConVar or VScript output
    # We can't easily read VScript output, but we can read gold from memory
    # For now, let the user confirm visually

    # Try writing to console to verify script runs
    print("  [>] script Msg('VSCRIPT_WORKS_12345')")
    cmd.execute("script Msg('VSCRIPT_WORKS_12345')")
    time.sleep(1)

    # === Test 2: VScript bot commands ===
    print("\n=== TEST 2: VScript bot creation ===")

    bot_cmds = [
        # SetBotThinkingEnabled — enable bot AI system
        'script GameRules:GetGameModeEntity():SetBotThinkingEnabled(true)',

        # BotPopulate — fill empty slots with bots
        'script GameRules:BotPopulate()',

        # Wait and check
    ]
    for c in bot_cmds:
        print(f"  [>] {c}")
        cmd.execute(c)
        time.sleep(3)

    # Check controllers
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

    # Check heroes
    print("\n  Heroes:")
    from cheat.offsets import BaseEntity, GameSceneNode, BaseNPC
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
            t = {2: "Rad", 3: "Dire"}.get(team, "?")
            print(f"    {uname:40s} {t} hp={hp} pos=({x:.0f},{y:.0f})")
        except:
            continue

    if state == GameState.GAME_IN_PROGRESS:
        print("\n=== TEST 3: AddBotPlayerWithEntityScript ===")

        # This is the official VScript API for adding bots
        # Parameters: hero_name, team (2=Rad, 3=Dire), hero_build_file, bot_script_file
        # Bot script file: empty string = use default bot AI
        add_cmds = [
            'script GameRules:AddBotPlayerWithEntityScript("npc_dota_hero_sven", 3, "", "")',
            'script GameRules:AddBotPlayerWithEntityScript("npc_dota_hero_crystal_maiden", 2, "", "")',
        ]
        for c in add_cmds:
            print(f"  [>] {c}")
            cmd.execute(c)
            time.sleep(3)

        # Check again
        print("\n  Controllers after AddBot:")
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

        # Heroes again
        print("\n  Heroes:")
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
                t = {2: "Rad", 3: "Dire"}.get(team, "?")
                print(f"    {uname:40s} {t} hp={hp} pos=({x:.0f},{y:.0f})")
            except:
                continue

    # === Test 4: Check if bots are moving (15s) ===
    print("\n=== TEST 4: Movement check (15s) ===")
    import math

    hero_init = {}
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
            hero_init[entity] = (uname, x, y, team)
        except:
            continue

    time.sleep(15)

    for entity, (uname, x0, y0, team) in hero_init.items():
        try:
            scene = mem.read_ptr(entity + BaseEntity.GAME_SCENE_NODE)
            x = mem.read_f32(scene + GameSceneNode.VEC_ABS_ORIGIN)
            y = mem.read_f32(scene + GameSceneNode.VEC_ABS_ORIGIN + 4)
            dist = math.sqrt((x-x0)**2 + (y-y0)**2)
            t = {2: "Rad", 3: "Dire"}.get(team, "?")
            status = "MOVING!" if dist > 50 else "static"
            print(f"  {uname:40s} {t} dist={dist:8.1f} {status}")
        except:
            pass

    print("\n[*] CHECK DOTA VISUALLY:")
    print("    - Did gold change to 99999?")
    print("    - Did time become night?")
    print("    - Are there new heroes?")
    print("    - Are bots moving?")
    print("\nDone!")


if __name__ == "__main__":
    main()
