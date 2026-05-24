"""
Find m_bIsBot in C_DOTA_PlayerResource by brute-force scanning.

Known: slot 0 = human (m_bIsBot=0), slot 1 = bot (m_bIsBot=1).
m_bIsBot offset within PlayerResourcePlayerData_t = 0xD4.
Need to find: base offset + stride of the per-player array.
"""
import sys, os, struct
sys.path.insert(0, os.path.dirname(__file__))
from cheat.memory import DotaMemory
from cheat.game_state import DotaGame

def main():
    mem = DotaMemory()
    print(f"[+] PID {mem.pid}")
    game = DotaGame(mem)
    game.init()

    # Find C_DOTA_PlayerResource
    pr = None
    for entity, ident, name in game.iter_entities():
        if name and "PlayerResource" in name:
            pr = entity
            break
    if not pr:
        print("[!] PlayerResource not found")
        return
    print(f"[+] PlayerResource at {hex(pr)}")

    # Read large chunk (256KB should cover the entity)
    scan_size = 256 * 1024
    try:
        data = mem.pm.read_bytes(pr, scan_size)
    except:
        # Try smaller
        scan_size = 64 * 1024
        data = mem.pm.read_bytes(pr, scan_size)
    print(f"[+] Read {scan_size} bytes")

    # === Strategy 1: Find bool pattern [0, 1, ...] at stride 1 ===
    print("\n=== STRATEGY 1: bool array (stride=1) ===")
    for off in range(scan_size - 24):
        b = data[off:off+24]
        # slot 0 = 0, slot 1 = 1, rest can be 0 or 1
        if b[0] == 0 and b[1] == 1:
            if all(v in (0, 1) for v in b[:10]):
                bots = sum(b[i] for i in range(10))
                if 1 <= bots <= 9:
                    vals = [b[i] for i in range(10)]
                    print(f"  +0x{off:05X}: {vals} (bots={bots})")

    # === Strategy 2: Find byte 0 at offset X, byte 1 at offset X+stride ===
    # Try common strides for PlayerResourcePlayerData_t
    print("\n=== STRATEGY 2: structured array (various strides) ===")
    # Source2 PlayerResourcePlayerData_t is typically large
    # Try strides: 0x100, 0x120, 0x150, 0x180, 0x200, 0x250, 0x300, 0x400, etc.
    for stride in [0x80, 0xC0, 0xD8, 0xE0, 0xF0, 0x100, 0x110, 0x120, 0x130,
                   0x140, 0x150, 0x160, 0x170, 0x180, 0x190, 0x1A0, 0x1B0,
                   0x1C0, 0x1D0, 0x1E0, 0x1F0, 0x200, 0x240, 0x280, 0x2C0,
                   0x300, 0x350, 0x400, 0x500, 0x600, 0x800, 0xA00, 0xC00,
                   0x1000, 0x1500, 0x2000]:
        # For each possible base, check if:
        # base + 0*stride + 0xD4 = 0 (slot 0, human)
        # base + 1*stride + 0xD4 = 1 (slot 1, bot)
        for base in range(0, scan_size - stride * 2 - 0xD4):
            off0 = base + 0 * stride + 0xD4
            off1 = base + 1 * stride + 0xD4
            if off1 >= scan_size:
                break
            if data[off0] == 0 and data[off1] == 1:
                # Verify: check slots 2-9 are also 0 or 1
                all_valid = True
                vals = [0, 1]
                for s in range(2, min(10, (scan_size - base - 0xD4) // stride)):
                    v = data[base + s * stride + 0xD4]
                    if v not in (0, 1):
                        all_valid = False
                        break
                    vals.append(v)
                if all_valid and len(vals) >= 3:
                    bots = sum(vals)
                    if 1 <= bots <= 9:
                        print(f"  base=0x{base:05X} stride=0x{stride:04X}: slots={vals} (bots={bots})")

    # === Strategy 3: Read source2sdk known offsets ===
    print("\n=== STRATEGY 3: source2sdk known offsets (with +8 shift) ===")
    # In source2sdk (dota branch Aug 2025):
    # C_DOTA_PlayerResource:
    #   m_vecPlayerTeamData at around 0x5A0
    #   m_vecPlayerData at around 0x600 (CUtlVector<PlayerResourcePlayerData_t>)
    # With +8 shift: +0x608
    #
    # CUtlVector layout: +0x00 = size, +0x08 = ptr to array
    # OR: +0x00 = ptr, +0x08 = count, +0x10 = capacity

    for base_name, base_off in [
        ("src2sdk_PlayerData", 0x600),
        ("src2sdk_PlayerData+8", 0x608),
        ("src2sdk_PlayerTeamData", 0x5A0),
        ("src2sdk_PlayerTeamData+8", 0x5A8),
    ]:
        if base_off + 0x18 > scan_size:
            continue
        # Read CUtlVector header
        ptr = struct.unpack("<Q", data[base_off:base_off+8])[0]
        count = struct.unpack("<I", data[base_off+8:base_off+12])[0]
        cap = struct.unpack("<I", data[base_off+12:base_off+16])[0]
        print(f"  {base_name:30s}: ptr={hex(ptr)} count={count} cap={cap}")

        # Also try as inline array (no CUtlVector, just embedded)
        ptr2 = struct.unpack("<Q", data[base_off+8:base_off+16])[0]
        print(f"  {base_name:30s}+8: {hex(ptr2)} (alt ptr)")

    # === Strategy 4: Search for SteamID pattern ===
    print("\n=== STRATEGY 4: Find player data via SteamID ===")
    # Known SteamIDs:
    # Bot 0 (qt317792): 76561198725850781 = 0x011000012DA1E69D
    # Bot 1 (Dominique/bot): SteamID unknown, but might be 0 or fake
    steam_id_bytes = struct.pack("<Q", 76561198725850781)
    pos = 0
    while True:
        pos = data.find(steam_id_bytes, pos)
        if pos == -1:
            break
        print(f"  SteamID found at PR+0x{pos:05X}")
        # If this is slot 0's SteamID, then m_bIsBot for slot 0 should be at pos + relative_offset
        # where relative_offset = 0xD4 - steamid_field_offset
        # Print surrounding bytes
        ctx = data[pos-16:pos+32]
        hex_str = ' '.join(f'{b:02X}' for b in ctx)
        print(f"    context: {hex_str}")
        pos += 8

    # === Strategy 5: Direct read at known source2sdk offsets ===
    print("\n=== STRATEGY 5: Try reading PlayerData array from source2sdk offsets ===")
    # source2sdk C_DOTA_PlayerResource (dota branch):
    # m_playerIDToPlayer: 0x510
    # m_vecPlayerTeamData: 0x538
    # m_vecPlayerData: 0x5B0  (CNetworkUtlVectorBase<PlayerResourcePlayerData_t>)
    # With +8 shift: 0x5B8
    #
    # CNetworkUtlVectorBase is NOT CUtlVector — it's simpler
    # Layout: inline array (fixed size, embedded in the class)
    # PlayerResourcePlayerData_t entries are right there at the offset
    #
    # Actually, in networked entities, CNetworkUtlVectorBase = CUtlVector with
    # m_pElements at +0x00, m_iSize at +0x08

    for try_name, vec_off in [
        ("m_vecPlayerData (0x5B0)", 0x5B0),
        ("m_vecPlayerData+8 (0x5B8)", 0x5B8),
        ("m_vecPlayerData (0x5C0)", 0x5C0),
        ("m_vecPlayerData (0x5D0)", 0x5D0),
        ("m_vecPlayerData (0x5E0)", 0x5E0),
        ("m_vecPlayerData (0x5F0)", 0x5F0),
        ("m_vecPlayerData (0x600)", 0x600),
        ("m_vecPlayerData (0x610)", 0x610),
        ("m_vecPlayerData (0x620)", 0x620),
        ("m_vecPlayerData (0x630)", 0x630),
    ]:
        if vec_off + 16 > scan_size:
            continue
        arr_ptr = struct.unpack("<Q", data[vec_off:vec_off+8])[0]
        arr_size = struct.unpack("<I", data[vec_off+8:vec_off+12])[0]

        if arr_ptr > 0x10000 and 0 < arr_size <= 64:
            print(f"\n  {try_name}: ptr={hex(arr_ptr)}, size={arr_size}")
            # Try reading from the external pointer
            # Read slot 0 and slot 1 of PlayerResourcePlayerData_t
            # Try various strides
            for stride in [0x100, 0x120, 0x140, 0x160, 0x180, 0x1A0, 0x1C0, 0x200, 0x240, 0x280, 0x300, 0x400]:
                try:
                    b0 = mem.read_u8(arr_ptr + 0 * stride + 0xD4)
                    b1 = mem.read_u8(arr_ptr + 1 * stride + 0xD4)
                    if b0 == 0 and b1 == 1:
                        # Verify more slots
                        vals = [b0, b1]
                        for s in range(2, min(10, arr_size)):
                            vals.append(mem.read_u8(arr_ptr + s * stride + 0xD4))
                        if all(v in (0, 1) for v in vals):
                            print(f"    FOUND! stride=0x{stride:X}: m_bIsBot = {vals}")
                            print(f"    Formula: arr_ptr + slot * 0x{stride:X} + 0xD4")
                            # Also check m_bFakeClient at +0x45
                            fakes = []
                            for s in range(min(10, arr_size)):
                                fakes.append(mem.read_u8(arr_ptr + s * stride + 0x45))
                            print(f"    m_bFakeClient (at +0x45): {fakes}")
                except:
                    continue

    print("\nDone!")


if __name__ == "__main__":
    main()
