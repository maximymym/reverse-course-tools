"""
Probe m_nHeroPickState offset in CDOTAGamerules at runtime.

Usage:
    1. Start a match (All Pick) and wait for hero selection
    2. Run: python probe_hero_pick_state.py
    3. Script snapshots gamerules memory during ban phase
    4. Wait ~30s for ban→pick transition
    5. Second snapshot + diff → candidate offsets

Requires Dota 2 to be in HERO_SELECTION state (game_state == 2).
"""
import sys
import os
import time
import struct

sys.path.insert(0, os.path.dirname(__file__))

from cheat.memory import DotaMemory
from cheat.game_state import DotaGame
from cheat.offsets import Gamerules

# Known offsets to exclude from candidates
KNOWN_OFFSETS = {
    Gamerules.PAUSED,           # 0x38
    Gamerules.SERVER_GAME_STATE,# 0x74
    Gamerules.GAME_STATE,       # 0x7C
    Gamerules.GAME_MODE,        # 0xE4
    Gamerules.GAME_TIME,        # 0x30
    Gamerules.GAME_START_TIME,  # 0x34
}

# Scan range around game_state (0x7C) — cover 0x60..0x180
SCAN_START = 0x60
SCAN_END = 0x180
SCAN_SIZE = SCAN_END - SCAN_START


def snapshot_gamerules(mem, gamerules_addr):
    """Read raw bytes from gamerules + SCAN_START..SCAN_END."""
    return mem.pm.read_bytes(gamerules_addr + SCAN_START, SCAN_SIZE)


def diff_snapshots(snap1, snap2):
    """Find int32 fields that changed, where both old and new are in [0, 20]."""
    candidates = []
    for off in range(0, SCAN_SIZE - 3, 4):
        val1 = struct.unpack_from("<i", snap1, off)[0]
        val2 = struct.unpack_from("<i", snap2, off)[0]
        if val1 == val2:
            continue
        real_off = SCAN_START + off
        if real_off in KNOWN_OFFSETS:
            continue
        if 0 <= val1 <= 20 and 0 <= val2 <= 20:
            candidates.append((real_off, val1, val2))
    return candidates


def main():
    print("[*] Probing m_nHeroPickState offset...")
    print("[*] Attaching to Dota 2...")

    mem = DotaMemory()
    game = DotaGame(mem)
    if not game.init():
        print("[!] Failed to init DotaGame")
        return

    state = game.get_game_state()
    print(f"[*] Current game state: {state} ({game.get_game_state_name()})")

    if state < 2:
        print("[*] Waiting for HERO_SELECTION (state 2)...")
        deadline = time.time() + 120
        while time.time() < deadline:
            game._find_gamerules()
            state = game.get_game_state()
            if state >= 2:
                break
            time.sleep(2)
        else:
            print("[!] Timeout waiting for hero selection")
            return

    if state > 2:
        print("[!] Already past hero selection (state > 2). Start a new match.")
        return

    print(f"[+] In HERO_SELECTION. Gamerules @ {hex(game.gamerules)}")
    print(f"[*] Taking snapshot #1 (ban phase)...")
    snap1 = snapshot_gamerules(mem, game.gamerules)
    t1 = time.time()

    print(f"[*] Waiting 30s for ban→pick transition...")
    print(f"    (watch the hero selection UI — when picks start, it's time)")
    time.sleep(30)

    print(f"[*] Taking snapshot #2 (should be pick phase now)...")
    snap2 = snapshot_gamerules(mem, game.gamerules)

    candidates = diff_snapshots(snap1, snap2)

    if not candidates:
        print("[!] No candidate offsets found. Try again with different timing.")
        return

    print(f"\n[+] Candidate offsets for m_nHeroPickState:")
    print(f"    {'Offset':<12} {'Ban value':<12} {'Pick value':<12}")
    print(f"    {'-'*36}")
    for off, v1, v2 in candidates:
        marker = " <-- likely" if v2 > v1 and v1 <= 2 and v2 <= 10 else ""
        print(f"    {hex(off):<12} {v1:<12} {v2:<12}{marker}")

    print(f"\n[*] To apply: set Gamerules.HERO_PICK_STATE = <offset> in cheat/offsets.py")
    print(f"[*] Then update HeroPickPhase values based on ban/pick values above")


if __name__ == "__main__":
    main()
