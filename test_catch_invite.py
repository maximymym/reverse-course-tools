"""Catch the ACTUAL CMsgInviteToParty (4501) from both clients."""
import sys, time, struct, json
sys.path.insert(0, '.')
from cheat.memory import DotaMemory
from cheat.gc import DotaGC, PROTOBUF_FLAG, _decode_protobuf
import ctypes

kernel32 = ctypes.windll.kernel32
for fn, argt, rest in [
    ("VirtualAllocEx", [ctypes.c_void_p, ctypes.c_ulonglong, ctypes.c_size_t, ctypes.c_ulong, ctypes.c_ulong], ctypes.c_ulonglong),
    ("VirtualFreeEx", [ctypes.c_void_p, ctypes.c_ulonglong, ctypes.c_size_t, ctypes.c_ulong], ctypes.c_bool),
    ("WriteProcessMemory", [ctypes.c_void_p, ctypes.c_ulonglong, ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)], ctypes.c_bool),
]:
    getattr(kernel32, fn).argtypes = argt
    getattr(kernel32, fn).restype = rest

MEM_COMMIT = 0x1000; MEM_RESERVE = 0x2000; MEM_RELEASE = 0x8000; PAGE_RWX = 0x40

def wr(handle, addr, data):
    buf = ctypes.create_string_buffer(data)
    w = ctypes.c_size_t(0)
    kernel32.WriteProcessMemory(handle, addr, buf, len(data), ctypes.byref(w))

def install_send_hook(mem, gc):
    handle = mem.pm.process_handle
    vt_ptr = mem.read_ptr(gc.gc_ptr)
    orig_send = mem.read_ptr(vt_ptr)
    orig_avail = mem.read_ptr(vt_ptr + 8)
    orig_retr = mem.read_ptr(vt_ptr + 16)
    alloc = kernel32.VirtualAllocEx(handle, 0, 0x4000, MEM_COMMIT | MEM_RESERVE, PAGE_RWX)
    log = alloc + 0x1000
    wr(handle, log, b'\x00' * 0x2000)
    sc = bytearray()
    sc += b'\x48\x83\xEC\x48'
    sc += b'\x48\x89\x4C\x24\x20'; sc += b'\x89\x54\x24\x28'
    sc += b'\x4C\x89\x44\x24\x30'; sc += b'\x44\x89\x4C\x24\x38'
    sc += b'\x49\xBA' + struct.pack('<Q', log + 4); sc += b'\x41\x89\x12'
    sc += b'\x49\xBA' + struct.pack('<Q', log + 8); sc += b'\x45\x89\x0A'
    sc += b'\x57\x56'
    sc += b'\x48\xBF' + struct.pack('<Q', log + 0x10)
    sc += b'\x4C\x89\xC6'; sc += b'\x44\x89\xC9'
    sc += b'\x81\xF9\x00\x0F\x00\x00'; sc += b'\x76\x05'; sc += b'\xB9\x00\x0F\x00\x00'
    sc += b'\xF3\xA4'; sc += b'\x5E\x5F'
    sc += b'\x49\xBA' + struct.pack('<Q', log); sc += b'\xF0\x41\xFF\x02'
    sc += b'\x48\x8B\x4C\x24\x20'; sc += b'\x8B\x54\x24\x28'
    sc += b'\x4C\x8B\x44\x24\x30'; sc += b'\x44\x8B\x4C\x24\x38'
    sc += b'\x48\xB8' + struct.pack('<Q', orig_send); sc += b'\xFF\xD0'
    sc += b'\x48\x83\xC4\x48'; sc += b'\xC3'
    wr(handle, alloc + 0x100, bytes(sc))
    wr(handle, alloc, struct.pack('<QQQ', alloc + 0x100, orig_avail, orig_retr))
    wr(handle, gc.gc_ptr, struct.pack('<Q', alloc))
    return alloc, log, vt_ptr

def read_log(mem, log):
    hdr = mem.pm.read_bytes(log, 12)
    return struct.unpack('<I', hdr[0:4])[0], struct.unpack('<I', hdr[4:8])[0], struct.unpack('<I', hdr[8:12])[0]

PID0, PID1 = 36700, 35028
mem0 = DotaMemory(pid=PID0); mem1 = DotaMemory(pid=PID1)
gc0 = DotaGC(mem0); gc0.init()
gc1 = DotaGC(mem1); gc1.init()

# Hook Send on BOTH
a0, l0, vt0 = install_send_hook(mem0, gc0)
a1, l1, vt1 = install_send_hook(mem1, gc1)
print("[Bot0 zrvqd] SendMessage hook ON")
print("[Bot1 tqbao] SendMessage hook ON")
print()
print(">>> Пригласи в пати с ЛЮБОГО бота. Ловлю 4501. Жду 90 сек...")
print()

prev0 = prev1 = 0
found = False
start = time.time()

try:
    while time.time() - start < 90:
        for label, mem, log, prev_ref in [("Bot0", mem0, l0, "prev0"), ("Bot1", mem1, l1, "prev1")]:
            prev = prev0 if prev_ref == "prev0" else prev1
            c, mt, sz = read_log(mem, log)
            if c > prev:
                mt_clean = mt & 0x7FFFFFFF
                if mt_clean in range(4495, 4515):  # party-related
                    data = mem.pm.read_bytes(log + 0x10, min(sz, 0xF00))
                    print(f"  [{label} SEND] #{c} type={mt_clean} size={sz}B *** PARTY ***")
                    print(f"    raw: {data[:min(sz,128)].hex()}")
                    # Parse
                    if sz >= 4:
                        hdr_sz = struct.unpack('<I', data[0:4])[0]
                        hdr = data[4:4+hdr_sz]
                        body = data[4+hdr_sz:sz]
                        if hdr:
                            print(f"    gc_header ({len(hdr)}B): {hdr[:64].hex()}")
                            hf = _decode_protobuf(hdr)
                            print(f"    gc_header fields: { {k: (hex(v) if isinstance(v,int) else f'bytes({len(v)})') for k,v in hf.items()} }")
                        if body:
                            bf = _decode_protobuf(body)
                            print(f"    body fields:")
                            for k,v in sorted(bf.items()):
                                if isinstance(v, int):
                                    print(f"      f{k}={v} (0x{v:x})")
                                elif isinstance(v, bytes):
                                    print(f"      f{k}=bytes({len(v)}) {v[:32].hex()}")
                    # Save
                    saved = {"msg_type": mt, "size": sz, "data_hex": data[:sz].hex()}
                    with open(f"config/party_{mt_clean}_{label.lower()}.json", "w") as f:
                        json.dump(saved, f, indent=2)
                    print(f"    Saved to config/party_{mt_clean}_{label.lower()}.json")
                    if mt_clean == 4501:
                        found = True
                else:
                    print(f"  [{label} SEND] #{c} type={mt_clean} size={sz}B")
                if prev_ref == "prev0": prev0 = c
                else: prev1 = c

        if found:
            print("\n*** 4501 CAPTURED! ***")
            time.sleep(1)
            break
        time.sleep(0.1)
    else:
        print("\nTimeout — no 4501 seen.")
        print("All types seen on Bot0:", end=" ")
        print("check logs")

finally:
    wr(mem0.pm.process_handle, gc0.gc_ptr, struct.pack('<Q', vt0))
    wr(mem1.pm.process_handle, gc1.gc_ptr, struct.pack('<Q', vt1))
    time.sleep(0.3)
    kernel32.VirtualFreeEx(mem0.pm.process_handle, a0, 0, MEM_RELEASE)
    kernel32.VirtualFreeEx(mem1.pm.process_handle, a1, 0, MEM_RELEASE)
    print("Hooks removed.")
