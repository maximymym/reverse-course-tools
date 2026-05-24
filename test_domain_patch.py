"""Test domain patch party invite accept.

Bot0 sends party invite to Bot1, Bot1 accepts via accept_party_invite_js()
(which uses the new domain check patch approach).
"""
import sys
import time

sys.path.insert(0, '.')
from cheat.memory import DotaMemory
from cheat.commands import DotaCommands
from cheat.gc import DotaGC, GC_MSG_INVITATION_CREATED

PID0 = 87924   # Bot0 — zrvqd87257
PID1 = 92076   # Bot1 — tqbao71896
BOT1_STEAM_ID = 76561198729640585

print("=== Test: Domain Patch Party Invite Accept ===")
print(f"Bot0 PID={PID0}")
print(f"Bot1 PID={PID1}")
print()

# --- Init Bot0 (inviter) ---
print("[1] Initializing Bot0...")
mem0 = DotaMemory(pid=PID0)
gc0 = DotaGC(mem0)
gc0.init()
print(f"    GC0 ready")

# --- Init Bot1 (accepter) ---
print("[2] Initializing Bot1...")
mem1 = DotaMemory(pid=PID1)
cmd1 = DotaCommands(mem1)
cmd1.init()
cmd1.init_panorama()
gc1 = DotaGC(mem1)
gc1.init()
print(f"    GC1 + Panorama ready")

# --- Hook RetrieveMessage on Bot1 to detect invite ---
gc1.hook_retrieve_message()
print("[3] Bot1 RetrieveMessage hook ON")
time.sleep(0.5)

# --- Bot0 sends invite ---
print(f"\n[4] Bot0 inviting Bot1 (steam_id={BOT1_STEAM_ID})...")
ok = gc0.invite_to_party(BOT1_STEAM_ID)
print(f"    invite_to_party() = {ok}")

if not ok:
    print("[!] Failed to send invite")
    gc1.unhook_retrieve_message()
    sys.exit(1)

# --- Wait for popup to appear on Bot1 ---
print("\n[5] Waiting for InvitationCreated on Bot1 (15s)...")
fields = gc1.wait_for_message(GC_MSG_INVITATION_CREATED, timeout=15)

if fields is None:
    print("    TIMEOUT -- no InvitationCreated received!")
    log = gc1.read_hook_log()
    print(f"    Hook log: count={log['count']}, last_type={log['last_msg_type']}")
    # Don't unhook yet -- we still need it for post-accept check
    print("\n[!] Trying accept_party_invite_js() anyway (popup may appear)...")
else:
    party_id = fields.get(1, 0)
    inviter = fields.get(2, 0)
    print(f"    Got InvitationCreated! party_id={party_id}, inviter={inviter}")

# --- Wait a moment for popup to render ---
print("\n[6] Waiting 3s for popup to render...")
time.sleep(3)

# --- Bot1 accepts via domain patch ---
print("\n[7] Bot1: accept_party_invite_js() [domain patch approach]...")
accept_ok = cmd1.accept_party_invite_js()
print(f"    accept_party_invite_js() = {accept_ok}")

# --- Verify: wait for party state change ---
print("\n[8] Waiting 5s for party confirmation...")
time.sleep(5)

log = gc1.read_hook_log()
print(f"    Bot1 hook log: count={log['count']}, last_type={log['last_msg_type']}")

# --- Cleanup ---
print("\n[9] Cleanup...")
gc1.unhook_retrieve_message()
cmd1.cleanup()
print("    Domain patch restored, hooks removed")

print("\n=== Done! Check Dota windows — should be in party together ===")
print(f"    Domain patched: {cmd1._domain_patched}")
