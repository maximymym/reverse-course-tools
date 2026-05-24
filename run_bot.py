"""
Run BotBrain — waits for game to start, then runs Think loop.
Usage: python run_bot.py [hero_name] [duration_sec]
"""
import sys, os, time
sys.path.insert(0, os.path.dirname(__file__))

from cheat.memory import DotaMemory
from cheat.game_state import DotaGame
from cheat.commands import DotaCommands
from cheat.bot_brain import BotBrain

def main():
    hero_name = sys.argv[1] if len(sys.argv) > 1 else None
    duration = int(sys.argv[2]) if len(sys.argv) > 2 else 120

    mem = DotaMemory()
    print(f"[+] PID {mem.pid}")

    game = DotaGame(mem)
    game.init()

    # Wait for game to be in progress
    print("[*] Waiting for game to start (state=5)...")
    while True:
        try:
            state = game.get_game_state()
            gt = game.get_game_time()
            if state == 5:  # GAME_IN_PROGRESS
                print(f"[+] Game in progress! time={gt:.1f}")
                break
            elif state == 4:  # PRE_GAME
                print(f"  pre-game (state=4), time={gt:.1f}, waiting...")
            elif state == 2:  # HERO_SELECTION
                print(f"  hero selection (state=2), pick a hero...")
            else:
                print(f"  state={state}, waiting...")
        except:
            pass
        time.sleep(2)

    # Wait a few seconds for entities to populate
    time.sleep(3)

    cmd = DotaCommands(mem)
    cmd.init(game)

    brain = BotBrain(
        mem, game, cmd,
        scripts_dir="C:/Users/aleks/Downloads/dota2bot-OpenHyperAI",
        hero_name=hero_name,
        algo_log=True,
    )

    if not brain.init():
        print("[!] BotBrain init failed")
        # Debug: show entity scan results
        brain.cache.update(game, mem)
        print(f"  Heroes: {[h['name'] for h in brain.cache.heroes]}")
        print(f"  Creeps: {len(brain.cache.creeps)}")
        print(f"  Game time: {brain.cache.game_time}")
        return

    print(f"\n[+] Hero: {brain.hero_name}")
    print(f"[+] Team: {'Radiant' if brain._our_team == 2 else 'Dire'}")
    print(f"[+] Heroes: {len(brain.cache.heroes)}")
    print(f"[+] Creeps: {len(brain.cache.creeps)}")
    print(f"[+] DotaTime: {brain.cache.game_time:.1f}")

    # Show new stats we just wired up
    d = brain._get_hero_data()
    if d:
        print(f"[+] Stats from memory:")
        print(f"    HP regen: {d.get('health_regen', '?')}")
        print(f"    Mana regen: {d.get('mana_regen', '?')}")
        print(f"    Move speed: {d.get('move_speed', '?')}")
        print(f"    Attack range: {d.get('attack_range', '?')}")
        print(f"    Armor: {d.get('physical_armor', '?')}")
        print(f"    Magic resist: {d.get('magic_resist', '?')}")
        print(f"    Day vision: {d.get('day_vision', '?')}")
        print(f"    Unit state: 0x{d.get('unit_state', 0):X}")

    gold = brain._get_gold()
    print(f"[+] Gold: {gold}")

    print(f"\n[+] Running for {duration}s (tick_rate=0.3)...")
    brain.run(tick_rate=0.3, duration=duration)
    print("\nDone!")


if __name__ == "__main__":
    main()
