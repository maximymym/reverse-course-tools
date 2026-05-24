"""
Phase 1c: ConVar entry layout reverse engineering.
ICvar+0x10 -> ConVar array, entry[0].name_ptr at +0x0 = "check_nofilefd"
Figure out entry stride and value offset.
"""
import sys, struct
sys.path.insert(0, ".")
from cheat.memory import DotaMemory
from cheat.offsets import Interfaces

def read_qword(mem, addr):
    try:
        return mem.read_u64(addr)
    except:
        return None

def try_read_string(mem, ptr):
    if not ptr or ptr < 0x10000 or ptr > 0x7FFFFFFFFFFF:
        return None
    try:
        s = mem.read_string(ptr, 64)
        if s and len(s) > 1 and all(c.isprintable() or c == '\0' for c in s[:16]):
            return s
        return None
    except:
        return None

def main():
    mem = DotaMemory()
    icvar = mem.find_interface("tier0.dll", Interfaces.CVAR)
    print(f"ICvar = {hex(icvar)}")

    # Read the three arrays from ICvar
    arr1_ptr = mem.read_ptr(icvar + 0x10)  # First entry: "check_nofilefd"
    arr1_cap = mem.read_u64(icvar + 0x18)  # 0x80
    arr1_cnt = mem.read_u64(icvar + 0x20)  # 0x44

    arr2_ptr = mem.read_ptr(icvar + 0x28)
    arr2_cap = mem.read_u64(icvar + 0x30)  # 0x80
    arr2_cnt = mem.read_u64(icvar + 0x38)  # 0xD000000329

    arr3_ptr = mem.read_ptr(icvar + 0x48)

    print(f"\nArray 1: ptr={hex(arr1_ptr)}, cap={arr1_cap}, count_raw={hex(mem.read_u64(icvar+0x20))}")
    print(f"Array 2: ptr={hex(arr2_ptr)}, cap={arr2_cap}, count_raw={hex(mem.read_u64(icvar+0x38))}")
    print(f"Array 3: ptr={hex(arr3_ptr)}")

    # Dump first entry raw (256 bytes)
    print(f"\n=== Array1 entry[0] raw dump (256 bytes at {hex(arr1_ptr)}) ===")
    entry_data = mem.pm.read_bytes(arr1_ptr, 256)
    for i in range(0, 256, 8):
        val = struct.unpack("<Q", entry_data[i:i+8])[0]
        lo = val & 0xFFFFFFFF
        hi = (val >> 32) & 0xFFFFFFFF
        f32_lo = struct.unpack("<f", struct.pack("<I", lo))[0]
        f32_hi = struct.unpack("<f", struct.pack("<I", hi))[0]
        extra = ""
        s = try_read_string(mem, val)
        if s:
            extra = f'  -> "{s[:40]}"'
        # Also try i32 interpretation
        i32 = struct.unpack("<i", struct.pack("<I", lo))[0]
        print(f"  +{hex(i):>5s}: {hex(val):>20s}  lo={lo:<12d} hi={hi:<12d} f32={f32_lo:>12.4f}{extra}")

    # Now let's figure out the stride by finding the next name string
    print(f"\n=== Finding entry stride ===")
    # Read 4KB and scan for string pointers
    big_data = mem.pm.read_bytes(arr1_ptr, 4096)
    name_offsets = []
    for i in range(0, 4096, 8):
        val = struct.unpack("<Q", big_data[i:i+8])[0]
        s = try_read_string(mem, val)
        if s and '_' in s and len(s) > 3 and all(c.isalnum() or c in '_.-' for c in s):
            name_offsets.append((i, s))
            if len(name_offsets) >= 10:
                break

    print("Name string pointers found:")
    for off, name in name_offsets:
        print(f"  +{hex(off)}: '{name}'")

    if len(name_offsets) >= 2:
        stride = name_offsets[1][0] - name_offsets[0][0]
        print(f"\n  Probable entry stride: {hex(stride)} ({stride} bytes)")

        # Verify with more entries
        expected_strides = [name_offsets[i+1][0] - name_offsets[i][0] for i in range(len(name_offsets)-1)]
        print(f"  All strides: {[hex(s) for s in expected_strides]}")

        if all(s == stride for s in expected_strides):
            print(f"  CONFIRMED stride: {hex(stride)}")

            # Dump first 3 entries with field annotations
            print(f"\n=== Annotated entry dump (stride={hex(stride)}) ===")
            for entry_idx in range(3):
                entry_start = arr1_ptr + entry_idx * stride
                e = mem.pm.read_bytes(entry_start, stride)
                name_ptr = struct.unpack("<Q", e[0:8])[0]
                name_str = try_read_string(mem, name_ptr)
                print(f"\n  Entry[{entry_idx}] @ {hex(entry_start)}: '{name_str}'")
                for field_off in range(0, stride, 8):
                    val = struct.unpack("<Q", e[field_off:field_off+8])[0]
                    lo = val & 0xFFFFFFFF
                    f32 = struct.unpack("<f", struct.pack("<I", lo))[0]
                    i32 = struct.unpack("<i", struct.pack("<I", lo))[0]
                    extra = ""
                    s = try_read_string(mem, val)
                    if s:
                        extra = f'  -> "{s[:40]}"'
                    print(f"    +{hex(field_off):>4s}: {hex(val):>20s}  i32={i32:>12d}  f32={f32:>12.4f}{extra}")

    # Also check array2 (might be ConCommands)
    print(f"\n=== Array2 entry[0] (possible ConCommands) ===")
    entry2_data = mem.pm.read_bytes(arr2_ptr, 256)
    for i in range(0, 128, 8):
        val = struct.unpack("<Q", entry2_data[i:i+8])[0]
        extra = ""
        s = try_read_string(mem, val)
        if s:
            extra = f'  -> "{s[:40]}"'
        print(f"  +{hex(i):>5s}: {hex(val):>20s}{extra}")

    # Check array3
    print(f"\n=== Array3 first entries ===")
    arr3_data = mem.pm.read_bytes(arr3_ptr, 256)
    for i in range(0, 128, 8):
        val = struct.unpack("<Q", arr3_data[i:i+8])[0]
        extra = ""
        s = try_read_string(mem, val)
        if s:
            extra = f'  -> "{s[:40]}"'
        lo = val & 0xFFFFFFFF
        hi = (val >> 32) & 0xFFFFFFFF
        print(f"  +{hex(i):>5s}: {hex(val):>20s}  lo={lo:<12d} hi={hi:<12d}{extra}")

    # Look for the sorted name table we found in client.dll
    # (the alphabetically sorted list of ConVar name pointers at client.dll+0x5b68e00)
    print(f"\n=== Exploring sorted ConVar name table (client.dll+0x5b68e00) ===")
    client_base = mem.module_base("client.dll")
    name_table = client_base + 0x5b68e00
    # This was a sorted array of 8-byte pointers to name strings
    # This might be the ConVar registration callback table in client.dll
    # Read 20 entries before and around the target
    target_idx = None
    for i in range(0, 200):
        ptr = mem.read_ptr(name_table + i * 8)
        s = try_read_string(mem, ptr)
        if s and "auto_attack_mode" in s:
            target_idx = i
            print(f"  [{i}] '{s}' <-- TARGET")
        elif s and i < 10:
            print(f"  [{i}] '{s}'")
        elif s and target_idx is not None and i <= target_idx + 3:
            print(f"  [{i}] '{s}'")

    mem.close()
    print("\n[+] Done")

if __name__ == "__main__":
    main()
