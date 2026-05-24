"""
Deep bot research Phase 3:
1. Find m_bIsBot offset by diffing PlayerResource data for human vs bot slot
2. Find AddBot function and understand parameters
3. Try calling AddBot via shellcode or VScript
"""
import sys, os, time, struct
sys.path.insert(0, os.path.dirname(__file__))

from cheat.memory import DotaMemory
from cheat.commands import DotaCommands
from cheat.game_state import DotaGame
from cheat.offsets import GameState


def search_schema_all_scopes(mem, class_keyword):
    """Search all SchemaSystem scopes for classes matching keyword."""
    from cheat.offsets import Interfaces, SchemaSystem

    schema_system = mem.find_interface("schemasystem.dll", Interfaces.SCHEMA_SYSTEM)
    scopes_ptr = mem.read_ptr(schema_system + SchemaSystem.SCOPES_LIST)

    all_results = []
    for scope_idx in range(20):
        try:
            scope_ptr = mem.read_ptr(scopes_ptr + scope_idx * 8)
            if not scope_ptr or scope_ptr < 0x10000:
                continue
            scope_name = mem.read_string(scope_ptr + SchemaSystem.SCOPE_NAME, 64)
        except:
            continue

        # Use the hash table approach from our working dumper
        # The containers at scope+0x580 are a hash table
        # Each bucket: stride 0x28, ptr at +0x18 to ClassDescription
        containers_base = scope_ptr + SchemaSystem.CONTAINERS_ARRAY

        for cont_idx in range(512):  # more buckets
            try:
                entry_addr = containers_base + cont_idx * SchemaSystem.CONTAINERS_ARRAY_STRIDE
                container_ptr = mem.read_ptr(entry_addr + SchemaSystem.CONTAINER_PTR)
                if not container_ptr or container_ptr < 0x10000:
                    continue

                # Walk chain if needed
                current = container_ptr
                for _ in range(10):  # max chain length
                    if not current or current < 0x10000:
                        break

                    name_ptr = mem.read_ptr(current + 0x08)
                    if not name_ptr or name_ptr < 0x10000:
                        # Try next in chain
                        current = mem.read_ptr(current + 0x20)
                        continue

                    cname = mem.read_string(name_ptr, 128)
                    if class_keyword.lower() in cname.lower():
                        class_size = mem.read_u32(current + 0x18)
                        member_count = mem.read_u32(current + 0x1C)
                        members_base = mem.read_ptr(current + 0x28)

                        fields = []
                        if members_base and members_base > 0x10000 and member_count < 500:
                            for mi in range(member_count):
                                field_addr = members_base + mi * 0x20
                                try:
                                    fn_ptr = mem.read_ptr(field_addr + 0x00)
                                    if fn_ptr and fn_ptr > 0x10000:
                                        fname = mem.read_string(fn_ptr, 128)
                                        foffset = mem.read_u32(field_addr + 0x10)
                                        fields.append((fname, foffset))
                                except:
                                    continue

                        all_results.append({
                            "scope": scope_name,
                            "name": cname,
                            "size": class_size,
                            "fields": fields,
                        })

                    # Next in chain
                    current = mem.read_ptr(current + 0x20)
            except:
                continue

    return all_results


def main():
    mem = DotaMemory()
    print(f"[+] PID {mem.pid}")
    game = DotaGame(mem)
    game.init()
    cmd = DotaCommands(mem)
    cmd.init(game)

    # === 1. Schema search for PlayerResource across ALL scopes ===
    print("\n" + "=" * 60)
    print("PHASE 1: Schema search — PlayerResource + PlayerData")
    print("=" * 60)

    for kw in ["PlayerResource", "PlayerData_t"]:
        results = search_schema_all_scopes(mem, kw)
        for r in results:
            print(f"\n  [{r['scope']}] {r['name']} (size=0x{r['size']:X}, {len(r['fields'])} fields)")
            for fname, foff in sorted(r["fields"], key=lambda x: x[1]):
                marker = ""
                if "bot" in fname.lower() or "fake" in fname.lower():
                    marker = " <<<< BOT FLAG"
                print(f"    +0x{foff:04X} {fname}{marker}")

    # === 2. Find C_DOTA_PlayerResource entity and dump ===
    print("\n" + "=" * 60)
    print("PHASE 2: PlayerResource entity — memory diff human vs bot")
    print("=" * 60)

    pr_entity = None
    for entity, ident, name in game.iter_entities():
        if name and "PlayerResource" in name:
            pr_entity = entity
            print(f"  [+] PlayerResource: {hex(entity)} (designerName: {name})")
            break

    if not pr_entity:
        # search by partial name
        for entity, ident, name in game.iter_entities():
            if name and "resource" in name.lower():
                print(f"  candidate: {name} at {hex(entity)}")

    if pr_entity:
        # PlayerResource stores per-player data in arrays
        # Let's search for m_bIsBot by comparing slot 0 (human) vs slot 1 (bot)
        # Common pattern: base + per_player_stride * slot_index

        # First, let's try reading a large block and look for known patterns
        # Slot 0: human (qt317792), Slot 1: bot (Dominique)
        # m_bIsBot for slot 0 = 0 (false), slot 1 = 1 (true)
        # m_bFakeClient for slot 0 = 0, slot 1 = 1

        # Read 16KB block from entity
        block_size = 16384
        print(f"\n  Reading {block_size} bytes from PlayerResource...")

        # Read multiple blocks to cover the entity
        for block_start in range(0, 0x10000, block_size):
            try:
                block = mem.pm.read_bytes(pr_entity + block_start, block_size)
            except:
                continue

            # Search for pattern: byte at offset X is 0 (human) and
            # byte at offset X+stride is 1 (bot)
            # Common strides: 1 (packed bools), 4 (int32 per player), 8, 0x10, etc.
            # PlayerResource has MAX_PLAYERS slots (24 or 64)

            # For packed bool arrays with stride=1:
            for off in range(len(block) - 64):
                # Pattern: 10 consecutive bytes where:
                # [0] = 0 (slot 0, human)
                # [1] = 1 (slot 1, bot Dominique)
                # [2..9] = could be 0 or 1 (other bot slots?)
                b = block[off:off+24]
                # Human at slot 0 = 0, Bot at slot 1 = 1
                if b[0] == 0 and b[1] == 1:
                    # Check if rest looks like a bool array
                    # Values should all be 0 or 1
                    if all(v in (0, 1) for v in b[:10]):
                        # Count how many are "true" (bots)
                        true_count = sum(b[i] for i in range(10))
                        if 1 <= true_count <= 9:  # reasonable number of bots
                            abs_off = block_start + off
                            vals = ' '.join(f'{b[i]}' for i in range(10))
                            print(f"  CANDIDATE m_bIsBot bool[10] at +0x{abs_off:04X}: [{vals}]")

            # For int32 arrays with stride=4:
            for off in range(0, len(block) - 256, 4):
                vals = []
                for s in range(10):
                    v = struct.unpack("<i", block[off + s*4:off + s*4 + 4])[0]
                    vals.append(v)
                if vals[0] == 0 and vals[1] == 1 and all(v in (0, 1) for v in vals[:10]):
                    true_count = sum(vals[:10])
                    if 1 <= true_count <= 9:
                        abs_off = block_start + off
                        v_str = ' '.join(f'{v}' for v in vals[:10])
                        print(f"  CANDIDATE m_bIsBot int32[10] at +0x{abs_off:04X}: [{v_str}]")

    # === 3. Disassemble AddBot xref ===
    print("\n" + "=" * 60)
    print("PHASE 3: AddBot function analysis")
    print("=" * 60)

    server_base = mem.module_base("server.dll")
    # XREF found at server.dll+0x02791C6D
    xref_rva = 0x02791C6D
    xref_addr = server_base + xref_rva

    print(f"  AddBot xref at {hex(xref_addr)} (RVA +0x{xref_rva:08X})")

    # Read surrounding code (back up to find function start)
    # Read 256 bytes before and 512 after
    code_start = xref_addr - 256
    code_size = 768
    try:
        code = mem.pm.read_bytes(code_start, code_size)
        print(f"  Read {code_size} bytes around xref")

        # Find function prologue (look for common patterns before xref)
        # Common prologues: 48 89 xx 24 xx (mov [rsp+xx], rXX) or 40 55/53/56/57 (push)
        # Or: CC CC CC (int3 padding before function)

        # Search backward from xref for CC CC CC pattern (function boundary)
        xref_rel = 256  # offset of xref within our buffer
        func_start_rel = xref_rel
        for i in range(xref_rel - 1, max(0, xref_rel - 200), -1):
            if code[i] == 0xCC and (i == 0 or code[i-1] == 0xCC):
                func_start_rel = i + 1
                while func_start_rel < xref_rel and code[func_start_rel] == 0xCC:
                    func_start_rel += 1
                break

        func_rva = xref_rva - (xref_rel - func_start_rel)
        print(f"  Estimated function start: server.dll+0x{func_rva:08X}")

        # Print hex dump of function
        func_bytes = code[func_start_rel:min(func_start_rel + 256, len(code))]
        print(f"  First 128 bytes of function:")
        for i in range(0, min(128, len(func_bytes)), 16):
            hex_str = ' '.join(f'{func_bytes[i+j]:02X}' for j in range(min(16, len(func_bytes)-i)))
            print(f"    +{i:04X}: {hex_str}")

    except Exception as e:
        print(f"  Error reading code: {e}")

    # === 4. Try VScript with proper Dota API ===
    print("\n" + "=" * 60)
    print("PHASE 4: VScript — proper Dota 2 API calls")
    print("=" * 60)

    # Dota 2 VScript API for bots:
    # CDOTABaseGameMode:SetBotThinkingEnabled(bool)
    # CDOTAGamerules:BotPopulate()
    # CDOTAGamerules:AddBotPlayerWithEntityScript(heroName, team, heroScript, abilityScript)

    vscript_cmds = [
        # SetBotThinkingEnabled
        'script GameRules:GetGameModeEntity():SetBotThinkingEnabled(true)',
        # BotPopulate (the VScript equivalent of dota_bot_populate)
        'script GameRules:BotPopulate()',
        # AddBot with proper params (team 3 = Dire)
        'script Think(function() GameRules:AddBotPlayerWithEntityScript("npc_dota_hero_sven", 3, "", "") end, "add_bot", 0.1)',
    ]

    for c in vscript_cmds:
        print(f"  [>] {c}")
        cmd.execute(c)
        time.sleep(2)

    # Check results
    time.sleep(5)
    print("\n  Controllers after VScript:")
    game.init()
    for entity, ident, name in game.iter_entities():
        if name == "dota_player_controller":
            slot = mem.read_i32(entity + 0x908)
            pname = ""
            try:
                pname = mem.read_string(entity + 0x6E0, 32)
            except:
                pass
            print(f"    slot={slot:3d} name='{pname}'")

    # Count heroes
    from cheat.offsets import BaseNPC
    hero_count = 0
    for entity, ident, name in game.iter_entities():
        try:
            hp = mem.read_i32(entity + 0x354)
            team = mem.read_u8(entity + 0x3F3)
            if team not in (2, 3) or hp <= 0 or hp > 10000:
                continue
            uname_ptr = mem.read_ptr(entity + BaseNPC.UNIT_NAME)
            if uname_ptr and uname_ptr > 0x10000:
                uname = mem.read_string(uname_ptr, 64)
                if "hero" in uname:
                    hero_count += 1
                    t = {2: "Rad", 3: "Dire"}.get(team, "?")
                    print(f"    Hero: {uname:40s} {t} hp={hp}")
        except:
            continue
    print(f"  Total heroes: {hero_count}")

    print("\nDone!")


if __name__ == "__main__":
    main()
