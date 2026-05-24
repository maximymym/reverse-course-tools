"""
Phase 1.5 Research A — Find PrepareUnitOrders + Player Controller in client.dll
"""
import sys, os
sys.path.insert(0, os.path.dirname(__file__))

from cheat.memory import DotaMemory
from cheat.game_state import DotaGame

mem = DotaMemory()
game = DotaGame(mem)
game.init()

client_base = mem.module_base("client.dll")
client_size = mem.module_size("client.dll")
print(f"\n[*] client.dll: {hex(client_base)}, size={client_size // (1024*1024)}MB")

# === 1. AOB Scan for PrepareUnitOrders ===
print("\n=== PrepareUnitOrders AOB Scan ===")

# Short signature (soyware/heck_dota, 19 bytes)
sig_short = "4C 89 4C 24 20 44 89 44 24 18 89 54 24 10 48 89 4C 24 08"
# Extended signature (ExistedGit/Dota2Cheat, 26 bytes)
sig_long = "4C 89 4C 24 20 44 89 44 24 18 89 54 24 10 55 53 57 41 55 41 57 48 8D 6C 24 C0"

for name, sig in [("short(19B)", sig_short), ("long(26B)", sig_long)]:
    addr = mem.pattern_scan("client.dll", sig)
    if addr:
        rva = addr - client_base
        print(f"  [+] {name}: {hex(addr)} (RVA +{hex(rva)})")
    else:
        print(f"  [-] {name}: NOT FOUND")

# Also try string xref approach
print("\n=== String Search: waypoint_flag.vpcf ===")
# Search for the string that references PrepareUnitOrders
waypoint_sig = "70 61 72 74 69 63 6C 65 73 2F 75 69 5F 6D 6F 75 73 65"  # "particles/ui_mouse"
addr = mem.pattern_scan("client.dll", waypoint_sig)
if addr:
    rva = addr - client_base
    print(f"  [+] Found at {hex(addr)} (RVA +{hex(rva)})")
else:
    print(f"  [-] NOT FOUND")

# Try "courier_go_to_secretshop"
courier_sig = "63 6F 75 72 69 65 72 5F 67 6F 5F 74 6F 5F 73 65 63 72 65 74"  # "courier_go_to_secret"
addr = mem.pattern_scan("client.dll", courier_sig)
if addr:
    rva = addr - client_base
    print(f"  [+] courier_go_to_secretshop at {hex(addr)} (RVA +{hex(rva)})")
else:
    print(f"  [-] courier_go_to_secretshop NOT FOUND")

# === 2. Find Player Controller entities ===
print("\n=== Player Controller Search ===")
player_entities = []
for entity, ident, name in game.iter_entities():
    if "player" in name.lower() or "controller" in name.lower():
        player_entities.append((entity, ident, name))
        print(f"  Entity: {name} @ {hex(entity)} (ident={hex(ident)})")

# Also look for CDOTAPlayerController by designer name
for dn in ["dota_player_controller", "player_controller", "dota_player"]:
    ent = game._find_entity_by_designer_name(dn)
    if ent:
        print(f"  [+] '{dn}' entity: {hex(ent)}")

# === 3. Dump all entity types with "player" ===
print("\n=== All Player-like Entities ===")
player_types = set()
for entity, ident, name in game.iter_entities():
    if "player" in name.lower():
        player_types.add(name)
for n in sorted(player_types):
    print(f"  {n}")

# === 4. Check Panorama-related strings ===
print("\n=== Panorama JS Registration Strings ===")
for s in ["PrepareUnitOrders", "GameUI", "Game.Prepare"]:
    sig_bytes = " ".join(f"{b:02X}" for b in s.encode("ascii"))
    addr = mem.pattern_scan("client.dll", sig_bytes)
    if addr:
        rva = addr - client_base
        print(f"  [+] '{s}' at {hex(addr)} (RVA +{hex(rva)})")
        # Read more context
        ctx = mem.read_string(addr, 128)
        print(f"      Context: {repr(ctx[:80])}")
    else:
        print(f"  [-] '{s}' NOT FOUND in client.dll")

# Try panorama_client.dll too
try:
    pan_base = mem.module_base("panorama_client.dll")
    print(f"\n  panorama_client.dll: {hex(pan_base)}")
    for s in ["PrepareUnitOrders", "Game.PrepareUnitOrders"]:
        sig_bytes = " ".join(f"{b:02X}" for b in s.encode("ascii"))
        addr = mem.pattern_scan("panorama_client.dll", sig_bytes)
        if addr:
            rva = addr - pan_base
            print(f"  [+] '{s}' in panorama_client.dll at {hex(addr)} (RVA +{hex(rva)})")
except:
    print("  panorama_client.dll not found")

print("\n[*] Done")
mem.close()
