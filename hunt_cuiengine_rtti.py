"""
Identify CUIEngine singleton among .data candidates via MSVC RTTI.

MSVC vtable layout:
  vtbl[-1] = ptr to RTTICompleteObjectLocator (in .rdata)
  Locator @ +0x14 (RVA from imagebase, 32-bit) -> RTTITypeDescriptor
  TypeDescriptor: vtable_of_type_info_ptr (8) | spare(8) | name_zstring(...)

We scan candidates, read vtable[-1], deref locator, walk to TypeDescriptor, read name.
Names typically start with ".?AV" (class) or ".?AU" (struct). For "CUIEngine"
we expect ".?AVCUIEngine@@" or similar.
"""
import sys, os, struct
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from cheat.memory import DotaMemory

m = DotaMemory()
pano_base = m.module_base("panorama.dll")
pano_size = m.module_size("panorama.dll")

# section bounds (from previous run)
text_lo = pano_base + 0x1000
text_hi = pano_base + 0x1000 + 0x3CAC54
rdata_lo = pano_base + 0x3CC000
rdata_hi = pano_base + 0x3CC000 + 0xED142
data_lo  = pano_base + 0x4BA000
data_hi  = pano_base + 0x4BA000 + 0xB06AC

def read_rtti_name(vtbl: int) -> str | None:
    try:
        col_ptr = m.read_u64(vtbl - 8)
        if not (rdata_lo <= col_ptr < rdata_hi):
            return None
        # COL: signature(4) offset(4) cdOffset(4) pTypeDescriptor(rva32) pClassDescriptor(rva32) pSelf(rva32)
        td_rva = m.read_u32(col_ptr + 0x0C)
        td = pano_base + td_rva
        # TypeDescriptor: vptr(8) spare(8) name (zstring)
        name = m.read_string(td + 0x10, 200)
        return name
    except Exception:
        return None

data_bytes = m.pm.read_bytes(data_lo, data_hi - data_lo)

# enumerate qwords, dereference, classify
hits = []
seen_vtables = {}
for off in range(0, len(data_bytes) - 8, 8):
    qw = struct.unpack_from("<Q", data_bytes, off)[0]
    if qw < 0x10000 or qw > 0x7FFFFFFFFFFF:
        continue
    try:
        vt = m.read_u64(qw)
    except Exception:
        continue
    if not (rdata_lo <= vt < rdata_hi):
        continue
    if vt in seen_vtables:
        name = seen_vtables[vt]
    else:
        name = read_rtti_name(vt)
        seen_vtables[vt] = name
    slot_rva = 0x4BA000 + off
    hits.append((slot_rva, qw, vt, name))

print(f"[+] total .data singleton candidates with vtable RTTI: {sum(1 for h in hits if h[3])}")
print(f"[+] unique vtables: {len(seen_vtables)}")

# Print all with names containing UIEngine / Engine
print("\n=== Candidates with 'Engine' / 'UI' / 'Panel' in RTTI name ===")
for slot_rva, ptr, vt, name in hits:
    if not name:
        continue
    if any(k in name for k in ("Engine", "UIEngine", "UIPanel", "Panel2D", "UIPanorama", "Panorama")):
        print(f"  slot RVA=0x{slot_rva:08X}  *slot=0x{ptr:X}  vt RVA=0x{vt-pano_base:X}  name={name}")

# Print ALL named candidates (limit)
print("\n=== ALL named candidates (top 60) ===")
shown = 0
for slot_rva, ptr, vt, name in hits:
    if not name:
        continue
    print(f"  RVA=0x{slot_rva:08X}  *=0x{ptr:X}  vt=0x{vt-pano_base:X}  {name}")
    shown += 1
    if shown >= 60:
        break
