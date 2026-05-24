"""
Deep bot AI research — search server.dll strings, find bot init code.
"""
import sys, os, time, struct
sys.path.insert(0, os.path.dirname(__file__))

from cheat.memory import DotaMemory
from cheat.commands import DotaCommands, ConVarSystem
from cheat.game_state import DotaGame


def search_module_strings(mem, module_name, keywords, context_bytes=64):
    """Search for ASCII strings in a module's memory."""
    base = mem.module_base(module_name)
    size = mem.module_size(module_name)
    print(f"\n[+] Searching {module_name}: base={hex(base)}, size={size/1024/1024:.1f}MB")

    results = {}
    # Read in chunks
    chunk_size = 4 * 1024 * 1024  # 4MB
    for offset in range(0, size, chunk_size):
        read_size = min(chunk_size, size - offset)
        try:
            data = mem.pm.read_bytes(base + offset, read_size)
        except:
            continue

        for kw in keywords:
            kw_bytes = kw.encode('ascii')
            pos = 0
            while True:
                pos = data.find(kw_bytes, pos)
                if pos == -1:
                    break
                rva = offset + pos
                # Extract surrounding context
                ctx_start = max(0, pos - 16)
                ctx_end = min(len(data), pos + len(kw_bytes) + context_bytes)
                ctx = data[ctx_start:ctx_end]
                # Try to read as null-terminated string
                str_start = pos
                str_end = data.find(b'\x00', pos)
                if str_end == -1:
                    str_end = min(pos + 128, len(data))
                full_str = data[str_start:str_end].decode('ascii', errors='replace')

                if kw not in results:
                    results[kw] = []
                results[kw].append({
                    "rva": rva,
                    "addr": hex(base + rva),
                    "string": full_str[:120],
                })
                pos += len(kw_bytes)

    return results


def main():
    mem = DotaMemory()
    print(f"[+] PID {mem.pid}")

    # Check which modules are loaded
    print("\n=== LOADED MODULES (bot-relevant) ===")
    bot_modules = []
    for name, info in mem.modules.items():
        if any(kw in name.lower() for kw in ["server", "client", "engine", "tier0", "vscript"]):
            print(f"  {name:30s} base={hex(info['base'])} size={info['size']/1024/1024:.1f}MB")
            bot_modules.append(name)

    # Search for bot-related strings in server.dll
    keywords = [
        "CDOTABot",
        "BotThink",
        "m_bIsBot",
        "m_bFakeClient",
        "BotController",
        "SetActiveBehavior",
        "bot_controller",
        "CDOTABotController",
        "BotManager",
        "CreateBot",
        "AddBot",
        "PopulateBot",
        "dota_bot_populate",
        "dota_bot_takeover",
        "bot_takeover",
        "BotTakeover",
        "AssignBot",
        "bot_practice",
        "npc_dota_hero",
        "Think",
        "SetBotController",
    ]

    # Search server.dll first (most likely location for bot AI)
    if "server.dll" in mem.modules:
        results = search_module_strings(mem, "server.dll", keywords)
        print(f"\n=== SERVER.DLL STRING MATCHES ===")
        for kw, matches in sorted(results.items()):
            print(f"\n  '{kw}' ({len(matches)} matches):")
            for m in matches[:10]:  # limit output
                print(f"    RVA +0x{m['rva']:08X} ({m['addr']}): {m['string']}")
    else:
        print("\n[!] server.dll NOT loaded!")
        print("    Available modules:")
        for name in sorted(mem.modules.keys()):
            if "server" in name.lower() or "vscript" in name.lower():
                print(f"      {name}")

    # Also search client.dll for bot strings (client may have some bot code)
    if "client.dll" in mem.modules:
        client_keywords = [
            "CDOTABot",
            "m_bIsBot",
            "dota_bot_populate",
            "dota_bot_takeover",
            "BotController",
            "CreateBot",
            "PopulateBot",
            "bot_practice",
        ]
        results = search_module_strings(mem, "client.dll", client_keywords)
        print(f"\n=== CLIENT.DLL STRING MATCHES ===")
        for kw, matches in sorted(results.items()):
            print(f"\n  '{kw}' ({len(matches)} matches):")
            for m in matches[:5]:
                print(f"    RVA +0x{m['rva']:08X} ({m['addr']}): {m['string']}")

    # Search for VScript-related strings
    print("\n=== VSCRIPT SEARCH ===")
    vscript_kw = [
        "vscripts/bots",
        "bot_generic",
        "BotThink",
        "ability_item_usage",
        "item_purchase",
        "script_reload",
        "RunScript",
    ]
    for mod in ["server.dll", "client.dll"]:
        if mod in mem.modules:
            results = search_module_strings(mem, mod, vscript_kw)
            if any(results.values()):
                print(f"\n  {mod}:")
                for kw, matches in sorted(results.items()):
                    if matches:
                        print(f"    '{kw}' ({len(matches)} matches):")
                        for m in matches[:5]:
                            print(f"      RVA +0x{m['rva']:08X}: {m['string']}")

    # Test VScript execution commands
    print("\n=== TESTING VSCRIPT COMMANDS ===")
    game = DotaGame(mem)
    game.init()
    cmd = DotaCommands(mem)
    cmd.init(game)

    # Try script commands
    test_cmds = [
        'script print("hello from vscript")',
        'script_reload_code bots/bot_generic',
        'script GameRules:GetGameModeEntity():SetBotThinkingEnabled(true)',
        'script PlayerResource:SetHaveBot(0, true)',
    ]
    for c in test_cmds:
        print(f"  [>] {c}")
        cmd.execute(c)
        time.sleep(1)

    print("\nDone!")


if __name__ == "__main__":
    main()
