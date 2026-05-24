"""
Full 5-Bot Flow Test — от запуска Steam до бот-скриптов.

Usage:
    python test_full_5bot.py              # full flow (launch → party → queue → pick → brain)
    python test_full_5bot.py --skip-launch # skip Steam/Dota launch (already running)
    python test_full_5bot.py --skip-queue  # skip matchmaking (already in match)
    python test_full_5bot.py --bots 2      # only 2 bots

Requires:
    - config/accounts.json (5 accounts)
    - config/invite_template.json (cv + ping_data)
    - sandbox.py (BotSandbox)
"""
import sys
import os
import time
import argparse
import threading
import json

sys.path.insert(0, os.path.dirname(__file__))

from sandbox import BotSandbox
from cheat.memory import DotaMemory
from cheat.game_state import DotaGame
from cheat.commands import DotaCommands
from cheat.gc import DotaGC
from cheat.bot_brain import BotBrain
from cheat.gc import GC_MSG_CACHE_SUBSCRIBED, extract_group_id_from_cache_subscribed

# ─── Hero list (verified internal names) ─────────────────────
HERO_POOL = [
    "npc_dota_hero_skeleton_king",   # Wraith King
    "npc_dota_hero_viper",
    "npc_dota_hero_sniper",
    "npc_dota_hero_bristleback",
    "npc_dota_hero_ogre_magi",
    "npc_dota_hero_abaddon",
    "npc_dota_hero_lich",
    "npc_dota_hero_lion",
    "npc_dota_hero_tidehunter",
    "npc_dota_hero_sven",
]


def parse_args():
    p = argparse.ArgumentParser(description="Full 5-bot flow test")
    p.add_argument("--bots", type=int, default=5, help="Number of bots (default: 5)")
    p.add_argument("--skip-launch", action="store_true", help="Skip Steam/Dota launch")
    p.add_argument("--skip-queue", action="store_true", help="Skip party+queue (already in match)")
    p.add_argument("--tick-rate", type=float, default=0.3, help="Bot brain tick rate (default: 0.3)")
    return p.parse_args()


# ═══════════════════════════════════════════════════════════════
# Phase 1: Launch Steam + Dota instances
# ═══════════════════════════════════════════════════════════════

def phase_launch(count: int) -> list[int]:
    """Launch Steam+Dota instances via sandbox. Returns list of dota PIDs."""
    print(f"\n{'='*60}")
    print(f"  PHASE 1: Launch {count} Steam + Dota instances")
    print(f"{'='*60}\n")

    sb = BotSandbox(count=count)
    sb.status()

    existing_pids = sb.get_dota_pids()
    if len(existing_pids) >= count:
        print(f"[+] Already have {len(existing_pids)} Dota instances running")
        return existing_pids[:count]

    if existing_pids:
        print(f"[!] Found {len(existing_pids)} existing Dota instances, need {count}")
        print(f"    Kill them first with sb.kill_all() or launch missing ones manually")

    # Launch one by one
    for i in range(count):
        pids_before = set(sb.get_dota_pids())
        print(f"\n[*] Launching instance #{i}...")

        # launch_dota kills mutex of existing dota, then launches Steam+Dota
        sb.launch_dota(i)

        # Wait for NEW dota2.exe to appear
        deadline = time.time() + 90
        new_pid = None
        while time.time() < deadline:
            pids_now = set(sb.get_dota_pids())
            new_pids = pids_now - pids_before
            if new_pids:
                new_pid = new_pids.pop()
                print(f"[+] Instance #{i} running (PID {new_pid})")
                break
            time.sleep(5)
        else:
            print(f"[!] Timeout waiting for instance #{i}")
            return []

        # After launch, the NEW dota also holds a mutex — kill it for next instance
        if i < count - 1:
            print(f"[*] Waiting 30s for Dota #{i} to fully load...")
            time.sleep(30)
            # Pre-kill mutex of newest dota so next launch_dota doesn't fail
            from sandbox import close_dota_mutex
            close_dota_mutex(new_pid)
            print(f"[+] Mutex killed for PID {new_pid}")
            time.sleep(2)

    # Wait for last instance to fully load
    print(f"\n[*] Waiting 45s for all instances to fully load...")
    time.sleep(45)

    pids = sb.get_dota_pids()
    print(f"[+] All {len(pids)} Dota instances running: {pids}")
    return pids[:count]


# ═══════════════════════════════════════════════════════════════
# Phase 2: Initialize memory + GC + commands for all bots
# ═══════════════════════════════════════════════════════════════

def phase_init(pids: list[int]) -> list[dict]:
    """Initialize DotaMemory, DotaGC, DotaCommands for each bot."""
    print(f"\n{'='*60}")
    print(f"  PHASE 2: Initialize {len(pids)} bot instances")
    print(f"{'='*60}\n")

    bots = []
    for i, pid in enumerate(pids):
        print(f"[*] Init bot #{i} (PID {pid})...")
        # Retry up to 3 times (DLLs may still be loading)
        for attempt in range(3):
            try:
                mem = DotaMemory(pid)
                gc = DotaGC(mem)
                gc.init()
                cmd = DotaCommands(mem)
                cmd.init()
                bots.append({
                    "idx": i,
                    "pid": pid,
                    "mem": mem,
                    "gc": gc,
                    "cmd": cmd,
                    "steam_id": gc.steam_id,
                })
                print(f"  [+] Bot #{i}: steam_id={gc.steam_id}")
                break
            except Exception as e:
                if attempt < 2:
                    print(f"  [!] Bot #{i} attempt {attempt+1} failed: {e}, retrying in 15s...")
                    time.sleep(15)
                else:
                    print(f"  [!] Bot #{i} init failed after 3 attempts: {e}")
                    return []

    print(f"\n[+] All {len(bots)} bots initialized")
    return bots


def _wait_and_accept_with_hook(gc: DotaGC, timeout: float = 15) -> bool:
    """Wait for CacheSubscribed on an ALREADY installed hook, extract party_id, accept."""
    prev = 0
    party_id = 0
    start = time.time()
    try:
        while time.time() - start < timeout:
            log = gc.read_hook_log()
            if log['count'] > prev:
                raw = gc.read_last_message_raw()
                mt, fields = gc.parse_retrieve_buffer(raw)
                if mt == GC_MSG_CACHE_SUBSCRIBED:
                    party_id = extract_group_id_from_cache_subscribed(fields)
                    if party_id:
                        print(f"  [+] Invite received, group_id={party_id}")
                        break
                prev = log['count']
            time.sleep(0.3)
    finally:
        gc.unhook_retrieve_message()

    if not party_id:
        return False

    time.sleep(1.5)
    # Native accept: joins party + closes popup in one call
    ok = gc.accept_party_invite_native(party_id)
    if not ok:
        # Fallback to GC accept (joins party but popup stays)
        print("  [~] Native accept failed, falling back to GC accept")
        ok = gc.accept_party_invite(party_id)
    return ok


def _dismiss_invite_popup(bot: dict):
    """Try to dismiss stale party invite popup after GC accept.

    GC accept joins the party but doesn't close the UI popup,
    leaving 'Приглашение не действительно' on screen.

    NOTE: Only uses safe methods. Traversing/deleting panels crashes Dota.
    The popup is cosmetic — doesn't block gameplay.
    """
    try:
        cmd = bot["cmd"]
        if not cmd._panorama_ready:
            cmd.init_panorama()

        # CloseAllVisiblePopups from default (global) context — safest method
        cmd.run_panorama_js(
            "try{UiToolkitAPI.CloseAllVisiblePopups();}catch(e){}",
            panel=0
        )

        print(f"  [+] Bot #{bot['idx']}: popup dismiss attempted")
    except Exception as e:
        print(f"  [~] Bot #{bot['idx']}: popup dismiss failed: {e}")


# ═══════════════════════════════════════════════════════════════
# Phase 3: Party — bot[0] invites all, others accept
# ═══════════════════════════════════════════════════════════════

def phase_party(bots: list[dict]):
    """Form party: bot[0] invites all others, they accept."""
    print(f"\n{'='*60}")
    print(f"  PHASE 3: Form party ({len(bots)} bots)")
    print(f"{'='*60}\n")

    leader = bots[0]

    for i in range(1, len(bots)):
        bot = bots[i]

        # IMPORTANT: install hook BEFORE sending invite,
        # otherwise CacheSubscribed arrives before hook and is missed
        print(f"[*] Bot #{i}: installing hook, then inviting...")
        bot["gc"].hook_retrieve_message()
        time.sleep(0.5)

        leader["gc"].invite_to_party(bot["steam_id"])
        print(f"  [+] Invite sent to bot #{i} ({bot['steam_id']})")

        # Now wait for CacheSubscribed and accept
        ok = _wait_and_accept_with_hook(bot["gc"], timeout=15)
        if ok:
            print(f"  [+] Bot #{i} joined party")
        else:
            print(f"  [!] Bot #{i} failed — retry...")
            time.sleep(2)
            bot["gc"].hook_retrieve_message()
            time.sleep(0.5)
            leader["gc"].invite_to_party(bot["steam_id"])
            ok = _wait_and_accept_with_hook(bot["gc"], timeout=15)
            if ok:
                print(f"  [+] Bot #{i} joined party (retry)")
            else:
                print(f"  [!] Bot #{i} FAILED to join party")
                return False

        time.sleep(1)

    print(f"\n[+] Party formed with {len(bots)} bots")
    return True


# ═══════════════════════════════════════════════════════════════
# Phase 4: Queue + Accept match
# ═══════════════════════════════════════════════════════════════

def phase_queue(bots: list[dict]):
    """Start matchmaking, wait for match, all accept."""
    print(f"\n{'='*60}")
    print(f"  PHASE 4: Queue for match")
    print(f"{'='*60}\n")

    leader = bots[0]

    # Install hooks on ALL bots BEFORE queueing
    print("[*] Installing hooks on all bots...")
    for bot in bots:
        bot["gc"].hook_retrieve_message()
        time.sleep(0.3)
    print(f"[+] Hooks installed on {len(bots)} bots")

    # Start finding match — replay captured packet if available, else default
    template_path = os.path.join(os.path.dirname(__file__), "config", "find_match_template.json")
    if os.path.exists(template_path):
        with open(template_path) as f:
            tmpl = json.load(f)
        body = bytes.fromhex(tmpl["body_hex"])
        print(f"[*] Replaying captured FindMatch ({len(body)}B)")
        leader["gc"].send_gc_message(7033, body)
    else:
        print("[*] No find_match_template.json — using default (All Pick, Russia)")
        leader["gc"].start_finding_match()
    print("[+] Queue started")

    # Each bot auto_accept_match in parallel threads
    print("[*] Waiting for match (timeout 20 min)...")
    print("[*] Each bot will auto-accept when ReadyUpStatus arrives")

    accept_results = {}

    def bot_auto_accept(bot):
        try:
            # Wait for ReadyUpStatus, then accept via BOTH Panorama click + GC message
            lobby_id = bot["gc"].wait_for_ready_up(timeout=1200)
            if not lobby_id:
                accept_results[bot["idx"]] = False
                print(f"  [!] Bot #{bot['idx']} auto-accept timeout")
                return

            # 1. Panorama JS click (primary — this is what the UI does)
            try:
                cmd_bot = DotaCommands(DotaMemory(bot["pid"]))
                cmd_bot.init()
                cmd_bot.init_panorama()
                cmd_bot.run_panorama_js(
                    "$.DispatchEvent('DOTAMatchReadyAccept');"
                )
                print(f"  [+] Bot #{bot['idx']} Panorama accept dispatched")
            except Exception as e:
                print(f"  [!] Bot #{bot['idx']} Panorama accept failed: {e}")

            # 2. GC message (backup)
            bot["gc"].accept_match(lobby_id)

            accept_results[bot["idx"]] = True
            print(f"  [+] Bot #{bot['idx']} auto-accepted match (panorama + GC)")
        except Exception as e:
            accept_results[bot["idx"]] = False
            print(f"  [!] Bot #{bot['idx']} auto-accept error: {e}")

    threads = []
    for bot in bots:
        t = threading.Thread(target=bot_auto_accept, args=(bot,), daemon=True)
        threads.append(t)
        t.start()

    # Wait for all to finish (or timeout)
    for t in threads:
        t.join(timeout=1260)

    # Unhook all
    for bot in bots:
        try:
            bot["gc"].unhook_retrieve_message()
        except:
            pass

    accepted = sum(1 for v in accept_results.values() if v)
    print(f"\n[+] Accept results: {accepted}/{len(bots)} bots accepted")

    if accepted == 0:
        print("[!] No bots accepted — match not found or accept failed")
        return False

    print("[+] Match accepted!")
    time.sleep(5)  # Wait for server to load
    return True


# ═══════════════════════════════════════════════════════════════
# Phase 5: Hero selection (ban/pick phase aware)
# ═══════════════════════════════════════════════════════════════

# Ban phase duration per game mode (seconds, includes ~5s safety margin)
BAN_PHASE_DURATION = {
    1: 30,    # All Pick
    22: 30,   # Ranked All Pick
    23: 20,   # Turbo
}
DEFAULT_BAN_DURATION = 35  # safety fallback for unknown modes


def _pick_and_verify(bot: dict, hero_name: str, game: DotaGame, max_retries: int = 5) -> bool:
    """Pick a hero and verify via m_hAssignedHero. Returns True if picked."""
    for attempt in range(max_retries):
        bot["cmd"].execute(f"dota_select_hero {hero_name}")
        time.sleep(2)

        # If game moved past hero selection, pick is done
        try:
            state = game.get_game_state()
            if state >= 3:  # STRATEGY_TIME or later
                bot["hero_name"] = hero_name
                print(f"  [+] Bot #{bot['idx']} → {hero_name} (state={state}, past hero selection)")
                return True
        except:
            pass

        # Check m_hAssignedHero on local controller
        try:
            handle = game.check_hero_assigned()
            if handle > 0:
                bot["hero_name"] = hero_name
                print(f"  [+] Bot #{bot['idx']} → {hero_name} (handle={hex(handle)})")
                return True
            elif handle == 0:
                print(f"  [~] Bot #{bot['idx']} attempt {attempt+1}/{max_retries}: not assigned yet")
        except:
            pass

        time.sleep(1)

    return False


def phase_hero_pick(bots: list[dict]):
    """Pick heroes for all bots with ban-phase detection + verification."""
    print(f"\n{'='*60}")
    print(f"  PHASE 5: Hero selection (ban/pick aware)")
    print(f"{'='*60}\n")

    # 1. Wait for HERO_SELECTION state
    print("[*] Waiting for HERO_SELECTION state...")
    deadline = time.time() + 60
    game = None
    while time.time() < deadline:
        try:
            game = DotaGame(bots[0]["mem"])
            game.init()
            state = game.get_game_state()
            if state >= 2:  # HERO_SELECTION or later
                print(f"[+] Game state: {state} ({game.get_game_state_name()})")
                break
        except:
            pass
        time.sleep(2)
    else:
        print("[!] Timeout waiting for hero selection")
        return False

    # If already past hero selection (state >= 3), skip picking
    if game.get_game_state() >= 3:
        print("[+] Already past hero selection, skipping pick phase")
        for bot in bots:
            bot["hero_name"] = HERO_POOL[bot["idx"] % len(HERO_POOL)]
        return True

    # 2. Wait for pick phase (skip ban phase)
    phase_start = time.time()
    game_mode = game.get_game_mode()
    ban_wait = BAN_PHASE_DURATION.get(game_mode, DEFAULT_BAN_DURATION)

    print(f"[*] Ban phase — waiting up to {ban_wait}s (game_mode={game_mode})...")
    while time.time() - phase_start < ban_wait:
        pick_phase = game.is_pick_phase()
        if pick_phase is True:
            print("[+] Pick phase detected via m_nHeroPickState!")
            break
        # Also check if game moved past hero selection
        try:
            if game.get_game_state() >= 3:
                print("[+] Game moved past hero selection during ban wait")
                for bot in bots:
                    bot["hero_name"] = HERO_POOL[bot["idx"] % len(HERO_POOL)]
                return True
        except:
            pass
        time.sleep(2)
    else:
        print(f"[*] Ban phase timer expired ({ban_wait}s), proceeding to pick")

    # 3. Pick heroes with verification
    for bot in bots:
        hero = HERO_POOL[bot["idx"] % len(HERO_POOL)]
        print(f"[*] Bot #{bot['idx']} picking {hero}...")

        picked = _pick_and_verify(bot, hero, game, max_retries=5)
        if not picked:
            # Fallback: try alternative heroes from pool
            for fb_idx in range(1, 4):
                alt = HERO_POOL[(bot["idx"] + fb_idx * 3) % len(HERO_POOL)]
                print(f"  [!] Fallback -> {alt}")
                if _pick_and_verify(bot, alt, game, max_retries=2):
                    break
            else:
                print(f"  [!] Bot #{bot['idx']} could not pick any hero (will get random)")
                bot["hero_name"] = hero  # best guess

        time.sleep(1)

    # 4. Wait for game to start (strategy time → pre-game → in-game)
    print("\n[*] Waiting for GAME_IN_PROGRESS...")
    deadline = time.time() + 120
    while time.time() < deadline:
        try:
            game._find_gamerules()
            state = game.get_game_state()
            if state >= 5:  # GAME_IN_PROGRESS
                print(f"[+] Game started! State: {game.get_game_state_name()}")
                return True
        except:
            pass
        time.sleep(3)

    # Accept any state >= 3 (strategy/pre-game is also fine to start bots)
    try:
        state = game.get_game_state()
        if state >= 3:
            print(f"[+] Game state: {state} ({game.get_game_state_name()}) — starting bots anyway")
            return True
    except:
        pass

    print("[!] Timeout waiting for game start")
    return False


# ═══════════════════════════════════════════════════════════════
# Phase 6: Run bot brains (parallel threads)
# ═══════════════════════════════════════════════════════════════

def run_single_bot(bot: dict, tick_rate: float, stop_event: threading.Event):
    """Run a single bot brain in a thread until game ends."""
    idx = bot["idx"]
    pid = bot["pid"]
    hero = bot.get("hero_name", None)

    try:
        mem = DotaMemory(pid)
        game = DotaGame(mem)
        game.init()
        cmd = DotaCommands(mem)
        cmd.init(game)

        brain = BotBrain(
            mem, game, cmd,
            hero_name=hero,
        )

        if not brain.init():
            print(f"  [!] Bot #{idx} brain init failed")
            return

        print(f"  [+] Bot #{idx} brain started: {brain.hero_name} "
              f"({'Rad' if brain._our_team == 2 else 'Dire'})")

        # Run until game ends (POST_GAME) or stop_event is set
        tick = 0
        start = time.time()
        while not stop_event.is_set():
            try:
                brain.think_once()
            except Exception as e:
                brain.log.error("think", str(e))

            tick += 1
            if tick % 100 == 0:
                elapsed = int(time.time() - start)
                try:
                    state = game.get_game_state()
                    if state >= 6:  # POST_GAME or DISCONNECT
                        print(f"  [+] Bot #{idx}: game ended (state={state}), stopping")
                        stop_event.set()  # Signal all bots to stop
                        break
                except:
                    pass
                print(f"  [~] Bot #{idx}: tick {tick}, {elapsed}s elapsed")

            time.sleep(tick_rate)

        elapsed = int(time.time() - start)
        print(f"  [+] Bot #{idx} brain finished after {tick} ticks ({elapsed}s)")

    except Exception as e:
        print(f"  [!] Bot #{idx} brain error: {e}")


def phase_brain(bots: list[dict], tick_rate: float):
    """Launch bot brains for all bots in parallel threads. Runs until game ends."""
    print(f"\n{'='*60}")
    print(f"  PHASE 6: Run {len(bots)} bot brains (until game ends)")
    print(f"{'='*60}\n")

    stop_event = threading.Event()
    threads = []
    for bot in bots:
        t = threading.Thread(
            target=run_single_bot,
            args=(bot, tick_rate, stop_event),
            name=f"bot-{bot['idx']}",
            daemon=True,
        )
        threads.append(t)

    # Start all threads
    for t in threads:
        t.start()
        time.sleep(0.5)  # Stagger starts slightly

    print(f"[+] All {len(threads)} bot threads started, waiting for game to end...")
    print(f"    (Ctrl+C to stop early)\n")

    # Wait for game to end or Ctrl+C
    try:
        while not stop_event.is_set():
            stop_event.wait(timeout=5)
            alive = sum(1 for t in threads if t.is_alive())
            if alive == 0:
                break
    except KeyboardInterrupt:
        print("\n[!] Ctrl+C — stopping all bots...")
        stop_event.set()

    # Give threads a moment to clean up
    for t in threads:
        t.join(timeout=5)

    print(f"\n[+] All bot brains finished")


# ═══════════════════════════════════════════════════════════════
# Main
# ═══════════════════════════════════════════════════════════════

def main():
    args = parse_args()
    count = args.bots
    assert 1 <= count <= 5, "Bot count must be 1-5"

    print(f"╔══════════════════════════════════════════════════╗")
    print(f"║  Dota 2 Full {count}-Bot Flow Test                    ║")
    print(f"╚══════════════════════════════════════════════════╝")

    # Phase 1: Launch
    if args.skip_launch:
        print("\n[*] Skipping launch (--skip-launch)")
        sb = BotSandbox(count=count)
        pids = sb.get_dota_pids()
        if len(pids) < count:
            print(f"[!] Only {len(pids)} Dota instances running, need {count}")
            return
        pids = pids[:count]
        print(f"[+] Using existing PIDs: {pids}")
    else:
        pids = phase_launch(count)
        if not pids:
            return

    # Phase 2: Init
    bots = phase_init(pids)
    if not bots:
        return

    if args.skip_queue:
        # Skip party+queue, assign hero names from pool
        print("\n[*] Skipping party+queue (--skip-queue)")
        for bot in bots:
            bot["hero_name"] = HERO_POOL[bot["idx"] % len(HERO_POOL)]
    else:
        # Phase 3: Party
        if not phase_party(bots):
            return

        # Phase 4: Queue + Accept
        if not phase_queue(bots):
            return

        # Phase 5: Hero pick
        if not phase_hero_pick(bots):
            return

    # Phase 6: Bot brains (runs until game ends)
    phase_brain(bots, tick_rate=args.tick_rate)

    print(f"\n{'='*60}")
    print(f"  DONE — {count}-bot test complete")
    print(f"{'='*60}")


if __name__ == "__main__":
    main()
