"""Replica Phase 6 Python purchase: shellcode + CreateRemoteThread.
Если **это** работает — значит in-process call broken context.
Если **не** работает — Python Phase 6 dev_log was fabricated, ищем дальше.
"""
import sys, ctypes, struct, time
from ctypes import wintypes

PID = int(sys.argv[1])
ITEM_DEF_ID = int(sys.argv[2]) if len(sys.argv) > 2 else 44  # default item_tango

PROCESS_ALL_ACCESS = 0x1F0FFF
MEM_COMMIT = 0x1000
MEM_RESERVE = 0x2000
PAGE_EXECUTE_READWRITE = 0x40

k = ctypes.windll.kernel32
ps = ctypes.windll.psapi

def open_proc(pid):
    h = k.OpenProcess(PROCESS_ALL_ACCESS, False, pid)
    if not h: raise RuntimeError(f"OpenProcess failed: {ctypes.GetLastError()}")
    return h

def alloc(h, size):
    a = k.VirtualAllocEx(h, None, size, MEM_COMMIT|MEM_RESERVE, PAGE_EXECUTE_READWRITE)
    if not a: raise RuntimeError(f"VirtualAllocEx failed: {ctypes.GetLastError()}")
    return a

def write(h, addr, data):
    written = ctypes.c_size_t(0)
    ok = k.WriteProcessMemory(h, addr, data, len(data), ctypes.byref(written))
    if not ok: raise RuntimeError(f"WriteProcessMemory failed: {ctypes.GetLastError()}")

def read(h, addr, size):
    buf = (ctypes.c_ubyte * size)()
    n = ctypes.c_size_t(0)
    if not k.ReadProcessMemory(h, addr, buf, size, ctypes.byref(n)): raise RuntimeError("read fail")
    return bytes(buf)

# Get client.dll base in target via EnumProcessModulesEx
def get_module_base(h, name):
    needed = wintypes.DWORD(0)
    modules = (wintypes.HMODULE * 1024)()
    ps.EnumProcessModulesEx(h, modules, ctypes.sizeof(modules), ctypes.byref(needed), 0x03)
    count = needed.value // ctypes.sizeof(wintypes.HMODULE)
    for i in range(count):
        m = modules[i]
        buf = ctypes.create_unicode_buffer(260)
        ps.GetModuleBaseNameW(h, m, buf, 260)
        if buf.value.lower() == name.lower():
            return m
    return None

# Find C_DOTAPlayerController via entity table scan — too complex from outside.
# Простой shortcut: спросить Andromeda через debug_log какой controller, или
# через GetCL_DOTAPlayerController()->GetLocal() exposed Andromeda DLL.
# Для замены — пользуем shortcut: Andromeda's CGameState уже знает controller,
# мы можем прочитать m_LocalPlayer.pController из его global g_GameState.
# Но это сложно — нужно знать offset.

# ALTERNATIVE: использовать Andromeda's RvaCache + AOB scan ourselves.
# Простейший: попросить юзера передать controller_addr через CLI.

if len(sys.argv) < 4:
    print("Usage: python python_phase6_replica.py <pid> <item_def_id> <controller_hex>")
    print("Get controller from Frida: Memory.scanSync for C_DOTAPlayerController vtable")
    print("Or from Andromeda debug_log — find 'lp.pController=0x...' line")
    sys.exit(1)

CONTROLLER = int(sys.argv[3], 16)

h = open_proc(PID)
client_base = get_module_base(h, "client.dll")
print(f"[+] client.dll base = 0x{client_base:X}")

# PrepareUnitOrders RVA verified via Frida earlier = 0x1E0F150
PREPARE_UNIT_ORDERS = client_base + 0x1E0F150
print(f"[+] PrepareUnitOrders = 0x{PREPARE_UNIT_ORDERS:X}")
print(f"[+] controller        = 0x{CONTROLLER:X}")
print(f"[+] item_def_id       = {ITEM_DEF_ID}")

# Allocate RWX buffer
buf = alloc(h, 0x400)
print(f"[+] RWX buffer @ 0x{buf:X}")

# Write Vector3{0,0,0} at buf+0x200
vec3_addr = buf + 0x200
write(h, vec3_addr, struct.pack('<fff', 0.0, 0.0, 0.0))

# Build shellcode — copy from cheat/commands.py:285-311
ORDER_PURCHASE_ITEM = 16
ISSUER = 2  # HERO_ONLY
sc = bytearray()
sc += b'\x48\x83\xEC\x58'                                       # sub rsp, 0x58
sc += b'\x48\xB9' + struct.pack('<Q', CONTROLLER)               # mov rcx, controller
sc += b'\xBA' + struct.pack('<I', ORDER_PURCHASE_ITEM)          # mov edx, orderType
sc += b'\x41\xB8' + struct.pack('<I', 0)                        # mov r8d, targetIndex=0
sc += b'\x49\xB9' + struct.pack('<Q', vec3_addr)                # mov r9, vec3*
sc += b'\xC7\x44\x24\x20' + struct.pack('<i', ITEM_DEF_ID)      # mov [rsp+20h], ability=item_id
sc += b'\xC7\x44\x24\x28' + struct.pack('<i', ISSUER)           # mov [rsp+28h], issuer=2
sc += b'\x48\xC7\x44\x24\x30' + struct.pack('<i', 0)            # mov qword[rsp+30h], unit=0
sc += b'\xC7\x44\x24\x38' + struct.pack('<i', 0)                # mov [rsp+38h], queue=0
sc += b'\xC6\x44\x24\x40' + struct.pack('<B', 0)                # mov byte[rsp+40h], show=0
sc += b'\x48\xB8' + struct.pack('<Q', PREPARE_UNIT_ORDERS)      # mov rax, fn
sc += b'\xFF\xD0'                                                # call rax
sc += b'\x48\x83\xC4\x58'                                        # add rsp, 0x58
sc += b'\x33\xC0'                                                # xor eax, eax
sc += b'\xC3'                                                    # ret

write(h, buf, bytes(sc))
print(f"[+] shellcode written ({len(sc)} bytes)")

# CreateRemoteThread
thread_id = wintypes.DWORD(0)
thread = k.CreateRemoteThread(h, None, 0, buf, 0, 0, ctypes.byref(thread_id))
if not thread: raise RuntimeError(f"CreateRemoteThread failed: {ctypes.GetLastError()}")
print(f"[+] CreateRemoteThread OK tid={thread_id.value}")

# Wait
result = k.WaitForSingleObject(thread, 3000)
print(f"[+] WaitForSingleObject = {result} (0=OK)")
k.CloseHandle(thread)

print()
print("[*] Покупка отправлена. Проверь в Доте: gold/inventory изменились?")
