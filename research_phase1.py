"""
Phase 1 Research: Find ICvar, ConVar system, command execution function.

Run with Dota 2 open (demo mode or menu).
"""
import sys
import struct
sys.path.insert(0, ".")
from cheat.memory import DotaMemory
from cheat.offsets import Interfaces

def main():
    mem = DotaMemory()
    print(f"[+] Attached to dota2.exe (PID: {mem.pid})")
    print(f"[+] engine2.dll base: {hex(mem.module_base('engine2.dll'))}")
    print(f"[+] tier0.dll base: {hex(mem.module_base('tier0.dll'))}")
    print(f"[+] client.dll base: {hex(mem.module_base('client.dll'))}")

    # 1. Find ICvar interface (VEngineCvar007)
    print("\n=== ICvar (VEngineCvar007) ===")
    icvar = mem.find_interface("engine2.dll", Interfaces.CVAR)
    if not icvar:
        # Try tier0.dll — some Source 2 builds have it there
        icvar = mem.find_interface("tier0.dll", Interfaces.CVAR)
    if not icvar:
        print("[!] ICvar not found!")
        return
    print(f"[+] ICvar: {hex(icvar)}")

    # Read ICvar vtable
    vtable_ptr = mem.read_ptr(icvar)
    print(f"[+] ICvar vtable: {hex(vtable_ptr)}")

    # Dump first 30 vtable entries
    print("\n--- ICvar vtable entries ---")
    for i in range(30):
        fn = mem.read_ptr(vtable_ptr + i * 8)
        # Check which module it belongs to
        mod = "?"
        for mname, minfo in mem.modules.items():
            if minfo["base"] <= fn < minfo["base"] + minfo["size"]:
                mod = mname
                break
        rva = fn - mem.modules.get(mod, {}).get("base", 0) if mod != "?" else 0
        print(f"  [{i:2d}] {hex(fn)}  ({mod}+{hex(rva)})")

    # 2. Find Source2EngineToClient interface
    print("\n=== Source2EngineToClient001 ===")
    engine_client = mem.find_interface("engine2.dll", Interfaces.ENGINE_TO_CLIENT)
    if engine_client:
        print(f"[+] EngineToClient: {hex(engine_client)}")
        vtable2 = mem.read_ptr(engine_client)
        print(f"[+] vtable: {hex(vtable2)}")
        print("\n--- EngineToClient vtable entries ---")
        for i in range(40):
            fn = mem.read_ptr(vtable2 + i * 8)
            mod = "?"
            for mname, minfo in mem.modules.items():
                if minfo["base"] <= fn < minfo["base"] + minfo["size"]:
                    mod = mname
                    break
            rva = fn - mem.modules.get(mod, {}).get("base", 0) if mod != "?" else 0
            print(f"  [{i:2d}] {hex(fn)}  ({mod}+{hex(rva)})")

    # 3. Explore ICvar ConVar storage
    # In Source 2, ICvar typically stores ConVars in a flat array or hash map.
    # Let's read around the ICvar object to find pointers to ConVar storage.
    print("\n=== ICvar object dump (first 0x200 bytes as pointers) ===")
    for off in range(0, 0x200, 8):
        val = mem.read_ptr(icvar + off)
        # Filter: only show valid pointers
        if val > 0x10000 and val < 0x7FFFFFFFFFFF:
            # Check if it looks like a pointer to something interesting
            mod = None
            for mname, minfo in mem.modules.items():
                if minfo["base"] <= val < minfo["base"] + minfo["size"]:
                    mod = mname
                    break
            extra = f" ({mod})" if mod else ""
            print(f"  +{hex(off)}: {hex(val)}{extra}")

    # 4. Search for Cbuf_AddText pattern in tier0.dll and engine2.dll
    # Common Source 2 patterns for Cbuf_AddText:
    # - References string "Cbuf_AddText" or similar
    # - Called with (int slot, const char* text)
    print("\n=== Searching for command execution patterns ===")

    # Pattern: "Cbuf_AddText" string reference
    # Let's search for known strings in engine2.dll and tier0.dll
    for mod_name in ["tier0.dll", "engine2.dll"]:
        base = mem.module_base(mod_name)
        size = mem.module_size(mod_name)
        try:
            data = mem.pm.read_bytes(base, min(size, 0x2000000))
        except:
            print(f"[!] Could not read {mod_name}")
            continue

        for search_str in [b"Cbuf_AddText", b"ClientCmd", b"ExecuteClientCmd",
                           b"Cmd_ExecuteCommand", b"DispatchConCommand",
                           b"Cbuf_Execute", b"cmd_execution_enabled"]:
            idx = data.find(search_str)
            if idx >= 0:
                print(f"  Found '{search_str.decode()}' in {mod_name} at RVA {hex(idx)} (abs {hex(base + idx)})")
                # Read some context around it
                ctx_start = max(0, idx - 32)
                ctx = data[ctx_start:idx + len(search_str) + 32]
                # Show as ASCII with dots for non-printable
                ascii_ctx = ''.join(chr(b) if 32 <= b < 127 else '.' for b in ctx)
                print(f"    Context: ...{ascii_ctx}...")

    # 5. Look for ConVar by searching for known convar names in memory
    # If we can find a ConVar string and trace back to its ConVar struct,
    # we can figure out the layout
    print("\n=== Searching for known ConVar strings ===")
    for mod_name in ["client.dll", "engine2.dll"]:
        base = mem.module_base(mod_name)
        size = mem.module_size(mod_name)
        # Search in smaller chunks to avoid memory issues
        chunk = 0x1000000  # 16MB
        for chunk_off in range(0, min(size, 0x8000000), chunk):
            try:
                data = mem.pm.read_bytes(base + chunk_off, min(chunk, size - chunk_off))
            except:
                continue

            for cv_name in [b"dota_player_units_auto_attack_mode\x00",
                           b"sv_cheats\x00",
                           b"developer\x00"]:
                idx = data.find(cv_name)
                if idx >= 0:
                    abs_addr = base + chunk_off + idx
                    print(f"  Found '{cv_name[:-1].decode()}' in {mod_name} at {hex(abs_addr)}")

    # 6. Try to find ConVar registration pattern
    # In Source 2, ConVars are registered via ConVar::Create or ConVar_Register
    # The ICvar typically has a method at vtable[~17-20] for GetConVar
    # Try calling GetConVar via reading what's at those vtable entries

    print("\n=== Trying ICvar ConVar lookup ===")
    # In Source 2, ICvar has an internal array of ConVarData
    # Let's look at ICvar+0x40 through +0x100 for array-like structures
    for off in [0x40, 0x48, 0x50, 0x58, 0x60, 0x68, 0x70, 0x78, 0x80]:
        ptr = mem.read_ptr(icvar + off)
        if ptr and 0x10000 < ptr < 0x7FFFFFFFFFFF:
            # Try to read a few entries as if it's an array of pointers
            try:
                vals = []
                for j in range(4):
                    v = mem.read_ptr(ptr + j * 8)
                    vals.append(hex(v) if v else "NULL")
                print(f"  ICvar+{hex(off)} → {hex(ptr)} → [{', '.join(vals[:4])}]")
            except:
                print(f"  ICvar+{hex(off)} → {hex(ptr)} (unreadable)")

    mem.close()
    print("\n[+] Done")

if __name__ == "__main__":
    main()
