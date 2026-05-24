"""
Dota 2 SchemaSystem Dumper — dumps netvars/offsets from running process.
Based on Wolf49406/Dota2Patcher CSchemaSystem layout.

Usage: python dump_offsets.py
Outputs: offsets_dump.json + prints key offsets to console.
"""
import json
import sys
import os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from cheat.memory import DotaMemory

# SchemaSystem layout constants (from Dota2Patcher)
CLASS_DESCRIPTION_CONTAINERS_ARRAY_OFFSET = 0x580
CLASS_DESCRIPTION_CONTAINERS_ARRAY_SIZE = 0x28
CLASS_DESCRIPTION_CONTAINERS_ARRAY_MAX_INDEX = 256
CLASS_DESCRIPTION_CONTAINER_SIZE = 0x20
SCHEMA_CLASS_FIELD_DATA_SIZE = 0x20


def dump_schema(mem: DotaMemory) -> dict:
    """Dump all netvars from SchemaSystem for client.dll scope."""
    schema_sys = mem.find_interface("schemasystem.dll", "SchemaSystem_001")
    if not schema_sys:
        print("[!] SchemaSystem_001 not found!")
        return {}
    print(f"[+] SchemaSystem: {hex(schema_sys)}")

    # Find "client.dll" type scope
    scopes_list = mem.read_ptr(schema_sys + 0x198)
    if not scopes_list:
        print("[!] Scopes list is null")
        return {}

    client_scope = None
    for i in range(18):
        scope = mem.read_ptr(scopes_list + i * 8)
        if not scope:
            continue
        try:
            name = mem.read_string(scope + 0x8, 64)
            if name == "client.dll":
                client_scope = scope
                print(f"[+] Found client.dll scope at {hex(scope)}")
                break
        except:
            continue

    if not client_scope:
        print("[!] client.dll scope not found!")
        return {}

    # Walk class description containers
    all_netvars = {}
    total_netvars = 0
    container_index = 0

    while container_index <= CLASS_DESCRIPTION_CONTAINERS_ARRAY_MAX_INDEX:
        container_base = client_scope + CLASS_DESCRIPTION_CONTAINERS_ARRAY_OFFSET
        container_ptr_addr = container_base + CLASS_DESCRIPTION_CONTAINERS_ARRAY_SIZE * container_index

        try:
            container_ptr = mem.read_ptr(container_ptr_addr + 0x18)
        except:
            container_index += 1
            continue

        if not container_ptr or container_ptr < 0x10000:
            container_index += 1
            continue

        # Walk class descriptions in this container
        class_idx = 0
        while True:
            class_desc_addr_loc = container_ptr + CLASS_DESCRIPTION_CONTAINER_SIZE * class_idx + 0x10
            try:
                class_desc = mem.read_ptr(class_desc_addr_loc)
            except:
                break
            if not class_desc or class_desc < 0x10000:
                break

            # Read class name
            try:
                class_name_ptr = mem.read_ptr(class_desc + 0x8)
                if not class_name_ptr:
                    class_idx += 1
                    continue
                class_name = mem.read_string(class_name_ptr, 128)
            except:
                class_idx += 1
                continue

            if not class_name:
                class_idx += 1
                continue

            # Read members
            members_count = mem.read_u32(class_desc + 0x1C) if mem.read_u32(class_desc + 0x1C) else 0
            members_base = mem.read_ptr(class_desc + 0x28)

            if members_base and members_count > 0 and members_count < 500:
                class_fields = {}
                for field_idx in range(members_count):
                    field_addr = members_base + SCHEMA_CLASS_FIELD_DATA_SIZE * field_idx

                    # Check is_netvar: +0x14 < 10
                    try:
                        netvar_flag = mem.read_i32(field_addr + 0x14)
                        if netvar_flag >= 10:
                            break
                    except:
                        break

                    try:
                        field_name_ptr = mem.read_ptr(field_addr)
                        if not field_name_ptr:
                            continue
                        field_name = mem.read_string(field_name_ptr, 128)
                        field_offset = mem.read_i32(field_addr + 0x10)

                        # Read type name
                        type_ptr = mem.read_ptr(field_addr + 0x8)
                        type_name = ""
                        if type_ptr:
                            type_name_ptr = mem.read_ptr(type_ptr + 0x8)
                            if type_name_ptr:
                                type_name = mem.read_string(type_name_ptr, 64)

                        if field_name and field_offset != 0:
                            class_fields[field_name] = {
                                "offset": field_offset,
                                "type": type_name
                            }
                            total_netvars += 1
                    except:
                        continue

                if class_fields:
                    all_netvars[class_name] = class_fields

            class_idx += 1

        container_index += 1

    print(f"[+] Dumped {len(all_netvars)} classes, {total_netvars} netvars")
    return all_netvars


def print_key_offsets(netvars: dict):
    """Print offsets we care about."""
    key_classes = {
        "C_BaseEntity": [
            "m_pGameSceneNode", "m_iHealth", "m_lifeState", "m_iTeamNum",
            "m_fFlags", "m_vecVelocity"
        ],
        "CGameSceneNode": [
            "m_vecOrigin", "m_vecAbsOrigin"
        ],
        "C_DOTA_BaseNPC": [
            "m_iCurrentLevel", "m_flMana", "m_flMaxMana", "m_iDamageMin",
            "m_iDamageMax", "m_iszUnitName", "m_iTaggedAsVisibleByTeam",
            "m_ModifierManager", "m_bIsClone", "m_flManaRegen"
        ],
        "C_DOTA_BaseNPC_Hero": [
            "m_iHeroID", "m_hReplicatingOtherHeroModel", "m_iCurrentXP",
            "m_flStrength", "m_flAgility", "m_flIntellect"
        ],
        "C_DOTAGamerules": [
            "m_nGameState", "m_nServerGameState", "m_nHeroPickState",
            "m_flStateTransitionTime", "m_iGameMode", "m_bGamePaused",
            "m_flGameTime", "m_iPlayerIDsInControl"
        ],
        "CDOTAPlayerController": [
            "m_hAssignedHero", "m_nPlayerID"
        ],
        "C_DOTAPlayerController": [
            "m_hAssignedHero", "m_nPlayerID"
        ],
    }

    print("\n" + "=" * 60)
    print("KEY OFFSETS (runtime dump)")
    print("=" * 60)

    for cls, fields in key_classes.items():
        if cls not in netvars:
            # Try without C_ prefix
            alt = cls.replace("C_", "")
            if alt in netvars:
                cls = alt
            else:
                print(f"\n[!] {cls} — NOT FOUND")
                continue

        print(f"\n--- {cls} ---")
        for field in fields:
            if field in netvars[cls]:
                info = netvars[cls][field]
                print(f"  {field}: 0x{info['offset']:X} ({info['type']})")
            else:
                print(f"  {field}: NOT FOUND")


def main():
    print("=== Dota 2 SchemaSystem Dumper ===\n")
    mem = DotaMemory()
    print(f"[+] PID: {mem.pid}")

    netvars = dump_schema(mem)

    if netvars:
        # Save full dump
        dump_path = os.path.join(os.path.dirname(__file__), "offsets_dump.json")
        # Convert for JSON serialization
        json_data = {}
        for cls, fields in netvars.items():
            json_data[cls] = {}
            for fname, finfo in fields.items():
                json_data[cls][fname] = {
                    "offset": finfo["offset"],
                    "offset_hex": f"0x{finfo['offset']:X}",
                    "type": finfo["type"]
                }

        with open(dump_path, "w") as f:
            json.dump(json_data, f, indent=2)
        print(f"\n[+] Full dump saved to: {dump_path}")

        print_key_offsets(netvars)

    mem.close()


if __name__ == "__main__":
    main()
