"""
Research: Bot AI Takeover — scan ConVars for bot-related, test commands.

Run with one Dota 2 instance attached and IN A LOCAL LOBBY WITH BOTS.
"""
import sys, os, time, struct
sys.path.insert(0, os.path.dirname(__file__))

from cheat.memory import DotaMemory
from cheat.commands import DotaCommands, ConVarSystem
from cheat.game_state import DotaGame

def scan_convars_by_keyword(convar: ConVarSystem, keyword: str):
    """Scan all ConVars, return those matching keyword in name."""
    results = []
    batch = min(convar.arr3_count, 8000)
    data = convar.mem.pm.read_bytes(convar.arr3_ptr, batch * 0x10)

    for i in range(batch):
        entry_ptr = struct.unpack("<Q", data[i*0x10:i*0x10+8])[0]
        if not entry_ptr or entry_ptr < 0x10000:
            continue
        try:
            name_ptr = convar.mem.read_ptr(entry_ptr + ConVarSystem.CV_NAME)
            if not name_ptr or name_ptr < 0x10000:
                continue
            name = convar.mem.read_string(name_ptr, 128)
            if keyword.lower() in name.lower():
                # Read value
                try:
                    val_i = convar.mem.read_i32(entry_ptr + ConVarSystem.CV_VALUE)
                    val_f = convar.mem.read_f32(entry_ptr + ConVarSystem.CV_VALUE)
                except:
                    val_i, val_f = 0, 0.0
                # Read flags
                try:
                    flags = convar.mem.read_u32(entry_ptr + ConVarSystem.CV_FLAGS)
                except:
                    flags = 0
                # Read description
                desc = ""
                try:
                    desc_ptr = convar.mem.read_ptr(entry_ptr + ConVarSystem.CV_DESC)
                    if desc_ptr and desc_ptr > 0x10000:
                        desc = convar.mem.read_string(desc_ptr, 256)
                except:
                    pass
                results.append({
                    "name": name,
                    "value_int": val_i,
                    "value_float": val_f,
                    "flags": flags,
                    "description": desc,
                    "entry_ptr": hex(entry_ptr),
                })
        except:
            continue
    return results


def main():
    print("=" * 60)
    print("Bot AI Takeover Research")
    print("=" * 60)

    # Attach
    mem = DotaMemory()
    print(f"[+] Attached to dota2.exe PID {mem.pid}")

    game = DotaGame(mem)
    game.init()

    cmd = DotaCommands(mem)
    cmd.init(game)

    # 1. Scan all ConVars with "bot" in name
    print("\n" + "=" * 60)
    print("PHASE 1: ConVar scan — keyword 'bot'")
    print("=" * 60)

    bot_cvars = scan_convars_by_keyword(cmd.convar, "bot")
    print(f"\nFound {len(bot_cvars)} ConVars with 'bot':\n")
    for cv in sorted(bot_cvars, key=lambda x: x["name"]):
        cheat_flag = " [CHEAT]" if cv["flags"] & 0x4000 else ""
        sv_flag = " [SV]" if cv["flags"] & 0x2 else ""
        print(f"  {cv['name']:50s} = {cv['value_int']:8d}  flags=0x{cv['flags']:08X}{cheat_flag}{sv_flag}")
        if cv["description"]:
            print(f"    desc: {cv['description'][:120]}")

    # 2. Scan for "takeover" / "ai_" / "script" related
    print("\n" + "=" * 60)
    print("PHASE 2: ConVar scan — keywords 'takeover', 'ai_', 'fake'")
    print("=" * 60)

    for kw in ["takeover", "ai_", "fake_client", "fake_player", "puppet"]:
        results = scan_convars_by_keyword(cmd.convar, kw)
        if results:
            print(f"\n  Keyword '{kw}' — {len(results)} matches:")
            for cv in results:
                cheat_flag = " [CHEAT]" if cv["flags"] & 0x4000 else ""
                print(f"    {cv['name']:50s} = {cv['value_int']:8d}  flags=0x{cv['flags']:08X}{cheat_flag}")
                if cv["description"]:
                    print(f"      desc: {cv['description'][:120]}")
        else:
            print(f"\n  Keyword '{kw}' — no matches")

    # 3. Scan for "sv_cheats", "developer", "sv_lan" to understand server authority
    print("\n" + "=" * 60)
    print("PHASE 3: Server authority ConVars")
    print("=" * 60)

    for name in ["sv_cheats", "developer", "sv_lan", "host_force_frametime_to_equal_tick_interval",
                  "dota_force_gamemode", "dota_auto_hero_selection"]:
        val = cmd.convar.get_int(name)
        if val is not None:
            print(f"  {name:50s} = {val}")
        else:
            print(f"  {name:50s} = NOT FOUND")

    # 4. Search entities for bot-related designerNames
    print("\n" + "=" * 60)
    print("PHASE 4: Entity scan — looking for bot/controller entities")
    print("=" * 60)

    bot_keywords = ["bot", "controller", "ai", "npc"]
    found_types = {}
    count = 0
    for entity, ident, name in game.iter_entities():
        count += 1
        if name not in found_types:
            found_types[name] = 0
        found_types[name] += 1

    print(f"\nTotal entities: {count}")
    print(f"Unique types: {len(found_types)}")

    # Print all entity types
    print("\nAll entity types (with count):")
    for name, cnt in sorted(found_types.items(), key=lambda x: -x[1]):
        marker = ""
        name_lower = name.lower() if name else ""
        if any(kw in name_lower for kw in ["bot", "controller", "ai", "player"]):
            marker = " <<<<<"
        print(f"  {cnt:4d}x {name}{marker}")

    # 5. Check specific interesting fields on player controllers
    print("\n" + "=" * 60)
    print("PHASE 5: PlayerController field analysis")
    print("=" * 60)

    for entity, ident, name in game.iter_entities():
        if name == "dota_player_controller":
            slot = mem.read_i32(entity + 0x908)
            player_name = ""
            try:
                player_name = mem.read_string(entity + 0x6E0, 64)
            except:
                pass
            print(f"\n  Controller {hex(entity)}: slot={slot}, name='{player_name}'")

            # Dump nearby fields looking for bot flags
            # Try reading m_bIsBot-like fields around common offsets
            for off_name, off in [
                ("m_steamID", 0x708),
                ("m_hPawn", 0x810),
                ("m_bIsLocalPlayerController", 0x878),
                ("field_0x880", 0x880),
                ("field_0x888", 0x888),
                ("field_0x890", 0x890),
                ("field_0x6D0", 0x6D0),
                ("field_0x6D8", 0x6D8),
                ("field_0x6E8", 0x6E8),
            ]:
                try:
                    val = mem.read_u64(entity + off)
                    val32 = val & 0xFFFFFFFF
                    val8 = val & 0xFF
                    print(f"    +0x{off:03X} ({off_name:35s}): u64=0x{val:016X}  u32={val32}  u8={val8}")
                except:
                    print(f"    +0x{off:03X} ({off_name:35s}): READ ERROR")

    print("\n" + "=" * 60)
    print("DONE — review output above")
    print("=" * 60)


if __name__ == "__main__":
    main()
