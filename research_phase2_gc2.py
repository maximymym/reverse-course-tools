"""
Phase 2 Research B — Get ISteamGameCoordinator* via SteamAPI exports.

Strategy:
  1. Find exports in steam_api64.dll: SteamClient(), SteamAPI_GetHSteamUser(), SteamAPI_GetHSteamPipe()
  2. Find GetISteamGenericInterface vtable offset on ISteamClient
  3. Shellcode: SteamClient() → GetISteamGenericInterface("SteamGameCoordinator001") → store GC*
  4. Read GC* back, then send messages via GC->SendMessage()

Alternative simpler approach: find CGCClientSystem singleton and read GC ptr directly.
"""
import sys, os, struct, ctypes, time
sys.path.insert(0, os.path.dirname(__file__))

from cheat.memory import DotaMemory

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

def write_remote(handle, addr, data):
    buf = ctypes.create_string_buffer(data)
    written = ctypes.c_size_t(0)
    return bool(kernel32.WriteProcessMemory(handle, addr, buf, len(data), ctypes.byref(written)))

def run_shellcode(handle, alloc, sc):
    write_remote(handle, alloc, sc)
    tid = ctypes.c_ulong(0)
    thread = kernel32.CreateRemoteThread(handle, None, 0, alloc, 0, 0, ctypes.byref(tid))
    if not thread:
        return False
    kernel32.WaitForSingleObject(thread, 10000)
    kernel32.CloseHandle(thread)
    return True


mem = DotaMemory()
api_base = mem.module_base("steam_api64.dll")
sc_base = mem.module_base("steamclient64.dll")
client_base = mem.module_base("client.dll")
print(f"steam_api64.dll: {hex(api_base)}")
print(f"steamclient64.dll: {hex(sc_base)}")

# === Step 1: Find SteamClient() export ===
# SteamClient export at steam_api64.dll + 0x2CB10 (from search)
# But that's the STRING location, not the function. Let's find the actual export.
print("\n=== Finding SteamAPI exports ===")

# Use pymem to resolve exports
import pymem.process
import pymem.ressources.structure

# Get PE export directory
def find_export(pm, module_name, export_name):
    """Find export RVA from PE export table."""
    for mod in pm.list_modules():
        if mod.name.lower() == module_name.lower():
            base = mod.lpBaseOfDll
            # Read PE header
            e_lfanew = struct.unpack("<I", pm.read_bytes(base + 0x3C, 4))[0]
            pe_sig = base + e_lfanew
            # Optional header offset
            opt_hdr = pe_sig + 0x18
            # Export directory RVA (at +0x70 in optional header for x64)
            export_rva = struct.unpack("<I", pm.read_bytes(opt_hdr + 0x70, 4))[0]
            export_size = struct.unpack("<I", pm.read_bytes(opt_hdr + 0x74, 4))[0]
            if export_rva == 0:
                return None
            export_dir = base + export_rva
            # Export directory fields
            num_names = struct.unpack("<I", pm.read_bytes(export_dir + 0x18, 4))[0]
            addr_table_rva = struct.unpack("<I", pm.read_bytes(export_dir + 0x1C, 4))[0]
            name_table_rva = struct.unpack("<I", pm.read_bytes(export_dir + 0x20, 4))[0]
            ordinal_table_rva = struct.unpack("<I", pm.read_bytes(export_dir + 0x24, 4))[0]

            for i in range(min(num_names, 5000)):
                name_rva = struct.unpack("<I", pm.read_bytes(base + name_table_rva + i * 4, 4))[0]
                name = mem.read_string(base + name_rva, 128)
                if name == export_name:
                    ordinal = struct.unpack("<H", pm.read_bytes(base + ordinal_table_rva + i * 2, 2))[0]
                    func_rva = struct.unpack("<I", pm.read_bytes(base + addr_table_rva + ordinal * 4, 4))[0]
                    return base + func_rva
            return None

# Find key exports
exports = {}
for name in ["SteamClient", "SteamAPI_GetHSteamUser", "SteamAPI_GetHSteamPipe",
             "SteamInternal_ContextInit", "SteamGameCoordinator"]:
    addr = find_export(mem.pm, "steam_api64.dll", name)
    if addr:
        exports[name] = addr
        print(f"  [+] {name}: {hex(addr)} (RVA +{hex(addr - api_base)})")
    else:
        print(f"  [-] {name}: NOT FOUND")

if "SteamClient" not in exports:
    print("[!] SteamClient export not found, cannot proceed")
    sys.exit(1)

# === Step 2: Shellcode to get ISteamGameCoordinator* ===
# Plan:
#   1. Call SteamClient() → ISteamClient*
#   2. Call SteamAPI_GetHSteamUser() → hUser
#   3. Call SteamAPI_GetHSteamPipe() → hPipe
#   4. Call ISteamClient::GetISteamGenericInterface(hUser, hPipe, "SteamGameCoordinator001")
#      vtable offset: from decompilation it's +0x60 → vtable index 12
#   5. Store result at output_addr for Python to read back
print("\n=== Building GC resolve shellcode ===")

handle = mem.pm.process_handle
alloc = kernel32.VirtualAllocEx(handle, 0, 0x2000, MEM_COMMIT | MEM_RESERVE, PAGE_RWX)
if not alloc:
    print("[!] VirtualAllocEx failed")
    sys.exit(1)

# Layout: [0x000-0x200] = shellcode, [0x200-0x230] = string, [0x230-0x240] = output
gc_str_addr = alloc + 0x200  # "SteamGameCoordinator001\0"
output_addr = alloc + 0x230  # ISteamGameCoordinator* written here

# Write GC string
write_remote(handle, gc_str_addr, b"SteamGameCoordinator001\x00")
# Clear output
write_remote(handle, output_addr, b"\x00" * 8)

sc = bytearray()
sc += b'\x48\x83\xEC\x48'  # sub rsp, 0x48

# 1. Call SteamClient() → rax = ISteamClient*
sc += b'\x48\xB8' + struct.pack('<Q', exports["SteamClient"])
sc += b'\xFF\xD0'           # call rax
sc += b'\x48\x89\xC7'      # mov rdi, rax (save ISteamClient*)

# 2. Call SteamAPI_GetHSteamUser() → eax = hUser
sc += b'\x48\xB8' + struct.pack('<Q', exports["SteamAPI_GetHSteamUser"])
sc += b'\xFF\xD0'           # call rax
sc += b'\x89\xC6'           # mov esi, eax (save hUser)

# 3. Call SteamAPI_GetHSteamPipe() → eax = hPipe
sc += b'\x48\xB8' + struct.pack('<Q', exports["SteamAPI_GetHSteamPipe"])
sc += b'\xFF\xD0'           # call rax
sc += b'\x41\x89\xC0'      # mov r8d, eax (hPipe = 3rd param)

# 4. ISteamClient::GetISteamGenericInterface(this, hUser, hPipe, "SteamGameCoordinator001")
# this=rcx, hUser=edx, hPipe=r8d, pchVersion=r9
sc += b'\x48\x89\xF9'      # mov rcx, rdi (this = ISteamClient*)
sc += b'\x89\xF2'           # mov edx, esi (hUser)
# r8d already set (hPipe)
sc += b'\x49\xB9' + struct.pack('<Q', gc_str_addr)  # mov r9, "SteamGameCoordinator001"
sc += b'\x48\x8B\x07'      # mov rax, [rdi] (vtable)
sc += b'\xFF\x50\x60'      # call [rax+0x60] (vtable[12])

# 5. Store ISteamGameCoordinator* at output_addr
sc += b'\x48\xA3' + struct.pack('<Q', output_addr)  # mov [output_addr], rax — WRONG encoding for x64

# Actually mov [abs64], rax uses REX.W + A3 but only for AL/AX/EAX/RAX with moffs
# Let me use mov [r10], rax instead
sc = bytearray()  # rebuild
sc += b'\x48\x83\xEC\x48'  # sub rsp, 0x48

# 1. SteamClient()
sc += b'\x48\xB8' + struct.pack('<Q', exports["SteamClient"])
sc += b'\xFF\xD0'
sc += b'\x48\x89\xC7'      # mov rdi, rax

# Check null
sc += b'\x48\x85\xFF'      # test rdi, rdi
sc += b'\x74\x50'           # jz skip (jump to end if null) — approximate

# 2. GetHSteamUser()
sc += b'\x48\xB8' + struct.pack('<Q', exports["SteamAPI_GetHSteamUser"])
sc += b'\xFF\xD0'
sc += b'\x89\xC6'           # mov esi, eax

# 3. GetHSteamPipe()
sc += b'\x48\xB8' + struct.pack('<Q', exports["SteamAPI_GetHSteamPipe"])
sc += b'\xFF\xD0'
sc += b'\x41\x89\xC0'       # mov r8d, eax

# 4. GetISteamGenericInterface
sc += b'\x48\x89\xF9'       # mov rcx, rdi
sc += b'\x89\xF2'            # mov edx, esi
sc += b'\x49\xB9' + struct.pack('<Q', gc_str_addr)
sc += b'\x48\x8B\x07'       # mov rax, [rdi] (vtable)
sc += b'\xFF\x50\x60'       # call [rax+0x60]

# 5. Store to output
sc += b'\x49\xBA' + struct.pack('<Q', output_addr)  # mov r10, output_addr
sc += b'\x49\x89\x02'       # mov [r10], rax

# skip label lands here
sc += b'\x48\x83\xC4\x48'   # add rsp, 0x48
sc += b'\x33\xC0'            # xor eax, eax
sc += b'\xC3'                # ret

print(f"  Shellcode size: {len(sc)} bytes")
print(f"  GC string at: {hex(gc_str_addr)}")
print(f"  Output at: {hex(output_addr)}")

# Execute
ok = run_shellcode(handle, alloc, bytes(sc))
print(f"  Shellcode executed: {ok}")

# Read result
time.sleep(0.1)
gc_ptr = mem.read_ptr(output_addr)
print(f"\n[*] ISteamGameCoordinator*: {hex(gc_ptr)}")

if gc_ptr and gc_ptr > 0x10000:
    # Read vtable
    vtable = mem.read_ptr(gc_ptr)
    send_msg = mem.read_ptr(vtable + 0x00)  # vtable[0] = SendMessage
    is_available = mem.read_ptr(vtable + 0x08)  # vtable[1] = IsMessageAvailable
    retrieve = mem.read_ptr(vtable + 0x10)  # vtable[2] = RetrieveMessage

    print(f"  vtable: {hex(vtable)}")
    print(f"  SendMessage: {hex(send_msg)}")
    print(f"  IsMessageAvailable: {hex(is_available)}")
    print(f"  RetrieveMessage: {hex(retrieve)}")

    # Verify functions are in steamclient64.dll
    for name, addr in [("SendMessage", send_msg), ("IsMessageAvailable", is_available), ("RetrieveMessage", retrieve)]:
        if sc_base <= addr < sc_base + mem.module_size("steamclient64.dll"):
            rva = addr - sc_base
            print(f"  [+] {name} → steamclient64.dll+{hex(rva)}")
        else:
            print(f"  [?] {name} → NOT in steamclient64.dll ({hex(addr)})")
else:
    print("[!] GC pointer is null/invalid")

# Cleanup
kernel32.VirtualFreeEx(handle, alloc, 0, MEM_RELEASE)
print("\n[*] Done")
mem.close()
