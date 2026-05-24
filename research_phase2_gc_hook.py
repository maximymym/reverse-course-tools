"""
Phase 2 Research — VMT hook on ISteamGameCoordinator::SendMessage to record GC messages.

Plan:
1. Read original vtable from gc_ptr
2. Allocate new vtable + hook shellcode + log buffer
3. Hook SendMessage: copy msg_type+size+data to log buffer, then call original
4. Replace vtable pointer at [gc_ptr] with new vtable
5. User starts search in UI → hook captures CMsgStartFindingMatch (7033)
6. Python reads log buffer, saves raw bytes
7. Restore original vtable

Log buffer layout (at log_base):
  +0x00: uint32 message_count (incremented by hook)
  +0x04: uint32 last_msg_type
  +0x08: uint32 last_msg_size
  +0x10: byte[4096] last_msg_data (raw copy of pubData)
"""
import sys, os, struct, ctypes, time, json
sys.path.insert(0, os.path.dirname(__file__))

from cheat.memory import DotaMemory
from cheat.gc import DotaGC, _write_remote, _find_export

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


mem = DotaMemory()
gc = DotaGC(mem)
gc.init()

handle = mem.pm.process_handle
gc_ptr = gc.gc_ptr
print(f"[*] ISteamGameCoordinator: {hex(gc_ptr)}")

# Read original vtable
orig_vtable_ptr = mem.read_ptr(gc_ptr)
orig_send_msg = mem.read_ptr(orig_vtable_ptr + 0x00)
orig_is_avail = mem.read_ptr(orig_vtable_ptr + 0x08)
orig_retrieve = mem.read_ptr(orig_vtable_ptr + 0x10)
print(f"[*] Original vtable: {hex(orig_vtable_ptr)}")
print(f"    SendMessage: {hex(orig_send_msg)}")

# Allocate memory for: new vtable (24B) + hook shellcode (256B) + log buffer (8KB)
alloc = kernel32.VirtualAllocEx(handle, 0, 0x4000, MEM_COMMIT | MEM_RESERVE, PAGE_RWX)
if not alloc:
    print("[!] VirtualAllocEx failed")
    sys.exit(1)
print(f"[*] Allocated: {hex(alloc)}")

new_vtable_addr = alloc + 0x0000     # 24 bytes for 3 function pointers
hook_code_addr = alloc + 0x0100      # hook shellcode
log_base = alloc + 0x1000            # log buffer (4KB)

# Clear log buffer
_write_remote(handle, log_base, b'\x00' * 0x1000)

# Build hook shellcode for SendMessage(this, msgType, data, size)
# RCX=this, EDX=msgType, R8=data, R9D=size
# We need to: save params → copy data to log → call original → return
sc = bytearray()

# Save non-volatile registers and params
sc += b'\x48\x89\x5C\x24\x08'    # mov [rsp+8], rbx (save)
sc += b'\x48\x89\x6C\x24\x10'    # mov [rsp+10h], rbp (save)
sc += b'\x48\x89\x74\x24\x18'    # mov [rsp+18h], rsi (save)
sc += b'\x57'                      # push rdi
sc += b'\x48\x83\xEC\x40'        # sub rsp, 0x40

# Save params for later call
sc += b'\x48\x89\xCB'            # mov rbx, rcx (this)
sc += b'\x89\xD5'                 # mov ebp, edx (msgType)
sc += b'\x49\x89\xC6'            # mov r14, r8 (data) — wait, r14 is non-vol
# Let's use stack locals instead
sc += b'\x48\x89\x4C\x24\x20'    # mov [rsp+20h], rcx (this)
# edx already in ebp
sc += b'\x4C\x89\xC6'            # mov rsi, r8 (data)
sc += b'\x41\x89\xCF'            # mov r15d, r9d (size)

# Write msg_type to log_base+4
sc += b'\x48\xB8' + struct.pack('<Q', log_base + 4)
sc += b'\x89\x28'                 # mov [rax], ebp (msgType)

# Write size to log_base+8
sc += b'\x48\xB8' + struct.pack('<Q', log_base + 8)
sc += b'\x44\x89\x38'            # mov [rax], r15d (size)

# Copy data: memcpy(log_base+0x10, data, min(size, 4000))
# Simple byte-by-byte copy would be slow. Use REP MOVSB.
sc += b'\x48\xBF' + struct.pack('<Q', log_base + 0x10)  # mov rdi, dest
sc += b'\x48\x89\xF6'            # mov rsi, rsi (src = data, already in rsi)
# Wait, rsi was set to r8 (data). But we just used rsi for the mov above.
# Let me reorganize...

# Actually, let me rewrite more carefully
sc = bytearray()
sc += b'\x48\x83\xEC\x48'        # sub rsp, 0x48

# Save ALL 4 params on stack for later original call
sc += b'\x48\x89\x4C\x24\x20'    # mov [rsp+20h], rcx  (this)
sc += b'\x89\x54\x24\x28'        # mov [rsp+28h], edx  (msgType)
sc += b'\x4C\x89\x44\x24\x30'    # mov [rsp+30h], r8   (data)
sc += b'\x44\x89\x4C\x24\x38'    # mov [rsp+38h], r9d  (size)

# --- Log: write msgType to log_base+4 ---
sc += b'\x49\xBA' + struct.pack('<Q', log_base + 4)  # mov r10, &log.msgType
sc += b'\x41\x89\x12'            # mov [r10], edx

# --- Log: write size to log_base+8 ---
sc += b'\x49\xBA' + struct.pack('<Q', log_base + 8)  # mov r10, &log.size
sc += b'\x45\x89\x0A'            # mov [r10], r9d

# --- Log: copy data (min(size, 0xF00) bytes) via REP MOVSB ---
sc += b'\x57'                     # push rdi (save)
sc += b'\x56'                     # push rsi (save)
sc += b'\x48\xBF' + struct.pack('<Q', log_base + 0x10)  # mov rdi, dest
sc += b'\x4C\x89\xC6'            # mov rsi, r8 (src = data)
sc += b'\x44\x89\xC9'            # mov ecx, r9d (count = size)
sc += b'\x81\xF9\x00\x0F\x00\x00'  # cmp ecx, 0xF00
sc += b'\x76\x05'                 # jbe skip_clamp
sc += b'\xB9\x00\x0F\x00\x00'    # mov ecx, 0xF00
                                  # skip_clamp:
sc += b'\xF3\xA4'                 # rep movsb
sc += b'\x5E'                     # pop rsi
sc += b'\x5F'                     # pop rdi

# --- Log: increment message_count (lock xadd for safety) ---
sc += b'\x49\xBA' + struct.pack('<Q', log_base)  # mov r10, &log.count
sc += b'\xF0\x41\xFF\x02'        # lock inc dword [r10]

# --- Call original SendMessage(this, msgType, data, size) ---
sc += b'\x48\x8B\x4C\x24\x20'    # mov rcx, [rsp+20h] (this)
sc += b'\x8B\x54\x24\x28'        # mov edx, [rsp+28h] (msgType)
sc += b'\x4C\x8B\x44\x24\x30'    # mov r8, [rsp+30h] (data)
sc += b'\x44\x8B\x4C\x24\x38'    # mov r9d, [rsp+38h] (size)
sc += b'\x48\xB8' + struct.pack('<Q', orig_send_msg)  # mov rax, originalSendMessage
sc += b'\xFF\xD0'                 # call rax

# --- Return ---
sc += b'\x48\x83\xC4\x48'        # add rsp, 0x48
sc += b'\xC3'                     # ret

print(f"[*] Hook shellcode: {len(sc)} bytes")

# Write hook shellcode
_write_remote(handle, hook_code_addr, bytes(sc))

# Build new vtable
new_vtable = struct.pack('<QQQ', hook_code_addr, orig_is_avail, orig_retrieve)
_write_remote(handle, new_vtable_addr, new_vtable)

# === INSTALL HOOK: replace vtable pointer ===
print("[*] Installing VMT hook...")
_write_remote(handle, gc_ptr, struct.pack('<Q', new_vtable_addr))
print("[+] Hook installed! GC::SendMessage is now hooked.")

# === Wait for user to start search ===
print("\n" + "=" * 50)
print("[*] NOW: Start a match search in Dota 2 UI!")
print("[*] (Click Find Match button)")
print("=" * 50)

prev_count = 0
captured = None
timeout = 30
start = time.time()

while time.time() - start < timeout:
    count = struct.unpack('<I', mem.pm.read_bytes(log_base, 4))[0]
    if count > prev_count:
        msg_type = struct.unpack('<I', mem.pm.read_bytes(log_base + 4, 4))[0]
        msg_size = struct.unpack('<I', mem.pm.read_bytes(log_base + 8, 4))[0]
        # Strip protobuf flag
        clean_type = msg_type & 0x7FFFFFFF
        print(f"  [MSG #{count}] type={clean_type} (0x{msg_type:X}), size={msg_size}")

        if clean_type == 7033:  # CMsgStartFindingMatch
            raw_data = mem.pm.read_bytes(log_base + 0x10, min(msg_size, 0xF00))
            captured = {
                "msg_type": msg_type,
                "clean_type": clean_type,
                "size": msg_size,
                "data_hex": raw_data.hex(),
            }
            print(f"  [+] CAPTURED CMsgStartFindingMatch! size={msg_size}")
            print(f"      Raw hex: {raw_data[:64].hex()}")
            break

        prev_count = count
    time.sleep(0.2)

# === RESTORE original vtable ===
print("\n[*] Restoring original vtable...")
_write_remote(handle, gc_ptr, struct.pack('<Q', orig_vtable_ptr))
print("[+] Original vtable restored.")

if captured:
    # Save to file
    save_path = os.path.join(os.path.dirname(__file__), "config", "last_search.json")
    with open(save_path, "w") as f:
        json.dump(captured, f, indent=2)
    print(f"\n[+] Saved to {save_path}")
    print(f"    msg_type: {captured['clean_type']}")
    print(f"    size: {captured['size']} bytes")
    print(f"    data: {captured['data_hex'][:128]}...")
else:
    print("\n[-] No CMsgStartFindingMatch captured (timeout)")

# Cleanup (don't free alloc yet if hook was just restored — wait for any in-flight calls)
time.sleep(0.5)
kernel32.VirtualFreeEx(handle, alloc, 0, MEM_RELEASE)
print("[*] Done")
mem.close()
