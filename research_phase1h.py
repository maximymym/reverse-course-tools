"""
Phase 1h: Find global CCommandBuffer instance by tracing engine2 code.
Also: test command execution via shellcode + CreateRemoteThread.
"""
import sys, struct, ctypes
sys.path.insert(0, ".")
from cheat.memory import DotaMemory
from cheat.offsets import Interfaces

ADDTEXT_RVA = 0x5a4d0  # CCommandBuffer::AddText in tier0.dll

def safe_str(mem, ptr, maxlen=64):
    if not ptr or ptr < 0x10000 or ptr > 0x7FFFFFFFFFFF:
        return None
    try:
        s = mem.read_string(ptr, maxlen)
        return ''.join(c if 32 <= ord(c) < 127 else '' for c in s) or None
    except:
        return None

def main():
    mem = DotaMemory()
    tier0_base = mem.module_base("tier0.dll")
    engine2_base = mem.module_base("engine2.dll")
    engine2_size = mem.module_size("engine2.dll")
    addtext_addr = tier0_base + ADDTEXT_RVA

    print(f"[+] CCommandBuffer::AddText: {hex(addtext_addr)}")

    # 1. Find engine2 IAT entry for AddText
    # The IAT contains the actual resolved addresses of imported functions.
    # Search engine2 data sections for the AddText address.
    print("\n=== Finding AddText import in engine2.dll ===")
    target = struct.pack("<Q", addtext_addr)

    # Read engine2 in chunks and search for the AddText pointer
    iat_refs = []
    chunk_size = 0x1000000
    for off in range(0, min(engine2_size, 0x8000000), chunk_size):
        try:
            data = mem.pm.read_bytes(engine2_base + off, min(chunk_size, engine2_size - off))
        except:
            continue
        idx = 0
        while True:
            idx = data.find(target, idx)
            if idx < 0:
                break
            abs_addr = engine2_base + off + idx
            rva = off + idx
            iat_refs.append((rva, abs_addr))
            print(f"  engine2+{hex(rva)} ({hex(abs_addr)}) = AddText ptr")
            idx += 8

    if not iat_refs:
        print("  Not found via direct pointer search")
        # Maybe it's imported via different mechanism (delay load?)
        # Search for AddText string in engine2 imports
        # Or search for CALL [rip+X] where [rip+X] contains AddText addr

    # 2. Find code in engine2 that calls AddText via IAT
    # Pattern: FF 15 XX XX XX XX (CALL [rip+disp]) where target = IAT entry
    if iat_refs:
        iat_rva = iat_refs[0][0]
        iat_abs = iat_refs[0][1]
        print(f"\n=== Finding CALL [IAT] for AddText ===")
        # Read engine2 code sections (roughly first half of module)
        code_size = min(engine2_size // 2, 0x4000000)
        code_data = mem.pm.read_bytes(engine2_base, code_size)

        callers = []
        for off in range(code_size - 6):
            if code_data[off] == 0xFF and code_data[off+1] == 0x15:
                disp = struct.unpack("<i", code_data[off+2:off+6])[0]
                call_target = engine2_base + off + 6 + disp
                if call_target == iat_abs:
                    callers.append(off)
                    print(f"  CALL [AddText IAT] at engine2+{hex(off)} ({hex(engine2_base + off)})")

        # For each caller, analyze the surrounding function to find the CCommandBuffer ptr
        for caller_off in callers[:5]:
            print(f"\n  --- Analyzing caller at engine2+{hex(caller_off)} ---")
            # Read 128 bytes before the call to see function prologue
            start = max(0, caller_off - 128)
            ctx = code_data[start:caller_off + 32]
            # Find function start (look for typical prologue: 48 89 / 40 53 / sub rsp)
            func_start = None
            for j in range(len(ctx) - 1, 0, -1):
                # CC byte before prologue
                if ctx[j] == 0xCC and j + 1 < len(ctx) and ctx[j+1] != 0xCC:
                    func_start = start + j + 1
                    break
            if func_start:
                print(f"  Function likely starts at engine2+{hex(func_start)}")

            # Look for LEA rcx, [rip+X] before the CALL (this = CCommandBuffer ptr)
            # Pattern: 48 8D 0D XX XX XX XX  (LEA rcx, [rip+disp32])
            for j in range(max(0, caller_off - 64), caller_off):
                if code_data[j] == 0x48 and code_data[j+1] == 0x8D and code_data[j+2] == 0x0D:
                    disp = struct.unpack("<i", code_data[j+3:j+7])[0]
                    buffer_addr = engine2_base + j + 7 + disp
                    print(f"  LEA rcx (this), [rip+{hex(disp)}] at engine2+{hex(j)}")
                    print(f"  => CCommandBuffer instance @ {hex(buffer_addr)}")

                    # Verify: read the first qword of the buffer (should be vtable)
                    try:
                        vtable = mem.read_ptr(buffer_addr)
                        print(f"     vtable: {hex(vtable)}")
                        # The vtable should point to tier0.dll
                        if tier0_base <= vtable < tier0_base + mem.module_size("tier0.dll"):
                            print(f"     CONFIRMED: vtable in tier0.dll (tier0+{hex(vtable - tier0_base)})")
                            # Verify by reading vtable[0] = constructor or first virtual method
                            first_vfn = mem.read_ptr(vtable)
                            print(f"     vtable[0]: {hex(first_vfn)} (tier0+{hex(first_vfn - tier0_base)})")
                        else:
                            print(f"     WARNING: vtable NOT in tier0 - might not be CCommandBuffer")
                    except:
                        print(f"     Could not read buffer at {hex(buffer_addr)}")

    # 3. Also search for CCommandBuffer instance by vtable scan
    # The CCommandBuffer constructor (tier0+0x59b00) sets up the vtable.
    # Read the constructor to find what vtable it writes
    print(f"\n=== CCommandBuffer vtable identification ===")
    ctor_addr = tier0_base + 0x59b00
    ctor_code = mem.pm.read_bytes(ctor_addr, 64)
    # Constructor typically: LEA rax, [rip+X] / MOV [rcx], rax
    for j in range(len(ctor_code) - 7):
        if ctor_code[j] == 0x48 and ctor_code[j+1] == 0x8D and ctor_code[j+2] == 0x05:
            disp = struct.unpack("<i", ctor_code[j+3:j+7])[0]
            vt_addr = ctor_addr + j + 7 + disp
            print(f"  Constructor LEA rax, [rip+{hex(disp)}] -> vtable @ {hex(vt_addr)}")
            # Verify
            first_entry = mem.read_ptr(vt_addr)
            print(f"  vtable[0] = {hex(first_entry)} (tier0+{hex(first_entry - tier0_base)})")

    # 4. Search entire engine2 data for CCommandBuffer instances
    # (they have the vtable pointer at +0x0)
    print(f"\n=== Finding CCommandBuffer instances in engine2 data ===")
    # Read the vtable address from constructor output
    # Re-read constructor to get vtable
    for j in range(len(ctor_code) - 7):
        if ctor_code[j] == 0x48 and ctor_code[j+1] == 0x8D and ctor_code[j+2] == 0x05:
            disp = struct.unpack("<i", ctor_code[j+3:j+7])[0]
            ccb_vtable = ctor_addr + j + 7 + disp
            break
    else:
        ccb_vtable = 0

    if ccb_vtable:
        vt_bytes = struct.pack("<Q", ccb_vtable)
        # Search engine2 data sections
        for off in range(0, min(engine2_size, 0x8000000), 0x1000000):
            try:
                data = mem.pm.read_bytes(engine2_base + off, min(0x1000000, engine2_size - off))
            except:
                continue
            idx = 0
            while True:
                idx = data.find(vt_bytes, idx)
                if idx < 0:
                    break
                abs_addr = engine2_base + off + idx
                rva = off + idx
                print(f"  CCommandBuffer @ engine2+{hex(rva)} ({hex(abs_addr)})")
                # Check if this looks like a valid buffer instance
                # Read a few fields
                try:
                    buf_data = mem.pm.read_bytes(abs_addr, 64)
                    for fi in range(0, 64, 8):
                        val = struct.unpack("<Q", buf_data[fi:fi+8])[0]
                        if val > 0:
                            print(f"    +{hex(fi)}: {hex(val)}")
                except:
                    pass
                idx += 8

    mem.close()
    print("\n[+] Done")

if __name__ == "__main__":
    main()
