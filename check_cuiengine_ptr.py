"""Read panorama.dll+0x569C78 in running dota2.exe — is CUIEngine singleton ptr valid?"""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from cheat.memory import DotaMemory

CUIENGINE_PTR_RVA = 0x569C78

m = DotaMemory()
panorama = m.module_base("panorama.dll")
print(f"[+] panorama.dll base: 0x{panorama:X}")
slot = panorama + CUIENGINE_PTR_RVA
print(f"[+] slot @ 0x{slot:X}  (RVA 0x{CUIENGINE_PTR_RVA:X})")

ptr = m.read_ptr(slot)
print(f"[+] *slot = 0x{ptr:X}")

if ptr == 0:
    print("[!] NULL — CUIENGINE_PTR_RVA не указывает на инициализированный singleton")
else:
    # check if it's a valid pointer (within some heap range)
    # also peek vtable: first qword of object is vtable ptr
    try:
        vtbl = m.read_ptr(ptr)
        print(f"[+] (*slot)->vtable = 0x{vtbl:X}")
        if vtbl == 0:
            print("[!] vtable null — garbage at slot")
        else:
            # check if vtable lies inside panorama.dll
            # rough: get module bounds
            print(f"[~] check: vtable inside panorama.dll? offset = 0x{vtbl-panorama:X} (negative or huge = NO)")
    except Exception as e:
        print(f"[!] read vtable failed: {e}")

# also dump 32 bytes around the slot
raw = m.read(slot, 32)
print(f"[+] raw bytes @ slot: {raw.hex(' ')}")
