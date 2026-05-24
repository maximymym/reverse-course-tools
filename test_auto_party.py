"""Auto party: Bot0 invites Bot1, Bot1 auto-accepts."""
import sys, time, struct
sys.path.insert(0, '.')
from cheat.memory import DotaMemory
from cheat.gc import DotaGC, GC_MSG_INVITATION_CREATED, _decode_protobuf

PID0, PID1 = 36700, 35028
BOT1_STEAM_ID = 76561198729640585  # tqbao71896

print("=== Auto Party: Bot0 invites Bot1 ===")
print(f"Bot0 PID={PID0} (zrvqd87257)")
print(f"Bot1 PID={PID1} (tqbao71896)")
print()

# Init
mem0 = DotaMemory(pid=PID0)
mem1 = DotaMemory(pid=PID1)
gc0 = DotaGC(mem0); gc0.init()
gc1 = DotaGC(mem1); gc1.init()

# Install RetrieveMessage hook on Bot1 (to catch InvitationCreated)
gc1.hook_retrieve_message()
print("[Bot1] RetrieveMessage hook ON")
time.sleep(0.5)

# Step 1: Bot0 sends invite
print(f"\n[1] Bot0 inviting Bot1 (steam_id={BOT1_STEAM_ID})...")
ok = gc0.invite_to_party(BOT1_STEAM_ID)
print(f"    invite_to_party() = {ok}")

# Step 2: Wait for InvitationCreated (4502) on Bot1
print("\n[2] Waiting for CMsgInvitationCreated on Bot1...")
fields = gc1.wait_for_message(GC_MSG_INVITATION_CREATED, timeout=15)

if fields is None:
    print("    TIMEOUT — no InvitationCreated received!")
    print("    Checking hook log...")
    log = gc1.read_hook_log()
    print(f"    Hook log: count={log['count']}, last_type={log['last_msg_type']}")
    if log['count'] > 0:
        raw = gc1.read_last_message_raw()
        mt, f = gc1.parse_retrieve_buffer(raw)
        print(f"    Last message: type={mt}, fields={f}")
    gc1.unhook_retrieve_message()
    sys.exit(1)

print(f"    Got it! fields={fields}")
party_id = fields.get(1, 0)
inviter = fields.get(2, 0)
print(f"    party_id={party_id}, inviter_steam_id={inviter}")

if not party_id:
    print("    ERROR: no party_id!")
    gc1.unhook_retrieve_message()
    sys.exit(1)

# Step 3: Bot1 accepts
print(f"\n[3] Bot1 accepting party invite (party_id={party_id})...")
ok2 = gc1.accept_party_invite(party_id)
print(f"    accept_party_invite() = {ok2}")

# Wait a moment and check for any follow-up messages
time.sleep(3)
log = gc1.read_hook_log()
print(f"\n[4] Bot1 hook log after accept: count={log['count']}, last_type={log['last_msg_type']}")

# Cleanup
gc1.unhook_retrieve_message()
print("\n=== Done! Check both Dota windows — should be in party together ===")
