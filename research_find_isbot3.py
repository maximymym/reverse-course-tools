"""
Find PlayerResource data layout:
1. Search for m_vecPlayerData string in client.dll → find its offset in C_DOTA_PlayerResource
2. Navigate from PR entity to the vector → read per-player data
3. Verify m_bIsBot
"""
import sys, os, struct
sys.path.insert(0, os.path.dirname(__file__))
from cheat.memory import DotaMemory
from cheat.game_state import DotaGame


def find_string_in_module(mem, mod_name, target_str):
    """Find ASCII string in module memory. Returns list of RVAs."""
    base = mem.module_base(mod_name)
    size = mem.module_size(mod_name)
    target = target_str.encode('ascii')
    results = []
    chunk = 4 * 1024 * 1024
    for off in range(0, size, chunk):
        try:
            data = mem.pm.read_bytes(base + off, min(chunk, size - off))
        except:
            continue
        pos = 0
        while True:
            pos = data.find(target, pos)
            if pos == -1:
                break
            results.append(off + pos)
            pos += len(target)
    return results


def find_ptr_in_module(mem, mod_name, target_addr):
    """Find 8-byte pointers to target_addr. Returns list of RVAs."""
    base = mem.module_base(mod_name)
    size = mem.module_size(mod_name)
    target = struct.pack("<Q", target_addr)
    results = []
    chunk = 4 * 1024 * 1024
    for off in range(0, size, chunk):
        try:
            data = mem.pm.read_bytes(base + off, min(chunk, size - off))
        except:
            continue
        pos = 0
        while True:
            pos = data.find(target, pos)
            if pos == -1:
                break
            results.append(off + pos)
            pos += len(target)
    return results


def main():
    mem = DotaMemory()
    print(f"[+] PID {mem.pid}")
    game = DotaGame(mem)
    game.init()

    client_base = mem.module_base("client.dll")

    # Find C_DOTA_PlayerResource
    pr = None
    for entity, ident, name in game.iter_entities():
        if name and "PlayerResource" in name:
            pr = entity
            break
    print(f"[+] PlayerResource: {hex(pr)}")
    print(f"    Relative to client.dll: +0x{pr - client_base:X}")

    # === 1. Find m_vecPlayerData string ===
    print("\n=== Find m_vecPlayerData string in client.dll ===")

    search_strings = [
        "m_vecPlayerData",
        "m_vecPlayerTeamData",
        "m_playerIDToPlayer",
        "m_iPlayerTeams",
    ]

    for ss in search_strings:
        rvas = find_string_in_module(mem, "client.dll", ss)
        for rva in rvas:
            addr = client_base + rva
            s = mem.read_string(addr, 64)
            print(f"  '{ss}' at client.dll+0x{rva:08X}: '{s}'")

            # Find schema field descriptor pointing to this string
            xrefs = find_ptr_in_module(mem, "client.dll", addr)
            for xrva in xrefs:
                xaddr = client_base + xrva
                try:
                    field_off = mem.read_u32(xaddr + 0x10)
                    field_net = mem.read_u32(xaddr + 0x14)
                    print(f"    -> descriptor at +0x{xrva:08X}: field_offset=0x{field_off:04X} (netvar={field_net})")
                except:
                    pass

    # === 2. Manually explore PR entity structure ===
    print("\n=== Explore PlayerResource structure ===")

    # Read first 0x1000 bytes and look for pointers to heap (likely CUtlVector data)
    pr_data = mem.pm.read_bytes(pr, 0x1000)

    print("  Looking for heap pointers (CUtlVector.m_pElements)...")
    for off in range(0, 0x1000, 8):
        ptr = struct.unpack("<Q", pr_data[off:off+8])[0]
        # Heap pointers typically start with 0x0000002x or 0x0000001x
        if 0x100000000 < ptr < 0x800000000000:
            # Check next 4 bytes for array size
            if off + 12 <= len(pr_data):
                size_val = struct.unpack("<I", pr_data[off+8:off+12])[0]
                if 0 < size_val <= 64:
                    # This could be a CUtlVector: {ptr, size, capacity}
                    print(f"  +0x{off:04X}: ptr={hex(ptr)} size={size_val}")

                    # Check if this is m_vecPlayerData by reading m_bIsBot at +0xD4
                    # within each element at various strides
                    for stride in range(0x100, 0x600, 0x10):
                        try:
                            b0 = mem.read_u8(ptr + 0 * stride + 0xD4)
                            b1 = mem.read_u8(ptr + 1 * stride + 0xD4)
                            if b0 == 0 and b1 == 1:
                                # Check more
                                vals = [b0, b1]
                                for s in range(2, min(10, size_val)):
                                    vals.append(mem.read_u8(ptr + s * stride + 0xD4))
                                if all(v in (0, 1) for v in vals):
                                    bots = sum(vals)
                                    if 1 <= bots <= 9:
                                        print(f"    >>> m_bIsBot FOUND! stride=0x{stride:X} vals={vals}")
                                        # Print more details
                                        print(f"        PR+0x{off:04X} -> CUtlVector(ptr={hex(ptr)}, size={size_val})")
                                        print(f"        m_bIsBot: ptr + slot * 0x{stride:X} + 0xD4")
                                        # Also check m_bFakeClient at +0x45
                                        fc = [mem.read_u8(ptr + s * stride + 0x45) for s in range(min(10, size_val))]
                                        print(f"        m_bFakeClient (+0x45): {fc}")
                                        # Read steam IDs to verify slots
                                        for s in range(min(3, size_val)):
                                            for sid_off in [0x00, 0x08, 0x10, 0x18, 0x20, 0x28, 0x30]:
                                                sid = struct.unpack("<Q", mem.pm.read_bytes(ptr + s * stride + sid_off, 8))[0]
                                                if 76561198000000000 < sid < 76561199000000000:
                                                    print(f"        slot {s} SteamID at +0x{sid_off:02X}: {sid}")
                        except:
                            continue

    # === 3. Also look at PR entity's vtable to identify class ===
    print("\n=== PR entity vtable check ===")
    vtable = struct.unpack("<Q", pr_data[0:8])[0]
    print(f"  vtable: {hex(vtable)} (client.dll+0x{vtable - client_base:X})")

    # Check if entity starts at a different base (maybe PR at 0x7ffe88ab60b0 is not the entity itself)
    # Read identity and check
    print(f"\n=== Verify: is 0x{pr:X} really the entity? ===")
    # The identity for this entity should point back to it
    # entity + 0x10 = CEntityIdentity* (EntityInstance.IDENTITY)
    ident_ptr = mem.read_ptr(pr + 0x10)
    if ident_ptr:
        print(f"  entity+0x10 (identity): {hex(ident_ptr)}")
        # Read identity's entity_ptr (should point back to entity)
        back_ptr = mem.read_ptr(ident_ptr + 0x00)
        print(f"  identity+0x00 (entity back-ref): {hex(back_ptr)}")
        if back_ptr == pr:
            print(f"  [OK] Entity verified!")
        else:
            print(f"  [!] Mismatch! back_ptr != pr")
            print(f"      back_ptr = {hex(back_ptr)}, pr = {hex(pr)}")
            # Maybe our PR entity is wrong. Let me check what's at back_ptr
            print(f"  Trying back_ptr as real entity...")
            pr = back_ptr

    print("\nDone!")


if __name__ == "__main__":
    main()
