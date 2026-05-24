"""
Dota 2 Memory Access — pymem wrappers for external read/write.
Handles module resolution, interface walking, pattern scanning.
"""
import struct
import re
import pymem
import pymem.process


class DotaMemory:
    def __init__(self, pid: int = None):
        """Attach to Dota 2 process. Pass pid for multi-instance, or None for auto-detect."""
        if pid is not None:
            self.pm = pymem.Pymem(pid)
        else:
            self.pm = pymem.Pymem("dota2.exe")
        self.pid = self.pm.process_id
        self.modules = {}
        self._load_modules()

    def _load_modules(self):
        for mod in self.pm.list_modules():
            self.modules[mod.name.lower()] = {
                "base": mod.lpBaseOfDll,
                "size": mod.SizeOfImage,
                "name": mod.name,
            }

    def module_base(self, name: str) -> int:
        return self.modules[name.lower()]["base"]

    def module_size(self, name: str) -> int:
        return self.modules[name.lower()]["size"]

    def read_u8(self, addr: int) -> int:
        return struct.unpack("<B", self.pm.read_bytes(addr, 1))[0]

    def read_i32(self, addr: int) -> int:
        return struct.unpack("<i", self.pm.read_bytes(addr, 4))[0]

    def read_u32(self, addr: int) -> int:
        return struct.unpack("<I", self.pm.read_bytes(addr, 4))[0]

    def read_u64(self, addr: int) -> int:
        return struct.unpack("<Q", self.pm.read_bytes(addr, 8))[0]

    def read_f32(self, addr: int) -> float:
        return struct.unpack("<f", self.pm.read_bytes(addr, 4))[0]

    def read_ptr(self, addr: int) -> int:
        """Read a 64-bit pointer."""
        return self.read_u64(addr)

    def read_string(self, addr: int, max_len: int = 128) -> str:
        raw = self.pm.read_bytes(addr, max_len)
        end = raw.find(b'\x00')
        if end >= 0:
            raw = raw[:end]
        return raw.decode("utf-8", errors="replace")

    def read_vec3(self, addr: int) -> tuple:
        data = self.pm.read_bytes(addr, 12)
        return struct.unpack("<fff", data)

    def absolute_address(self, rip_addr: int, offset_in_instr: int = 3, instr_len: int = 7) -> int:
        """Resolve RIP-relative address (LEA rax, [rip+disp32])."""
        disp = struct.unpack("<i", self.pm.read_bytes(rip_addr + offset_in_instr, 4))[0]
        return rip_addr + instr_len + disp

    def pattern_scan(self, module_name: str, pattern: str) -> int | None:
        """AOB scan in module. Pattern uses ?? for wildcards, spaces between bytes."""
        base = self.module_base(module_name)
        size = self.module_size(module_name)

        # Read module into buffer
        try:
            data = self.pm.read_bytes(base, size)
        except Exception:
            # Try smaller chunks if full read fails
            return self._pattern_scan_chunked(base, size, pattern)

        parts = pattern.split()
        regex_parts = []
        for p in parts:
            if p == "?" or p == "??":
                regex_parts.append(b".")
            else:
                regex_parts.append(re.escape(bytes([int(p, 16)])))

        regex = b"".join(regex_parts)
        match = re.search(regex, data, re.DOTALL)
        if match:
            return base + match.start()
        return None

    def _pattern_scan_chunked(self, base: int, size: int, pattern: str, chunk_size: int = 0x100000) -> int | None:
        parts = pattern.split()
        pat_len = len(parts)
        regex_parts = []
        for p in parts:
            if p == "?" or p == "??":
                regex_parts.append(b".")
            else:
                regex_parts.append(re.escape(bytes([int(p, 16)])))
        regex = b"".join(regex_parts)

        overlap = pat_len
        offset = 0
        while offset < size:
            read_size = min(chunk_size + overlap, size - offset)
            try:
                data = self.pm.read_bytes(base + offset, read_size)
            except Exception:
                offset += chunk_size
                continue
            match = re.search(regex, data, re.DOTALL)
            if match:
                return base + offset + match.start()
            offset += chunk_size
        return None

    # --- Source 2 Interface Walking ---

    def find_interface(self, module_name: str, interface_name: str) -> int | None:
        """Walk Source 2 CreateInterface linked list to find interface by name."""
        # Pattern for CreateInterface export
        ci_pattern = "4C 8B ?? ?? ?? ?? ?? 4C 8B ?? 4C 8B ?? 4D 85 ?? 74 ?? 49 8B ?? ?? 4D 8B"
        ci_addr = self.pattern_scan(module_name, ci_pattern)
        if not ci_addr:
            print(f"[!] CreateInterface pattern not found in {module_name}")
            return None

        # Resolve the first interface node pointer
        first_iface_ptr = self.absolute_address(ci_addr, offset_in_instr=3, instr_len=7)
        node = self.read_ptr(first_iface_ptr)

        visited = set()
        while node and node not in visited:
            visited.add(node)
            # Interface node layout: +0x0 = CreateInterfaceFn, +0x8 = name_ptr, +0x10 = next
            name_ptr = self.read_ptr(node + 0x8)
            if name_ptr:
                try:
                    name = self.read_string(name_ptr, 64)
                    if name == interface_name:
                        # +0x0 is the function pointer, resolve its LEA
                        fn_ptr = self.read_ptr(node)
                        # The function typically does: lea rax, [rip+X]; retn
                        # Read first bytes to check
                        result = self.absolute_address(fn_ptr, offset_in_instr=3, instr_len=7)
                        return result
                except Exception:
                    pass

            next_node = self.read_ptr(node + 0x10)
            if next_node == 0:
                break
            node = next_node

        print(f"[!] Interface {interface_name} not found in {module_name}")
        return None

    def close(self):
        self.pm.close_process()
