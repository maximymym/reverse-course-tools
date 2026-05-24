"""
Phase 1b: Deep ConVar research.
1. Find ConVar struct by xref to name string
2. Determine ConVar value layout
3. Try direct value write
4. Search for Cbuf_AddText / ClientCmd in ALL modules
"""
import sys, struct, ctypes
sys.path.insert(0, ".")
from cheat.memory import DotaMemory
from cheat.offsets import Interfaces

def search_ptr_in_module(mem, module_name, target_ptr):
    """Search module for 8-byte value matching target_ptr."""
    base = mem.module_base(module_name)
    size = mem.module_size(module_name)
    target_bytes = struct.pack("<Q", target_ptr)
    results = []
    chunk = 0x1000000
    for off in range(0, min(size, 0x8000000), chunk):
        try:
            data = mem.pm.read_bytes(base + off, min(chunk, size - off))
        except:
            continue
        idx = 0
        while True:
            idx = data.find(target_bytes, idx)
            if idx < 0:
                break
            results.append(base + off + idx)
            idx += 8
    return results

def main():
    mem = DotaMemory()
    print(f"[+] Attached PID: {mem.pid}")

    # 1. Find xrefs to "dota_player_units_auto_attack_mode" string
    str_addr = 0x7ffa401032c0
    # Verify string is still there
    s = mem.read_string(str_addr, 64)
    print(f"[+] String at {hex(str_addr)}: '{s}'")
    if "auto_attack" not in s:
        print("[!] String moved! Rescanning...")
        # Rescan in client.dll
        base = mem.module_base("client.dll")
        size = mem.module_size("client.dll")
        target = b"dota_player_units_auto_attack_mode\x00"
        chunk = 0x1000000
        for off in range(0, min(size, 0x8000000), chunk):
            try:
                data = mem.pm.read_bytes(base + off, min(chunk, size - off))
            except:
                continue
            idx = data.find(target)
            if idx >= 0:
                str_addr = base + off + idx
                print(f"[+] Found at {hex(str_addr)}")
                break

    # Search for pointers to this string in client.dll and tier0.dll
    print(f"\n=== Searching for xrefs to {hex(str_addr)} ===")
    for mod in ["client.dll", "tier0.dll", "engine2.dll"]:
        refs = search_ptr_in_module(mem, mod, str_addr)
        for r in refs:
            rva = r - mem.module_base(mod)
            print(f"  {mod}+{hex(rva)} ({hex(r)})")
            # Dump 128 bytes around this xref to understand the struct
            start = r - 64
            try:
                dump = mem.pm.read_bytes(start, 192)
                # Show as qwords with offset
                print(f"  --- Context dump (start={hex(start)}) ---")
                for i in range(0, 192, 8):
                    val = struct.unpack("<Q", dump[i:i+8])[0]
                    marker = " <-- xref" if (start + i) == r else ""
                    # Try to read as string if looks like pointer
                    extra = ""
                    if 0x10000 < val < 0x7FFFFFFFFFFF:
                        try:
                            maybe_str = mem.read_string(val, 32)
                            if maybe_str and all(32 <= ord(c) < 127 for c in maybe_str[:8]) and len(maybe_str) > 2:
                                extra = f'  -> "{maybe_str[:40]}"'
                        except:
                            pass
                    off_label = (start + i) - r
                    print(f"    [{off_label:+4d}] {hex(val):>20s}{marker}{extra}")
                print()
            except Exception as e:
                print(f"  Could not dump context: {e}")

    # 2. Search for Cbuf_AddText / ClientCmd strings in ALL loaded modules
    print("\n=== Searching ALL modules for command strings ===")
    search_strings = [b"Cbuf_AddText", b"Cbuf_Execute", b"ClientCmd",
                      b"ExecuteClientCmd", b"DispatchConCommand",
                      b"Cmd_Dispatch", b"ConCommand", b"cmd_execution",
                      b"Cbuf_Clear", b"InsertCommand"]

    for mname, minfo in sorted(mem.modules.items()):
        base = minfo["base"]
        size = minfo["size"]
        if size > 0x10000000:  # skip huge modules
            continue
        try:
            data = mem.pm.read_bytes(base, size)
        except:
            continue
        for ss in search_strings:
            idx = data.find(ss)
            if idx >= 0:
                # Get surrounding context
                ctx_start = max(0, idx - 8)
                ctx_end = min(len(data), idx + len(ss) + 16)
                ctx = data[ctx_start:ctx_end]
                ascii_ctx = ''.join(chr(b) if 32 <= b < 127 else '.' for b in ctx)
                print(f"  {mname}+{hex(idx)}: '{ss.decode()}'  ctx: {ascii_ctx}")

    # 3. Look at ICvar internal structure more carefully
    print("\n=== ICvar deep analysis ===")
    icvar = mem.find_interface("tier0.dll", Interfaces.CVAR)
    if not icvar:
        print("[!] ICvar not found")
        return
    print(f"ICvar = {hex(icvar)}")

    # Read ICvar as raw data
    icvar_data = mem.pm.read_bytes(icvar, 0x200)
    print("\nICvar raw dump (qwords):")
    for i in range(0, 0x100, 8):
        val = struct.unpack("<Q", icvar_data[i:i+8])[0]
        lo = val & 0xFFFFFFFF
        hi = (val >> 32) & 0xFFFFFFFF
        extra = ""
        if 0x10000 < val < 0x7FFFFFFFFFFF:
            try:
                s = mem.read_string(val, 32)
                if s and len(s) > 2 and all(32 <= ord(c) < 127 for c in s[:8]):
                    extra = f' -> "{s[:30]}"'
            except:
                pass
        print(f"  +{hex(i):>5s}: {hex(val):>20s}  (lo={hex(lo)}, hi={hex(hi)}){extra}")

    # 4. Try to find ConVar flat array in ICvar
    # In Source 2, ICvar typically has:
    #   +0x40: ConVar data array pointer
    #   +0x48: ConVar data array count/capacity
    # Let's try different offsets
    print("\n=== Probing ICvar for ConVar array ===")
    for arr_off in range(0x10, 0x100, 8):
        arr_ptr = struct.unpack("<Q", icvar_data[arr_off:arr_off+8])[0]
        if not (0x10000 < arr_ptr < 0x7FFFFFFFFFFF):
            continue
        # Try to read first entry and look for a string pointer
        try:
            # Try different struct sizes (0x40, 0x48, 0x50, 0x60, 0x70, 0x80)
            entry_data = mem.pm.read_bytes(arr_ptr, 0x80)
            # Check if any qword in first entry looks like a pointer to a ConVar name
            found_name = False
            for entry_off in range(0, 0x80, 8):
                name_candidate = struct.unpack("<Q", entry_data[entry_off:entry_off+8])[0]
                if 0x10000 < name_candidate < 0x7FFFFFFFFFFF:
                    try:
                        maybe_name = mem.read_string(name_candidate, 64)
                        # ConVar names are like "sv_cheats", "developer", "dota_..."
                        if maybe_name and '_' in maybe_name and len(maybe_name) > 3 and all(c.isalnum() or c in '_.' for c in maybe_name):
                            print(f"  ICvar+{hex(arr_off)} -> {hex(arr_ptr)} -> entry_off={hex(entry_off)}: '{maybe_name}'")
                            found_name = True
                            break
                    except:
                        pass
        except:
            pass

    # 5. Alternative: scan tier0.dll for ConVarData array by looking for
    # multiple known ConVar name strings close together
    print("\n=== Scanning tier0.dll .data for ConVar name patterns ===")
    tier0_base = mem.module_base("tier0.dll")
    tier0_size = mem.module_size("tier0.dll")

    # Read .data section (typically starts around 0x80000+ in tier0)
    # Let's scan the whole module for "sv_cheats" pointer value
    sv_cheats_addr = 0x7ffa66d49698  # found earlier in engine2.dll
    # But ConVar names might be in tier0.dll data section too
    # Let's look for the string in tier0
    try:
        tier0_data = mem.pm.read_bytes(tier0_base, min(tier0_size, 0x2000000))
        idx = tier0_data.find(b"sv_cheats\x00")
        if idx >= 0:
            print(f"  'sv_cheats' in tier0.dll at +{hex(idx)} ({hex(tier0_base + idx)})")
    except:
        pass

    mem.close()
    print("\n[+] Done")

if __name__ == "__main__":
    main()
