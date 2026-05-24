"""
Phase 2 Research — Find ISteamGameCoordinator for GC message sending.

Goal: Find the GC interface so we can call SendMessage(msgType, data, size)
to start matchmaking (7033), accept match (7070), etc.

Approaches:
1. steam_api64.dll exports SteamGameCoordinator() → ISteamGameCoordinator*
2. Pattern scan for "SteamGameCoordinator001" string → GetISteamGenericInterface call
3. Find the GC pointer stored in client.dll (the internal cheat uses GC.SendMessage)
"""
import sys, os, struct
sys.path.insert(0, os.path.dirname(__file__))

from cheat.memory import DotaMemory

mem = DotaMemory()

# === 1. Check steam_api64.dll exports ===
print("=== steam_api64.dll ===")
try:
    base = mem.module_base("steam_api64.dll")
    size = mem.module_size("steam_api64.dll")
    print(f"  Base: {hex(base)}, Size: {size}")
except:
    print("  NOT FOUND")
    base = 0

# === 2. Check steamclient64.dll ===
print("\n=== steamclient64.dll ===")
try:
    sc_base = mem.module_base("steamclient64.dll")
    sc_size = mem.module_size("steamclient64.dll")
    print(f"  Base: {hex(sc_base)}, Size: {sc_size // (1024*1024)}MB")
except:
    print("  NOT FOUND")
    sc_base = 0

# === 3. Search for "SteamGameCoordinator001" string ===
print("\n=== String: SteamGameCoordinator001 ===")
gc_str = "53 74 65 61 6D 47 61 6D 65 43 6F 6F 72 64 69 6E 61 74 6F 72 30 30 31"  # "SteamGameCoordinator001"

for mod_name in ["steam_api64.dll", "steamclient64.dll", "client.dll"]:
    try:
        addr = mem.pattern_scan(mod_name, gc_str)
        if addr:
            mod_base = mem.module_base(mod_name)
            rva = addr - mod_base
            print(f"  [+] Found in {mod_name} at {hex(addr)} (RVA +{hex(rva)})")
            # Read context
            ctx = mem.read_string(addr, 64)
            print(f"      String: {repr(ctx)}")
    except Exception as e:
        print(f"  [-] {mod_name}: {e}")

# === 4. Find SteamGameCoordinator export in steam_api64.dll ===
print("\n=== steam_api64.dll exports ===")
if base:
    # Search for known export names
    for export_name in ["SteamGameCoordinator", "SteamClient", "SteamAPI_ISteamClient_GetISteamGenericInterface"]:
        sig = " ".join(f"{b:02X}" for b in export_name.encode("ascii"))
        addr = mem.pattern_scan("steam_api64.dll", sig)
        if addr:
            rva = addr - base
            print(f"  [+] '{export_name}' at RVA +{hex(rva)}")

# === 5. Try to find GC interface pointer in client.dll ===
print("\n=== GC Interface in client.dll ===")
client_base = mem.module_base("client.dll")

# The internal cheat uses "GC" global object. Search for related patterns.
# In Source 2, the GC singleton is typically accessed via a global pointer.
# Look for xrefs to "SteamGameCoordinator001" string
for mod_name in ["client.dll"]:
    try:
        mod_base = mem.module_base(mod_name)
        # Search for the string first
        str_addr = mem.pattern_scan(mod_name, gc_str)
        if str_addr:
            # Now search for references to this string address (LEA instruction)
            # LEA reg, [rip+disp32] where disp32 points to str_addr
            # This is hard to do with simple AOB scan, but let's try
            print(f"  String at {hex(str_addr)}")

            # Try reading the area around the string for more context
            for off in range(-0x100, 0x100, 8):
                try:
                    val = mem.read_u64(str_addr + off)
                    if val == str_addr:
                        print(f"  [+] Pointer to string at {hex(str_addr + off)}")
                except:
                    pass
    except Exception as e:
        print(f"  [-] {mod_name}: {e}")

# === 6. Search for GC-related patterns in tier0/engine2 ===
print("\n=== ISteamGameCoordinator interface walking ===")
# Try using the same CreateInterface pattern used for other interfaces
# but in steamclient64.dll
if sc_base:
    ci_pattern = "4C 8B ?? ?? ?? ?? ?? 4C 8B ?? 4C 8B ?? 4D 85 ?? 74 ?? 49 8B ?? ?? 4D 8B"
    ci_addr = mem.pattern_scan("steamclient64.dll", ci_pattern)
    if ci_addr:
        print(f"  [+] CreateInterface in steamclient64.dll at {hex(ci_addr)}")
    else:
        print(f"  [-] CreateInterface pattern NOT FOUND in steamclient64.dll")

# === 7. Look for SteamAPI_GetISteamGenericInterface or SteamInternal_* ===
print("\n=== SteamInternal patterns ===")
for s in ["SteamInternal_GameServer", "SteamInternal_CreateInterface", "SteamInternal_FindOrCreate"]:
    sig = " ".join(f"{b:02X}" for b in s.encode("ascii"))
    for mod in ["steam_api64.dll"]:
        try:
            addr = mem.pattern_scan(mod, sig)
            if addr:
                rva = addr - mem.module_base(mod)
                print(f"  [+] '{s}' in {mod} at RVA +{hex(rva)}")
        except:
            pass

# === 8. List all modules with "steam" in name ===
print("\n=== Steam modules ===")
for name, info in mem.modules.items():
    if "steam" in name.lower():
        print(f"  {info['name']}: base={hex(info['base'])}, size={info['size']}")

print("\n[*] Done")
mem.close()
