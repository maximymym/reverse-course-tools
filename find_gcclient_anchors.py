"""
Автопоиск всех Phase B якорей в dota2.exe client.dll.

Что находит (за один прогон, ~10-30 сек):
  1. Строка "CDOTAGCClientSystem\0" (1 hit в .rdata)
  2. RTTI .?AVCDOTAGCClientSystem@@
  3. RTTI .?AVCGCClientSharedObjectCache@GCSDK@@
  4. RTTI .?AVCGCClientSharedObjectTypeCache@GCSDK@@
  5. COL + primary vtable для каждого класса
  6. CDOTAGCClientSystem singleton (inline static в client.dll) — через LEA xref на строку
  7. SOCache pointer offset в singleton (обычно +0x538)
  8. Текущий SOCache и список TypeCache'ей с type_id и count

Использование:
    python find_gcclient_anchors.py           # auto-attach к dota2.exe, ищет всё
    python find_gcclient_anchors.py --pid N   # attach к конкретному PID (если несколько Dota)
"""
import sys, os, argparse, struct
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)) + "/..")
from bridge_client import CEBridge


def packle(n: int, sz: int = 8) -> str:
    return " ".join(f"{b:02X}" for b in n.to_bytes(sz, "little"))


def scan_string(ce, s: str, prot="+R-X"):
    pat = " ".join(f"{ord(c):02X}" for c in s) + " 00"
    r = ce.send_command("aob_scan", {"pattern": pat, "max_results": 50, "protection": prot, "align": 1})
    return [int(h, 16) for h in r.get("results", [])]


def scan_qword(ce, qw: int, prot="+R-X", align=1):
    pat = packle(qw, 8)
    r = ce.send_command("aob_scan", {"pattern": pat, "max_results": 100, "protection": prot, "align": align})
    return [int(h, 16) for h in r.get("results", [])]


def scan_dword(ce, dw: int, prot="+R-X", align=1):
    pat = packle(dw, 4)
    r = ce.send_command("aob_scan", {"pattern": pat, "max_results": 200, "protection": prot, "align": align})
    return [int(h, 16) for h in r.get("results", [])]


def read_q(ce, addr: int) -> int:
    r = ce.read_memory(f"0x{addr:x}", size=8)
    return struct.unpack("<Q", bytes.fromhex(r["bytes"].replace(" ", "")))[0]


def read_struct(ce, addr: int, size: int) -> bytes:
    r = ce.read_memory(f"0x{addr:x}", size=size)
    return bytes.fromhex(r["bytes"].replace(" ", ""))


def find_col_and_vtable(ce, base: int, end: int, rtti_name_abs: int, rtti_label: str):
    """По абсолютному адресу RTTI name находит TypeDescriptor → COL → primary vtable."""
    # TypeDescriptor begins 0x10 before the name string (vftable + spare)
    td_rva = (rtti_name_abs - 0x10) - base
    # Look for 4-byte TD RVA in .rdata
    hits = scan_dword(ce, td_rva, prot="+R-X", align=1)
    cols = []
    for a in hits:
        if not (base <= a < end):
            continue
        col_rva = (a - base) - 0x0C
        try:
            d = read_struct(ce, base + col_rva, 24)
            sig, off, cdOff, td, cd, self_rva = struct.unpack("<IIIIII", d)
            if sig == 1 and td == td_rva and self_rva == col_rva:
                cols.append((col_rva, off))
        except Exception:
            pass
    # Find primary COL (offset=0) and scan for pointer → its vtable
    primary_vt_rva = None
    for cr, off in cols:
        if off != 0:
            continue
        col_abs = base + cr
        vt_hits = scan_qword(ce, col_abs, prot="+R-X", align=1)
        for h in vt_hits:
            if base <= h < end:
                primary_vt_rva = (h - base) + 8
                break
        break
    return cols, primary_vt_rva


def find_singleton_via_xref(ce, base: int, end: int, str_abs: int):
    """LEA xref scanner: find global stored via `lea rcx,[rip+disp]; ...; mov [global]=rcx` pattern."""
    # Use Lua-side scan for speed
    lua = f"""
    local str_addr = 0x{str_abs:X}
    local base = 0x{base:X}
    local mod_end = 0x{end:X}
    local results = {{}}
    local chunk_size = 4 * 1024 * 1024
    local addr = base
    while addr < mod_end do
        local sz = math.min(chunk_size, mod_end - addr)
        local ok, buf = pcall(readBytes, addr, sz, true)
        if ok and buf then
            local n = #buf
            for i = 1, n - 6 do
                local b0, b1, b2 = buf[i], buf[i+1], buf[i+2]
                if (b0 == 0x48 or b0 == 0x4C) and b1 == 0x8D and (b2 & 0xC7) == 0x05 then
                    local disp = buf[i+3] | (buf[i+4]<<8) | (buf[i+5]<<16) | (buf[i+6]<<24)
                    if disp >= 0x80000000 then disp = disp - 0x100000000 end
                    local instr = addr + i - 1
                    if instr + 7 + disp == str_addr then
                        results[#results+1] = string.format("0x%X", instr)
                        if #results >= 20 then break end
                    end
                end
            end
        end
        if #results >= 20 then break end
        addr = addr + sz
    end
    return table.concat(results, ",")
    """
    r = ce.execute_lua(lua)
    out = r.get("result", "") if isinstance(r, dict) else ""
    if not out:
        return []
    return [int(x, 16) for x in out.split(",") if x]


def scan_xref_region_for_singleton(ce, base: int, end: int, xrefs: list[int]):
    """
    Для каждого xref на строку "CDOTAGCClientSystem":
    прочитать ~128 байт вокруг, найти паттерн `mov [rip+disp], rcx` после `lea rcx, [rip+disp]`
    где первый disp ведёт в .data (writable). Это — адрес singleton'а.
    """
    import capstone
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    md.detail = True

    candidates = {}
    for xa in xrefs:
        try:
            d = read_struct(ce, xa - 0x20, 0x80)
        except Exception:
            continue
        rcx_global = None
        for ins in md.disasm(d, xa - 0x20):
            # lea rcx, [rip+disp32]
            if ins.mnemonic == "lea" and len(ins.operands) == 2:
                op0, op1 = ins.operands
                if op0.type == capstone.x86.X86_OP_REG and op1.type == capstone.x86.X86_OP_MEM and op1.mem.base == capstone.x86.X86_REG_RIP:
                    target = ins.address + ins.size + op1.mem.disp
                    # Track what was loaded to rcx specifically
                    reg_name = ins.reg_name(op0.reg)
                    if reg_name == "rcx":
                        rcx_global = target
            # mov [rip+disp], rcx — storing rcx to global
            if ins.mnemonic == "mov" and len(ins.operands) == 2:
                op0, op1 = ins.operands
                if (op1.type == capstone.x86.X86_OP_REG and ins.reg_name(op1.reg) == "rcx"
                    and op0.type == capstone.x86.X86_OP_MEM and op0.mem.base == capstone.x86.X86_REG_RIP):
                    if rcx_global and base <= rcx_global < end:
                        # This is a "singleton self-ref" store: stores &global at some location
                        # The `lea rcx, [global]` loaded the ADDRESS OF singleton into rcx
                        candidates[rcx_global] = candidates.get(rcx_global, 0) + 1
            # Also track `mov rax, [rip+disp]` — loads content of singleton (vtable)
            # `jmp [rax+X]` — virtual call pattern
    return candidates


def verify_singleton(ce, base: int, candidate_addr: int, expected_primary_vt: int) -> bool:
    """Check that *candidate == expected_primary_vtable."""
    try:
        v = read_q(ce, candidate_addr)
        return v == expected_primary_vt
    except Exception:
        return False


def find_socache_offset(ce, base: int, end: int, singleton_addr: int, socache_vt: int) -> tuple[int, int] | None:
    """
    Scan memory region around singleton for a slot containing a heap pointer p where *p == socache_vt.
    Returns (offset_in_singleton, heap_ptr) or None.
    """
    # Read first 0x1000 bytes of singleton
    size = 0x1000
    try:
        d = read_struct(ce, singleton_addr, size)
    except Exception:
        return None
    for i in range(0, len(d) - 8, 8):
        p = struct.unpack("<Q", d[i:i+8])[0]
        # heap pointers in Windows user space look like 0x0000...
        if not (p >> 32):
            continue  # skip low values
        # must not be inside client.dll itself
        if base <= p < end:
            continue
        # verify p is heap and *p == socache_vt
        try:
            vt = read_q(ce, p)
            if vt == socache_vt:
                return (i, p)
        except Exception:
            continue
    return None


def parse_typecaches(ce, socache_addr: int, tc_vt: int) -> list[dict]:
    """Parse CGCClientSharedObjectCache: count@+0x10, array@+0x18, each TypeCache has type_id@+0x28, count@+0x18."""
    try:
        head = read_struct(ce, socache_addr, 0x40)
    except Exception:
        return []
    count = struct.unpack("<I", head[0x10:0x14])[0]
    arr_ptr = struct.unpack("<Q", head[0x18:0x20])[0]
    if count == 0 or count > 200 or arr_ptr == 0:
        return []
    tcs = []
    try:
        arr = read_struct(ce, arr_ptr, 8 * count)
    except Exception:
        return []
    for i in range(count):
        tc_ptr = struct.unpack("<Q", arr[i*8:(i+1)*8])[0]
        if not tc_ptr:
            continue
        try:
            d = read_struct(ce, tc_ptr, 0x30)
        except Exception:
            continue
        vt = struct.unpack("<Q", d[0x00:0x08])[0]
        obj_arr = struct.unpack("<Q", d[0x10:0x18])[0]
        obj_count = struct.unpack("<I", d[0x18:0x1C])[0]
        type_id = struct.unpack("<I", d[0x28:0x2C])[0]
        tcs.append({
            "addr": tc_ptr,
            "vt": vt,
            "vt_ok": vt == tc_vt,
            "obj_arr": obj_arr,
            "obj_count": obj_count,
            "type_id": type_id,
        })
    return tcs


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--pid", type=int, help="attach к конкретному PID (если несколько Dota)")
    args = ap.parse_args()

    with CEBridge(timeout=120) as ce:
        if args.pid:
            ce.send_command("attach", {"pid": args.pid})
        else:
            ce.attach("dota2.exe")
        info = ce.process_info()
        print(f"attached: pid={info.get('pid')} is_64bit={info.get('is_64bit')}")

        mods = ce.module_list()
        client = next((m for m in mods if m["name"].lower() == "client.dll"), None)
        if not client:
            print("client.dll not loaded — дота ещё не догрузилась, подожди ~30 сек после запуска")
            return 1
        base = int(client["base"], 16)
        end = base + client["size"]
        print(f"client.dll: 0x{base:x} .. 0x{end:x}  ({client['size']/1024/1024:.1f} MB)")
        print()

        # 1) Find CDOTAGCClientSystem string
        print("[1] scan string 'CDOTAGCClientSystem'...")
        str_hits = [h for h in scan_string(ce, "CDOTAGCClientSystem") if base <= h < end]
        if not str_hits:
            print("  NOT FOUND in client.dll — клиент не инициализировался?")
            return 1
        str_abs = str_hits[0]
        print(f"  client.dll+0x{str_abs-base:X}")

        # 2) RTTI for CDOTAGCClientSystem
        print()
        print("[2] scan RTTI .?AVCDOTAGCClientSystem@@...")
        rtti_sys = [h for h in scan_string(ce, ".?AVCDOTAGCClientSystem@@") if base <= h < end]
        if not rtti_sys:
            print("  NOT FOUND")
            return 1
        rtti_sys_abs = rtti_sys[0]
        print(f"  client.dll+0x{rtti_sys_abs-base:X}")
        cols, sys_vt_rva = find_col_and_vtable(ce, base, end, rtti_sys_abs, "CDOTAGCClientSystem")
        print(f"  COLs: {[(hex(c), o) for c, o in cols]}")
        print(f"  primary vtable: client.dll+0x{sys_vt_rva:X}" if sys_vt_rva else "  primary vtable NOT FOUND")
        sys_vt_abs = base + sys_vt_rva if sys_vt_rva else None

        # 3) RTTI for CGCClientSharedObjectCache
        print()
        print("[3] scan RTTI .?AVCGCClientSharedObjectCache@GCSDK@@...")
        rtti_soc = [h for h in scan_string(ce, ".?AVCGCClientSharedObjectCache@GCSDK@@") if base <= h < end]
        soc_vt_rva = None
        if rtti_soc:
            _, soc_vt_rva = find_col_and_vtable(ce, base, end, rtti_soc[0], "CGCClientSharedObjectCache")
            print(f"  RTTI: client.dll+0x{rtti_soc[0]-base:X}")
            print(f"  primary vtable: client.dll+0x{soc_vt_rva:X}" if soc_vt_rva else "  vt NOT FOUND")
        else:
            print("  NOT FOUND")
        soc_vt_abs = base + soc_vt_rva if soc_vt_rva else None

        # 4) RTTI for CGCClientSharedObjectTypeCache
        print()
        print("[4] scan RTTI .?AVCGCClientSharedObjectTypeCache@GCSDK@@...")
        rtti_tc = [h for h in scan_string(ce, ".?AVCGCClientSharedObjectTypeCache@GCSDK@@") if base <= h < end]
        tc_vt_rva = None
        if rtti_tc:
            _, tc_vt_rva = find_col_and_vtable(ce, base, end, rtti_tc[0], "CGCClientSharedObjectTypeCache")
            print(f"  RTTI: client.dll+0x{rtti_tc[0]-base:X}")
            print(f"  primary vtable: client.dll+0x{tc_vt_rva:X}" if tc_vt_rva else "  vt NOT FOUND")
        else:
            print("  NOT FOUND")
        tc_vt_abs = base + tc_vt_rva if tc_vt_rva else None

        # 5) Singleton via LEA xref
        if sys_vt_abs:
            print()
            print("[5] LEA xref scan для поиска singleton...")
            xrefs = find_singleton_via_xref(ce, base, end, str_abs)
            print(f"  xrefs на строку: {len(xrefs)}")
            for xr in xrefs:
                print(f"    client.dll+0x{xr-base:X}")
            cands = scan_xref_region_for_singleton(ce, base, end, xrefs)
            print(f"  candidates (loaded to rcx near xref): {len(cands)}")
            verified = []
            for addr, cnt in sorted(cands.items(), key=lambda x: -x[1]):
                ok = verify_singleton(ce, base, addr, sys_vt_abs)
                marker = "✓" if ok else " "
                rva = addr - base
                print(f"    {marker} client.dll+0x{rva:X}  (seen {cnt}x)")
                if ok:
                    verified.append(addr)
            if verified:
                singleton = verified[0]
                print(f"  SINGLETON: client.dll+0x{singleton-base:X}")
            else:
                singleton = None
                print("  SINGLETON not verified — проверь xref вручную")
        else:
            singleton = None

        # 6) SOCache offset in singleton + dump TypeCaches
        if singleton and soc_vt_abs:
            print()
            print("[6] поиск SOCache pointer в singleton (scan слот в первых 4 KB)...")
            found = find_socache_offset(ce, base, end, singleton, soc_vt_abs)
            if found:
                off, soc_addr = found
                print(f"  SOCache* @ singleton+0x{off:X}  =>  0x{soc_addr:X}")
                print()
                print(f"[7] TypeCaches в SOCache @ 0x{soc_addr:X}:")
                tcs = parse_typecaches(ce, soc_addr, tc_vt_abs or 0)
                print(f"  count = {len(tcs)}")
                print(f"  {'idx':<4} {'tc_addr':<18} {'type_id':<8} {'objs':<6} {'vt_ok'}")
                for i, tc in enumerate(tcs):
                    print(f"  {i:<4} 0x{tc['addr']:016x}  {tc['type_id']:<8} {tc['obj_count']:<6} {tc['vt_ok']}")
                # Known Dota type_ids to highlight
                interesting = {2003: "CSODOTAParty", 2004: "CSODOTAPartyInvite", 2006: "CSODOTAPartyInvite(alt)", 2010: "CSODOTALobby"}
                hits_int = [tc for tc in tcs if tc["type_id"] in interesting]
                if hits_int:
                    print()
                    print("  ★ HIT Phase B target type_ids:")
                    for tc in hits_int:
                        print(f"    type_id={tc['type_id']} ({interesting[tc['type_id']]}) count={tc['obj_count']} @ 0x{tc['addr']:x}")
            else:
                print("  SOCache slot не найден — singleton не инициализирован или offset другой")

        print()
        print("=== ИТОГОВАЯ ТАБЛИЦА RVA (для копи-пасты в DLL) ===")
        print(f"  String CDOTAGCClientSystem       +0x{str_abs-base:X}")
        if sys_vt_rva:
            print(f"  vtable CDOTAGCClientSystem       +0x{sys_vt_rva:X}")
        if soc_vt_rva:
            print(f"  vtable CGCClientSharedObjectCache+0x{soc_vt_rva:X}")
        if tc_vt_rva:
            print(f"  vtable CGCClientSharedObjectTC   +0x{tc_vt_rva:X}")
        if singleton:
            print(f"  CDOTAGCClientSystem singleton    +0x{singleton-base:X}")
        if singleton and soc_vt_abs:
            if found:
                print(f"  offset SOCache* in singleton     +0x{off:X}")
        return 0


if __name__ == "__main__":
    sys.exit(main())
