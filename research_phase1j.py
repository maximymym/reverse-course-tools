"""
Phase 1j: Verify CCommandBuffer address and test command execution via shellcode.

CCommandBuffer[0] = engine2 + 0x8C6138
AddText = tier0 + 0x5A4D0
Signature: bool AddText(const char* cmd, int delay, int unk, bool unk, bool unk, uint64_t flags)
"""
import sys, struct, ctypes, time
sys.path.insert(0, ".")
from cheat.memory import DotaMemory

# Windows API
kernel32 = ctypes.windll.kernel32

MEM_COMMIT = 0x1000
MEM_RESERVE = 0x2000
MEM_RELEASE = 0x8000
PAGE_EXECUTE_READWRITE = 0x40

def virtual_alloc_ex(process_handle, size, prot=PAGE_EXECUTE_READWRITE):
    addr = kernel32.VirtualAllocEx(
        process_handle, 0, size,
        MEM_COMMIT | MEM_RESERVE, prot
    )
    return addr

def virtual_free_ex(process_handle, addr):
    kernel32.VirtualFreeEx(process_handle, addr, 0, MEM_RELEASE)

def create_remote_thread(process_handle, start_addr, param=0):
    thread_handle = kernel32.CreateRemoteThread(
        process_handle, None, 0, start_addr, param, 0, None
    )
    return thread_handle

def wait_for_thread(thread_handle, timeout_ms=5000):
    kernel32.WaitForSingleObject(thread_handle, timeout_ms)
    exit_code = ctypes.c_ulong(0)
    kernel32.GetExitCodeThread(thread_handle, ctypes.byref(exit_code))
    kernel32.CloseHandle(thread_handle)
    return exit_code.value


def build_addtext_shellcode(cmd_buffer_addr, addtext_addr, cmd_string_addr):
    """
    Build x64 shellcode to call CCommandBuffer::AddText(cmd, 0, 0, false, false, 0)

    MSVC x64 calling convention:
    rcx = this (CCommandBuffer*)
    rdx = const char* cmd
    r8d = int delay (0)
    r9d = int unk (0)
    [rsp+20h] = bool unk (false)
    [rsp+28h] = bool unk (false)  <- actually this might be double (0.0)
    [rsp+30h] = uint64 flags (0)
    """
    sc = bytearray()

    # sub rsp, 0x58 (align + shadow space + args)
    sc += b'\x48\x83\xEC\x58'

    # mov rcx, cmd_buffer_addr
    sc += b'\x48\xB9' + struct.pack('<Q', cmd_buffer_addr)

    # mov rdx, cmd_string_addr
    sc += b'\x48\xBA' + struct.pack('<Q', cmd_string_addr)

    # xor r8d, r8d (delay = 0)
    sc += b'\x45\x33\xC0'

    # xor r9d, r9d (unk = 0)
    sc += b'\x45\x33\xC9'

    # mov byte [rsp+20h], 0
    sc += b'\xC6\x44\x24\x20\x00'

    # mov byte [rsp+28h], 0
    sc += b'\xC6\x44\x24\x28\x00'

    # mov qword [rsp+30h], 0
    sc += b'\x48\xC7\x44\x24\x30\x00\x00\x00\x00'

    # mov rax, addtext_addr
    sc += b'\x48\xB8' + struct.pack('<Q', addtext_addr)

    # call rax
    sc += b'\xFF\xD0'

    # add rsp, 0x58
    sc += b'\x48\x83\xC4\x58'

    # ret
    sc += b'\xC3'

    return bytes(sc)


def execute_command(mem, cmd_buffer_addr, addtext_addr, command: str):
    """Execute a console command in Dota 2 via CCommandBuffer::AddText shellcode."""
    # Encode command string
    cmd_bytes = command.encode('ascii') + b'\x00'

    # Build shellcode
    # We'll put the command string right after the shellcode
    # But we need to know the shellcode size first to calculate the string address
    # Solution: allocate a fixed block, shellcode at start, string at +256

    alloc_size = 512
    alloc_addr = virtual_alloc_ex(mem.pm.process_handle, alloc_size)
    if not alloc_addr:
        print(f"[!] VirtualAllocEx failed")
        return False

    cmd_string_addr = alloc_addr + 256

    # Build and write shellcode
    shellcode = build_addtext_shellcode(cmd_buffer_addr, addtext_addr, cmd_string_addr)

    # Write shellcode
    mem.pm.write_bytes(alloc_addr, shellcode, len(shellcode))

    # Write command string
    mem.pm.write_bytes(cmd_string_addr, cmd_bytes, len(cmd_bytes))

    # Execute
    thread = create_remote_thread(mem.pm.process_handle, alloc_addr)
    if not thread:
        print(f"[!] CreateRemoteThread failed: {ctypes.GetLastError()}")
        virtual_free_ex(mem.pm.process_handle, alloc_addr)
        return False

    exit_code = wait_for_thread(thread, 5000)

    # Cleanup
    virtual_free_ex(mem.pm.process_handle, alloc_addr)

    return True


def main():
    mem = DotaMemory()
    engine2_base = mem.module_base("engine2.dll")
    tier0_base = mem.module_base("tier0.dll")

    cmd_buffer_addr = engine2_base + 0x8C6138
    addtext_addr = tier0_base + 0x5A4D0

    print(f"[+] CCommandBuffer[0]: {hex(cmd_buffer_addr)}")
    print(f"[+] AddText: {hex(addtext_addr)}")

    # 1. Verify CCommandBuffer by reading some fields
    print("\n=== Verifying CCommandBuffer ===")
    # From constructor: +0x8028 = 0x400 (max arg buffer size)
    max_arg = mem.read_i32(cmd_buffer_addr + 0x8028)
    print(f"  +0x8028 (max_arg_buf): {hex(max_arg)} (expect 0x400)")

    is_processing = mem.read_u8(cmd_buffer_addr + 0x802C)
    print(f"  +0x802C (is_processing): {is_processing}")

    # Read +0x8000 to +0x8040 for more info
    for off in [0x0, 0x8, 0x10, 0x18, 0x8000, 0x8008, 0x8010, 0x8020, 0x8028, 0x802C, 0x8030]:
        try:
            val = mem.read_u64(cmd_buffer_addr + off)
            print(f"  +{hex(off)}: {hex(val)}")
        except:
            print(f"  +{hex(off)}: unreadable")

    if max_arg != 0x400:
        print("\n[!] CCommandBuffer verification FAILED!")
        print("    max_arg_buf should be 0x400, got", hex(max_arg))
        print("    Trying alternative: maybe the +0x588 offset was for a different caller")

        # Try without the +0x588 offset
        alt_addr = engine2_base + 0x8C5BB0
        alt_max = mem.read_i32(alt_addr + 0x8028)
        print(f"    Alt base ({hex(alt_addr)})+0x8028: {hex(alt_max)}")

        # Try with just the LEA rax value
        for offset in [0, 0x588, 0x8068]:
            test_addr = engine2_base + 0x8C6138 - offset
            try:
                test_val = mem.read_i32(test_addr + 0x8028)
                print(f"    engine2+{hex(0x8C6138 - offset)}+0x8028: {hex(test_val)}")
            except:
                pass
        return

    print("\n[+] CCommandBuffer VERIFIED!")

    # 2. Test with simple command
    print("\n=== Testing command execution ===")
    print("Executing: echo Phase1_test_ok")

    ok = execute_command(mem, cmd_buffer_addr, addtext_addr, "echo Phase1_test_ok")
    if ok:
        print("[+] Command sent! Check Dota 2 console for 'Phase1_test_ok'")
    else:
        print("[!] Command execution failed")
        mem.close()
        return

    time.sleep(0.5)

    # 3. Test with dota-specific command
    print("\nExecuting: dota_player_units_auto_attack_mode 2")
    execute_command(mem, cmd_buffer_addr, addtext_addr, "dota_player_units_auto_attack_mode 2")
    time.sleep(0.5)

    # Verify via ConVar read
    sys.path.insert(0, ".")
    from research_phase1e import ConVarSystem
    from cheat.offsets import Interfaces
    cv = ConVarSystem(mem)
    cv.init()
    val = cv.get_int("dota_player_units_auto_attack_mode")
    print(f"  ConVar read: dota_player_units_auto_attack_mode = {val}")
    if val == 2:
        print("  [OK] Command execution WORKS via console!")
    else:
        print(f"  [?] Value is {val}, expected 2. Command might not have processed yet.")

    # Restore
    execute_command(mem, cmd_buffer_addr, addtext_addr, "dota_player_units_auto_attack_mode 1")

    mem.close()
    print("\n[+] Done")

if __name__ == "__main__":
    main()
