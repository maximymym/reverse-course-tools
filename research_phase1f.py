"""
Phase 1f: EngineClient vtable + ConCommand analysis for command execution.
Focus: find ClientCmd/DispatchConCommand/Cbuf_AddText entry point.
"""
import sys, struct
sys.path.insert(0, ".")
from cheat.memory import DotaMemory
from cheat.offsets import Interfaces

def safe_str(mem, ptr, maxlen=64):
    if not ptr or ptr < 0x10000 or ptr > 0x7FFFFFFFFFFF:
        return None
    try:
        s = mem.read_string(ptr, maxlen)
        # Filter to ASCII only to avoid encoding issues
        s = ''.join(c if 32 <= ord(c) < 127 else '?' for c in s)
        if len(s) < 2:
            return None
        return s
    except:
        return None

def analyze_function(mem, fn_addr, modules):
    """Analyze a function: size estimate, calls, string refs."""
    try:
        code = mem.pm.read_bytes(fn_addr, 128)
    except:
        return {}

    result = {
        "calls": [],
        "string_refs": [],
        "size_estimate": 0,
        "first_bytes": ' '.join(f'{b:02X}' for b in code[:20]),
    }

    # Estimate function size (find first CC CC or C3 followed by CC)
    for j in range(3, len(code)):
        if code[j] == 0xC3 and j + 1 < len(code) and code[j+1] == 0xCC:
            result["size_estimate"] = j + 1
            break
        if code[j] == 0xCC and code[j-1] == 0xCC:
            result["size_estimate"] = j - 1
            break

    # Find CALL instructions
    for j in range(len(code) - 5):
        if code[j] == 0xE8:
            rel = struct.unpack("<i", code[j+1:j+5])[0]
            target = fn_addr + j + 5 + rel
            for mname, minfo in modules.items():
                if minfo["base"] <= target < minfo["base"] + minfo["size"]:
                    rva = target - minfo["base"]
                    result["calls"].append((j, target, f"{mname}+{hex(rva)}"))
                    break

    # Find LEA with string refs
    for j in range(len(code) - 7):
        if code[j] == 0x48 and code[j+1] == 0x8D:
            modrm = code[j+2]
            if (modrm & 0xC7) in (0x05, 0x0D, 0x15, 0x1D, 0x25, 0x2D, 0x35, 0x3D):
                disp = struct.unpack("<i", code[j+3:j+7])[0]
                str_addr = fn_addr + j + 7 + disp
                s = safe_str(mem, str_addr, 48)
                if s and len(s) > 3:
                    result["string_refs"].append((j, s))

    return result

def main():
    mem = DotaMemory()
    modules = mem.modules

    # EngineClient vtable
    engine_client = mem.find_interface("engine2.dll", Interfaces.ENGINE_TO_CLIENT)
    vtable = mem.read_ptr(engine_client)
    engine2_base = mem.module_base("engine2.dll")

    print("=== EngineToClient vtable (non-trivial functions) ===")
    for i in range(50):
        fn = mem.read_ptr(vtable + i * 8)
        rva = fn - engine2_base
        info = analyze_function(mem, fn, modules)

        # Skip trivial (tiny) functions
        if info.get("size_estimate", 999) < 10 and not info.get("string_refs"):
            continue

        print(f"\n  [{i:2d}] engine2+{hex(rva)}  (size~{info.get('size_estimate', '?')})")
        print(f"       {info['first_bytes']}")
        for off, s in info.get("string_refs", []):
            print(f"       +{off}: ref \"{s}\"")
        for off, target, label in info.get("calls", []):
            print(f"       +{off}: call {label}")

    # ConCommand analysis (Array2 from ICvar)
    print("\n\n=== ConCommand Objects (ICvar Array2) ===")
    icvar = mem.find_interface("tier0.dll", Interfaces.CVAR)
    arr2_ptr = mem.read_ptr(icvar + 0x28)
    arr2_raw = mem.read_u64(icvar + 0x38)
    arr2_count = arr2_raw & 0xFFFF
    print(f"Array2: ptr={hex(arr2_ptr)}, raw_count={hex(arr2_raw)}, lo16={arr2_count}")

    # Read first ConCommand object
    obj0 = mem.read_ptr(arr2_ptr)
    print(f"\nFirst ConCommand obj: {hex(obj0)}")
    data = mem.pm.read_bytes(obj0, 0x100)
    for i in range(0, 0x100, 8):
        val = struct.unpack("<Q", data[i:i+8])[0]
        lo = val & 0xFFFFFFFF
        s = safe_str(mem, val)
        extra = f'  -> "{s}"' if s else ""
        if val > 0 or extra:
            print(f"  +{hex(i):>4s}: {hex(val):>20s}  lo={lo:<12d}{extra}")

    # Search for known ConCommand names in Array2 objects
    # ConCommands are: dota_purchase_item, dota_select_hero, etc.
    print("\n=== Searching for dota_ commands in ConCommand array ===")
    # Array2 might also be pointer array. Let's read first objects
    # and check what stride/format they have

    # First check if Array2 is also indexed like Array3 (stride 0x10: ptr+idx)
    arr2_data = mem.pm.read_bytes(arr2_ptr, min(arr2_count * 16, 0x100000))
    dota_cmds = []
    for i in range(min(arr2_count, 2000)):
        # Try both stride 0x10 (indexed) and stride 0x08 (pointer array)
        for stride in [0x10, 0x08]:
            try:
                entry_ptr = struct.unpack("<Q", arr2_data[i*stride:i*stride+8])[0]
                if not entry_ptr or entry_ptr < 0x10000:
                    continue
                name_ptr = mem.read_ptr(entry_ptr)
                name = safe_str(mem, name_ptr)
                if name and name.startswith("dota_"):
                    dota_cmds.append((i, entry_ptr, name, stride))
                    if len(dota_cmds) >= 5:
                        break
            except:
                continue
        if len(dota_cmds) >= 5:
            break

    if dota_cmds:
        print(f"  Found {len(dota_cmds)} dota commands (stride={dota_cmds[0][3]}):")
        for idx, ptr, name, stride in dota_cmds[:5]:
            print(f"    [{idx}] {name} @ {hex(ptr)}")
    else:
        # Try reading objects directly at different strides
        print("  Not found via pointer array. Trying inline objects...")
        # The objects at Array2 might have a different layout
        # Let's just dump the first few objects and look for strings
        for off in range(0, 0x400, 8):
            val = struct.unpack("<Q", arr2_data[off:off+8])[0]
            s = safe_str(mem, val)
            if s and ('dota' in s.lower() or 'purchase' in s.lower() or 'select' in s.lower()):
                print(f"    +{hex(off)}: \"{s}\"")

    # Search for "Cbuf" related functions in tier0.dll
    print("\n=== Searching tier0.dll for command buffer functions ===")
    tier0_base = mem.module_base("tier0.dll")
    tier0_size = mem.module_size("tier0.dll")

    # Look for InsertCommandsBeforeCheckpoint (found at tier0+0x327158)
    insert_cmd_str = tier0_base + 0x327158
    s = safe_str(mem, insert_cmd_str, 64)
    print(f"InsertCommands string: \"{s}\" at {hex(insert_cmd_str)}")

    # Find xrefs to this string in tier0
    target_bytes = struct.pack("<Q", insert_cmd_str)
    tier0_data = mem.pm.read_bytes(tier0_base, min(tier0_size, 0x400000))
    idx = 0
    while True:
        idx = tier0_data.find(target_bytes, idx)
        if idx < 0:
            break
        print(f"  xref at tier0+{hex(idx)}")
        idx += 8

    # Also look for RIP-relative references to the string
    # LEA reg, [rip+disp] where disp points to our string
    str_off = 0x327158  # RVA of the string
    for code_off in range(0, min(len(tier0_data), 0x200000)):
        if tier0_data[code_off] == 0x48 and code_off + 7 <= len(tier0_data):
            if tier0_data[code_off + 1] == 0x8D:
                modrm = tier0_data[code_off + 2]
                if (modrm & 0xC7) in (0x05, 0x0D, 0x15):
                    disp = struct.unpack("<i", tier0_data[code_off+3:code_off+7])[0]
                    target_rva = code_off + 7 + disp
                    if target_rva == str_off:
                        print(f"  LEA xref at tier0+{hex(code_off)}")
                        # Show surrounding code
                        start = max(0, code_off - 16)
                        surrounding = ' '.join(f'{b:02X}' for b in tier0_data[start:code_off+20])
                        print(f"    code: {surrounding}")

    # Look for engine2.dll export "ClientCmd" or similar
    print("\n=== engine2.dll exports ===")
    # Just check a few common patterns
    # In Source 2, command execution often goes through:
    # 1. IVEngineClient::ClientCmd (vtable)
    # 2. Cbuf_AddText (internal)
    # 3. ConCommandHandle::Dispatch

    # Let's search for the string "Cbuf" in tier0 .rdata
    for search in [b"Cbuf", b"cbuf", b"CCommandBuffer", b"CmdBuffer"]:
        idx = tier0_data.find(search)
        while idx >= 0 and idx < len(tier0_data):
            # Get context
            start = max(0, idx - 4)
            end = min(len(tier0_data), idx + 32)
            ctx = tier0_data[start:end]
            ascii_ctx = ''.join(chr(b) if 32 <= ord(chr(b)) < 127 else '.' for b in ctx)
            print(f"  tier0+{hex(idx)}: {ascii_ctx}")
            idx = tier0_data.find(search, idx + 1)
            if idx < 0:
                break

    mem.close()
    print("\n[+] Done")

if __name__ == "__main__":
    main()
