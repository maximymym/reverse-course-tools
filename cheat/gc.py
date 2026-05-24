"""
Dota 2 GC (Game Coordinator) Message Sender — external via ISteamGameCoordinator shellcode.

Resolves ISteamGameCoordinator* via SteamAPI exports, then calls SendMessage/RetrieveMessage.
Manual protobuf encoding for matchmaking messages (no external protobuf lib needed).

Usage:
    from cheat.gc import DotaGC

    gc = DotaGC(mem)  # DotaMemory instance
    gc.init()

    gc.start_finding_match(game_modes=0x02, matchgroups=0x80)  # All Pick, Russia
    gc.stop_finding_match()
    gc.accept_match(lobby_id)  # XOR key computed internally from account_id
"""
import struct
import ctypes
import time
import os
import json
from .memory import DotaMemory

kernel32 = ctypes.windll.kernel32
for fn, argt, rest in [
    ("VirtualAllocEx", [ctypes.c_void_p, ctypes.c_ulonglong, ctypes.c_size_t, ctypes.c_ulong, ctypes.c_ulong], ctypes.c_ulonglong),
    ("VirtualFreeEx", [ctypes.c_void_p, ctypes.c_ulonglong, ctypes.c_size_t, ctypes.c_ulong], ctypes.c_bool),
    ("CreateRemoteThread", [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_ulonglong, ctypes.c_ulonglong, ctypes.c_ulong, ctypes.POINTER(ctypes.c_ulong)], ctypes.c_void_p),
    ("WaitForSingleObject", [ctypes.c_void_p, ctypes.c_ulong], ctypes.c_ulong),
    ("CloseHandle", [ctypes.c_void_p], ctypes.c_bool),
    ("WriteProcessMemory", [ctypes.c_void_p, ctypes.c_ulonglong, ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)], ctypes.c_bool),
]:
    getattr(kernel32, fn).argtypes = argt
    getattr(kernel32, fn).restype = rest

MEM_COMMIT = 0x1000; MEM_RESERVE = 0x2000; MEM_RELEASE = 0x8000; PAGE_RWX = 0x40

# ─── GC Message Types ───
GC_MSG_INVITE_TO_PARTY = 4501
GC_MSG_INVITATION_CREATED = 4502
GC_MSG_PARTY_INVITE_RESPONSE = 4503
GC_MSG_KICK_FROM_PARTY = 4504
GC_MSG_LEAVE_PARTY = 4505
GC_MSG_START_FINDING_MATCH = 7033
GC_MSG_START_FINDING_MATCH_RESULT = 7034
GC_MSG_STOP_FINDING_MATCH = 7036
GC_MSG_READY_UP = 7070
GC_MSG_READY_UP_STATUS = 7170
GC_MSG_CACHE_SUBSCRIBED = 24
GC_MSG_CLIENT_VERSION_UPDATED = 2528

PROTOBUF_FLAG = 0x80000000

# ─── Game Modes (bitmask) ───
class GameMode:
    ALL_PICK = 1 << 1
    RANDOM_DRAFT = 1 << 3
    SINGLE_DRAFT = 1 << 4
    ABILITY_DRAFT = 1 << 18
    TURBO = 1 << 23

# ─── Match Groups / Regions (bitmask) ───
class Region:
    US_WEST = 1 << 0
    US_EAST = 1 << 1
    EU_WEST = 1 << 2
    SE_ASIA = 1 << 3
    BRAZIL = 1 << 5
    ARGENTINA = 1 << 6
    RUSSIA = 1 << 7
    EU_EAST = 1 << 8
    AUSTRALIA = 1 << 9
    SOUTH_AFRICA = 1 << 10
    DUBAI = 1 << 13
    PERU = 1 << 15
    JAPAN = 1 << 19

# ─── DOTALobbyReadyState ───
READY_UNDECLARED = 0
READY_ACCEPTED = 1
READY_DECLINED = 2


# ─── Protobuf helpers (manual encoding, no external lib) ───

def _varint(value: int) -> bytes:
    """Encode unsigned varint."""
    result = bytearray()
    while value > 0x7F:
        result.append((value & 0x7F) | 0x80)
        value >>= 7
    result.append(value & 0x7F)
    return bytes(result)

def _pb_field_varint(field_num: int, value: int) -> bytes:
    """Protobuf field: varint wire type (0)."""
    tag = (field_num << 3) | 0  # wire type 0 = varint
    return _varint(tag) + _varint(value)

def _pb_field_fixed64(field_num: int, value: int) -> bytes:
    """Protobuf field: fixed64 wire type (1)."""
    tag = (field_num << 3) | 1  # wire type 1 = fixed64
    return _varint(tag) + struct.pack('<Q', value)

def _pb_field_bytes(field_num: int, data: bytes) -> bytes:
    """Protobuf field: length-delimited wire type (2)."""
    tag = (field_num << 3) | 2  # wire type 2 = length-delimited
    return _varint(tag) + _varint(len(data)) + data

def _pb_field_bool(field_num: int, value: bool) -> bytes:
    """Protobuf field: bool (varint 0/1)."""
    return _pb_field_varint(field_num, 1 if value else 0)


# ─── Protobuf decoders (minimal, for parsing GC responses) ───

def _decode_varint(data: bytes, offset: int) -> tuple[int, int]:
    """Decode varint at offset. Returns (value, new_offset)."""
    result = 0
    shift = 0
    while offset < len(data):
        b = data[offset]
        offset += 1
        result |= (b & 0x7F) << shift
        if not (b & 0x80):
            break
        shift += 7
    return result, offset

def _decode_protobuf(data: bytes) -> dict:
    """Decode protobuf message into {field_num: value} dict.
    Wire types: 0=varint, 1=fixed64, 2=bytes, 5=fixed32.
    Repeated fields: last value wins (sufficient for our use case).
    """
    fields = {}
    offset = 0
    while offset < len(data):
        tag, offset = _decode_varint(data, offset)
        field_num = tag >> 3
        wire_type = tag & 0x07
        if wire_type == 0:  # varint
            val, offset = _decode_varint(data, offset)
            fields[field_num] = val
        elif wire_type == 1:  # fixed64
            val = struct.unpack('<Q', data[offset:offset+8])[0]
            offset += 8
            fields[field_num] = val
        elif wire_type == 2:  # length-delimited
            length, offset = _decode_varint(data, offset)
            fields[field_num] = data[offset:offset+length]
            offset += length
        elif wire_type == 5:  # fixed32
            val = struct.unpack('<I', data[offset:offset+4])[0]
            offset += 4
            fields[field_num] = val
        else:
            break  # unknown wire type
    return fields


# ─── Message encoders ───

def encode_invite_to_party(steam_id: int, client_version: int = 0, ping_data: bytes = b'') -> bytes:
    """Encode CMsgInviteToParty: steam_id + client_version + ping_data.

    Args:
        steam_id: 64-bit Steam ID of the player to invite (fixed64 field 1)
        client_version: Dota client version (varint field 2), loaded from invite_template.json
        ping_data: CMsgClientPingData blob (bytes field 5), loaded from invite_template.json
    """
    body = _pb_field_fixed64(1, steam_id)
    if client_version:
        body += _pb_field_varint(2, client_version)
    if ping_data:
        body += _pb_field_bytes(5, ping_data)
    return body

def encode_party_invite_response(party_id: int, accept: bool,
                                  client_version: int = 0, ping_data: bytes = b'') -> bytes:
    """Encode CMsgPartyInviteResponse.

    Fields: party_id (f1 varint), accept (f2 bool), client_version (f3 varint), ping_data (f8 bytes).
    """
    body = b''
    body += _pb_field_varint(1, party_id)
    body += _pb_field_bool(2, accept)
    if client_version:
        body += _pb_field_varint(3, client_version)
    if ping_data:
        body += _pb_field_bytes(8, ping_data)
    return body

def encode_leave_party() -> bytes:
    """Encode CMsgLeaveParty: empty message."""
    return b''

def encode_kick_from_party(steam_id: int) -> bytes:
    """Encode CMsgKickFromParty: steam_id (field 1, fixed64)."""
    return _pb_field_fixed64(1, steam_id)

def encode_start_finding_match(matchgroups: int, game_modes: int,
                                client_version: int = 0) -> bytes:
    """Encode CMsgStartFindingMatch protobuf body.

    Fields captured from UI (2025-03-25):
      f2=matchgroups, f3=client_version, f4=game_modes,
      f6=0, f7=0, f10=2, f13=3, f14=0, f16=0, f17=0, f20=0, f22=30, f23=15
    """
    body = b''
    body += _pb_field_varint(2, matchgroups)
    if client_version:
        body += _pb_field_varint(3, client_version)
    body += _pb_field_varint(4, game_modes)
    body += _pb_field_varint(10, 2)   # match_type? (always 2 in UI)
    body += _pb_field_varint(13, 3)   # team_id? (always 3)
    body += _pb_field_varint(22, 30)  # unknown (always 30)
    body += _pb_field_varint(23, 15)  # unknown (always 15)
    return body

def encode_stop_finding_match() -> bytes:
    """Encode CMsgStopFindingMatch protobuf body."""
    return b''  # empty message

def encode_ready_up(ready_up_key: int, state: int = READY_ACCEPTED,
                     client_version: int = 0) -> bytes:
    """Encode CMsgReadyUp protobuf body.

    After 2026-03-25 update: cv (field 3) is required, otherwise GC silently rejects.
    """
    body = b''
    body += _pb_field_varint(1, state)          # state
    body += _pb_field_fixed64(2, ready_up_key)  # ready_up_key
    if client_version:
        body += _pb_field_varint(3, client_version)  # client_version
    return body

def compute_ready_up_key(lobby_id: int, account_id: int) -> int:
    """Compute ready_up_key from lobby_id and account_id.

    Formula from SteamKit issue #297:
        ready_up_key = lobby_id ^ (~(account_id | (account_id << 32)) & 0xFFFFFFFFFFFFFFFF)
    """
    mask = account_id | (account_id << 32)
    return lobby_id ^ ((~mask) & 0xFFFFFFFFFFFFFFFF)


def extract_group_id_from_cache_subscribed(fields: dict) -> int:
    """Extract group_id (party_id) from CMsgSOCacheSubscribed decoded fields.

    CacheSubscribed.objects (f2) contains SubscribedType:
      - f1 = type_id (2006 = CSODOTAPartyInvite)
      - f2 = object_data (serialized CSODOTAPartyInvite)
    CSODOTAPartyInvite.group_id = field 1 (varint).

    WARNING: field 3 of CacheSubscribed is `version`, NOT group_id!
    """
    if 2 not in fields or not isinstance(fields[2], bytes):
        return 0
    so = _decode_protobuf(fields[2])
    if 2 not in so or not isinstance(so[2], bytes):
        return 0
    invite = _decode_protobuf(so[2])
    return invite.get(1, 0)


def build_gc_message(msg_type: int, body: bytes) -> bytes:
    """Wrap protobuf body with full GC message format.

    Format: [msg_type_with_flag(4)][header_size(4)][CMsgProtoBufHeader(0B)][body]
    The msg_type with PROTOBUF_FLAG is included at the start of the data buffer.
    """
    msg_type_with_flag = msg_type | PROTOBUF_FLAG
    header = b''  # CMsgProtoBufHeader is empty for client→GC messages
    return struct.pack('<II', msg_type_with_flag, len(header)) + header + body


# ─── Helper ───

def _write_remote(process_handle, address, data):
    buf = ctypes.create_string_buffer(data)
    written = ctypes.c_size_t(0)
    return bool(kernel32.WriteProcessMemory(process_handle, address, buf, len(data), ctypes.byref(written)))

def _find_export(pm, module_name, export_name):
    """Find export function address from PE export table."""
    base = 0
    for mod in pm.list_modules():
        if mod.name.lower() == module_name.lower():
            base = mod.lpBaseOfDll
            break
    if not base:
        return None

    e_lfanew = struct.unpack("<I", pm.read_bytes(base + 0x3C, 4))[0]
    opt_hdr = base + e_lfanew + 0x18
    export_rva = struct.unpack("<I", pm.read_bytes(opt_hdr + 0x70, 4))[0]
    if export_rva == 0:
        return None
    export_dir = base + export_rva
    num_names = struct.unpack("<I", pm.read_bytes(export_dir + 0x18, 4))[0]
    addr_rva = struct.unpack("<I", pm.read_bytes(export_dir + 0x1C, 4))[0]
    name_rva = struct.unpack("<I", pm.read_bytes(export_dir + 0x20, 4))[0]
    ord_rva = struct.unpack("<I", pm.read_bytes(export_dir + 0x24, 4))[0]

    for i in range(min(num_names, 5000)):
        n_rva = struct.unpack("<I", pm.read_bytes(base + name_rva + i * 4, 4))[0]
        raw = pm.read_bytes(base + n_rva, len(export_name) + 1)
        name = raw.split(b'\x00')[0].decode('ascii', errors='replace')
        if name == export_name:
            ordinal = struct.unpack("<H", pm.read_bytes(base + ord_rva + i * 2, 2))[0]
            func_rva = struct.unpack("<I", pm.read_bytes(base + addr_rva + ordinal * 4, 4))[0]
            return base + func_rva
    return None


class DotaGC:
    """Game Coordinator message sender for Dota 2 (external, via shellcode)."""

    def __init__(self, mem: DotaMemory, steam_id: int = 0):
        self.mem = mem
        self.gc_ptr = 0  # ISteamGameCoordinator*
        self._steam_client_fn = 0
        self._get_user_fn = 0
        self._get_pipe_fn = 0
        self._initialized = False
        self.steam_id = steam_id
        self.account_id = steam_id & 0xFFFFFFFF if steam_id else 0

    def init(self) -> bool:
        """Resolve SteamAPI exports and get ISteamGameCoordinator pointer."""
        pm = self.mem.pm

        # Find SteamAPI exports
        self._steam_client_fn = _find_export(pm, "steam_api64.dll", "SteamClient")
        self._get_user_fn = _find_export(pm, "steam_api64.dll", "SteamAPI_GetHSteamUser")
        self._get_pipe_fn = _find_export(pm, "steam_api64.dll", "SteamAPI_GetHSteamPipe")

        if not all([self._steam_client_fn, self._get_user_fn, self._get_pipe_fn]):
            print("[!] SteamAPI exports not found")
            return False

        # Get ISteamGameCoordinator* via shellcode
        self.gc_ptr = self._resolve_gc_interface()
        if not self.gc_ptr:
            print("[!] Failed to resolve ISteamGameCoordinator")
            return False

        # Verify vtable is in steamclient64.dll
        vtable = self.mem.read_ptr(self.gc_ptr)
        sc_base = self.mem.module_base("steamclient64.dll")
        sc_size = self.mem.module_size("steamclient64.dll")
        send_fn = self.mem.read_ptr(vtable)
        if not (sc_base <= send_fn < sc_base + sc_size):
            print(f"[!] GC vtable[0] ({hex(send_fn)}) not in steamclient64.dll")
            return False

        self._initialized = True
        print(f"[+] ISteamGameCoordinator: {hex(self.gc_ptr)}")

        # Auto-resolve steam_id if not provided
        if not self.steam_id:
            self._resolve_steam_id()

        return True

    def _resolve_steam_id(self):
        """Shellcode: SteamAPI_SteamUser_v023() → SteamAPI_ISteamUser_GetSteamID().

        Uses flat C API exports from steam_api64.dll — no vtable guessing needed.
        """
        pm = self.mem.pm
        get_user_fn = _find_export(pm, "steam_api64.dll", "SteamAPI_SteamUser_v023")
        get_id_fn = _find_export(pm, "steam_api64.dll", "SteamAPI_ISteamUser_GetSteamID")
        if not get_user_fn or not get_id_fn:
            print(f"[!] SteamUser exports not found (user={hex(get_user_fn or 0)}, "
                  f"getId={hex(get_id_fn or 0)})")
            return

        handle = pm.process_handle
        alloc = kernel32.VirtualAllocEx(handle, 0, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_RWX)
        if not alloc:
            print("[!] Failed to alloc for _resolve_steam_id")
            return

        try:
            output_addr = alloc + 0x200
            _write_remote(handle, output_addr, b"\x00" * 8)

            sc = bytearray()
            sc += b'\x48\x83\xEC\x28'  # sub rsp, 0x28

            # SteamAPI_SteamUser_v023() → ISteamUser*
            sc += b'\x48\xB8' + struct.pack('<Q', get_user_fn)
            sc += b'\xFF\xD0'           # call rax
            sc += b'\x48\x85\xC0'      # test rax, rax
            sc += b'\x0F\x84' + b'\x00\x00\x00\x00'  # jz end (patch later)
            jz_offset = len(sc) - 4

            # SteamAPI_ISteamUser_GetSteamID(ISteamUser* self) → CSteamID (uint64)
            sc += b'\x48\x89\xC1'      # mov rcx, rax (this = ISteamUser*)
            sc += b'\x48\xB8' + struct.pack('<Q', get_id_fn)
            sc += b'\xFF\xD0'           # call rax

            # Store result
            sc += b'\x49\xBA' + struct.pack('<Q', output_addr)
            sc += b'\x49\x89\x02'      # mov [r10], rax

            # end:
            end_pos = len(sc)
            struct.pack_into('<i', sc, jz_offset, end_pos - jz_offset - 4)

            sc += b'\x48\x83\xC4\x28'  # add rsp, 0x28
            sc += b'\x33\xC0'          # xor eax, eax
            sc += b'\xC3'              # ret

            _write_remote(handle, alloc, bytes(sc))
            tid = ctypes.c_ulong(0)
            thread = kernel32.CreateRemoteThread(handle, None, 0, alloc, 0, 0, ctypes.byref(tid))
            if not thread:
                return
            kernel32.WaitForSingleObject(thread, 10000)
            kernel32.CloseHandle(thread)

            time.sleep(0.05)
            steam_id = struct.unpack('<Q', pm.read_bytes(output_addr, 8))[0]
            if steam_id:
                self.steam_id = steam_id
                self.account_id = steam_id & 0xFFFFFFFF
                print(f"[+] SteamID: {self.steam_id} (account_id: {self.account_id})")
            else:
                print("[!] Failed to resolve SteamID")
        finally:
            kernel32.VirtualFreeEx(handle, alloc, 0, MEM_RELEASE)

    def _resolve_gc_interface(self) -> int:
        """Shellcode: SteamClient() -> GetISteamGenericInterface("SteamGameCoordinator001")."""
        handle = self.mem.pm.process_handle
        alloc = kernel32.VirtualAllocEx(handle, 0, 0x2000, MEM_COMMIT | MEM_RESERVE, PAGE_RWX)
        if not alloc:
            return 0

        try:
            gc_str_addr = alloc + 0x200
            output_addr = alloc + 0x230
            _write_remote(handle, gc_str_addr, b"SteamGameCoordinator001\x00")
            _write_remote(handle, output_addr, b"\x00" * 8)

            sc = bytearray()
            sc += b'\x48\x83\xEC\x48'  # sub rsp, 0x48
            # SteamClient()
            sc += b'\x48\xB8' + struct.pack('<Q', self._steam_client_fn)
            sc += b'\xFF\xD0'
            sc += b'\x48\x89\xC7'      # mov rdi, rax (ISteamClient*)
            sc += b'\x48\x85\xFF'      # test rdi, rdi
            sc += b'\x0F\x84' + b'\x00' * 4  # jz end (patch later)
            jz_offset = len(sc) - 4
            # GetHSteamUser()
            sc += b'\x48\xB8' + struct.pack('<Q', self._get_user_fn)
            sc += b'\xFF\xD0'
            sc += b'\x89\xC6'          # mov esi, eax
            # GetHSteamPipe()
            sc += b'\x48\xB8' + struct.pack('<Q', self._get_pipe_fn)
            sc += b'\xFF\xD0'
            sc += b'\x41\x89\xC0'      # mov r8d, eax
            # GetISteamGenericInterface(this, hUser, hPipe, "SteamGameCoordinator001")
            sc += b'\x48\x89\xF9'      # mov rcx, rdi
            sc += b'\x89\xF2'          # mov edx, esi
            sc += b'\x49\xB9' + struct.pack('<Q', gc_str_addr)
            sc += b'\x48\x8B\x07'      # mov rax, [rdi] (vtable)
            sc += b'\xFF\x50\x60'      # call [rax+0x60]
            # Store result
            sc += b'\x49\xBA' + struct.pack('<Q', output_addr)
            sc += b'\x49\x89\x02'      # mov [r10], rax
            # end:
            end_pos = len(sc)
            struct.pack_into('<i', sc, jz_offset, end_pos - jz_offset - 4 + 4)  # fix jz offset
            # Hmm, the jz was at jz_offset which is relative to the next instruction
            # Let me recalculate: jz patches the 4 bytes at sc[jz_offset:jz_offset+4]
            # The jump target is at byte offset 'end_pos' from start of sc
            # The jz instruction ends at jz_offset+4, so rel32 = end_pos - (jz_offset + 4)
            struct.pack_into('<i', sc, jz_offset, end_pos - jz_offset - 4)

            sc += b'\x48\x83\xC4\x48'  # add rsp, 0x48
            sc += b'\x33\xC0'          # xor eax, eax
            sc += b'\xC3'              # ret

            _write_remote(handle, alloc, bytes(sc))
            tid = ctypes.c_ulong(0)
            thread = kernel32.CreateRemoteThread(handle, None, 0, alloc, 0, 0, ctypes.byref(tid))
            if not thread:
                return 0
            kernel32.WaitForSingleObject(thread, 10000)
            kernel32.CloseHandle(thread)

            time.sleep(0.05)
            return self.mem.read_ptr(output_addr)
        finally:
            kernel32.VirtualFreeEx(handle, alloc, 0, MEM_RELEASE)

    def send_gc_message(self, msg_type: int, body: bytes) -> bool:
        """Send a GC message via ISteamGameCoordinator::SendMessage shellcode.

        Args:
            msg_type: GC message type (e.g. 7033)
            body: Protobuf-encoded message body
        """
        if not self._initialized:
            raise RuntimeError("DotaGC not initialized")

        data = build_gc_message(msg_type, body)
        msg_type_with_flag = msg_type | PROTOBUF_FLAG

        handle = self.mem.pm.process_handle
        alloc = kernel32.VirtualAllocEx(handle, 0, 0x2000, MEM_COMMIT | MEM_RESERVE, PAGE_RWX)
        if not alloc:
            return False

        try:
            data_addr = alloc + 0x200
            _write_remote(handle, data_addr, data)

            # Shellcode: gc->SendMessage(msgType, data, size)
            # ISteamGameCoordinator::SendMessage(this, uint32 msgType, void* data, uint32 size)
            # RCX=this, EDX=msgType, R8=data, R9D=size
            sc = bytearray()
            sc += b'\x48\x83\xEC\x38'  # sub rsp, 0x38
            sc += b'\x48\xB9' + struct.pack('<Q', self.gc_ptr)         # mov rcx, gc_ptr
            sc += b'\xBA' + struct.pack('<I', msg_type_with_flag)      # mov edx, msgType
            sc += b'\x49\xB8' + struct.pack('<Q', data_addr)           # mov r8, data
            sc += b'\x41\xB9' + struct.pack('<I', len(data))           # mov r9d, size
            sc += b'\x48\x8B\x01'                                     # mov rax, [rcx] (vtable)
            sc += b'\xFF\x10'                                          # call [rax] (vtable[0] = SendMessage)
            sc += b'\x48\x83\xC4\x38'                                 # add rsp, 0x38
            sc += b'\xC3'                                              # ret

            _write_remote(handle, alloc, bytes(sc))
            tid = ctypes.c_ulong(0)
            thread = kernel32.CreateRemoteThread(handle, None, 0, alloc, 0, 0, ctypes.byref(tid))
            if not thread:
                return False
            kernel32.WaitForSingleObject(thread, 5000)
            kernel32.CloseHandle(thread)
            return True
        finally:
            kernel32.VirtualFreeEx(handle, alloc, 0, MEM_RELEASE)

    def send_raw_gc_message(self, msg_type_with_flag: int, raw_data: bytes) -> bool:
        """Send a raw GC message (data already includes header+body, msg_type already has flags)."""
        if not self._initialized:
            raise RuntimeError("DotaGC not initialized")

        handle = self.mem.pm.process_handle
        alloc = kernel32.VirtualAllocEx(handle, 0, 0x2000, MEM_COMMIT | MEM_RESERVE, PAGE_RWX)
        if not alloc:
            return False

        try:
            data_addr = alloc + 0x200
            _write_remote(handle, data_addr, raw_data)

            sc = bytearray()
            sc += b'\x48\x83\xEC\x38'
            sc += b'\x48\xB9' + struct.pack('<Q', self.gc_ptr)
            sc += b'\xBA' + struct.pack('<I', msg_type_with_flag)
            sc += b'\x49\xB8' + struct.pack('<Q', data_addr)
            sc += b'\x41\xB9' + struct.pack('<I', len(raw_data))
            sc += b'\x48\x8B\x01'
            sc += b'\xFF\x10'
            sc += b'\x48\x83\xC4\x38'
            sc += b'\xC3'

            _write_remote(handle, alloc, bytes(sc))
            tid = ctypes.c_ulong(0)
            thread = kernel32.CreateRemoteThread(handle, None, 0, alloc, 0, 0, ctypes.byref(tid))
            if not thread:
                return False
            kernel32.WaitForSingleObject(thread, 5000)
            kernel32.CloseHandle(thread)
            return True
        finally:
            kernel32.VirtualFreeEx(handle, alloc, 0, MEM_RELEASE)

    # ─── Record / Replay Search ───

    def record_search(self, timeout: float = 60) -> dict | None:
        """Install VMT hook, wait for user to click Find Match, capture the raw message.

        Returns: {"msg_type": int, "size": int, "data_hex": str} or None on timeout.
        The hook is automatically removed after capture or timeout.
        """
        if not self._initialized:
            raise RuntimeError("DotaGC not initialized")

        handle = self.mem.pm.process_handle
        orig_vtable_ptr = self.mem.read_ptr(self.gc_ptr)
        orig_send = self.mem.read_ptr(orig_vtable_ptr)
        orig_avail = self.mem.read_ptr(orig_vtable_ptr + 8)
        orig_retr = self.mem.read_ptr(orig_vtable_ptr + 16)

        alloc = kernel32.VirtualAllocEx(handle, 0, 0x4000, MEM_COMMIT | MEM_RESERVE, PAGE_RWX)
        if not alloc:
            return None

        new_vt = alloc
        hook_addr = alloc + 0x100
        log_base = alloc + 0x1000
        _write_remote(handle, log_base, b'\x00' * 0x1000)

        # Hook shellcode: save params → copy to log → call original → return
        sc = bytearray()
        sc += b'\x48\x83\xEC\x48'
        sc += b'\x48\x89\x4C\x24\x20'; sc += b'\x89\x54\x24\x28'
        sc += b'\x4C\x89\x44\x24\x30'; sc += b'\x44\x89\x4C\x24\x38'
        sc += b'\x49\xBA' + struct.pack('<Q', log_base + 4); sc += b'\x41\x89\x12'
        sc += b'\x49\xBA' + struct.pack('<Q', log_base + 8); sc += b'\x45\x89\x0A'
        sc += b'\x57\x56'
        sc += b'\x48\xBF' + struct.pack('<Q', log_base + 0x10)
        sc += b'\x4C\x89\xC6'; sc += b'\x44\x89\xC9'
        sc += b'\x81\xF9\x00\x0F\x00\x00'; sc += b'\x76\x05'; sc += b'\xB9\x00\x0F\x00\x00'
        sc += b'\xF3\xA4'; sc += b'\x5E\x5F'
        sc += b'\x49\xBA' + struct.pack('<Q', log_base); sc += b'\xF0\x41\xFF\x02'
        sc += b'\x48\x8B\x4C\x24\x20'; sc += b'\x8B\x54\x24\x28'
        sc += b'\x4C\x8B\x44\x24\x30'; sc += b'\x44\x8B\x4C\x24\x38'
        sc += b'\x48\xB8' + struct.pack('<Q', orig_send); sc += b'\xFF\xD0'
        sc += b'\x48\x83\xC4\x48'; sc += b'\xC3'

        _write_remote(handle, hook_addr, bytes(sc))
        _write_remote(handle, new_vt, struct.pack('<QQQ', hook_addr, orig_avail, orig_retr))
        _write_remote(handle, self.gc_ptr, struct.pack('<Q', new_vt))

        try:
            prev = 0
            start = time.time()
            while time.time() - start < timeout:
                count = struct.unpack('<I', self.mem.pm.read_bytes(log_base, 4))[0]
                if count > prev:
                    mt = struct.unpack('<I', self.mem.pm.read_bytes(log_base + 4, 4))[0]
                    sz = struct.unpack('<I', self.mem.pm.read_bytes(log_base + 8, 4))[0]
                    if (mt & 0x7FFFFFFF) == GC_MSG_START_FINDING_MATCH:
                        raw = self.mem.pm.read_bytes(log_base + 0x10, min(sz, 0xF00))
                        return {"msg_type": mt, "size": sz, "data_hex": raw.hex()}
                    prev = count
                time.sleep(0.2)
            return None
        finally:
            _write_remote(handle, self.gc_ptr, struct.pack('<Q', orig_vtable_ptr))
            time.sleep(0.3)
            kernel32.VirtualFreeEx(handle, alloc, 0, MEM_RELEASE)

    def replay_search(self, saved: dict = None, config_path: str = None) -> bool:
        """Replay a previously recorded search message.

        Args:
            saved: dict from record_search() with msg_type, size, data_hex
            config_path: path to last_search.json (used if saved is None)
        """
        if saved is None:
            import json
            if config_path is None:
                config_path = os.path.join(os.path.dirname(__file__), "..", "config", "last_search.json")
            with open(config_path, "r") as f:
                saved = json.load(f)

        raw_data = bytes.fromhex(saved["data_hex"])
        msg_type = saved["msg_type"]
        return self.send_raw_gc_message(msg_type, raw_data)

    # ─── RetrieveMessage Hook (capture incoming GC messages) ───

    def hook_retrieve_message(self) -> int:
        """Install VMT hook on RetrieveMessage (vtable[2]) to log incoming GC messages.

        Returns alloc address (needed for unhook/read), or 0 on failure.
        Layout at log_base (alloc+0x2000):
            +0x00: uint32 total_count (atomic increment)
            +0x04: uint32 last_msg_type
            +0x08: uint32 last_msg_size
            +0x10: last message body (up to 0xF00 bytes)

        NOTE: RetrieveMessage buffer format = [msg_type(4)][header_size(4)][header][body]
              (msg_type is INSIDE the buffer, unlike SendMessage)
        """
        if not self._initialized:
            raise RuntimeError("DotaGC not initialized")

        handle = self.mem.pm.process_handle
        orig_vtable_ptr = self.mem.read_ptr(self.gc_ptr)
        orig_send = self.mem.read_ptr(orig_vtable_ptr)        # vtable[0]
        orig_avail = self.mem.read_ptr(orig_vtable_ptr + 8)   # vtable[1] IsMessageAvailable
        orig_retr = self.mem.read_ptr(orig_vtable_ptr + 16)   # vtable[2] RetrieveMessage

        alloc = kernel32.VirtualAllocEx(handle, 0, 0x4000, MEM_COMMIT | MEM_RESERVE, PAGE_RWX)
        if not alloc:
            return 0

        new_vt = alloc                # fake vtable (3 qwords)
        hook_code = alloc + 0x100     # hook shellcode
        log_base = alloc + 0x2000     # log area

        # Zero out log area
        _write_remote(handle, log_base, b'\x00' * 0x2000)

        # RetrieveMessage hook — flat stack, only volatile registers.
        #
        # Signature: EGCResults RetrieveMessage(this=RCX, uint32* msgType=RDX,
        #                                       void* buf=R8, uint32 bufSize=R9,
        #                                       uint32* msgSize=[rsp+0x28])
        #
        # Strategy: save params → forward call to original → on success log → ret.
        # Only uses volatile registers (rax, rcx, rdx, r8, r9, r10, r11).
        #
        # Stack frame = 0x78 bytes:
        #   [rsp+0x00..0x1F] = shadow space for call
        #   [rsp+0x20]       = 5th param forwarded
        #   [rsp+0x28]       = saved RDX (msgType*)
        #   [rsp+0x30]       = saved R8 (buf)
        #   [rsp+0x38]       = saved msgSize*
        #   [rsp+0x40]       = saved return value
        #
        # Caller's 5th param: [rsp+0x78+0x28] = [rsp+0xA0]

        sc = bytearray()
        sc += b'\x48\x83\xEC\x78'                     # sub rsp, 0x78

        # Save params we need after the call
        sc += b'\x48\x89\x54\x24\x28'                 # mov [rsp+0x28], rdx   (msgType*)
        sc += b'\x4C\x89\x44\x24\x30'                 # mov [rsp+0x30], r8    (buf)

        # Load & forward 5th param
        sc += b'\x48\x8B\x84\x24\xA0\x00\x00\x00'    # mov rax, [rsp+0xA0]   (msgSize*)
        sc += b'\x48\x89\x44\x24\x38'                 # mov [rsp+0x38], rax   (save)
        sc += b'\x48\x89\x44\x24\x20'                 # mov [rsp+0x20], rax   (forward)

        # Call original (rcx, rdx, r8, r9 already set by caller)
        sc += b'\x48\xB8' + struct.pack('<Q', orig_retr)
        sc += b'\xFF\xD0'                              # call rax

        # Save return value
        sc += b'\x89\x44\x24\x40'                     # mov [rsp+0x40], eax

        # If result != 0, skip logging
        sc += b'\x85\xC0'                              # test eax, eax
        sc += b'\x0F\x85' + b'\x00\x00\x00\x00'       # jnz skip (patch later)
        jnz_patch = len(sc) - 4

        # ── Atomic increment count ──
        sc += b'\x49\xBA' + struct.pack('<Q', log_base)
        sc += b'\xF0\x41\xFF\x02'                      # lock inc dword [r10]

        # ── Store msgType from *msgType_ptr ──
        sc += b'\x48\x8B\x44\x24\x28'                 # mov rax, [rsp+0x28]  (msgType*)
        sc += b'\x8B\x00'                              # mov eax, [rax]
        sc += b'\x41\x89\x42\x04'                      # mov [r10+4], eax

        # ── Store msgSize from *msgSize_ptr ──
        sc += b'\x48\x8B\x44\x24\x38'                 # mov rax, [rsp+0x38]  (msgSize*)
        sc += b'\x8B\x08'                              # mov ecx, [rax]       (size)
        sc += b'\x41\x89\x4A\x08'                      # mov [r10+8], ecx

        # ── Copy min(size, 0xF00) bytes: buf → log+0x10 ──
        # Uses only rax (dst), rdx (src), ecx (count), r8b (tmp byte)
        sc += b'\x81\xF9\x00\x0F\x00\x00'             # cmp ecx, 0xF00
        sc += b'\x76\x05'                              # jbe size_ok
        sc += b'\xB9\x00\x0F\x00\x00'                 # mov ecx, 0xF00
        # size_ok:
        sc += b'\x48\x8B\x54\x24\x30'                 # mov rdx, [rsp+0x30]  (src = buf)
        sc += b'\x49\x8D\x42\x10'                      # lea rax, [r10+0x10]  (dst = log+0x10)
        sc += b'\x85\xC9'                              # test ecx, ecx
        sc += b'\x74\x10'                              # jz done_copy (16 bytes ahead)
        # copy_loop:                                   # 14 bytes + jnz(2) = 16 total
        sc += b'\x44\x8A\x02'                          # mov r8b, [rdx]       (3)
        sc += b'\x44\x88\x00'                          # mov [rax], r8b       (3)
        sc += b'\x48\xFF\xC2'                          # inc rdx              (3)
        sc += b'\x48\xFF\xC0'                          # inc rax              (3)
        sc += b'\xFF\xC9'                              # dec ecx              (2)
        sc += b'\x75\xF0'                              # jnz copy_loop (-16)  (2)
        # done_copy:

        # skip:
        skip_pos = len(sc)
        struct.pack_into('<i', sc, jnz_patch, skip_pos - jnz_patch - 4)

        # Restore return value and return
        sc += b'\x8B\x44\x24\x40'                     # mov eax, [rsp+0x40]
        sc += b'\x48\x83\xC4\x78'                     # add rsp, 0x78
        sc += b'\xC3'                                  # ret

        _write_remote(handle, hook_code, bytes(sc))

        # Write fake vtable: [SendMessage_orig, IsMessageAvailable_orig, hook_code]
        _write_remote(handle, new_vt, struct.pack('<QQQ', orig_send, orig_avail, hook_code))

        # Swap vtable pointer
        _write_remote(handle, self.gc_ptr, struct.pack('<Q', new_vt))

        self._retrieve_hook_alloc = alloc
        self._retrieve_hook_log = log_base
        self._retrieve_hook_orig_vt = orig_vtable_ptr
        print(f"[+] RetrieveMessage hook installed @ {hex(hook_code)}, log @ {hex(log_base)}")
        return alloc

    def unhook_retrieve_message(self):
        """Remove RetrieveMessage hook, restore original vtable."""
        if not hasattr(self, '_retrieve_hook_alloc') or not self._retrieve_hook_alloc:
            return
        handle = self.mem.pm.process_handle
        _write_remote(handle, self.gc_ptr, struct.pack('<Q', self._retrieve_hook_orig_vt))
        time.sleep(0.3)
        kernel32.VirtualFreeEx(handle, self._retrieve_hook_alloc, 0, MEM_RELEASE)
        self._retrieve_hook_alloc = 0
        print("[+] RetrieveMessage hook removed")

    def read_hook_log(self) -> dict:
        """Read current state of the RetrieveMessage hook log.

        Returns dict with:
            count: total messages intercepted
            last_msg_type: last message type (without protobuf flag)
            last_msg_size: last message size

        NOTE: RetrieveMessage buffer format = [msg_type(4)][header_size(4)][header][body]
              The msg_type in the buffer is the authoritative one (log+4 is from *msgType param).
        """
        if not hasattr(self, '_retrieve_hook_log') or not self._retrieve_hook_log:
            return {}
        log = self._retrieve_hook_log
        header = self.mem.pm.read_bytes(log, 0x10)
        count = struct.unpack('<I', header[0:4])[0]
        msg_type = struct.unpack('<I', header[4:8])[0] & 0x7FFFFFFF
        msg_size = struct.unpack('<I', header[8:12])[0]
        return {
            "count": count,
            "last_msg_type": msg_type,
            "last_msg_size": msg_size,
        }

    def read_last_message_raw(self, max_size: int = 0xF00) -> bytes:
        """Read raw buffer of the last intercepted RetrieveMessage.

        Buffer format: [msg_type(4)][header_size(4)][header(N)][body(M)]
        """
        if not hasattr(self, '_retrieve_hook_log') or not self._retrieve_hook_log:
            return b''
        log = self._retrieve_hook_log
        size = struct.unpack('<I', self.mem.pm.read_bytes(log + 8, 4))[0]
        size = min(size, max_size)
        if size == 0:
            return b''
        return self.mem.pm.read_bytes(log + 0x10, size)

    def parse_retrieve_buffer(self, raw: bytes) -> tuple[int, dict]:
        """Parse RetrieveMessage buffer into (msg_type, protobuf_fields).

        Buffer format: [msg_type(4)][header_size(4)][header][body]
        Returns (msg_type_clean, decoded_fields) or (0, {}) on failure.
        """
        if len(raw) < 8:
            return 0, {}
        msg_type = struct.unpack('<I', raw[0:4])[0] & 0x7FFFFFFF
        hdr_size = struct.unpack('<I', raw[4:8])[0]
        body = raw[8 + hdr_size:]
        if not body:
            return msg_type, {}
        return msg_type, _decode_protobuf(body)

    def wait_for_message(self, target_msg_type: int, timeout: float = 60, poll_interval: float = 0.2) -> dict | None:
        """Wait for a specific GC message type. Returns parsed protobuf fields or None.

        The hook must be installed first.
        """
        if not hasattr(self, '_retrieve_hook_log') or not self._retrieve_hook_log:
            raise RuntimeError("RetrieveMessage hook not installed")

        log = self._retrieve_hook_log
        prev_count = struct.unpack('<I', self.mem.pm.read_bytes(log, 4))[0]
        start = time.time()
        while time.time() - start < timeout:
            count = struct.unpack('<I', self.mem.pm.read_bytes(log, 4))[0]
            if count > prev_count:
                raw = self.read_last_message_raw()
                mt, fields = self.parse_retrieve_buffer(raw)
                if mt == target_msg_type:
                    return fields
                prev_count = count
            time.sleep(poll_interval)
        return None

    def wait_for_ready_up(self, timeout: float = 90, poll_interval: float = 0.3) -> int:
        """Wait for ReadyUpStatus (7170) and extract lobby_id.

        The hook must be installed first.
        Returns lobby_id, or 0 on timeout.
        """
        fields = self.wait_for_message(GC_MSG_READY_UP_STATUS, timeout, poll_interval)
        if fields and 1 in fields:
            return fields[1]  # lobby_id (fixed64)
        return 0

    # ─── High-level API ───

    def invite_to_party(self, steam_id: int) -> bool:
        """Invite a player to party by Steam ID (64-bit).

        Sends CMsgInviteToParty with steam_id + client_version (no ping_data).
        UI invites are minimal (20B); ping_data is only needed for accept.

        Args:
            steam_id: 64-bit Steam ID (e.g. 76561198725850781)
        """
        cv, _ = self._load_invite_template()
        body = encode_invite_to_party(steam_id, cv, b'')  # no ping for invite
        print(f"[>] InviteToParty steam_id={steam_id} cv={cv}")
        return self.send_gc_message(GC_MSG_INVITE_TO_PARTY, body)

    def _load_invite_template(self) -> tuple[int, bytes]:
        """Load client_version + ping_data from invite_template.json."""
        template_path = os.path.join(os.path.dirname(__file__), "..", "config", "invite_template.json")
        try:
            with open(template_path, "r") as f:
                tmpl = json.load(f)
            cv = tmpl.get("client_version", 0)
            pd = bytes.fromhex(tmpl.get("ping_data_hex", ""))
            return cv, pd
        except FileNotFoundError:
            return 0, b''

    def auto_detect_cv(self, save: bool = True, timeout: float = 15) -> int:
        """Auto-detect Dota client_version by capturing outgoing GC messages.

        Hooks SendMessage briefly, waits for any outgoing message (the game sends
        MatchmakingStatsRequest every ~10s), and extracts the client_version varint
        from the GC message header/body.

        Falls back to reading invite_template.json if no messages captured.
        """
        import time as _time

        # Hook SendMessage to capture outgoing traffic
        handle = self.mem.pm.process_handle
        orig_vt = self.mem.read_ptr(self.gc_ptr)
        orig_send = self.mem.read_ptr(orig_vt)
        orig_avail = self.mem.read_ptr(orig_vt + 8)
        orig_retr = self.mem.read_ptr(orig_vt + 16)

        alloc = kernel32.VirtualAllocEx(handle, 0, 0x4000,
                                        MEM_COMMIT | MEM_RESERVE, PAGE_RWX)
        if not alloc:
            print("[!] auto_detect_cv: alloc failed")
            cv, _ = self._load_invite_template()
            return cv

        log_addr = alloc + 0x1000
        _write_remote(handle, log_addr, b'\x00' * 0x2000)

        # Build SendMessage hook shellcode (same as test_party2)
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
        _write_remote(handle, self.gc_ptr, struct.pack('<Q', alloc))

        cv = 0
        try:
            start = _time.time()
            while _time.time() - start < timeout:
                hdr = self.mem.pm.read_bytes(log_addr, 12)
                count = struct.unpack('<I', hdr[0:4])[0]
                if count > 0:
                    mt = struct.unpack('<I', hdr[4:8])[0]
                    sz = struct.unpack('<I', hdr[8:12])[0]
                    mt_clean = mt & 0x7FFFFFFF
                    if sz > 8 and mt_clean in (4501, 4503, 7033, 7070):
                        # These messages contain client_version
                        raw = self.mem.pm.read_bytes(log_addr + 0x10, min(sz, 0xF00))
                        gc_hdr_sz = struct.unpack('<I', raw[4:8])[0]
                        body = raw[8 + gc_hdr_sz:]
                        fields = _decode_protobuf(body)
                        # cv is typically field 2 (invite) or field 3 (response)
                        for fid in (2, 3):
                            val = fields.get(fid, 0)
                            if isinstance(val, int) and 6700 <= val <= 7200:
                                cv = val
                                break
                        if cv:
                            break
                    # Clear log for next message
                    _write_remote(handle, log_addr, b'\x00' * 12)
                _time.sleep(0.5)
        finally:
            _write_remote(handle, self.gc_ptr, struct.pack('<Q', orig_vt))
            kernel32.VirtualFreeEx(handle, alloc, 0, MEM_RELEASE)

        if not cv:
            cv, _ = self._load_invite_template()
            if cv:
                print(f"[*] auto_detect_cv: no outgoing msg captured, using template cv={cv}")
            return cv

        print(f"[+] Auto-detected client_version={cv}")
        if save:
            template_path = os.path.join(os.path.dirname(__file__), "..", "config", "invite_template.json")
            try:
                with open(template_path, "r") as f:
                    tmpl = json.load(f)
            except FileNotFoundError:
                tmpl = {}
            old_cv = tmpl.get("client_version", 0)
            if cv != old_cv:
                tmpl["client_version"] = cv
                with open(template_path, "w") as f:
                    json.dump(tmpl, f, indent=2)
                print(f"[+] Updated invite_template.json: {old_cv} -> {cv}")
        return cv

    def accept_party_invite(self, party_id: int) -> bool:
        """Accept a party invite via GC 4503.

        Args:
            party_id: group_id from CSODOTAPartyInvite inside CacheSubscribed.
                      Use extract_group_id_from_cache_subscribed() to get it.
        """
        cv, pd = self._load_invite_template()
        body = encode_party_invite_response(party_id, accept=True,
                                            client_version=cv, ping_data=pd)
        print(f"[>] AcceptPartyInvite party_id={party_id} cv={cv} ping={len(pd)}B")
        return self.send_gc_message(GC_MSG_PARTY_INVITE_RESPONSE, body)

    def accept_party_invite_native(self, party_id: int) -> bool:
        """Accept party invite via native client.dll handler (shellcode).

        Calls DOTAAcceptPartyInvite handler directly — both joins party AND closes popup.
        Unlike GC accept, this goes through the same path as a real UI button click.

        RVAs verified 2026-03-25 via Frida trace + capstone disasm.
        """
        import ctypes
        import struct as st

        HANDLER_OBJ_RVA = 0x65599b0   # static DOTAAcceptPartyInvite singleton
        ACCEPT_FUNC_RVA = 0x2daaa10   # sub-handler (rcx=handler, rdx=party_id)

        client_base = self.mem.module_base("client.dll")
        if not client_base:
            print("[!] client.dll not found")
            return False

        handler_obj = client_base + HANDLER_OBJ_RVA
        target_func = client_base + ACCEPT_FUNC_RVA

        # shellcode: mov rcx,handler; mov rdx,party_id; sub rsp,0x28; mov rax,func; call rax; add rsp,0x28; ret
        sc = b''
        sc += b'\x48\xB9' + st.pack('<Q', handler_obj)
        sc += b'\x48\xBA' + st.pack('<Q', party_id)
        sc += b'\x48\x83\xEC\x28'
        sc += b'\x48\xB8' + st.pack('<Q', target_func)
        sc += b'\xFF\xD0'
        sc += b'\x48\x83\xC4\x28'
        sc += b'\xC3'

        kernel32 = ctypes.windll.kernel32
        handle = self.mem.pm.process_handle
        alloc = kernel32.VirtualAllocEx(handle, 0, len(sc), 0x3000, 0x40)
        if not alloc:
            print("[!] VirtualAllocEx failed")
            return False

        try:
            kernel32.WriteProcessMemory(handle, alloc, sc, len(sc), None)
            tid = ctypes.c_ulong()
            thread = kernel32.CreateRemoteThread(handle, None, 0, alloc, 0, 0, ctypes.byref(tid))
            if not thread:
                print("[!] CreateRemoteThread failed")
                return False
            result = kernel32.WaitForSingleObject(thread, 5000)
            kernel32.CloseHandle(thread)
            ok = result == 0
            if ok:
                print(f"[+] Native accept: party_id={party_id}")
            return ok
        finally:
            kernel32.VirtualFreeEx(handle, alloc, 0, 0x8000)

    def decline_party_invite(self, party_id: int) -> bool:
        """Decline a party invite."""
        cv, pd = self._load_invite_template()
        body = encode_party_invite_response(party_id, accept=False,
                                            client_version=cv, ping_data=pd)
        return self.send_gc_message(GC_MSG_PARTY_INVITE_RESPONSE, body)

    def wait_and_accept_invite(self, timeout: float = 30) -> bool:
        """Wait for party invite and accept it via GC.

        Hooks RetrieveMessage, waits for CacheSubscribed with invite SO,
        extracts group_id, sends CMsgPartyInviteResponse with accept=True.
        Fully programmatic — no Panorama JS or window focus needed.

        Returns True if invite was received and accept was sent.
        """
        self.hook_retrieve_message()
        import time
        start = time.time()
        prev = 0
        party_id = 0

        try:
            while time.time() - start < timeout:
                log = self.read_hook_log()
                if log['count'] > prev:
                    raw = self.read_last_message_raw()
                    mt, fields = self.parse_retrieve_buffer(raw)
                    if mt == GC_MSG_CACHE_SUBSCRIBED:
                        party_id = extract_group_id_from_cache_subscribed(fields)
                        if party_id:
                            print(f"[+] Invite received, group_id={party_id}")
                            break
                    prev = log['count']
                time.sleep(0.3)
        finally:
            self.unhook_retrieve_message()

        if not party_id:
            return False

        time.sleep(1)  # let popup render (cosmetic)
        return self.accept_party_invite(party_id)

    def leave_party(self) -> bool:
        """Leave current party."""
        body = encode_leave_party()
        print("[>] LeaveParty")
        return self.send_gc_message(GC_MSG_LEAVE_PARTY, body)

    def kick_from_party(self, steam_id: int) -> bool:
        """Kick a player from party."""
        body = encode_kick_from_party(steam_id)
        return self.send_gc_message(GC_MSG_KICK_FROM_PARTY, body)

    def start_finding_match(self, game_modes: int = GameMode.ALL_PICK,
                            matchgroups: int = Region.RUSSIA) -> bool:
        """Start matchmaking queue."""
        cv, _ = self._load_invite_template()
        body = encode_start_finding_match(matchgroups, game_modes, cv)
        print(f"[>] StartFindingMatch region={matchgroups} mode={game_modes} cv={cv}")
        return self.send_gc_message(GC_MSG_START_FINDING_MATCH, body)

    def stop_finding_match(self) -> bool:
        """Cancel matchmaking queue."""
        body = encode_stop_finding_match()
        return self.send_gc_message(GC_MSG_STOP_FINDING_MATCH, body)

    def accept_match(self, lobby_id: int) -> bool:
        """Accept a found match. Computes ready_up_key from lobby_id ^ ~(account_id|account_id<<32)."""
        if not self.account_id:
            raise RuntimeError("account_id unknown — pass steam_id to DotaGC() or call init()")
        cv, _ = self._load_invite_template()
        key = compute_ready_up_key(lobby_id, self.account_id)
        print(f"[>] AcceptMatch lobby={hex(lobby_id)} key={hex(key)} account_id={self.account_id} cv={cv}")
        body = encode_ready_up(key, READY_ACCEPTED, client_version=cv)
        return self.send_gc_message(GC_MSG_READY_UP, body)

    def decline_match(self, lobby_id: int) -> bool:
        """Decline a found match. Computes ready_up_key from lobby_id."""
        if not self.account_id:
            raise RuntimeError("account_id unknown — pass steam_id to DotaGC() or call init()")
        cv, _ = self._load_invite_template()
        key = compute_ready_up_key(lobby_id, self.account_id)
        print(f"[>] DeclineMatch lobby={hex(lobby_id)} key={hex(key)}")
        body = encode_ready_up(key, READY_DECLINED, client_version=cv)
        return self.send_gc_message(GC_MSG_READY_UP, body)

    def accept_party_via_keypress(self, acceptor_pid: int) -> bool:
        """Accept party invite by sending Enter key to the Dota window.

        The invite popup has defaultfocus="AcceptButton", so Enter activates it.
        Console must be hidden first (hideconsole).

        Args:
            acceptor_pid: PID of the Dota process that received the invite
        """
        import ctypes
        import ctypes.wintypes

        user32 = ctypes.windll.user32
        WNDENUMPROC = ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.wintypes.HWND, ctypes.wintypes.LPARAM)

        windows = []
        def enum_cb(hwnd, lparam):
            pid = ctypes.wintypes.DWORD()
            user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
            if pid.value == acceptor_pid and user32.IsWindowVisible(hwnd):
                length = user32.GetWindowTextLengthW(hwnd)
                if length > 0:
                    buf = ctypes.create_unicode_buffer(length + 1)
                    user32.GetWindowTextW(hwnd, buf, length + 1)
                    if 'Dota' in buf.value:
                        windows.append(hwnd)
            return True

        user32.EnumWindows(WNDENUMPROC(enum_cb), 0)
        if not windows:
            print(f"[!] No Dota window found for PID {acceptor_pid}")
            return False

        hwnd = windows[0]
        user32.SetForegroundWindow(hwnd)
        time.sleep(0.3)

        WM_KEYDOWN, WM_KEYUP, VK_RETURN = 0x0100, 0x0101, 0x0D
        user32.PostMessageW(hwnd, WM_KEYDOWN, VK_RETURN, 0x001C0001)
        time.sleep(0.05)
        user32.PostMessageW(hwnd, WM_KEYUP, VK_RETURN, 0xC01C0001)
        print(f"[+] Sent Enter to Dota window (PID {acceptor_pid})")
        return True

    def auto_party(self, inviter_gc: 'DotaGC', invitee_steam_id: int,
                    invitee_pid: int, invitee_cmd: 'DotaCommands' = None) -> bool:
        """Full auto party: invite + accept via keypress.

        Args:
            inviter_gc: DotaGC of the inviting bot (self can be same or different)
            invitee_steam_id: Steam ID of the bot to invite
            invitee_pid: PID of the invitee's Dota process
            invitee_cmd: DotaCommands of invitee (to hide console), optional
        """
        # Leave existing parties
        self.leave_party()
        inviter_gc.leave_party()
        time.sleep(2)

        # Send invite
        inviter_gc.hook_retrieve_message()
        inviter_gc.invite_to_party(invitee_steam_id)

        # Catch party_id from inviter's 4502
        fields = inviter_gc.wait_for_message(GC_MSG_INVITATION_CREATED,
                                              timeout=10, poll_interval=0.01)
        party_id = fields.get(1, 0) if fields else 0
        inviter_gc.unhook_retrieve_message()

        if not party_id:
            print("[!] No InvitationCreated received")
            return False
        print(f"[+] party_id={party_id}")

        time.sleep(2)

        # Hide console on invitee
        if invitee_cmd:
            invitee_cmd.execute('hideconsole')
            time.sleep(0.5)

        # Accept via Enter keypress
        return self.accept_party_via_keypress(invitee_pid)

    def auto_accept_match(self, timeout: float = 90) -> bool:
        """Wait for ReadyUpStatus, then auto-accept. Hook must be installed.

        Returns True if match was accepted, False on timeout.
        """
        key = self.wait_for_ready_up(timeout=timeout)
        if key:
            return self.accept_match(key)
        print("[!] Timeout waiting for match")
        return False
