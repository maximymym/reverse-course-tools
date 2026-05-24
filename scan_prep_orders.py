"""Scan client.dll for PrepareUnitOrders signature from CFunctionList.hpp."""
from pathlib import Path

# 19-byte pattern from CFunctionList.hpp:29
# "4C 89 4C 24 20 44 89 44 24 18 89 54 24 10 48 89 4C 24 08"
SIG = bytes([
    0x4C, 0x89, 0x4C, 0x24, 0x20,
    0x44, 0x89, 0x44, 0x24, 0x18,
    0x89, 0x54, 0x24, 0x10,
    0x48, 0x89, 0x4C, 0x24, 0x08,
])

def section_text(data: bytes):
    e_lfanew = int.from_bytes(data[0x3C:0x40], "little")
    nt = data[e_lfanew:]
    num_sections = int.from_bytes(nt[6:8], "little")
    size_opt = int.from_bytes(nt[0x14:0x16], "little")
    sec_off = e_lfanew + 0x18 + size_opt
    for _ in range(num_sections):
        s = data[sec_off:sec_off + 0x28]
        name = s[0:8].split(b"\x00", 1)[0].decode("ascii", errors="ignore")
        va = int.from_bytes(s[0x0C:0x10], "little")
        vs = int.from_bytes(s[0x08:0x0C], "little")
        ro = int.from_bytes(s[0x14:0x18], "little")
        rs = int.from_bytes(s[0x10:0x14], "little")
        if name == ".text":
            return va, vs, ro, rs
        sec_off += 0x28
    return None

p = Path("D:/SteamLibrary/steamapps/common/dota 2 beta/game/dota/bin/win64/client.dll")
data = p.read_bytes()
print(f"[+] client.dll size: {len(data)} bytes")

va, vs, ro, rs = section_text(data)
print(f"[+] .text VA=0x{va:X} VSize=0x{vs:X} RawOff=0x{ro:X}")

text = data[ro:ro + rs]
matches = []
start = 0
while True:
    idx = text.find(SIG, start)
    if idx < 0:
        break
    matches.append(va + idx)
    start = idx + 1
print(f"[+] PrepareUnitOrders pattern matches: {len(matches)}")
for m in matches[:20]:
    print(f"    RVA: 0x{m:X}")
if len(matches) > 20:
    print(f"    ... and {len(matches)-20} more")
