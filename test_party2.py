"""Record party invite: SendMessage hook on Bot0, RetrieveMessage hook on Bot1."""
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
    """VMT hook on SendMessage — logs ALL outgoing messages."""
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

def read_send_log(mem, log):
    hdr = mem.pm.read_bytes(log, 12)
    count = struct.unpack('<I', hdr[0:4])[0]
    mt = struct.unpack('<I', hdr[4:8])[0]
    sz = struct.unpack('<I', hdr[8:12])[0]
    data = mem.pm.read_bytes(log + 0x10, min(sz, 0xF00)) if sz > 0 else b''
    return count, mt, sz, data

# ─── Main ───
PID0, PID1 = 36700, 35028

mem0 = DotaMemory(pid=PID0)
mem1 = DotaMemory(pid=PID1)
gc0 = DotaGC(mem0); gc0.init()
gc1 = DotaGC(mem1); gc1.init()

# Bot0: SendMessage hook
alloc0, log0, orig_vt0 = install_send_hook(mem0, gc0)
print(f"[Bot0] SendMessage hook ON")

# Bot1: RetrieveMessage hook
gc1.hook_retrieve_message()
print(f"[Bot1] RetrieveMessage hook ON")

print()
print(">>> Bot0: пригласи Bot1 в пати из UI. Жду 90 сек...")
print()

prev0 = 0
prev1 = 0
all_send = []
all_recv = []
start = time.time()

try:
    while time.time() - start < 90:
        # Bot0 outgoing
        c0, mt0, sz0, data0 = read_send_log(mem0, log0)
        if c0 > prev0:
            mt_clean = mt0 & 0x7FFFFFFF
            print(f"  [Bot0 SEND] #{c0} type={mt_clean} size={sz0}B")
            if sz0 >= 8:
                hdr_sz = struct.unpack('<I', data0[0:4])[0]
                body = data0[4+hdr_sz:sz0]
                if body:
                    try:
                        fields = _decode_protobuf(body)
                        for k,v in sorted(fields.items()):
                            if isinstance(v, int):
                                print(f"    f{k}={v} (0x{v:x})")
                            elif isinstance(v, bytes):
                                print(f"    f{k}=bytes({len(v)})")
                    except: pass
            all_send.append({"msg_type": mt0, "size": sz0, "data_hex": data0[:sz0].hex()})
            prev0 = c0

        # Bot1 incoming
        log1 = gc1.read_hook_log()
        c1 = log1["count"]
        if c1 > prev1:
            raw1 = gc1.read_last_message_raw()
            mt1, fields1 = gc1.parse_retrieve_buffer(raw1)
            print(f"  [Bot1 RECV] #{c1} type={mt1} size={log1['last_msg_size']}B")
            for k,v in sorted(fields1.items()):
                if isinstance(v, int):
                    print(f"    f{k}={v} (0x{v:x})")
                elif isinstance(v, bytes):
                    print(f"    f{k}=bytes({len(v)})")
            all_recv.append({"msg_type": mt1, "size": log1['last_msg_size'], "data_hex": raw1.hex()})
            prev1 = c1

        # Stop condition: party invite sent AND received
        has_send_party = any((m["msg_type"] & 0x7FFFFFFF) in range(4500, 4515) for m in all_send)
        has_recv_party = any(m["msg_type"] in range(4500, 4515) for m in all_recv)
        if has_send_party and has_recv_party:
            print("\n*** Party messages on BOTH sides! ***")
            time.sleep(2)  # grab a few more
            # Read any extra
            c0b, mt0b, sz0b, data0b = read_send_log(mem0, log0)
            if c0b > prev0:
                print(f"  [Bot0 SEND extra] type={mt0b & 0x7FFFFFFF}")
            log1b = gc1.read_hook_log()
            if log1b["count"] > prev1:
                raw1b = gc1.read_last_message_raw()
                mt1b, _ = gc1.parse_retrieve_buffer(raw1b)
                print(f"  [Bot1 RECV extra] type={mt1b}")
            break

        time.sleep(0.1)
    else:
        print("\nTimeout.")

    print(f"\n=== Bot0 sent {len(all_send)}, Bot1 received {len(all_recv)} ===")
    with open("config/party_send.json", "w") as f:
        json.dump(all_send, f, indent=2)
    with open("config/party_recv.json", "w") as f:
        json.dump(all_recv, f, indent=2)
    print("Saved to config/party_send.json + config/party_recv.json")

finally:
    wr(mem0.pm.process_handle, gc0.gc_ptr, struct.pack('<Q', orig_vt0))
    gc1.unhook_retrieve_message()
    time.sleep(0.3)
    kernel32.VirtualFreeEx(mem0.pm.process_handle, alloc0, 0, MEM_RELEASE)
    print("Hooks removed.")
