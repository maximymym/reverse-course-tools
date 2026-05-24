"""
Capture StartFindingMatch (7033) GC message from UI.

1. Hooks SendMessage on bot#0
2. You press FIND MATCH in Dota UI
3. Saves raw body bytes to config/find_match_template.json
4. test_full_5bot.py replays this exact packet
"""
import sys, os, time, struct, json
sys.path.insert(0, os.path.dirname(__file__))

from cheat.memory import DotaMemory
from cheat.gc import (
    DotaGC, _write_remote, _decode_protobuf, kernel32,
    MEM_COMMIT, MEM_RESERVE, MEM_RELEASE, PAGE_RWX, PROTOBUF_FLAG
)


def capture(pid: int = None, timeout: float = 60):
    mem = DotaMemory(pid) if pid else DotaMemory()
    gc = DotaGC(mem); gc.init()
    print(f"[+] PID {mem.pid}, SteamID {gc.steam_id}")

    handle = mem.pm.process_handle
    orig_vt = mem.read_ptr(gc.gc_ptr)
    orig_send = mem.read_ptr(orig_vt)
    orig_avail = mem.read_ptr(orig_vt + 8)
    orig_retr = mem.read_ptr(orig_vt + 16)

    alloc = kernel32.VirtualAllocEx(handle, 0, 0x4000,
                                     MEM_COMMIT | MEM_RESERVE, PAGE_RWX)
    log_addr = alloc + 0x1000
    _write_remote(handle, log_addr, b'\x00' * 0x2000)

    # Build SendMessage hook (logs msg_type, size, raw data)
    sc = bytearray()
    sc += b'\x48\x83\xEC\x48'
    sc += b'\x48\x89\x4C\x24\x20'; sc += b'\x89\x54\x24\x28'
    sc += b'\x4C\x89\x44\x24\x30'; sc += b'\x44\x89\x4C\x24\x38'
    sc += b'\x49\xBA' + struct.pack('<Q', log_addr + 4); sc += b'\x41\x89\x12'
    sc += b'\x49\xBA' + struct.pack('<Q', log_addr + 8); sc += b'\x45\x89\x0A'
    sc += b'\x57\x56'
    sc += b'\x48\xBF' + struct.pack('<Q', log_addr + 0x10)
    sc += b'\x4C\x89\xC6'; sc += b'\x44\x89\xC9'
    sc += b'\x81\xF9\x00\x0F\x00\x00'; sc += b'\x76\x05'; sc += b'\xB9\x00\x0F\x00\x00'
    sc += b'\xF3\xA4'; sc += b'\x5E\x5F'
    sc += b'\x49\xBA' + struct.pack('<Q', log_addr); sc += b'\xF0\x41\xFF\x02'
    sc += b'\x48\x8B\x4C\x24\x20'; sc += b'\x8B\x54\x24\x28'
    sc += b'\x4C\x8B\x44\x24\x30'; sc += b'\x44\x8B\x4C\x24\x38'
    sc += b'\x48\xB8' + struct.pack('<Q', orig_send); sc += b'\xFF\xD0'
    sc += b'\x48\x83\xC4\x48'; sc += b'\xC3'

    _write_remote(handle, alloc + 0x100, bytes(sc))
    _write_remote(handle, alloc, struct.pack('<QQQ', alloc + 0x100, orig_avail, orig_retr))
    _write_remote(handle, gc.gc_ptr, struct.pack('<Q', alloc))

    print()
    print("=" * 50)
    print("  Нажми НАЙТИ МАТЧ в UI на этом аккаунте")
    print(f"  Жду {timeout:.0f} сек...")
    print("=" * 50)
    print()

    captured = None
    try:
        start = time.time()
        while time.time() - start < timeout:
            hdr = mem.pm.read_bytes(log_addr, 12)
            count = struct.unpack('<I', hdr[0:4])[0]
            if count > 0:
                mt = struct.unpack('<I', hdr[4:8])[0]
                sz = struct.unpack('<I', hdr[8:12])[0]
                mt_clean = mt & 0x7FFFFFFF

                if sz > 0 and sz < 0xF00:
                    raw = mem.pm.read_bytes(log_addr + 0x10, sz)

                if mt_clean == 7033:
                    # Parse GC header to get body
                    gc_hdr_sz = struct.unpack('<I', raw[4:8])[0]
                    body = raw[8 + gc_hdr_sz:]
                    fields = _decode_protobuf(body)

                    print(f"[+] CAPTURED StartFindingMatch (7033)!")
                    print(f"    Raw size: {sz}B, body: {len(body)}B")
                    print(f"    Fields: {dict(fields)}")

                    captured = body
                    break
                else:
                    print(f"  [{time.time()-start:.1f}s] msg {mt_clean} ({sz}B) — skipping")

                # Clear for next
                _write_remote(handle, log_addr, b'\x00' * 12)

            time.sleep(0.3)
    finally:
        _write_remote(handle, gc.gc_ptr, struct.pack('<Q', orig_vt))
        kernel32.VirtualFreeEx(handle, alloc, 0, MEM_RELEASE)
        print("[+] Hook removed")

    if captured:
        save = {
            "msg_type": 7033,
            "body_hex": captured.hex(),
            "body_size": len(captured),
            "fields": {str(k): (v if isinstance(v, int) else v.hex() if isinstance(v, bytes) else str(v))
                       for k, v in _decode_protobuf(captured).items()},
        }
        path = os.path.join(os.path.dirname(__file__), "config", "find_match_template.json")
        with open(path, "w") as f:
            json.dump(save, f, indent=2)
        print(f"\n[+] Saved to {path}")
        print(f"    test_full_5bot.py will replay this exact packet")
    else:
        print("\n[!] Timeout — StartFindingMatch not captured")


if __name__ == "__main__":
    pid = int(sys.argv[1]) if len(sys.argv) > 1 else None
    capture(pid)
