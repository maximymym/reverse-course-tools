"""
Hunt CUIEngine singleton in running panorama.dll.

Strategy: enumerate aligned qwords in panorama.dll .data section.
For each, dereference and check if it points to an object whose
vtable lies inside panorama.dll .rdata — that's a singleton candidate.

Then identify CUIEngine by walking RIP-relative refs FROM RunScript caller:
RunScript prologue is at panorama+0xA6C20 (we scanned). Its callers load
CUIEngine* into rcx via `mov rcx, [rip+disp32]` shortly before the call.
"""
import sys, os, struct
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from cheat.memory import DotaMemory
from pathlib import Path

m = DotaMemory()
pano_base = m.module_base("panorama.dll")
pano_size = m.module_size("panorama.dll")
print(f"[+] panorama.dll  base=0x{pano_base:X}  size=0x{pano_size:X}")

# Get section table from on-disk copy (sections layout same as in-memory)
disk = Path("D:/SteamLibrary/steamapps/common/dota 2 beta/game/bin/win64/panorama.dll").read_bytes()
e_lfanew = struct.unpack_from("<I", disk, 0x3C)[0]
num_sections = struct.unpack_from("<H", disk, e_lfanew + 6)[0]
size_opt = struct.unpack_from("<H", disk, e_lfanew + 0x14)[0]
sec_off = e_lfanew + 0x18 + size_opt
sections = {}
for _ in range(num_sections):
    s = disk[sec_off:sec_off + 0x28]
    name = s[0:8].split(b"\x00", 1)[0].decode("ascii", errors="ignore")
    vs   = struct.unpack_from("<I", s, 0x08)[0]
    va   = struct.unpack_from("<I", s, 0x0C)[0]
    sections[name] = (va, vs)
    sec_off += 0x28
for n, (va, vs) in sections.items():
    print(f"    {n:<10} VA=0x{va:08X} size=0x{vs:X}  bounds [0x{pano_base+va:X} .. 0x{pano_base+va+vs:X})")

rdata_va, rdata_size = sections[".rdata"]
data_va,  data_size  = sections[".data"]
text_va,  text_size  = sections[".text"]

rdata_lo = pano_base + rdata_va
rdata_hi = rdata_lo + rdata_size
text_lo  = pano_base + text_va
text_hi  = text_lo + text_size
data_lo  = pano_base + data_va
data_hi  = data_lo + data_size

# Read .data into buffer
data_bytes = m.pm.read_bytes(data_lo, data_size)

# Approach 1: find aligned qwords pointing to objects with vtable in .rdata of panorama.dll
candidates = []
for off in range(0, len(data_bytes) - 8, 8):
    qw = struct.unpack_from("<Q", data_bytes, off)[0]
    if qw == 0:
        continue
    # heuristic: heap-allocated objects on Win10/11 land in 0x000001xx... range or 0x000002xx
    if qw < 0x10000 or qw > 0x7FFFFFFFFFFF:
        continue
    try:
        vt = m.read_u64(qw)
    except Exception:
        continue
    if rdata_lo <= vt < rdata_hi:
        # vtable in panorama .rdata — likely a panorama singleton
        slot_rva = data_va + off
        candidates.append((slot_rva, qw, vt))

print(f"\n[+] singleton candidates in .data: {len(candidates)}")
for slot_rva, ptr, vt in candidates[:40]:
    vt_rva = vt - pano_base
    print(f"    slot RVA=0x{slot_rva:08X}  *slot=0x{ptr:X}  vtable RVA=0x{vt_rva:X}")
if len(candidates) > 40:
    print(f"    ... and {len(candidates)-40} more")

# Approach 2: find callers of RunScript and trace which slot loads into rcx
RUN_SCRIPT_RVA = 0xA6C20
run_script = pano_base + RUN_SCRIPT_RVA
print(f"\n[+] RunScript abs=0x{run_script:X}")

# Scan .text for call instructions targeting run_script (E8 disp32)
text_bytes = m.pm.read_bytes(text_lo, text_size)
callers = []
for off in range(len(text_bytes) - 5):
    if text_bytes[off] != 0xE8:
        continue
    disp = struct.unpack_from("<i", text_bytes, off + 1)[0]
    tgt = text_lo + off + 5 + disp
    if tgt == run_script:
        callers.append(text_lo + off)
print(f"[+] direct callers of RunScript: {len(callers)}")
for c in callers[:10]:
    print(f"    call site abs=0x{c:X} (RVA 0x{c - pano_base:X})")

# For first caller, look backward up to 64 bytes for `48 8B 0D xx xx xx xx` (mov rcx, [rip+disp32])
if callers:
    c = callers[0]
    window = m.pm.read_bytes(c - 64, 64)
    found_slot = None
    for i in range(len(window) - 7):
        # mov rcx, [rip+disp32] = 48 8B 0D ?? ?? ?? ??
        if window[i] == 0x48 and window[i+1] == 0x8B and window[i+2] == 0x0D:
            disp = struct.unpack_from("<i", window, i + 3)[0]
            instr_abs = (c - 64) + i
            slot_abs  = instr_abs + 7 + disp
            slot_rva  = slot_abs - pano_base
            print(f"\n[+] mov rcx, [rip+disp] at 0x{instr_abs:X}  →  slot 0x{slot_abs:X}  (RVA 0x{slot_rva:X})")
            try:
                val = m.read_u64(slot_abs)
                print(f"    *slot = 0x{val:X}")
                if val != 0:
                    vt = m.read_u64(val)
                    print(f"    (*slot)->vtable = 0x{vt:X}  (RVA 0x{vt-pano_base:X})")
                found_slot = slot_rva
            except Exception as e:
                print(f"    read failed: {e}")
            break
    if found_slot is None:
        print("[!] no `mov rcx, [rip+]` found in 64 bytes preceding the call — search wider")
