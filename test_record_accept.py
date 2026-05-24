"""Record real accept: Bot0 invites programmatically, user accepts on Bot1 manually."""
import sys, time, struct, json
sys.path.insert(0, '.')
from cheat.memory import DotaMemory
from cheat.gc import DotaGC, GC_MSG_INVITATION_CREATED, _decode_protobuf
import ctypes

kernel32 = ctypes.windll.kernel32
for fn, argt, rest in [
    ("VirtualAllocEx", [ctypes.c_void_p, ctypes.c_ulonglong, ctypes.c_size_t, ctypes.c_ulong, ctypes.c_ulong], ctypes.c_ulonglong),
    ("VirtualFreeEx", [ctypes.c_void_p, ctypes.c_ulonglong, ctypes.c_size_t, ctypes.c_ulong], ctypes.c_bool),
    ("WriteProcessMemory", [ctypes.c_void_p, ctypes.c_ulonglong, ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)], ctypes.c_bool),
]:
    getattr(kernel32, fn).argtypes = argt
    getattr(kernel32, fn).restype = rest
MEM_COMMIT=0x1000; MEM_RESERVE=0x2000; MEM_RELEASE=0x8000; PAGE_RWX=0x40

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

PID0, PID1 = 36700, 35028
BOT1_STEAM_ID = 76561198729640585

mem0 = DotaMemory(pid=PID0); mem1 = DotaMemory(pid=PID1)
gc0 = DotaGC(mem0); gc0.init()
gc1 = DotaGC(mem1); gc1.init()

gc0.leave_party(); gc1.leave_party()
time.sleep(2)

# Hook Retrieve on Bot0 (catch party_id) + Send on Bot1 (catch real accept)
gc0.hook_retrieve_message()
a1, l1, vt1 = install_send_hook(mem1, gc1)
time.sleep(0.3)

# Invite programmatically
print('[1] Bot0 inviting Bot1...')
gc0.invite_to_party(BOT1_STEAM_ID)

fields = gc0.wait_for_message(GC_MSG_INVITATION_CREATED, timeout=10, poll_interval=0.01)
party_id = fields.get(1, 0) if fields else 0
print(f'    party_id={party_id}')
gc0.unhook_retrieve_message()

print()
print('>>> Теперь ПРИМИ ИНВАЙТ на Bot1 (tqbao) вручную! Жду 60 сек...')
print()

prev1 = 0
start = time.time()
try:
    while time.time() - start < 60:
        hdr = mem1.pm.read_bytes(l1, 12)
        c = struct.unpack('<I', hdr[0:4])[0]
        if c > prev1:
            mt = struct.unpack('<I', hdr[4:8])[0] & 0x7FFFFFFF
            sz = struct.unpack('<I', hdr[8:12])[0]
            print(f'  [Bot1 SEND] #{c} type={mt} size={sz}B')
            
            if mt == 4503:
                data = mem1.pm.read_bytes(l1 + 0x10, min(sz, 0xF00))
                # Parse: [msg_type(4)][header_size(4)][header][body]
                buf_mt = struct.unpack('<I', data[0:4])[0]
                hdr_sz = struct.unpack('<I', data[4:8])[0]
                body = data[8+hdr_sz:sz]
                bf = _decode_protobuf(body)
                print(f'    REAL 4503 body fields:')
                for k,v in sorted(bf.items()):
                    if isinstance(v, int):
                        print(f'      f{k}={v} (0x{v:x})')
                    elif isinstance(v, bytes):
                        print(f'      f{k}=bytes({len(v)})')
                
                # Compare party_id
                real_pid = bf.get(1, 0)
                print(f'    Real party_id: {real_pid}')
                print(f'    Our  party_id: {party_id}')
                print(f'    Match: {real_pid == party_id}')
                
                # Save
                saved = {"msg_type": struct.unpack('<I', hdr[4:8])[0], "size": sz, "data_hex": data[:sz].hex()}
                with open("config/real_accept_4503.json", "w") as f:
                    json.dump(saved, f, indent=2)
                
                # Save Bot1's template
                tmpl = {"client_version": bf.get(3, 0)}
                if 8 in bf and isinstance(bf[8], bytes):
                    tmpl["ping_data_hex"] = bf[8].hex()
                with open("config/accept_template_bot1.json", "w") as f:
                    json.dump(tmpl, f, indent=2)
                print('    Saved to config/real_accept_4503.json + accept_template_bot1.json')
                break
            prev1 = c
        time.sleep(0.01)
    else:
        print('Timeout.')
finally:
    wr(mem1.pm.process_handle, gc1.gc_ptr, struct.pack('<Q', vt1))
    time.sleep(0.3)
    kernel32.VirtualFreeEx(mem1.pm.process_handle, a1, 0, MEM_RELEASE)
    print('Hooks removed.')
