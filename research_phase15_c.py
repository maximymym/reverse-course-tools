"""
Phase 1.5 Research C — Test PrepareUnitOrders MOVE_TO_POSITION.

Shellcode: call PrepareUnitOrders(controller, MOVE_TO_POS, 0, &vec3, 0, HERO_ONLY, 0, 0, false)
"""
import sys, os, struct, ctypes, time
sys.path.insert(0, os.path.dirname(__file__))

from cheat.memory import DotaMemory
from cheat.game_state import DotaGame

# ─── Windows API ───
kernel32 = ctypes.windll.kernel32
for fn, argt, rest in [
    ("VirtualAllocEx", [ctypes.c_void_p, ctypes.c_ulonglong, ctypes.c_size_t, ctypes.c_ulong, ctypes.c_ulong], ctypes.c_ulonglong),
    ("VirtualFreeEx", [ctypes.c_void_p, ctypes.c_ulonglong, ctypes.c_size_t, ctypes.c_ulong], ctypes.c_bool),
    ("CreateRemoteThread", [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_ulonglong, ctypes.c_ulonglong, ctypes.c_ulong, ctypes.POINTER(ctypes.c_ulong)], ctypes.c_void_p),
    ("WaitForSingleObject", [ctypes.c_void_p, ctypes.c_ulong], ctypes.c_ulong),
    ("CloseHandle", [ctypes.c_void_p], ctypes.c_bool),
    ("WriteProcessMemory", [ctypes.c_void_p, ctypes.c_ulonglong, ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)], ctypes.c_bool),
]:
    getattr(kernel32, fn).argtypes = argt
    getattr(kernel32, fn).restype = rest

MEM_COMMIT = 0x1000; MEM_RESERVE = 0x2000; MEM_RELEASE = 0x8000; PAGE_RWX = 0x40

PREPARE_UNIT_ORDERS_RVA = 0x1D16120

# Order constants
ORDER_MOVE_TO_POS = 1
ORDER_STOP = 21
ISSUER_HERO_ONLY = 2
QUEUE_DEFAULT = 0


def write_remote(handle, addr, data):
    buf = ctypes.create_string_buffer(data)
    written = ctypes.c_size_t(0)
    return bool(kernel32.WriteProcessMemory(handle, addr, buf, len(data), ctypes.byref(written)))


def build_prepare_orders_shellcode(controller_addr, func_addr, vec3_addr,
                                    order_type=ORDER_MOVE_TO_POS,
                                    target_index=0, ability_index=0,
                                    issuer=ISSUER_HERO_ONLY, unit=0,
                                    queue=QUEUE_DEFAULT, show_effects=0):
    """
    x64 shellcode for PrepareUnitOrders.
    Params: RCX=this, EDX=orderType, R8D=targetIdx, R9=vec3_ptr
    Stack: [+20h]=ability, [+28h]=issuer, [+30h]=unit, [+38h]=queue, [+40h]=showEffects
    """
    sc = bytearray()
    sc += b'\x48\x83\xEC\x58'                                      # sub rsp, 0x58
    sc += b'\x48\xB9' + struct.pack('<Q', controller_addr)          # mov rcx, controller
    sc += b'\xBA' + struct.pack('<I', order_type)                   # mov edx, orderType
    sc += b'\x41\xB8' + struct.pack('<I', target_index)             # mov r8d, targetIndex
    sc += b'\x49\xB9' + struct.pack('<Q', vec3_addr)                # mov r9, vec3_ptr
    sc += b'\xC7\x44\x24\x20' + struct.pack('<i', ability_index)   # mov [rsp+20h], ability
    sc += b'\xC7\x44\x24\x28' + struct.pack('<i', issuer)          # mov [rsp+28h], issuer
    sc += b'\x48\xC7\x44\x24\x30' + struct.pack('<i', unit & 0xFFFFFFFF)  # mov [rsp+30h], unit (lo32)
    sc += b'\xC7\x44\x24\x38' + struct.pack('<i', queue)           # mov [rsp+38h], queue
    sc += b'\xC6\x44\x24\x40' + struct.pack('<B', show_effects)    # mov byte [rsp+40h], showEffects
    sc += b'\x48\xB8' + struct.pack('<Q', func_addr)                # mov rax, PrepareUnitOrders
    sc += b'\xFF\xD0'                                               # call rax
    sc += b'\x48\x83\xC4\x58'                                      # add rsp, 0x58
    sc += b'\x33\xC0'                                               # xor eax, eax
    sc += b'\xC3'                                                   # ret
    return bytes(sc)


def call_prepare_orders(mem, controller_addr, func_addr, x, y, z, order_type=ORDER_MOVE_TO_POS, **kwargs):
    """Allocate memory, write shellcode + vec3, execute, free."""
    handle = mem.pm.process_handle
    alloc = kernel32.VirtualAllocEx(handle, 0, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_RWX)
    if not alloc:
        print("[!] VirtualAllocEx failed")
        return False

    try:
        vec3_addr = alloc + 0x200
        vec3_data = struct.pack('<fff', x, y, z)
        write_remote(handle, vec3_addr, vec3_data)

        sc = build_prepare_orders_shellcode(controller_addr, func_addr, vec3_addr, order_type, **kwargs)
        write_remote(handle, alloc, sc)

        tid = ctypes.c_ulong(0)
        thread = kernel32.CreateRemoteThread(handle, None, 0, alloc, 0, 0, ctypes.byref(tid))
        if not thread:
            print("[!] CreateRemoteThread failed")
            return False

        kernel32.WaitForSingleObject(thread, 5000)
        kernel32.CloseHandle(thread)
        return True
    finally:
        kernel32.VirtualFreeEx(handle, alloc, 0, MEM_RELEASE)


# ─── Main ───
mem = DotaMemory()
game = DotaGame(mem)
game.init()

client_base = mem.module_base("client.dll")
func_addr = client_base + PREPARE_UNIT_ORDERS_RVA
print(f"[*] PrepareUnitOrders: {hex(func_addr)}")

# Find local controller (slot=0)
local_controller = None
for entity, ident, name in game.iter_entities():
    if name == "dota_player_controller":
        slot = mem.read_i32(entity + 0x908)
        if slot == 0:
            local_controller = entity
            break

if not local_controller:
    print("[!] Local controller not found")
    sys.exit(1)
print(f"[*] Local controller (slot=0): {hex(local_controller)}")

# Find local hero
hero = game.find_local_hero()
if not hero:
    print("[!] Hero not found")
    sys.exit(1)
hero_addr, hero_name = hero
print(f"[*] Hero: {hero_name} @ {hex(hero_addr)}")

# Read current position
pos_before = game.read_position(hero_addr)
print(f"[*] Position BEFORE: ({pos_before[0]:.1f}, {pos_before[1]:.1f}, {pos_before[2]:.1f})")

# Target: move 500 units in +X direction
target_x = pos_before[0] + 500
target_y = pos_before[1]
target_z = pos_before[2]
print(f"[*] Moving to: ({target_x:.1f}, {target_y:.1f}, {target_z:.1f})")

# Execute PrepareUnitOrders
ok = call_prepare_orders(mem, local_controller, func_addr, target_x, target_y, target_z)
print(f"[*] PrepareUnitOrders call: {'OK' if ok else 'FAILED'}")

if ok:
    # Sample position over time
    print("[*] Sampling position (5 x 0.5s)...")
    for i in range(5):
        time.sleep(0.5)
        pos = game.read_position(hero_addr)
        dx = pos[0] - pos_before[0]
        dy = pos[1] - pos_before[1]
        dist = (dx*dx + dy*dy) ** 0.5
        print(f"  [{(i+1)*0.5:.1f}s] ({pos[0]:.1f}, {pos[1]:.1f}, {pos[2]:.1f}) | moved {dist:.1f} units")

    pos_after = game.read_position(hero_addr)
    total_dx = pos_after[0] - pos_before[0]
    total_dy = pos_after[1] - pos_before[1]
    total_dist = (total_dx**2 + total_dy**2) ** 0.5
    print(f"\n[*] Total movement: {total_dist:.1f} units")
    if total_dist > 10:
        print("[+] SUCCESS! Hero moved via PrepareUnitOrders!")
    else:
        print("[-] Hero did NOT move. Debugging needed.")

mem.close()
