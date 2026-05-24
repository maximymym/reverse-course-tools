"""
Phase 1e: ConVar read/write test + EngineClient command execution research.

1. Find ConVar by name in ICvar Array3
2. Read/write value at +0x58
3. Research EngineClient vtable for ClientCmd
"""
import sys, struct, time
sys.path.insert(0, ".")
from cheat.memory import DotaMemory
from cheat.offsets import Interfaces

def try_str(mem, ptr):
    if not ptr or ptr < 0x10000 or ptr > 0x7FFFFFFFFFFF:
        return None
    try:
        s = mem.read_string(ptr, 128)
        if s and len(s) > 0:
            return s
    except:
        pass
    return None

class ConVarSystem:
    """Source 2 ConVar system accessor."""

    # ConVarData layout (0x68 bytes)
    CV_NAME = 0x00       # char* name
    CV_VALUE_PTR = 0x08  # void* (points to this+0x60)
    CV_DESC = 0x20       # char* description
    CV_BOUNDS = 0x28     # type/bounds info
    CV_FLAGS = 0x30      # uint32 FCVAR_*
    CV_VALUE = 0x58      # CVValue_t current (i32/f32/etc)
    CV_DEFAULT = 0x60    # CVValue_t default
    CV_ENTRY_SIZE = 0x68

    def __init__(self, mem: DotaMemory):
        self.mem = mem
        self.icvar = None
        self.arr3_ptr = 0
        self.arr3_count = 0
        self._cache = {}  # name -> entry_ptr

    def init(self) -> bool:
        self.icvar = self.mem.find_interface("tier0.dll", Interfaces.CVAR)
        if not self.icvar:
            self.icvar = self.mem.find_interface("engine2.dll", Interfaces.CVAR)
        if not self.icvar:
            print("[!] ICvar not found")
            return False

        self.arr3_ptr = self.mem.read_ptr(self.icvar + 0x48)
        raw_count = self.mem.read_u64(self.icvar + 0x40)
        self.arr3_count = raw_count & 0xFFFF
        print(f"[+] ICvar: {hex(self.icvar)}, ConVar array: {hex(self.arr3_ptr)}, count: {self.arr3_count}")
        return True

    def find_convar(self, name: str) -> int:
        """Find ConVar data entry by name. Returns entry_ptr or 0."""
        if name in self._cache:
            return self._cache[name]

        # Read Array3 in bulk (pairs of ptr+idx, stride 0x10)
        batch_size = min(self.arr3_count, 8000)
        data = self.mem.pm.read_bytes(self.arr3_ptr, batch_size * 0x10)

        for i in range(batch_size):
            entry_ptr = struct.unpack("<Q", data[i*0x10:i*0x10+8])[0]
            if not entry_ptr or entry_ptr < 0x10000:
                continue
            try:
                name_ptr = self.mem.read_ptr(entry_ptr + self.CV_NAME)
                s = try_str(self.mem, name_ptr)
                if s and s == name:
                    self._cache[name] = entry_ptr
                    return entry_ptr
            except:
                continue
        return 0

    def get_int(self, name: str) -> int | None:
        ptr = self.find_convar(name)
        if not ptr:
            return None
        return self.mem.read_i32(ptr + self.CV_VALUE)

    def set_int(self, name: str, value: int) -> bool:
        ptr = self.find_convar(name)
        if not ptr:
            return False
        self.mem.pm.write_bytes(ptr + self.CV_VALUE, struct.pack("<i", value), 4)
        return True

    def get_float(self, name: str) -> float | None:
        ptr = self.find_convar(name)
        if not ptr:
            return None
        return self.mem.read_f32(ptr + self.CV_VALUE)

    def set_float(self, name: str, value: float) -> bool:
        ptr = self.find_convar(name)
        if not ptr:
            return False
        self.mem.pm.write_bytes(ptr + self.CV_VALUE, struct.pack("<f", value), 4)
        return True

    def get_flags(self, name: str) -> int | None:
        ptr = self.find_convar(name)
        if not ptr:
            return None
        return self.mem.read_u32(ptr + self.CV_FLAGS)

    def get_description(self, name: str) -> str | None:
        ptr = self.find_convar(name)
        if not ptr:
            return None
        desc_ptr = self.mem.read_ptr(ptr + self.CV_DESC)
        return try_str(self.mem, desc_ptr)


def main():
    mem = DotaMemory()
    cv = ConVarSystem(mem)
    if not cv.init():
        return

    # Test reading known ConVars
    print("\n=== ConVar Read Tests ===")
    test_vars = [
        "sv_cheats",
        "developer",
        "dota_player_units_auto_attack_mode",
        "dota_camera_distance",
        "sensitivity",
        "volume",
        "fps_max",
    ]

    for name in test_vars:
        val_i = cv.get_int(name)
        val_f = cv.get_float(name)
        flags = cv.get_flags(name)
        desc = cv.get_description(name)
        ptr = cv.find_convar(name)
        if ptr:
            print(f"  {name}: int={val_i}, float={val_f:.4f}, flags={hex(flags or 0)}")
            if desc:
                print(f"    desc: {desc[:60]}")
        else:
            print(f"  {name}: NOT FOUND")

    # Test ConVar WRITE
    print("\n=== ConVar Write Test: dota_player_units_auto_attack_mode ===")
    name = "dota_player_units_auto_attack_mode"
    old_val = cv.get_int(name)
    print(f"  Current value: {old_val}")

    # Toggle: 0->1 or 1->0, or cycle through 0,1,2,3
    new_val = (old_val + 1) % 4 if old_val is not None else 1
    print(f"  Writing new value: {new_val}")
    ok = cv.set_int(name, new_val)
    print(f"  Write result: {ok}")

    # Verify
    verify = cv.get_int(name)
    print(f"  Verify read: {verify}")
    if verify == new_val:
        print("  [OK] ConVar write WORKS!")
    else:
        print("  [FAIL] Value didn't change")

    # Restore
    cv.set_int(name, old_val if old_val is not None else 1)
    print(f"  Restored to: {cv.get_int(name)}")

    # === EngineClient vtable analysis ===
    print("\n=== EngineClient Command Execution Research ===")
    engine_client = mem.find_interface("engine2.dll", Interfaces.ENGINE_TO_CLIENT)
    if not engine_client:
        print("[!] EngineToClient not found")
        return

    vtable = mem.read_ptr(engine_client)
    print(f"EngineToClient: {hex(engine_client)}, vtable: {hex(vtable)}")

    # Read first 50 vtable entries and try to identify ClientCmd
    # ClientCmd signature: void __thiscall ClientCmd(const char* cmd)
    # It's typically a small function that calls Cbuf_AddText internally
    # We can identify it by:
    # 1. It should reference a string or call into tier0.dll (Cbuf_AddText is in tier0)
    # 2. It takes a single const char* parameter
    # 3. It's usually one of the first 30 methods

    print("\nAnalyzing EngineToClient vtable functions:")
    engine2_base = mem.module_base("engine2.dll")
    tier0_base = mem.module_base("tier0.dll")

    for i in range(50):
        fn = mem.read_ptr(vtable + i * 8)
        rva = fn - engine2_base

        # Read first 64 bytes of the function
        try:
            code = mem.pm.read_bytes(fn, 96)
        except:
            continue

        # Classify by size/pattern
        # Look for: immediate ret (0xC3), calls to tier0, string references
        code_hex = code.hex()

        # Check for simple return (tiny function)
        is_tiny = code[0] == 0xC3 or (code[0] == 0x33 and code[2] == 0xC3) or \
                  (code[0] == 0x48 and code[1] == 0x8B and code[3] == 0xC3)

        # Check for calls to tier0 (E8 xx xx xx xx where target is in tier0)
        calls_tier0 = False
        for j in range(len(code) - 5):
            if code[j] == 0xE8:  # CALL rel32
                rel = struct.unpack("<i", code[j+1:j+5])[0]
                target = fn + j + 5 + rel
                if tier0_base <= target < tier0_base + mem.module_size("tier0.dll"):
                    calls_tier0 = True
                    break

        # Check for LEA with string (48 8D xx xx xx xx xx pattern)
        has_string_ref = False
        string_val = ""
        for j in range(len(code) - 7):
            # LEA reg, [rip+disp32]
            if code[j] == 0x48 and code[j+1] == 0x8D and (code[j+2] & 0xC7) in (0x05, 0x0D, 0x15, 0x1D, 0x25, 0x2D, 0x35, 0x3D):
                disp = struct.unpack("<i", code[j+3:j+7])[0]
                str_addr = fn + j + 7 + disp
                s = try_str(mem, str_addr)
                if s and len(s) > 3:
                    has_string_ref = True
                    string_val = s[:40]
                    break

        # Mark interesting functions
        markers = []
        if is_tiny:
            markers.append("tiny")
        if calls_tier0:
            markers.append("calls_tier0")
        if has_string_ref:
            markers.append(f'ref:"{string_val}"')

        # Only print non-tiny or interesting functions
        if not is_tiny or has_string_ref:
            marker_str = f"  [{', '.join(markers)}]" if markers else ""
            print(f"  [{i:2d}] {hex(fn)} (engine2+{hex(rva)}){marker_str}")
            # Show first 16 bytes disassembly-like
            first_bytes = ' '.join(f'{b:02X}' for b in code[:16])
            print(f"       bytes: {first_bytes}")

    # Also check ICvar vtable for DispatchConCommand
    print("\n=== ICvar vtable analysis (looking for DispatchConCommand) ===")
    icvar_vtable = mem.read_ptr(cv.icvar)
    for i in range(30):
        fn = mem.read_ptr(icvar_vtable + i * 8)
        rva = fn - tier0_base
        try:
            code = mem.pm.read_bytes(fn, 64)
        except:
            continue

        is_tiny = code[0] == 0xC3 or (code[0] == 0x33 and code[2] == 0xC3)

        has_string_ref = False
        string_val = ""
        for j in range(len(code) - 7):
            if code[j] == 0x48 and code[j+1] == 0x8D and (code[j+2] & 0xC7) in (0x05, 0x0D, 0x15, 0x1D, 0x25, 0x2D, 0x35, 0x3D):
                disp = struct.unpack("<i", code[j+3:j+7])[0]
                str_addr = fn + j + 7 + disp
                s = try_str(mem, str_addr)
                if s and len(s) > 3:
                    has_string_ref = True
                    string_val = s[:40]
                    break

        if not is_tiny or has_string_ref:
            marker = f'  ref:"{string_val}"' if has_string_ref else ""
            print(f"  [{i:2d}] {hex(fn)} (tier0+{hex(rva)}){marker}")

    mem.close()
    print("\n[+] Done")

if __name__ == "__main__":
    main()
