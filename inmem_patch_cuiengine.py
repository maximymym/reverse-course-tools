"""
In-memory patch: copy CUIEngine singleton ptr from new slot (0x569C98) into
the legacy slot (0x569C78) the DLL hardcodes. Verifies AFK root-cause.

Does NOT survive Dota restart — for diagnosis only. Permanent fix is DLL rebuild.
"""
import sys, os, struct, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from cheat.memory import DotaMemory

OLD_SLOT_RVA = 0x569C78
NEW_SLOT_RVA = 0x569C98

m = DotaMemory()
pano = m.module_base("panorama.dll")
print(f"[+] panorama.dll base = 0x{pano:X}")

old_addr = pano + OLD_SLOT_RVA
new_addr = pano + NEW_SLOT_RVA

before_old = m.read_u64(old_addr)
singleton  = m.read_u64(new_addr)
print(f"[+] before: *0x{old_addr:X} (old) = 0x{before_old:X}")
print(f"[+] source: *0x{new_addr:X} (new) = 0x{singleton:X}")

if singleton == 0:
    print("[!] singleton ptr is NULL at new slot — abort"); sys.exit(1)
if before_old == singleton:
    print("[~] already patched; nothing to do")
    sys.exit(0)

# verify singleton vtable lies inside panorama .rdata
vt = m.read_u64(singleton)
pano_size = m.module_size("panorama.dll")
if not (pano <= vt < pano + pano_size):
    print(f"[!] singleton vtable 0x{vt:X} outside panorama range — refuse to patch")
    sys.exit(1)
print(f"[+] singleton->vtable = 0x{vt:X}  (inside panorama: OK)")

# write
m.pm.write_bytes(old_addr, struct.pack("<Q", singleton), 8)
time.sleep(0.05)
after = m.read_u64(old_addr)
print(f"[+] after:  *0x{old_addr:X} (old) = 0x{after:X}")

if after == singleton:
    print("\n>>> PATCH APPLIED. Watch the bots in Dota — they should start moving within ~1 sec. <<<")
else:
    print("[!] write verify failed")
