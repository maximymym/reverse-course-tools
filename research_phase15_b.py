"""
Phase 1.5 Research B — Find local player controller + test PrepareUnitOrders shellcode.

PrepareUnitOrders signature (x64 __fastcall):
  void PrepareUnitOrders(
      CDOTAPlayerController* this,  // RCX
      DotaUnitOrder_t orderType,    // EDX
      int targetIndex,              // R8D
      Vector3* position,            // R9
      int abilityIndex,             // [RSP+28h]
      PlayerOrderIssuer_t issuer,   // [RSP+30h]
      CBaseEntity* unit,            // [RSP+38h]
      OrderQueueBehavior_t queue,   // [RSP+40h]
      bool showEffects              // [RSP+48h]
  )

RVA in client.dll: +0x1D16120
"""
import sys, os, struct, ctypes, time
sys.path.insert(0, os.path.dirname(__file__))

from cheat.memory import DotaMemory
from cheat.game_state import DotaGame
from cheat.offsets import EntityIdentity, EntitySystem

# ─── Windows API ───
kernel32 = ctypes.windll.kernel32
kernel32.VirtualAllocEx.argtypes = [
    ctypes.c_void_p, ctypes.c_ulonglong, ctypes.c_size_t,
    ctypes.c_ulong, ctypes.c_ulong,
]
kernel32.VirtualAllocEx.restype = ctypes.c_ulonglong
kernel32.VirtualFreeEx.argtypes = [
    ctypes.c_void_p, ctypes.c_ulonglong, ctypes.c_size_t, ctypes.c_ulong,
]
kernel32.VirtualFreeEx.restype = ctypes.c_bool
kernel32.CreateRemoteThread.argtypes = [
    ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t,
    ctypes.c_ulonglong, ctypes.c_ulonglong,
    ctypes.c_ulong, ctypes.POINTER(ctypes.c_ulong),
]
kernel32.CreateRemoteThread.restype = ctypes.c_void_p
kernel32.WaitForSingleObject.argtypes = [ctypes.c_void_p, ctypes.c_ulong]
kernel32.WaitForSingleObject.restype = ctypes.c_ulong
kernel32.CloseHandle.argtypes = [ctypes.c_void_p]
kernel32.CloseHandle.restype = ctypes.c_bool
kernel32.WriteProcessMemory.argtypes = [
    ctypes.c_void_p, ctypes.c_ulonglong, ctypes.c_void_p,
    ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t),
]
kernel32.WriteProcessMemory.restype = ctypes.c_bool

MEM_COMMIT = 0x1000
MEM_RESERVE = 0x2000
MEM_RELEASE = 0x8000
PAGE_EXECUTE_READWRITE = 0x40

PREPARE_UNIT_ORDERS_RVA = 0x1D16120

# ─── Order Types ───
DOTA_UNIT_ORDER_NONE = 0
DOTA_UNIT_ORDER_MOVE_TO_POSITION = 1
DOTA_UNIT_ORDER_ATTACK_MOVE = 3
DOTA_UNIT_ORDER_STOP = 21

DOTA_ORDER_ISSUER_HERO_ONLY = 2
DOTA_ORDER_QUEUE_DEFAULT = 0

def _write_remote(process_handle, address, data):
    buf = ctypes.create_string_buffer(data)
    written = ctypes.c_size_t(0)
    return bool(kernel32.WriteProcessMemory(
        process_handle, address, buf, len(data), ctypes.byref(written)
    ))


mem = DotaMemory()
game = DotaGame(mem)
game.init()

client_base = mem.module_base("client.dll")
prepare_unit_orders_addr = client_base + PREPARE_UNIT_ORDERS_RVA
print(f"\n[*] PrepareUnitOrders: {hex(prepare_unit_orders_addr)}")

# === 1. Find all dota_player_controller entities ===
print("\n=== Player Controllers ===")
controllers = []
for entity, ident, name in game.iter_entities():
    if name == "dota_player_controller":
        controllers.append((entity, ident, name))

print(f"Found {len(controllers)} controllers")

for entity, ident, name in controllers:
    # Read key fields
    team = mem.read_u8(entity + 0x3F3)  # m_iTeamNum
    slot = mem.read_i32(entity + 0x908)  # player slot (param_1[0x121])

    # m_hPawn — handle to controlled pawn, at offset varies
    # In Source 2, CDOTAPlayerController::m_hPawn is typically at +0x60C or similar
    # Let's try reading around 0x9F0-0xA00 area (from decompilation: param_1+0x9f4)
    handle_9f4 = mem.read_u32(entity + 0x9F4)

    # m_bIsLocalPlayerController — try common offsets
    # In CS2 it was around +0x6E8 area. Let's scan for a bool that's 1 for only one controller
    local_flag_candidates = []
    for off in [0x6E0, 0x6E8, 0x6F0, 0x700, 0x708, 0x710, 0x718, 0x720]:
        val = mem.read_u8(entity + off)
        if val == 1:
            local_flag_candidates.append(hex(off))

    # m_steamID might be at some offset — 64-bit value
    # Try reading entity handle (CEntityIdentity+0x10)
    ent_handle = mem.read_u32(ident + EntityIdentity.HANDLE)
    ent_index = ent_handle & 0x7FFF

    print(f"\n  Controller @ {hex(entity)}")
    print(f"    team={team}, slot={slot}, handle_9f4={hex(handle_9f4)}")
    print(f"    entity_index={ent_index} (handle={hex(ent_handle)})")
    print(f"    local_flag_1_at: {local_flag_candidates}")

# === 2. Try to identify local via m_bIsLocalPlayerController ===
# Let's dump a wider area to find distinguishing bytes
print("\n=== Distinguishing Fields ===")
if len(controllers) >= 2:
    e0 = controllers[0][0]
    e1 = controllers[1][0]
    # Compare bytes at various offsets
    for off in range(0x6D0, 0x730, 8):
        v0 = mem.read_u64(e0 + off)
        v1 = mem.read_u64(e1 + off)
        if v0 != v1:
            print(f"  +{hex(off)}: ctrl0={hex(v0)}, ctrl1={hex(v1)}")

    # Also check 0x3E0-0x400 area
    for off in range(0x3D0, 0x420, 8):
        v0 = mem.read_u64(e0 + off)
        v1 = mem.read_u64(e1 + off)
        if v0 != v1:
            print(f"  +{hex(off)}: ctrl0={hex(v0)}, ctrl1={hex(v1)}")

# === 3. Use schema dump to find m_bIsLocalPlayerController ===
# Our offsets_dump.json should have it
import json
dump_path = os.path.join(os.path.dirname(__file__), "offsets_dump.json")
if os.path.exists(dump_path):
    with open(dump_path, "r") as f:
        dump = json.load(f)

    # Search for IsLocal or local player fields in CBasePlayerController or CDOTAPlayerController
    for cls_name, fields in dump.items():
        if "PlayerController" in cls_name or "BasePlayerController" in cls_name:
            for field in fields:
                fname = field.get("name", "")
                if "local" in fname.lower() or "islocal" in fname.lower():
                    real_off = field["offset"] + 8  # +8 shift
                    print(f"  [SCHEMA] {cls_name}::{fname} = +{hex(field['offset'])} (real +{hex(real_off)})")

    # Also find m_hPawn
    for cls_name, fields in dump.items():
        if cls_name in ["CBasePlayerController", "CDOTAPlayerController", "C_DOTAPlayerController"]:
            print(f"\n  === {cls_name} fields ===")
            for field in fields:
                real_off = field["offset"] + 8
                print(f"    +{hex(real_off)}: {field['name']} (schema +{hex(field['offset'])})")

print("\n[*] Done - inspect output to find local controller")
mem.close()
