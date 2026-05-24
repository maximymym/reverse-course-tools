"""Scan panorama.dll on disk for RunScript prologue signature."""
import sys
from pathlib import Path

# 33-byte prologue from CPanoramaJS.cpp:38-45
SIG = bytes([
    0x48, 0x89, 0x5C, 0x24, 0x18,
    0x4C, 0x89, 0x4C, 0x24, 0x20,
    0x48, 0x89, 0x54, 0x24, 0x10,
    0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57,
    0x48, 0x8D, 0x6C, 0x24, 0x80,
    0x48,
])

CUIENGINE_PTR_RVA = 0x569C78
RUNSCRIPT_FALLBACK_RVA = 0xA6A40

def find_section_text(data: bytes):
    # locate .text section via PE headers
    e_lfanew = int.from_bytes(data[0x3C:0x40], "little")
    nt = data[e_lfanew:]
    # nt->FileHeader at +4, SizeOfOptionalHeader at +0x14, NumberOfSections at +0x06
    num_sections = int.from_bytes(nt[6:8], "little")
    size_opt = int.from_bytes(nt[0x14:0x16], "little")
    sec_off = e_lfanew + 0x18 + size_opt
    for i in range(num_sections):
        s = data[sec_off:sec_off + 0x28]
        name = s[0:8].split(b"\x00", 1)[0].decode("ascii", errors="ignore")
        virt_size = int.from_bytes(s[0x08:0x0C], "little")
        virt_addr = int.from_bytes(s[0x0C:0x10], "little")
        raw_size = int.from_bytes(s[0x10:0x14], "little")
        raw_off  = int.from_bytes(s[0x14:0x18], "little")
        if name == ".text":
            return virt_addr, virt_size, raw_off, raw_size
        sec_off += 0x28
    return None

def main():
    p = Path("D:/SteamLibrary/steamapps/common/dota 2 beta/game/bin/win64/panorama.dll")
    data = p.read_bytes()
    print(f"[+] panorama.dll size: {len(data)} bytes")

    sec = find_section_text(data)
    if not sec:
        print("[!] .text section not found")
        return
    virt_addr, virt_size, raw_off, raw_size = sec
    print(f"[+] .text: VA=0x{virt_addr:X} VSize=0x{virt_size:X} RawOff=0x{raw_off:X} RawSize=0x{raw_size:X}")

    text = data[raw_off:raw_off + raw_size]
    matches = []
    start = 0
    while True:
        idx = text.find(SIG, start)
        if idx < 0:
            break
        rva = virt_addr + idx
        matches.append(rva)
        start = idx + 1
    print(f"[+] RunScript prologue matches: {len(matches)}")
    for m in matches:
        print(f"    RVA: 0x{m:X}")

    print(f"\n[~] Expected RVA (fallback hint in source): 0x{RUNSCRIPT_FALLBACK_RVA:X}")
    if matches:
        diff = matches[0] - RUNSCRIPT_FALLBACK_RVA
        print(f"[~] Drift from fallback: {diff:+d} bytes (0x{diff:+X})")
    print(f"\n[~] Bytes at CUIENGINE_PTR_RVA (0x{CUIENGINE_PTR_RVA:X}, .data slot):")
    # find which section contains CUIENGINE_PTR_RVA
    e_lfanew = int.from_bytes(data[0x3C:0x40], "little")
    nt = data[e_lfanew:]
    num_sections = int.from_bytes(nt[6:8], "little")
    size_opt = int.from_bytes(nt[0x14:0x16], "little")
    sec_off = e_lfanew + 0x18 + size_opt
    for i in range(num_sections):
        s = data[sec_off:sec_off + 0x28]
        name = s[0:8].split(b"\x00", 1)[0].decode("ascii", errors="ignore")
        va = int.from_bytes(s[0x0C:0x10], "little")
        vs = int.from_bytes(s[0x08:0x0C], "little")
        ro = int.from_bytes(s[0x14:0x18], "little")
        if va <= CUIENGINE_PTR_RVA < va + vs:
            off = ro + (CUIENGINE_PTR_RVA - va)
            print(f"    section: {name} (RawOff={ro:#x})")
            print(f"    bytes:   {data[off:off+16].hex(' ')}")
            break
        sec_off += 0x28

if __name__ == "__main__":
    main()
