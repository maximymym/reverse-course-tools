"""
Phase 1k: Command execution via shellcode — fixed for 64-bit.
Uses pymem's built-in allocate/write/inject.
"""
import sys, struct, ctypes, time
sys.path.insert(0, ".")
from cheat.memory import DotaMemory

kernel32 = ctypes.windll.kernel32

# Fix ctypes for 64-bit
kernel32.VirtualAllocEx.argtypes = [
    ctypes.c_void_p,  # hProcess
    ctypes.c_ulonglong,  # lpAddress
    ctypes.c_size_t,  # dwSize
    ctypes.c_ulong,  # flAllocationType
    ctypes.c_ulong,  # flProtect
]
kernel32.VirtualAllocEx.restype = ctypes.c_ulonglong

kernel32.VirtualFreeEx.argtypes = [
    ctypes.c_void_p, ctypes.c_ulonglong, ctypes.c_size_t, ctypes.c_ulong
]
kernel32.VirtualFreeEx.restype = ctypes.c_bool

kernel32.CreateRemoteThread.argtypes = [
    ctypes.c_void_p,  # hProcess
    ctypes.c_void_p,  # lpThreadAttributes
    ctypes.c_size_t,  # dwStackSize
    ctypes.c_ulonglong,  # lpStartAddress
    ctypes.c_ulonglong,  # lpParameter
    ctypes.c_ulong,  # dwCreationFlags
    ctypes.POINTER(ctypes.c_ulong),  # lpThreadId
]
kernel32.CreateRemoteThread.restype = ctypes.c_void_p

kernel32.WaitForSingleObject.argtypes = [ctypes.c_void_p, ctypes.c_ulong]
kernel32.WaitForSingleObject.restype = ctypes.c_ulong

kernel32.GetExitCodeThread.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_ulong)]
kernel32.GetExitCodeThread.restype = ctypes.c_bool

kernel32.CloseHandle.argtypes = [ctypes.c_void_p]
kernel32.CloseHandle.restype = ctypes.c_bool

# WriteProcessMemory with 64-bit address
kernel32.WriteProcessMemory.argtypes = [
    ctypes.c_void_p,  # hProcess
    ctypes.c_ulonglong,  # lpBaseAddress
    ctypes.c_void_p,  # lpBuffer
    ctypes.c_size_t,  # nSize
    ctypes.POINTER(ctypes.c_size_t),  # lpNumberOfBytesWritten
]
kernel32.WriteProcessMemory.restype = ctypes.c_bool

MEM_COMMIT = 0x1000
MEM_RESERVE = 0x2000
MEM_RELEASE = 0x8000
PAGE_EXECUTE_READWRITE = 0x40


def write_memory(process_handle, address, data):
    """Write bytes to process at 64-bit address."""
    buf = ctypes.create_string_buffer(data)
    written = ctypes.c_size_t(0)
    ok = kernel32.WriteProcessMemory(process_handle, address, buf, len(data), ctypes.byref(written))
    return ok


def build_addtext_shellcode(cmd_buffer_addr, addtext_addr, cmd_string_addr):
    """
    x64 shellcode: CCommandBuffer::AddText(cmd, 0, 0, false, false, 0)

    __thiscall (MSVC x64 = __fastcall):
    rcx = this
    rdx = const char* cmd
    r8d = int delay = 0
    r9d = int unk = 0
    [rsp+20h] = bool (0)
    [rsp+28h] = bool (0)
    [rsp+30h] = uint64 flags (0)
    """
    sc = bytearray()

    # sub rsp, 0x58
    sc += b'\x48\x83\xEC\x58'

    # mov rcx, cmd_buffer_addr (this)
    sc += b'\x48\xB9' + struct.pack('<Q', cmd_buffer_addr)

    # mov rdx, cmd_string_addr
    sc += b'\x48\xBA' + struct.pack('<Q', cmd_string_addr)

    # xor r8d, r8d (delay = 0)
    sc += b'\x45\x33\xC0'

    # xor r9d, r9d
    sc += b'\x45\x33\xC9'

    # mov byte [rsp+20h], 0
    sc += b'\xC6\x44\x24\x20\x00'

    # Use float 0.0 at [rsp+28h] (the signature has _N which might be double)
    # Actually the mangled sig shows _N = bool, _K = uint64
    # Let's just zero everything
    sc += b'\xC6\x44\x24\x28\x00'

    # mov qword [rsp+30h], 0
    sc += b'\x48\xC7\x44\x24\x30\x00\x00\x00\x00'

    # mov rax, addtext_addr
    sc += b'\x48\xB8' + struct.pack('<Q', addtext_addr)

    # call rax
    sc += b'\xFF\xD0'

    # add rsp, 0x58
    sc += b'\x48\x83\xC4\x58'

    # xor eax, eax (return 0)
    sc += b'\x33\xC0'

    # ret
    sc += b'\xC3'

    return bytes(sc)


def execute_command(process_handle, cmd_buffer_addr, addtext_addr, command: str) -> bool:
    """Execute console command via CCommandBuffer::AddText shellcode."""
    cmd_bytes = command.encode('ascii') + b'\x00'

    # Allocate RWX memory
    alloc_size = 0x1000  # 4KB
    alloc_addr = kernel32.VirtualAllocEx(
        process_handle, 0, alloc_size,
        MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE
    )
    if not alloc_addr:
        print(f"[!] VirtualAllocEx failed: {ctypes.GetLastError()}")
        return False

    print(f"  Allocated @ {hex(alloc_addr)}")

    # String at offset 0x200
    cmd_string_addr = alloc_addr + 0x200

    # Build shellcode
    shellcode = build_addtext_shellcode(cmd_buffer_addr, addtext_addr, cmd_string_addr)

    # Write shellcode at alloc_addr
    ok = write_memory(process_handle, alloc_addr, shellcode)
    if not ok:
        print(f"[!] Write shellcode failed: {ctypes.GetLastError()}")
        kernel32.VirtualFreeEx(process_handle, alloc_addr, 0, MEM_RELEASE)
        return False

    # Write command string at alloc_addr + 0x200
    ok = write_memory(process_handle, cmd_string_addr, cmd_bytes)
    if not ok:
        print(f"[!] Write string failed: {ctypes.GetLastError()}")
        kernel32.VirtualFreeEx(process_handle, alloc_addr, 0, MEM_RELEASE)
        return False

    # Execute via CreateRemoteThread
    thread_id = ctypes.c_ulong(0)
    thread = kernel32.CreateRemoteThread(
        process_handle, None, 0,
        alloc_addr, 0, 0,
        ctypes.byref(thread_id)
    )
    if not thread:
        print(f"[!] CreateRemoteThread failed: {ctypes.GetLastError()}")
        kernel32.VirtualFreeEx(process_handle, alloc_addr, 0, MEM_RELEASE)
        return False

    # Wait for completion
    result = kernel32.WaitForSingleObject(thread, 5000)
    if result != 0:
        print(f"[!] Thread wait timeout/error: {result}")

    exit_code = ctypes.c_ulong(0)
    kernel32.GetExitCodeThread(thread, ctypes.byref(exit_code))
    kernel32.CloseHandle(thread)

    # Cleanup
    kernel32.VirtualFreeEx(process_handle, alloc_addr, 0, MEM_RELEASE)

    print(f"  Thread exit code: {exit_code.value}")
    return True


def main():
    mem = DotaMemory()
    engine2_base = mem.module_base("engine2.dll")
    tier0_base = mem.module_base("tier0.dll")

    cmd_buffer_addr = engine2_base + 0x8C6138
    addtext_addr = tier0_base + 0x5A4D0

    print(f"[+] engine2.dll: {hex(engine2_base)}")
    print(f"[+] tier0.dll: {hex(tier0_base)}")
    print(f"[+] CCommandBuffer[0]: {hex(cmd_buffer_addr)}")
    print(f"[+] AddText: {hex(addtext_addr)}")

    # Verify CCommandBuffer
    max_arg = mem.read_i32(cmd_buffer_addr + 0x8028)
    print(f"[+] Verify: +0x8028 = {hex(max_arg)} (expect 0x400)")
    assert max_arg == 0x400, "CCommandBuffer verification failed!"

    # Test 1: echo command
    print("\n=== Test 1: echo command ===")
    ok = execute_command(mem.pm.process_handle, cmd_buffer_addr, addtext_addr,
                        "echo Phase1_command_injection_works")
    if ok:
        print("[+] Sent! Check Dota 2 console (press ` key)")
    time.sleep(0.5)

    # Test 2: set ConVar via command
    print("\n=== Test 2: ConVar via command ===")
    from research_phase1e import ConVarSystem
    from cheat.offsets import Interfaces
    cv = ConVarSystem(mem)
    cv.init()

    old_val = cv.get_int("dota_player_units_auto_attack_mode")
    print(f"  Before: dota_player_units_auto_attack_mode = {old_val}")

    execute_command(mem.pm.process_handle, cmd_buffer_addr, addtext_addr,
                   "dota_player_units_auto_attack_mode 3")
    time.sleep(0.5)

    new_val = cv.get_int("dota_player_units_auto_attack_mode")
    print(f"  After: dota_player_units_auto_attack_mode = {new_val}")

    if new_val == 3:
        print("  [OK] Command execution via shellcode WORKS!")
    else:
        print(f"  [?] Value didn't change (expected 3, got {new_val})")
        print("  The command might need to be processed in the next frame")
        time.sleep(1.0)
        new_val2 = cv.get_int("dota_player_units_auto_attack_mode")
        print(f"  After 1s: {new_val2}")

    # Restore
    execute_command(mem.pm.process_handle, cmd_buffer_addr, addtext_addr,
                   f"dota_player_units_auto_attack_mode {old_val}")

    mem.close()
    print("\n[+] Done")

if __name__ == "__main__":
    main()
