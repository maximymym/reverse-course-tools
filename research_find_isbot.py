"""
Find m_bIsBot offset via string xref in client.dll.

m_bIsBot string is at client.dll RVA +0x04A702D0.
Schema field descriptor has: +0x00 = name_ptr, +0x10 = offset (uint32).
Find pointers TO the string → find the field descriptor → read the offset.

Also: dump server.dll schema properly for PlayerResource.
"""
import sys, os, time, struct
sys.path.insert(0, os.path.dirname(__file__))

from cheat.memory import DotaMemory
from cheat.offsets import Interfaces, SchemaSystem


def find_ptr_xrefs(mem, module_name, target_addr, section_hint=".rdata"):
    """Find 8-byte pointers to target_addr in module's data sections."""
    base = mem.module_base(module_name)
    size = mem.module_size(module_name)
    target_bytes = struct.pack("<Q", target_addr)

    xrefs = []
    chunk_size = 4 * 1024 * 1024
    # Search the whole module
    for offset in range(0, size, chunk_size):
        read_size = min(chunk_size, size - offset)
        try:
            data = mem.pm.read_bytes(base + offset, read_size)
        except:
            continue

        pos = 0
        while True:
            pos = data.find(target_bytes, pos)
            if pos == -1:
                break
            rva = offset + pos
            xrefs.append(rva)
            pos += 8

    return xrefs


def main():
    mem = DotaMemory()
    print(f"[+] PID {mem.pid}")

    client_base = mem.module_base("client.dll")
    server_base = mem.module_base("server.dll")

    # === 1. Find m_bIsBot field descriptor in client.dll ===
    print("=" * 60)
    print("PHASE 1: Find m_bIsBot offset via string xref")
    print("=" * 60)

    # m_bIsBot string at client.dll RVA +0x04A702D0
    isbot_str_addr = client_base + 0x04A702D0

    # Verify string
    s = mem.read_string(isbot_str_addr, 32)
    print(f"  String at {hex(isbot_str_addr)}: '{s}'")

    # Find pointers to this string (field descriptors have name_ptr at +0x00)
    print(f"  Searching client.dll for pointers to m_bIsBot string...")
    xrefs = find_ptr_xrefs(mem, "client.dll", isbot_str_addr)
    print(f"  Found {len(xrefs)} pointer xrefs")

    for rva in xrefs:
        addr = client_base + rva
        # This is a field descriptor: +0x00 = name_ptr (our xref), +0x10 = offset
        try:
            field_offset = mem.read_u32(addr + 0x10)
            field_netvar = mem.read_u32(addr + 0x14)
            print(f"  XREF at client.dll+0x{rva:08X}: field offset = 0x{field_offset:04X} (netvar={field_netvar})")

            # Try to find which class this belongs to
            # Read type info at +0x08
            type_ptr = mem.read_ptr(addr + 0x08)
            if type_ptr and type_ptr > 0x10000:
                type_name = mem.read_string(type_ptr, 64)
                print(f"    type: {type_name}")
        except:
            pass

    # === 2. Same for m_bFakeClient in server.dll ===
    print("\n" + "=" * 60)
    print("PHASE 2: Find m_bFakeClient offset")
    print("=" * 60)

    fake_str_addr = server_base + 0x0412DAC0
    s = mem.read_string(fake_str_addr, 32)
    print(f"  String at {hex(fake_str_addr)}: '{s}'")

    xrefs = find_ptr_xrefs(mem, "server.dll", fake_str_addr)
    print(f"  Found {len(xrefs)} pointer xrefs")

    for rva in xrefs:
        addr = server_base + rva
        try:
            field_offset = mem.read_u32(addr + 0x10)
            print(f"  XREF at server.dll+0x{rva:08X}: field offset = 0x{field_offset:04X}")
        except:
            pass

    # m_bIsBot in server.dll
    isbot_srv_addr = server_base + 0x0412DE48
    s = mem.read_string(isbot_srv_addr, 32)
    print(f"\n  m_bIsBot string at {hex(isbot_srv_addr)}: '{s}'")

    xrefs = find_ptr_xrefs(mem, "server.dll", isbot_srv_addr)
    print(f"  Found {len(xrefs)} pointer xrefs")

    for rva in xrefs:
        addr = server_base + rva
        try:
            field_offset = mem.read_u32(addr + 0x10)
            print(f"  XREF at server.dll+0x{rva:08X}: field offset = 0x{field_offset:04X}")
        except:
            pass

    # === 3. Dump server.dll schema for PlayerResource properly ===
    print("\n" + "=" * 60)
    print("PHASE 3: Dump server.dll schema — full approach")
    print("=" * 60)

    schema_system = mem.find_interface("schemasystem.dll", Interfaces.SCHEMA_SYSTEM)
    scopes_ptr = mem.read_ptr(schema_system + SchemaSystem.SCOPES_LIST)

    for scope_idx in range(20):
        try:
            scope_ptr = mem.read_ptr(scopes_ptr + scope_idx * 8)
            if not scope_ptr or scope_ptr < 0x10000:
                continue
            scope_name = mem.read_string(scope_ptr + SchemaSystem.SCOPE_NAME, 64)
        except:
            continue

        if "server" not in scope_name.lower():
            continue

        print(f"\n  Scope: '{scope_name}' at {hex(scope_ptr)}")

        # Read the hash map properly
        # The hash map is at scope + 0x580
        # Layout: count at offset within hash map header, then buckets
        # Let me read the hash map header
        hm_base = scope_ptr + 0x580
        # TSHashMap structure:
        # +0x00: m_nCount (uint32 or uint64)
        # +0x08 or similar: buckets array
        # Let me read a block and analyze
        try:
            hm_data = mem.pm.read_bytes(hm_base, 0x40)
            print(f"  HashMap header (first 64 bytes):")
            for i in range(0, 64, 8):
                v = struct.unpack("<Q", hm_data[i:i+8])[0]
                print(f"    +0x{i:02X}: 0x{v:016X} ({v})")
        except Exception as e:
            print(f"  Error reading hash map: {e}")

        # The containers array might be at a different offset
        # Let me try the approach from our working dump_offsets.py
        # In our Phase 0, we used:
        #   containers at scope + 0x580, stride 0x28, ptr at +0x18
        # But that was for iterating a flat array of buckets

        # Let me read more and find actual class data
        # Try reading entries at stride 0x28
        found_classes = []
        for i in range(1024):
            entry_addr = hm_base + i * 0x28
            try:
                ptr = mem.read_ptr(entry_addr + 0x18)
                if not ptr or ptr < 0x10000:
                    continue

                # Check if this looks like a ClassDescription
                # ClassDescription: +0x08 = name_ptr
                name_ptr = mem.read_ptr(ptr + 0x08)
                if not name_ptr or name_ptr < 0x10000:
                    continue

                cname = mem.read_string(name_ptr, 128)
                if not cname or len(cname) < 2 or not cname[0].isalpha():
                    continue

                found_classes.append((cname, ptr))

                # Only print interesting ones
                if any(kw in cname for kw in ["PlayerResource", "PlayerData", "Bot",
                                               "BasePlayer", "GameMode", "DOTAPlayer",
                                               "FakeClient"]):
                    class_size = mem.read_u32(ptr + 0x18)
                    member_count = mem.read_u32(ptr + 0x1C)
                    members_base = mem.read_ptr(ptr + 0x28)

                    print(f"\n  CLASS: {cname} (size=0x{class_size:X}, {member_count} members)")

                    if members_base and members_base > 0x10000 and member_count < 500:
                        for mi in range(member_count):
                            field_addr = members_base + mi * 0x20
                            try:
                                fn_ptr = mem.read_ptr(field_addr + 0x00)
                                if fn_ptr and fn_ptr > 0x10000:
                                    fname = mem.read_string(fn_ptr, 128)
                                    foffset = mem.read_u32(field_addr + 0x10)
                                    marker = ""
                                    if "bot" in fname.lower() or "fake" in fname.lower():
                                        marker = " <<<< BOT"
                                    if "hero" in fname.lower() or "team" in fname.lower():
                                        marker = " <<<< RELEVANT"
                                    print(f"    +0x{foffset:04X} {fname}{marker}")
                            except:
                                continue
            except:
                continue

        print(f"\n  Total classes found in server.dll scope: {len(found_classes)}")

        # Print all class names containing "Player" or "Bot"
        for cname, ptr in found_classes:
            if "Player" in cname or "Bot" in cname:
                print(f"    {cname}")

    print("\nDone!")


if __name__ == "__main__":
    main()
