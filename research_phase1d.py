"""
Phase 1d: Reverse ConVar data object layout.
Array1 = array of ptrs to ConVar data objects.
Array3 = indexed entries (stride 0x68, ptr+idx pairs in stride 0x10).
Explore the actual ConVar data to find name, value, flags.
"""
import sys, struct
sys.path.insert(0, ".")
from cheat.memory import DotaMemory
from cheat.offsets import Interfaces

def try_str(mem, ptr):
    if not ptr or ptr < 0x10000 or ptr > 0x7FFFFFFFFFFF:
        return None
    try:
        s = mem.read_string(ptr, 64)
        if s and len(s) > 1 and all(c.isprintable() for c in s[:8]):
            return s
    except:
        pass
    return None

def dump_object(mem, addr, size, label=""):
    """Dump an object with pointer dereference and string attempts."""
    print(f"\n  === {label} @ {hex(addr)} ({size} bytes) ===")
    data = mem.pm.read_bytes(addr, size)
    for i in range(0, size, 8):
        val = struct.unpack("<Q", data[i:i+8])[0]
        lo = val & 0xFFFFFFFF
        hi = (val >> 32) & 0xFFFFFFFF
        f32 = struct.unpack("<f", struct.pack("<I", lo))[0]
        i32 = struct.unpack("<i", struct.pack("<I", lo))[0]
        extra = ""
        s = try_str(mem, val)
        if s:
            extra = f'  -> "{s[:50]}"'
        elif lo > 0 and lo < 100000:
            extra = f"  (small: {lo})"
        print(f"    +{hex(i):>4s}: {hex(val):>20s}  i32={i32:>12d}  f32={f32:>12.4f}{extra}")

def main():
    mem = DotaMemory()
    icvar = mem.find_interface("tier0.dll", Interfaces.CVAR)

    # Array1: pointer array (ConVar objects)
    arr1_ptr = mem.read_ptr(icvar + 0x10)
    arr1_count = mem.read_u64(icvar + 0x20) & 0xFFFF  # low 16 bits
    print(f"Array1: {arr1_count} ConVar object pointers at {hex(arr1_ptr)}")

    # Read first 3 ConVar objects from Array1
    for idx in range(3):
        obj_ptr = mem.read_ptr(arr1_ptr + idx * 8)
        dump_object(mem, obj_ptr, 0x100, f"ConVar obj [{idx}] (Array1)")

    # Array3: indexed entries (stride 0x10: ptr, idx)
    arr3_ptr = mem.read_ptr(icvar + 0x48)
    total_count = mem.read_u64(icvar + 0x40) & 0xFFFF  # 0x146A
    print(f"\nArray3: ~{total_count} indexed entries at {hex(arr3_ptr)}")

    # The actual data objects are at stride 0x68 from base 0x404b21a0000
    # Read first 3 data objects via Array3
    for idx in range(3):
        entry_ptr = mem.read_ptr(arr3_ptr + idx * 0x10)
        entry_idx = mem.read_u64(arr3_ptr + idx * 0x10 + 8)
        print(f"\n  Array3[{idx}]: ptr={hex(entry_ptr)}, idx={hex(entry_idx)}")
        dump_object(mem, entry_ptr, 0x68, f"Data entry [{idx}]")

    # Now find our target ConVar: "dota_player_units_auto_attack_mode"
    # Strategy: scan Array3 for entries whose data object contains the name string
    print(f"\n=== Searching for 'dota_player_units_auto_attack_mode' in Array3 ===")

    # Since there are ~5226 entries, let's read them in bulk
    # Array3 format: pairs of (ptr, idx) at stride 0x10
    batch = min(total_count, 6000)
    arr3_data = mem.pm.read_bytes(arr3_ptr, batch * 0x10)

    target_name = "dota_player_units_auto_attack_mode"
    found_entries = []

    for i in range(batch):
        entry_ptr = struct.unpack("<Q", arr3_data[i*0x10:i*0x10+8])[0]
        if not entry_ptr or entry_ptr < 0x10000:
            continue
        # Read the data object - check the name field
        # Name could be at various offsets, let's try reading the whole 0x68
        try:
            obj_data = mem.pm.read_bytes(entry_ptr, 0x68)
        except:
            continue
        # Search for a pointer to the name string within this object
        for off in range(0, 0x68, 8):
            ptr = struct.unpack("<Q", obj_data[off:off+8])[0]
            s = try_str(mem, ptr)
            if s and s == target_name:
                print(f"  FOUND at Array3[{i}], entry_ptr={hex(entry_ptr)}, name at +{hex(off)}")
                found_entries.append((i, entry_ptr, off))
                dump_object(mem, entry_ptr, 0x68, f"TARGET ConVar '{target_name}'")
                break

    if not found_entries:
        print("  Not found in Array3! Trying Array1...")
        # Scan Array1 objects
        for i in range(arr1_count):
            obj_ptr = mem.read_ptr(arr1_ptr + i * 8)
            if not obj_ptr or obj_ptr < 0x10000:
                continue
            try:
                obj_data = mem.pm.read_bytes(obj_ptr, 0x200)
            except:
                continue
            for off in range(0, 0x200, 8):
                ptr = struct.unpack("<Q", obj_data[off:off+8])[0]
                s = try_str(mem, ptr)
                if s and s == target_name:
                    print(f"  FOUND in Array1[{i}], obj_ptr={hex(obj_ptr)}, name at +{hex(off)}")
                    dump_object(mem, obj_ptr, 0x200, f"TARGET ConVar '{target_name}'")
                    break

    # Also find "sv_cheats" for comparison (well-known, always = 0)
    print(f"\n=== Searching for 'sv_cheats' in Array3 ===")
    for i in range(batch):
        entry_ptr = struct.unpack("<Q", arr3_data[i*0x10:i*0x10+8])[0]
        if not entry_ptr or entry_ptr < 0x10000:
            continue
        try:
            obj_data = mem.pm.read_bytes(entry_ptr, 0x68)
        except:
            continue
        for off in range(0, 0x68, 8):
            ptr = struct.unpack("<Q", obj_data[off:off+8])[0]
            s = try_str(mem, ptr)
            if s and s == "sv_cheats":
                print(f"  FOUND at Array3[{i}], entry_ptr={hex(entry_ptr)}, name at +{hex(off)}")
                dump_object(mem, entry_ptr, 0x68, "sv_cheats ConVar")
                break
        else:
            continue
        break

    # Also look for "developer" (known to have int value, usually 0)
    print(f"\n=== Searching for 'developer' in Array3 ===")
    for i in range(batch):
        entry_ptr = struct.unpack("<Q", arr3_data[i*0x10:i*0x10+8])[0]
        if not entry_ptr or entry_ptr < 0x10000:
            continue
        try:
            obj_data = mem.pm.read_bytes(entry_ptr, 0x68)
        except:
            continue
        for off in range(0, 0x68, 8):
            ptr = struct.unpack("<Q", obj_data[off:off+8])[0]
            s = try_str(mem, ptr)
            if s and s == "developer":
                print(f"  FOUND at Array3[{i}], entry_ptr={hex(entry_ptr)}, name at +{hex(off)}")
                dump_object(mem, entry_ptr, 0x68, "developer ConVar")
                break
        else:
            continue
        break

    mem.close()
    print("\n[+] Done")

if __name__ == "__main__":
    main()
