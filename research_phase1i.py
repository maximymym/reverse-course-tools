"""
Phase 1i: Analyze AddText callers to find CCommandBuffer pointer.
Read raw bytes around each CALL site, find LEA rcx setup.
"""
import sys, struct
sys.path.insert(0, ".")
from cheat.memory import DotaMemory

def main():
    mem = DotaMemory()
    tier0_base = mem.module_base("tier0.dll")
    engine2_base = mem.module_base("engine2.dll")

    # AddText callers in engine2
    callers = [0x79e31, 0x1c0629, 0x1cdd0f, 0x1cebe3]

    for caller_rva in callers:
        caller_abs = engine2_base + caller_rva
        print(f"\n=== Caller at engine2+{hex(caller_rva)} ===")

        # Read 256 bytes before and 32 after the call
        start = caller_abs - 256
        data = mem.pm.read_bytes(start, 320)

        # Show last 128 bytes before call as hex with annotations
        # Focus on finding: LEA rcx, [rip+X] (48 8D 0D) or MOV rcx, [rip+X] (48 8B 0D)
        for off in range(0, 288):
            abs_off = start + off
            rva = abs_off - engine2_base

            # LEA rcx, [rip+disp32]
            if data[off] == 0x48 and off + 6 < len(data):
                if data[off+1] == 0x8D and data[off+2] == 0x0D:
                    disp = struct.unpack("<i", data[off+3:off+7])[0]
                    target = abs_off + 7 + disp
                    target_rva = target - engine2_base
                    print(f"  +{hex(rva)}: LEA rcx, [{hex(target)}] (engine2+{hex(target_rva)})")

                # LEA rdx, [rip+disp32]
                if data[off+1] == 0x8D and data[off+2] == 0x15:
                    disp = struct.unpack("<i", data[off+3:off+7])[0]
                    target = abs_off + 7 + disp
                    print(f"  +{hex(rva)}: LEA rdx, [{hex(target)}]")

                # MOV rcx, [rip+disp32]
                if data[off+1] == 0x8B and data[off+2] == 0x0D:
                    disp = struct.unpack("<i", data[off+3:off+7])[0]
                    target = abs_off + 7 + disp
                    target_rva = target - engine2_base
                    val = mem.read_ptr(target)
                    print(f"  +{hex(rva)}: MOV rcx, [{hex(target)}] (engine2+{hex(target_rva)}) => {hex(val)}")

                # MOV rax, [rip+disp32]
                if data[off+1] == 0x8B and data[off+2] == 0x05:
                    disp = struct.unpack("<i", data[off+3:off+7])[0]
                    target = abs_off + 7 + disp
                    target_rva = target - engine2_base
                    try:
                        val = mem.read_ptr(target)
                        if val > 0x100000:
                            print(f"  +{hex(rva)}: MOV rax, [{hex(target)}] (engine2+{hex(target_rva)}) => {hex(val)}")
                    except:
                        pass

            # Also look for 4C 8D 0D (LEA r9, [rip+X]) etc
            if data[off] == 0x4C and off + 6 < len(data):
                if data[off+1] == 0x8D and data[off+2] == 0x0D:
                    disp = struct.unpack("<i", data[off+3:off+7])[0]
                    target = abs_off + 7 + disp
                    print(f"  +{hex(rva)}: LEA r9, [{hex(target)}]")

        # Show the call instruction itself
        call_off = 256  # relative offset in our buffer
        print(f"  +{hex(caller_rva)}: CALL [AddText IAT]")

        # Show 32 bytes of hex around the call
        ctx_start = max(0, call_off - 48)
        ctx = data[ctx_start:call_off + 16]
        hex_lines = []
        for i in range(0, len(ctx), 16):
            line_bytes = ' '.join(f'{b:02X}' for b in ctx[i:i+16])
            line_rva = hex(caller_rva - 48 + i + (ctx_start - (call_off - 48)))
            hex_lines.append(f"    {line_rva}: {line_bytes}")
        print("  Raw bytes:")
        for l in hex_lines:
            print(l)

    # Also dump the first caller's function to understand the calling convention
    print("\n\n=== First caller full function analysis ===")
    # engine2+0x79e31 is inside EngineClient vtable[18] (engine2+0x79e80)
    # Wait, 0x79e31 < 0x79e80, so the call is in a DIFFERENT function that's called FROM vtable[18]
    # Let's find the function containing 0x79e31
    fn_addr = engine2_base + 0x79e31
    # Search backward for function prologue (CC boundary)
    pre = mem.pm.read_bytes(fn_addr - 256, 256)
    func_start_rva = 0
    for j in range(255, 0, -1):
        if pre[j] == 0xCC and pre[j-1] == 0xCC:
            func_start_rva = 0x79e31 - (256 - j - 1)
            break

    if func_start_rva:
        print(f"Function starts at engine2+{hex(func_start_rva)}")
        fn_data = mem.pm.read_bytes(engine2_base + func_start_rva, 256)
        # Print as hex dump
        for i in range(0, 256, 16):
            addr = hex(func_start_rva + i)
            hexbytes = ' '.join(f'{b:02X}' for b in fn_data[i:i+16])
            print(f"  {addr}: {hexbytes}")

    # Also try CCommandBuffer constructor analysis
    print("\n=== CCommandBuffer constructor (tier0+0x59b00) ===")
    ctor = mem.pm.read_bytes(tier0_base + 0x59b00, 128)
    for i in range(0, 128, 16):
        addr = hex(0x59b00 + i)
        hexbytes = ' '.join(f'{b:02X}' for b in ctor[i:i+16])
        print(f"  {addr}: {hexbytes}")

    # Look for LEA rax, [rip+X] in constructor (vtable assignment)
    for j in range(len(ctor) - 7):
        if ctor[j] == 0x48 and ctor[j+1] == 0x8D:
            reg = (ctor[j+2] >> 3) & 7
            rm = ctor[j+2] & 7
            if rm == 5:  # [rip+disp32]
                disp = struct.unpack("<i", ctor[j+3:j+7])[0]
                target = tier0_base + 0x59b00 + j + 7 + disp
                regs = ["rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi"]
                print(f"  +{j}: LEA {regs[reg]}, [rip+{hex(disp)}] => {hex(target)} (tier0+{hex(target - tier0_base)})")

    mem.close()
    print("\n[+] Done")

if __name__ == "__main__":
    main()
