"""
Test BotBrain — load bot_generic.lua with mocked API.
Run while in a game (bot practice or any match).
"""
import sys, os
sys.path.insert(0, os.path.dirname(__file__))

from cheat.memory import DotaMemory
from cheat.game_state import DotaGame
from cheat.commands import DotaCommands
from cheat.bot_brain import BotBrain

def main():
    mem = DotaMemory()
    print(f"[+] PID {mem.pid}")

    game = DotaGame(mem)
    game.init()

    cmd = DotaCommands(mem)
    cmd.init(game)

    # Create BotBrain — specify OUR hero
    brain = BotBrain(
        mem, game, cmd,
        scripts_dir="C:/Users/aleks/Downloads/dota2bot-OpenHyperAI",
        hero_name="npc_dota_hero_sven",  # our hero on Radiant
    )

    if not brain.init():
        print("[!] BotBrain init failed")
        return

    print(f"\n[+] Hero: {brain.hero_name}")
    print(f"[+] Team: {'Radiant' if brain._our_team == 2 else 'Dire'}")
    print(f"[+] Heroes in cache: {len(brain.cache.heroes)}")
    print(f"[+] Creeps in cache: {len(brain.cache.creeps)}")
    print(f"[+] Towers in cache: {len(brain.cache.towers)}")
    print(f"[+] Modes loaded: {list(brain._modes.keys())}")

    # Show require errors from Lua
    try:
        errs = brain.lua.globals()._require_errors
        if errs:
            err_list = []
            for k in errs:
                err_list.append(str(k))
            if err_list:
                print(f"[!] Require fallbacks ({len(err_list)}): {', '.join(err_list[:10])}")
    except:
        pass

    # Run with modes enabled (fallback AI if no modes work)
    print(f"\n[+] Running Think loop for 60s (modes={'ON' if brain._modes else 'FALLBACK'})...")
    brain.run(tick_rate=0.3, duration=60)

    print("\nDone!")


if __name__ == "__main__":
    main()
