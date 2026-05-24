"""
Test GC — init + start finding match.
Run with Dota 2 open in main menu.
Check if "Searching" appears in the UI.
"""
import sys, os, time
sys.path.insert(0, os.path.dirname(__file__))

from cheat.memory import DotaMemory
from cheat.gc import DotaGC, GameMode, Region

mem = DotaMemory()
gc = DotaGC(mem)

print("=== Init GC ===")
ok = gc.init()
print(f"GC init: {ok}")
if not ok:
    sys.exit(1)

# Test: start finding match (Turbo, Russia)
print("\n=== Start Finding Match (Turbo, Russia) ===")
ok = gc.start_finding_match(game_modes=GameMode.TURBO, matchgroups=Region.RUSSIA)
print(f"start_finding_match: {ok}")

if ok:
    print("[*] Check Dota 2 UI - should show 'Searching for match'")
    print("[*] Waiting 5 seconds...")
    time.sleep(5)

    print("\n=== Stop Finding Match ===")
    ok = gc.stop_finding_match()
    print(f"stop_finding_match: {ok}")

mem.close()
