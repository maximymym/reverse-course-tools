"""
Phase 1g: Find CCommandBuffer::InsertText and global command buffer.
Walk tier0.dll PE exports, then find the instance pointer.
"""
import sys, struct
import ctypes
import ctypes.wintypes
sys.path.insert(0, ".")
from cheat.memory import DotaMemory

def safe_str(mem, ptr, maxlen=256):
    if not ptr or ptr < 0x10000 or ptr > 0x7FFFFFFFFFFF:
        return None
    try:
        s = mem.read_string(ptr, maxlen)
        return ''.join(c if 32 <= ord(c) < 127 else '' for c in s)
    except:
        return None

def walk_pe_exports(mem, module_base, module_size):
    """Walk PE export table and return {name: rva} dict."""
    exports = {}
    try:
        # Read PE header
        pe_data = mem.pm.read_bytes(module_base, 0x1000)
        e_lfanew = struct.unpack("<I", pe_data[0x3C:0x40])[0]
        # PE signature + COFF header (20 bytes) + optional header
        opt_hdr_off = e_lfanew + 4 + 20
        # Export directory RVA is at optional header + 112 (for PE32+)
        magic = struct.unpack("<H", pe_data[opt_hdr_off:opt_hdr_off+2])[0]
        if magic == 0x20B:  # PE32+
            export_dir_rva = struct.unpack("<I", pe_data[opt_hdr_off + 112:opt_hdr_off + 116])[0]
            export_dir_size = struct.unpack("<I", pe_data[opt_hdr_off + 116:opt_hdr_off + 120])[0]
        else:
            return exports

        if export_dir_rva == 0:
            return exports

        # Read export directory
        exp = mem.pm.read_bytes(module_base + export_dir_rva, 40)
        num_functions = struct.unpack("<I", exp[20:24])[0]
        num_names = struct.unpack("<I", exp[24:28])[0]
        addr_table_rva = struct.unpack("<I", exp[28:32])[0]
        name_table_rva = struct.unpack("<I", exp[32:36])[0]
        ordinal_table_rva = struct.unpack("<I", exp[36:40])[0]

        # Read tables
        name_ptrs = mem.pm.read_bytes(module_base + name_table_rva, num_names * 4)
        ordinals = mem.pm.read_bytes(module_base + ordinal_table_rva, num_names * 2)
        addr_table = mem.pm.read_bytes(module_base + addr_table_rva, num_functions * 4)

        for i in range(num_names):
            name_rva = struct.unpack("<I", name_ptrs[i*4:(i+1)*4])[0]
            ordinal = struct.unpack("<H", ordinals[i*2:(i+1)*2])[0]
            fn_rva = struct.unpack("<I", addr_table[ordinal*4:(ordinal+1)*4])[0]

            name = safe_str(mem, module_base + name_rva, 256)
            if name:
                exports[name] = fn_rva

    except Exception as e:
        print(f"[!] PE export walk error: {e}")

    return exports

def main():
    mem = DotaMemory()
    tier0_base = mem.module_base("tier0.dll")
    tier0_size = mem.module_size("tier0.dll")
    engine2_base = mem.module_base("engine2.dll")

    # 1. Walk tier0.dll exports for CCommandBuffer methods
    print("=== tier0.dll exports (CCommandBuffer + Cbuf) ===")
    exports = walk_pe_exports(mem, tier0_base, tier0_size)
    print(f"Total exports: {len(exports)}")

    cbuf_exports = {}
    for name, rva in sorted(exports.items()):
        if "CommandBuffer" in name or "Cbuf" in name or "cbuf" in name or "InsertText" in name:
            abs_addr = tier0_base + rva
            cbuf_exports[name] = abs_addr
            print(f"  {name}")
            print(f"    RVA={hex(rva)}, abs={hex(abs_addr)}")

    # Also look for Cbuf_AddText directly
    for name, rva in sorted(exports.items()):
        if "Cbuf" in name:
            print(f"  {name}: tier0+{hex(rva)}")

    # 2. Look for the key method: InsertText
    insert_text_addr = None
    insert_text_name = None
    for name, addr in cbuf_exports.items():
        if "InsertText" in name:
            insert_text_addr = addr
            insert_text_name = name
            print(f"\n[+] InsertText found: {name} @ {hex(addr)}")

    if not insert_text_addr:
        print("\n[!] InsertText not found in exports!")
        # Try demangled partial match
        for name, rva in exports.items():
            if "nsertT" in name or "ddText" in name or "nsertC" in name:
                print(f"  Candidate: {name} @ tier0+{hex(rva)}")

    # 3. Disassemble InsertText to understand its signature
    if insert_text_addr:
        print(f"\n=== InsertText disassembly (first 128 bytes) ===")
        code = mem.pm.read_bytes(insert_text_addr, 128)
        code_hex = ' '.join(f'{b:02X}' for b in code[:64])
        print(f"  {code_hex}")

    # 4. Look for global CCommandBuffer* or Cbuf instance
    # Search engine2.dll for references to CCommandBuffer methods
    # The global buffer is typically accessed via a static pointer
    print("\n=== Searching for global CCommandBuffer instance ===")

    # In Source 2, there's typically a global function like:
    # Cbuf_AddText(slot, cmd) which accesses a global CCommandBuffer array
    # Let's look for this pattern in engine2.dll

    # First, let's check if engine2 has Cbuf-related exports
    engine2_exports = walk_pe_exports(mem, engine2_base, mem.module_size("engine2.dll"))
    for name, rva in sorted(engine2_exports.items()):
        if "Cbuf" in name or "cbuf" in name or "CommandBuffer" in name or "ClientCmd" in name:
            print(f"  engine2: {name} @ {hex(rva)}")

    # 5. Search for any module with Cbuf exports
    print("\n=== All modules with Cbuf/ClientCmd exports ===")
    for mname, minfo in sorted(mem.modules.items()):
        if minfo["size"] > 0x10000000:
            continue
        try:
            exps = walk_pe_exports(mem, minfo["base"], minfo["size"])
        except:
            continue
        for ename, rva in exps.items():
            if any(kw in ename for kw in ["Cbuf", "cbuf", "ClientCmd", "clientcmd",
                                            "ExecuteClientCmd", "ExecuteCommand"]):
                print(f"  {mname}: {ename} @ {hex(rva)}")

    # 6. Look for Cbuf_AddText or Cbuf_InsertText pattern
    # In tier0, there should be global functions wrapping CCommandBuffer
    for name, rva in sorted(exports.items()):
        name_lower = name.lower()
        if any(kw in name_lower for kw in ["addtext", "inserttext", "executestring",
                                             "executecommand", "insertcommand"]):
            abs_addr = tier0_base + rva
            print(f"\n  Interesting export: {name} @ {hex(abs_addr)}")
            try:
                code = mem.pm.read_bytes(abs_addr, 64)
                print(f"  bytes: {' '.join(f'{b:02X}' for b in code[:32])}")
            except:
                pass

    # 7. Analyze engine2 vtable [0] and [8] more deeply (candidates for ClientCmd)
    print("\n=== Deep analysis of EngineClient vtable[0] and [8] ===")
    from cheat.offsets import Interfaces
    engine_client = mem.find_interface("engine2.dll", Interfaces.ENGINE_TO_CLIENT)
    vtable = mem.read_ptr(engine_client)

    for vi in [0, 1, 3, 8]:
        fn = mem.read_ptr(vtable + vi * 8)
        rva = fn - engine2_base
        print(f"\n  vtable[{vi}] = engine2+{hex(rva)}")
        code = mem.pm.read_bytes(fn, 196)

        # Find all CALL targets
        for j in range(len(code) - 5):
            if code[j] == 0xE8:
                rel = struct.unpack("<i", code[j+1:j+5])[0]
                target = fn + j + 5 + rel
                # Check if it's in tier0 and matches a known export
                if tier0_base <= target < tier0_base + tier0_size:
                    target_rva = target - tier0_base
                    # Find which export
                    for ename, erva in exports.items():
                        if erva == target_rva:
                            print(f"    +{j}: CALL {ename} (tier0+{hex(target_rva)})")
                            break
                    else:
                        print(f"    +{j}: CALL tier0+{hex(target_rva)}")
                elif engine2_base <= target < engine2_base + mem.module_size("engine2.dll"):
                    target_rva = target - engine2_base
                    # Check engine2 exports
                    for ename, erva in engine2_exports.items():
                        if erva == target_rva:
                            print(f"    +{j}: CALL {ename} (engine2+{hex(target_rva)})")
                            break
                    else:
                        print(f"    +{j}: call engine2+{hex(target_rva)}")

    mem.close()
    print("\n[+] Done")

if __name__ == "__main__":
    main()
