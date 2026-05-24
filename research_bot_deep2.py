"""
Deep bot research Phase 2:
1. Dump server.dll schema for PlayerResourcePlayerData_t → find m_bIsBot offset
2. Find dota_playerresource entity
3. Try VScript commands with feedback
4. Try setting m_bIsBot flag
"""
import sys, os, time, struct
sys.path.insert(0, os.path.dirname(__file__))

from cheat.memory import DotaMemory
from cheat.commands import DotaCommands
from cheat.game_state import DotaGame
from cheat.offsets import (GameState, BaseEntity, GameSceneNode, BaseNPC,
                           EntitySystem, EntityIdentity, Interfaces, SchemaSystem)


def dump_server_schema_class(mem, class_name_keyword):
    """Search server.dll schema scope for classes matching keyword."""
    print(f"\n[+] Searching server.dll schema for '{class_name_keyword}'...")

    # Find SchemaSystem
    schema_system = mem.find_interface("schemasystem.dll", Interfaces.SCHEMA_SYSTEM)
    if not schema_system:
        print("[!] SchemaSystem not found")
        return []

    # Iterate scopes
    scopes_ptr = mem.read_ptr(schema_system + SchemaSystem.SCOPES_LIST)
    results = []

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

        print(f"  [+] Found scope: '{scope_name}' at {hex(scope_ptr)}")

        # Iterate containers
        containers_base = scope_ptr + SchemaSystem.CONTAINERS_ARRAY
        for cont_idx in range(SchemaSystem.CONTAINERS_MAX):
            try:
                entry_addr = containers_base + cont_idx * SchemaSystem.CONTAINERS_ARRAY_STRIDE
                container_ptr = mem.read_ptr(entry_addr + SchemaSystem.CONTAINER_PTR)
                if not container_ptr or container_ptr < 0x10000:
                    continue

                # Read class descriptions in this container
                # Container format varies; let's try reading class name
                # The container is a hash bucket with chained entries
                # Each entry: +0x00 = hash, +0x08 = data_ptr, +0x10 = next
                # Or it could be an array. Let me try reading directly.

                # Actually, let's read ClassDescription at container_ptr
                name_ptr = mem.read_ptr(container_ptr + 0x08)  # class name
                if not name_ptr or name_ptr < 0x10000:
                    continue
                cname = mem.read_string(name_ptr, 128)
                if class_name_keyword.lower() not in cname.lower():
                    continue

                # Found matching class!
                class_size = mem.read_u32(container_ptr + 0x18)
                member_count = mem.read_u32(container_ptr + 0x1C)
                members_base = mem.read_ptr(container_ptr + 0x28)

                print(f"\n  CLASS: {cname} (size=0x{class_size:X}, {member_count} members)")

                fields = []
                if members_base and members_base > 0x10000:
                    for mi in range(member_count):
                        field_addr = members_base + mi * 0x20
                        try:
                            fn_ptr = mem.read_ptr(field_addr + 0x00)
                            if not fn_ptr or fn_ptr < 0x10000:
                                continue
                            fname = mem.read_string(fn_ptr, 128)
                            foffset = mem.read_u32(field_addr + 0x10)
                            fnetvar = mem.read_u32(field_addr + 0x14)
                            fields.append((fname, foffset, fnetvar))
                            print(f"    +0x{foffset:04X} {fname} (netvar={fnetvar})")
                        except:
                            continue

                results.append({"name": cname, "size": class_size, "fields": fields})
            except:
                continue

    return results


def find_entity_by_name_substring(game, mem, substring):
    """Find entities whose designerName contains substring."""
    found = []
    for entity, ident, name in game.iter_entities():
        if name and substring.lower() in name.lower():
            found.append((entity, ident, name))
    return found


def main():
    mem = DotaMemory()
    print(f"[+] PID {mem.pid}")
    game = DotaGame(mem)
    game.init()
    cmd = DotaCommands(mem)
    cmd.init(game)

    state = game.get_game_state()
    print(f"[+] Game state: {state} ({GameState.NAMES.get(state, '?')})")

    # === 1. Search server.dll schema for bot-related classes ===
    print("\n" + "=" * 60)
    print("PHASE 1: Server.dll Schema — Bot Classes")
    print("=" * 60)

    for kw in ["PlayerResource", "PlayerData", "BotController", "CDOTABot",
               "GameMode", "BaseGameMode"]:
        dump_server_schema_class(mem, kw)

    # === 2. Find playerresource entity ===
    print("\n" + "=" * 60)
    print("PHASE 2: Find PlayerResource Entity")
    print("=" * 60)

    pr_ents = find_entity_by_name_substring(game, mem, "playerresource")
    if not pr_ents:
        pr_ents = find_entity_by_name_substring(game, mem, "player_resource")
    if not pr_ents:
        # Try all entities, look for "resource" or "player"
        pr_ents = find_entity_by_name_substring(game, mem, "resource")

    if pr_ents:
        for entity, ident, name in pr_ents:
            print(f"  Found: {name} at {hex(entity)}")
    else:
        print("  [!] PlayerResource not found via designerName")
        # Try scanning entity types
        print("  Scanning for known entity types...")
        type_counts = {}
        for entity, ident, name in game.iter_entities():
            if name and len(name) < 100 and name.isascii():
                type_counts[name] = type_counts.get(name, 0) + 1
        for name, cnt in sorted(type_counts.items()):
            if "player" in name.lower() or "resource" in name.lower() or "game" in name.lower():
                print(f"    {cnt:4d}x {name}")

    # === 3. m_bIsBot via client.dll schema ===
    print("\n" + "=" * 60)
    print("PHASE 3: Find m_bIsBot in client.dll schema")
    print("=" * 60)

    # Search client.dll scope too
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

        if "client" not in scope_name.lower():
            continue

        print(f"  [+] Scope: '{scope_name}'")

        containers_base = scope_ptr + SchemaSystem.CONTAINERS_ARRAY
        for cont_idx in range(SchemaSystem.CONTAINERS_MAX):
            try:
                entry_addr = containers_base + cont_idx * SchemaSystem.CONTAINERS_ARRAY_STRIDE
                container_ptr = mem.read_ptr(entry_addr + SchemaSystem.CONTAINER_PTR)
                if not container_ptr or container_ptr < 0x10000:
                    continue

                name_ptr = mem.read_ptr(container_ptr + 0x08)
                if not name_ptr or name_ptr < 0x10000:
                    continue
                cname = mem.read_string(name_ptr, 128)

                # Look for PlayerResource or PlayerData
                if "PlayerResource" not in cname and "PlayerData" not in cname:
                    continue

                class_size = mem.read_u32(container_ptr + 0x18)
                member_count = mem.read_u32(container_ptr + 0x1C)
                members_base = mem.read_ptr(container_ptr + 0x28)

                print(f"\n  CLASS: {cname} (size=0x{class_size:X}, {member_count} members)")

                if members_base and members_base > 0x10000:
                    for mi in range(member_count):
                        field_addr = members_base + mi * 0x20
                        try:
                            fn_ptr = mem.read_ptr(field_addr + 0x00)
                            fname = mem.read_string(fn_ptr, 128)
                            foffset = mem.read_u32(field_addr + 0x10)
                            # Print bot-relevant fields
                            if any(kw in fname.lower() for kw in ["bot", "fake", "flag", "team", "hero", "slot", "steam"]):
                                print(f"    +0x{foffset:04X} {fname}")
                        except:
                            continue
            except:
                continue

    # === 4. VScript test with feedback ===
    print("\n" + "=" * 60)
    print("PHASE 4: VScript Execution Test")
    print("=" * 60)

    # In Dota 2 local server, VScript = Lua
    # Try executing code and writing result to a ConVar for feedback
    vscript_cmds = [
        # Test if script works by setting a convar
        'script_reload_code',
        'script print("BOT_RESEARCH_ALIVE")',
        'script GameRules:GetGameModeEntity():SetBotThinkingEnabled(true)',
        # Try to find the bot adding function
        'script SendToServerConsole("dota_bot_populate")',
        # Try to manually create a bot
        'script GameRules:AddBotPlayerWithEntityScript("npc_dota_hero_axe", 3, "", "")',
        # Try adding with team (2=Radiant, 3=Dire)
        'script local bot = PlayerResource:AddFakeClient("BotTest"); print("Added bot: " .. tostring(bot))',
    ]

    for c in vscript_cmds:
        print(f"  [>] {c}")
        cmd.execute(c)
        time.sleep(1)

    # Check if any new controllers appeared
    print("\n  Controllers after VScript:")
    for entity, ident, name in game.iter_entities():
        if name == "dota_player_controller":
            slot = mem.read_i32(entity + 0x908)
            pname = ""
            try:
                pname = mem.read_string(entity + 0x6E0, 32)
            except:
                pass
            print(f"    slot={slot:3d} name='{pname}'")

    # Check BotThinkingEnabled ConVar
    bt = cmd.convar.get_int("dota_bot_client_debug")
    print(f"\n  dota_bot_client_debug = {bt}")

    # === 5. Direct string xref: find AddBot function ===
    print("\n" + "=" * 60)
    print("PHASE 5: Find AddBot function via string xref")
    print("=" * 60)

    server_base = mem.module_base("server.dll")
    # "AddBot" string at RVA +0x04404C70
    addbot_str = server_base + 0x04404C70
    print(f"  'AddBot' string at {hex(addbot_str)}")

    # Search for references to this string address in server.dll code
    # Look for LEA rxx, [rip + offset] patterns pointing to this string
    server_size = mem.module_size("server.dll")
    print(f"  Searching {server_size/1024/1024:.0f}MB for xrefs...")

    # Read code sections and search for the string address
    # LEA pattern: 48 8D xx xx xx xx xx where the relative offset points to our string
    target_addr = addbot_str
    found_xrefs = []

    chunk_size = 4 * 1024 * 1024
    for offset in range(0, min(server_size, 0x4000000), chunk_size):  # first ~64MB (code section)
        try:
            data = mem.pm.read_bytes(server_base + offset, min(chunk_size, server_size - offset))
        except:
            continue

        for i in range(len(data) - 7):
            # LEA r??, [rip + imm32] = 48 8D xx xx xx xx xx (or 4C 8D)
            if data[i] in (0x48, 0x4C) and data[i+1] == 0x8D:
                modrm = data[i+2]
                if (modrm & 0xC7) == 0x05:  # [rip+disp32] addressing
                    disp = struct.unpack("<i", data[i+3:i+7])[0]
                    effective_addr = server_base + offset + i + 7 + disp
                    if effective_addr == target_addr:
                        rva = offset + i
                        found_xrefs.append(rva)
                        print(f"  XREF at server.dll+0x{rva:08X} ({hex(server_base + rva)})")
                        if len(found_xrefs) >= 5:
                            break
        if len(found_xrefs) >= 5:
            break

    if not found_xrefs:
        print("  No direct LEA xrefs found in first 64MB")
        # Try "AddBotWithSettings_Script" instead
        addbot2_str = server_base + 0x04404C78
        print(f"\n  Trying 'AddBotWithSettings_Script' at {hex(addbot2_str)}...")
        for offset in range(0, min(server_size, 0x4000000), chunk_size):
            try:
                data = mem.pm.read_bytes(server_base + offset, min(chunk_size, server_size - offset))
            except:
                continue
            for i in range(len(data) - 7):
                if data[i] in (0x48, 0x4C) and data[i+1] == 0x8D:
                    modrm = data[i+2]
                    if (modrm & 0xC7) == 0x05:
                        disp = struct.unpack("<i", data[i+3:i+7])[0]
                        effective_addr = server_base + offset + i + 7 + disp
                        if effective_addr == addbot2_str:
                            rva = offset + i
                            print(f"  XREF at server.dll+0x{rva:08X}")
                            found_xrefs.append(rva)
                            if len(found_xrefs) >= 3:
                                break
            if len(found_xrefs) >= 3:
                break

    print("\nDone!")


if __name__ == "__main__":
    main()
