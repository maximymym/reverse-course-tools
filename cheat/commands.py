"""
Dota 2 Console Command Injection + PrepareUnitOrders + Panorama JS — external via shellcode.

Four methods available:
1. execute_command() — inject console command via CCommandBuffer::AddText + CreateRemoteThread
2. ConVarSystem — direct memory read/write for ConVars (no code injection needed)
3. PrepareUnitOrders — native unit order injection (move, attack, cast, buy)
4. run_panorama_js() — execute JavaScript in Panorama V8 context via CUIEngine::RunScript

Usage:
    from cheat.commands import DotaCommands

    cmd = DotaCommands(mem)  # DotaMemory instance
    cmd.init()

    # Execute console commands
    cmd.execute("echo Hello from Python")
    cmd.execute("dota_purchase_item item_tango")

    # Read/write ConVars directly (faster, no thread creation)
    cmd.convar_get_int("dota_player_units_auto_attack_mode")
    cmd.convar_set_int("dota_player_units_auto_attack_mode", 2)

    # Move hero to position (no mouse needed!)
    cmd.move_to(x, y, z)
    cmd.attack_move(x, y, z)
    cmd.stop()

    # Execute Panorama JS (for UI actions: accept party invite, dismiss popups, etc.)
    cmd.init_panorama()
    cmd.run_panorama_js("$.DispatchEvent('DOTAAcceptPartyInvite', $.GetContextPanel().FindChildTraverse('AcceptButton'))")
    cmd.accept_party_invite_js()
"""
import struct
import ctypes
import time
from .memory import DotaMemory
from .offsets import Interfaces

# ─── Windows API (64-bit safe) ───────────────────────────────

kernel32 = ctypes.windll.kernel32

kernel32.VirtualAllocEx.argtypes = [
    ctypes.c_void_p, ctypes.c_ulonglong, ctypes.c_size_t,
    ctypes.c_ulong, ctypes.c_ulong,
]
kernel32.VirtualAllocEx.restype = ctypes.c_ulonglong

kernel32.VirtualFreeEx.argtypes = [
    ctypes.c_void_p, ctypes.c_ulonglong, ctypes.c_size_t, ctypes.c_ulong,
]
kernel32.VirtualFreeEx.restype = ctypes.c_bool

kernel32.CreateRemoteThread.argtypes = [
    ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t,
    ctypes.c_ulonglong, ctypes.c_ulonglong,
    ctypes.c_ulong, ctypes.POINTER(ctypes.c_ulong),
]
kernel32.CreateRemoteThread.restype = ctypes.c_void_p

kernel32.WaitForSingleObject.argtypes = [ctypes.c_void_p, ctypes.c_ulong]
kernel32.WaitForSingleObject.restype = ctypes.c_ulong

kernel32.GetExitCodeThread.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_ulong)]
kernel32.GetExitCodeThread.restype = ctypes.c_bool

kernel32.CloseHandle.argtypes = [ctypes.c_void_p]
kernel32.CloseHandle.restype = ctypes.c_bool

kernel32.WriteProcessMemory.argtypes = [
    ctypes.c_void_p, ctypes.c_ulonglong, ctypes.c_void_p,
    ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t),
]
kernel32.WriteProcessMemory.restype = ctypes.c_bool

kernel32.VirtualProtectEx.argtypes = [
    ctypes.c_void_p, ctypes.c_ulonglong, ctypes.c_size_t,
    ctypes.c_ulong, ctypes.POINTER(ctypes.c_ulong),
]
kernel32.VirtualProtectEx.restype = ctypes.c_bool

kernel32.ReadProcessMemory.argtypes = [
    ctypes.c_void_p, ctypes.c_ulonglong, ctypes.c_void_p,
    ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t),
]
kernel32.ReadProcessMemory.restype = ctypes.c_bool

MEM_COMMIT = 0x1000
MEM_RESERVE = 0x2000
MEM_RELEASE = 0x8000
PAGE_EXECUTE_READWRITE = 0x40
PAGE_EXECUTE_READ = 0x20

# ─── ConVar System ───────────────────────────────────────────

class ConVarSystem:
    """Source 2 ConVar direct memory accessor. No code injection needed."""

    CV_NAME = 0x00       # char* name
    CV_VALUE = 0x58      # CVValue_t current value (union: i32/f32/ptr)
    CV_DEFAULT = 0x60    # CVValue_t default value
    CV_DESC = 0x20       # char* description
    CV_FLAGS = 0x30      # uint32 FCVAR_* flags
    CV_ENTRY_SIZE = 0x68

    def __init__(self, mem: DotaMemory):
        self.mem = mem
        self.icvar = 0
        self.arr3_ptr = 0
        self.arr3_count = 0
        self._cache = {}

    def init(self) -> bool:
        self.icvar = self.mem.find_interface("tier0.dll", Interfaces.CVAR)
        if not self.icvar:
            self.icvar = self.mem.find_interface("engine2.dll", Interfaces.CVAR)
        if not self.icvar:
            return False

        self.arr3_ptr = self.mem.read_ptr(self.icvar + 0x48)
        self.arr3_count = self.mem.read_u64(self.icvar + 0x40) & 0xFFFF
        return True

    def find(self, name: str) -> int:
        """Find ConVar data entry by name. Returns entry_ptr or 0."""
        if name in self._cache:
            return self._cache[name]

        batch = min(self.arr3_count, 8000)
        data = self.mem.pm.read_bytes(self.arr3_ptr, batch * 0x10)

        for i in range(batch):
            entry_ptr = struct.unpack("<Q", data[i*0x10:i*0x10+8])[0]
            if not entry_ptr or entry_ptr < 0x10000:
                continue
            try:
                name_ptr = self.mem.read_ptr(entry_ptr + self.CV_NAME)
                if name_ptr and name_ptr > 0x10000:
                    s = self.mem.read_string(name_ptr, 128)
                    if s == name:
                        self._cache[name] = entry_ptr
                        return entry_ptr
            except:
                continue
        return 0

    def get_int(self, name: str) -> int | None:
        ptr = self.find(name)
        return self.mem.read_i32(ptr + self.CV_VALUE) if ptr else None

    def set_int(self, name: str, value: int) -> bool:
        ptr = self.find(name)
        if not ptr:
            return False
        self.mem.pm.write_bytes(ptr + self.CV_VALUE, struct.pack("<i", value), 4)
        return True

    def get_float(self, name: str) -> float | None:
        ptr = self.find(name)
        return self.mem.read_f32(ptr + self.CV_VALUE) if ptr else None

    def set_float(self, name: str, value: float) -> bool:
        ptr = self.find(name)
        if not ptr:
            return False
        self.mem.pm.write_bytes(ptr + self.CV_VALUE, struct.pack("<f", value), 4)
        return True

    def get_flags(self, name: str) -> int | None:
        ptr = self.find(name)
        return self.mem.read_u32(ptr + self.CV_FLAGS) if ptr else None


# ─── Command Execution ──────────────────────────────────────

# CCommandBuffer layout (engine2.dll):
#   base_array = engine2 + 0x8CF460
#   CCommandBuffer[slot] = base_array + slot * 0x86D8 + 0x588
#   CCommandBuffer[0] (client) = engine2 + 0x8CF9E8
#   6 slots verified (stride 0x86D8), found via scan .data for verify pattern
CMDBUF_OFFSET = 0x8CF9E8  # RVA in engine2.dll for client slot 0 (post-update 2026-03-25)

# CCommandBuffer::AddText in tier0.dll (exported mangled name)
ADDTEXT_RVA = 0x5BC60  # post-update 2026-03-25

# Verification: CCommandBuffer+0x8028 should be 0x400
CMDBUF_VERIFY_OFFSET = 0x8028
CMDBUF_VERIFY_VALUE = 0x400

# PrepareUnitOrders in client.dll (AOB: 4C 89 4C 24 20 44 89 44 24 18 89 54 24 10 48 89 4C 24 08)
PREPARE_UNIT_ORDERS_RVA = 0x1E05970  # AOB scan hit #2 (post-update 2026-03-25)

# ─── Panorama JS Execution (CUIEngine::RunScript) ──────────
# panorama.dll — CUIEngine::RunScript(this, panel, jsCode, origin, flags)
# Reversed via Ghidra: FUN_1800a8970, source: panorama/uiengine.cpp line 0xF68
RUNSCRIPT_RVA = 0xA6A40

# CUIEngine* singleton global pointer (DAT_18056d9e8 in panorama.dll)
CUIENGINE_PTR_RVA = 0x569C78

# CUIEngine layout:
#   +0x600 = V8 Isolate*
#   +0x5D0 = default V8 Context (v8::Global<Context>) — used when panel=NULL
#   +0x660 = CUtlRBTree<panel ->v8::Global<Context>*> — per-panel contexts

# RunScriptInPanelContext (V8 callback) — has domain check comparing origin URLs
RUNSCRIPT_IN_PANEL_CONTEXT_RVA = 0xEAF50

# Domain check bypass: CMP RCX,RSI / JZ success -> NOP*3 / JMP success / NOP
# This is the panel equality check in RunScriptInPanelContext. If panels differ,
# origin URL comparisons follow (code://, http://, https://). Patching the CMP+JZ
# to unconditional JMP bypasses ALL domain checks — RunScriptInPanelContext always
# proceeds to execute the script regardless of calling context.
# AOB: CMP RCX,RSI / JZ near / MOV RBX,[RSI+0x240] — unique within the function
# Note: CMP has two encodings: 48 39 F1 (Ghidra) vs 48 3B CE (actual binary). Use ??
DOMAIN_CHECK_AOB = "48 ?? ?? 0F 84 ?? ?? ?? ?? 48 8B 9E 48 02 00 00"
DOMAIN_CHECK_PATCH_SIZE = 9  # bytes to patch (CMP=3 + JZ=6)

# CDOTAPlayerController field offsets
PLAYER_SLOT_OFFSET = 0x900  # int32 m_nPlayerID (post-update 2026-03-25)

# ─── Unit Order Constants ─────────────────────────────────
class UnitOrder:
    NONE = 0
    MOVE_TO_POSITION = 1
    MOVE_TO_TARGET = 2
    ATTACK_MOVE = 3
    ATTACK_TARGET = 4
    CAST_POSITION = 5
    CAST_TARGET = 6
    CAST_NO_TARGET = 8
    HOLD_POSITION = 10
    TRAIN_ABILITY = 11  # ability_index = entity_index (handle & 0x7FFF)
    DROP_ITEM = 12
    GIVE_ITEM = 13
    PURCHASE_ITEM = 16
    SELL_ITEM = 17
    STOP = 21
    BUYBACK = 23
    MOVE_TO_DIRECTION = 28

class OrderIssuer:
    SELECTED_UNITS = 0
    CURRENT_UNIT_ONLY = 1
    HERO_ONLY = 2
    PASSED_UNIT_ONLY = 3

class QueueBehavior:
    DEFAULT = 0
    NEVER = 1
    ALWAYS = 2


def _build_shellcode(cmd_buffer_addr: int, addtext_addr: int, cmd_string_addr: int) -> bytes:
    """
    x64 shellcode for CCommandBuffer::AddText(cmd, 0, 0, false, false, 0)

    MSVC x64 calling convention:
    rcx = this (CCommandBuffer*)
    rdx = const char* cmd
    r8d = int delay (0)
    r9d = int unk (0)
    [rsp+20h] = bool (0)
    [rsp+28h] = bool (0)
    [rsp+30h] = uint64 flags (0)
    """
    sc = bytearray()
    sc += b'\x48\x83\xEC\x58'                              # sub rsp, 0x58
    sc += b'\x48\xB9' + struct.pack('<Q', cmd_buffer_addr)  # mov rcx, cmd_buffer
    sc += b'\x48\xBA' + struct.pack('<Q', cmd_string_addr)  # mov rdx, cmd_string
    sc += b'\x45\x33\xC0'                                  # xor r8d, r8d
    sc += b'\x45\x33\xC9'                                  # xor r9d, r9d
    sc += b'\xC6\x44\x24\x20\x00'                          # mov byte [rsp+20h], 0
    sc += b'\xC6\x44\x24\x28\x00'                          # mov byte [rsp+28h], 0
    sc += b'\x48\xC7\x44\x24\x30\x00\x00\x00\x00'         # mov qword [rsp+30h], 0
    sc += b'\x48\xB8' + struct.pack('<Q', addtext_addr)     # mov rax, AddText
    sc += b'\xFF\xD0'                                       # call rax
    sc += b'\x48\x83\xC4\x58'                              # add rsp, 0x58
    sc += b'\x33\xC0'                                      # xor eax, eax
    sc += b'\xC3'                                          # ret
    return bytes(sc)


def _build_prepare_orders_shellcode(controller_addr: int, func_addr: int, vec3_addr: int,
                                     order_type: int, target_index: int = 0,
                                     ability_index: int = 0, issuer: int = 2,
                                     unit: int = 0, queue: int = 0,
                                     show_effects: int = 0) -> bytes:
    """
    x64 shellcode for PrepareUnitOrders (9 params).
    RCX=this, EDX=orderType, R8D=targetIdx, R9=vec3*
    Stack: [+20h]=ability, [+28h]=issuer, [+30h]=unit, [+38h]=queue, [+40h]=showEffects
    """
    sc = bytearray()
    sc += b'\x48\x83\xEC\x58'                                      # sub rsp, 0x58
    sc += b'\x48\xB9' + struct.pack('<Q', controller_addr)          # mov rcx, controller
    sc += b'\xBA' + struct.pack('<I', order_type)                   # mov edx, orderType
    sc += b'\x41\xB8' + struct.pack('<I', target_index)             # mov r8d, targetIndex
    sc += b'\x49\xB9' + struct.pack('<Q', vec3_addr)                # mov r9, vec3_ptr
    sc += b'\xC7\x44\x24\x20' + struct.pack('<i', ability_index)   # mov [rsp+20h], ability
    sc += b'\xC7\x44\x24\x28' + struct.pack('<i', issuer)          # mov [rsp+28h], issuer
    sc += b'\x48\xC7\x44\x24\x30' + struct.pack('<i', unit & 0xFFFFFFFF)  # mov [rsp+30h], unit
    sc += b'\xC7\x44\x24\x38' + struct.pack('<i', queue)           # mov [rsp+38h], queue
    sc += b'\xC6\x44\x24\x40' + struct.pack('<B', show_effects)    # mov byte [rsp+40h], fx
    sc += b'\x48\xB8' + struct.pack('<Q', func_addr)                # mov rax, func
    sc += b'\xFF\xD0'                                               # call rax
    sc += b'\x48\x83\xC4\x58'                                      # add rsp, 0x58
    sc += b'\x33\xC0'                                               # xor eax, eax
    sc += b'\xC3'                                                   # ret
    return bytes(sc)


def _build_runscript_shellcode(engine_ptr: int, panel_ptr: int, js_addr: int,
                                origin_addr: int, func_addr: int) -> bytes:
    """
    x64 shellcode for CUIEngine::RunScript(this, panel, jsCode, origin, flags)

    Reversed from panorama.dll FUN_1800a8970:
      - Enters V8 Isolate, gets V8 Context for panel (NULL ->default context at engine+0x5D0)
      - Compiles JS via v8::ScriptCompiler::Compile
      - Runs via v8::Script::Run
      - Exits V8 Isolate

    MSVC x64 calling convention:
    RCX = CUIEngine* this
    RDX = CPanel2D* panel (0 for default context)
    R8  = const char* jsCode (UTF-8)
    R9  = const char* origin (script name for errors)
    [RSP+20h] = uint64 flags (1 = skip script cache)
    """
    sc = bytearray()
    sc += b'\x48\x83\xEC\x48'                              # sub rsp, 0x48 (72, aligned)
    sc += b'\x48\xB9' + struct.pack('<Q', engine_ptr)       # mov rcx, CUIEngine*
    sc += b'\x48\xBA' + struct.pack('<Q', panel_ptr)        # mov rdx, panel (0=default)
    sc += b'\x49\xB8' + struct.pack('<Q', js_addr)          # mov r8, jsCode
    sc += b'\x49\xB9' + struct.pack('<Q', origin_addr)      # mov r9, origin
    sc += b'\x48\xC7\x44\x24\x20\x01\x00\x00\x00'         # mov qword [rsp+20h], 1 (no cache)
    sc += b'\x48\xB8' + struct.pack('<Q', func_addr)        # mov rax, RunScript
    sc += b'\xFF\xD0'                                       # call rax
    sc += b'\x48\x83\xC4\x48'                              # add rsp, 0x48
    sc += b'\x33\xC0'                                      # xor eax, eax
    sc += b'\xC3'                                          # ret
    return bytes(sc)


def _write_remote(process_handle, address: int, data: bytes) -> bool:
    buf = ctypes.create_string_buffer(data)
    written = ctypes.c_size_t(0)
    return bool(kernel32.WriteProcessMemory(
        process_handle, address, buf, len(data), ctypes.byref(written)
    ))


class DotaCommands:
    """Console command injection + PrepareUnitOrders + ConVar + Panorama JS for Dota 2."""

    def __init__(self, mem: DotaMemory):
        self.mem = mem
        self.cmd_buffer_addr = 0
        self.addtext_addr = 0
        self.prepare_orders_addr = 0
        self.local_controller = 0
        self.convar = ConVarSystem(mem)
        self._initialized = False
        self._order_buf = 0           # persistent RWX buffer for orders
        self._last_order_time = 0.0   # rate limiting
        self._order_min_interval = 0.05  # max ~20 orders/sec
        # Panorama JS
        self._cuiengine = 0
        self._runscript_addr = 0
        self._panorama_ready = False
        # Domain check patch state
        self._domain_patch_addr = 0
        self._domain_patch_orig = b''
        self._domain_patched = False

    def init(self, game=None) -> bool:
        """Resolve all addresses. Call once after attaching.
        Pass DotaGame instance to enable PrepareUnitOrders (movement without mouse).
        """
        engine2_base = self.mem.module_base("engine2.dll")
        tier0_base = self.mem.module_base("tier0.dll")
        client_base = self.mem.module_base("client.dll")

        self.cmd_buffer_addr = engine2_base + CMDBUF_OFFSET
        self.addtext_addr = tier0_base + ADDTEXT_RVA

        # PrepareUnitOrders in client.dll
        self.prepare_orders_addr = client_base + PREPARE_UNIT_ORDERS_RVA

        # Verify CCommandBuffer
        try:
            verify = self.mem.read_i32(self.cmd_buffer_addr + CMDBUF_VERIFY_OFFSET)
            if verify != CMDBUF_VERIFY_VALUE:
                print(f"[!] CCommandBuffer verification failed: +0x8028 = {hex(verify)}, expected {hex(CMDBUF_VERIFY_VALUE)}")
                return False
        except:
            print("[!] Cannot read CCommandBuffer")
            return False

        # Init ConVar system
        self.convar.init()

        # Find local player controller if game provided
        if game:
            self._find_local_controller(game)

        self._initialized = True
        return True

    def _find_local_controller(self, game, hero_entity: int = 0) -> bool:
        """Find local CDOTAPlayerController.

        If hero_entity is provided, matches controller whose m_hAssignedHero
        resolves to that entity. Otherwise collects all controllers and picks
        the one whose assigned hero we can identify.

        IMPORTANT: In multi-bot scenarios, if hero_entity is given but no match
        is found, we do NOT fallback to a random controller — that would send
        orders to the wrong hero. Instead local_controller stays 0 and orders
        go through Panorama JS fallback (which always targets the correct
        local player for this process).
        """
        from .offsets import PlayerController, BaseHero

        controllers = []
        for entity, ident, name in game.iter_entities():
            if name == "dota_player_controller":
                slot = self.mem.read_i32(entity + PLAYER_SLOT_OFFSET)
                hero_handle = self.mem.read_i32(entity + PlayerController.ASSIGNED_HERO)
                controllers.append((entity, slot, hero_handle))

        if not controllers:
            return False

        # If hero_entity given, find matching controller
        if hero_entity:
            for ent, slot, hh in controllers:
                resolved = game.resolve_handle(hh)
                if resolved == hero_entity:
                    self.local_controller = ent
                    print(f"[+] Local controller: {hex(ent)} (slot={slot}, matched hero)")
                    return True
            # No match — do NOT fallback to wrong controller, use Panorama JS
            self.local_controller = 0
            print(f"[~] Local controller: not matched (hero_entity={hex(hero_entity)}), using Panorama JS fallback")
            return False

        # Fallback: pick first controller with valid hero (only when no hero_entity specified)
        for ent, slot, hh in controllers:
            if hh and hh != -1:
                self.local_controller = ent
                print(f"[+] Local controller: {hex(ent)} (slot={slot}, fallback)")
                return True

        return False

    def set_local_controller(self, controller_addr: int):
        """Manually set local controller address."""
        self.local_controller = controller_addr

    def execute(self, command: str) -> bool:
        """Execute a console command in Dota 2 via shellcode injection."""
        if not self._initialized:
            raise RuntimeError("DotaCommands not initialized — call init() first")

        process_handle = self.mem.pm.process_handle
        cmd_bytes = command.encode('ascii') + b'\x00'

        # Allocate RWX memory
        alloc_addr = kernel32.VirtualAllocEx(
            process_handle, 0, 0x1000,
            MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE
        )
        if not alloc_addr:
            return False

        try:
            cmd_string_addr = alloc_addr + 0x200
            shellcode = _build_shellcode(self.cmd_buffer_addr, self.addtext_addr, cmd_string_addr)

            # Write shellcode + command string
            if not _write_remote(process_handle, alloc_addr, shellcode):
                return False
            if not _write_remote(process_handle, cmd_string_addr, cmd_bytes):
                return False

            # Execute
            thread_id = ctypes.c_ulong(0)
            thread = kernel32.CreateRemoteThread(
                process_handle, None, 0, alloc_addr, 0, 0, ctypes.byref(thread_id)
            )
            if not thread:
                return False

            kernel32.WaitForSingleObject(thread, 5000)
            kernel32.CloseHandle(thread)
            return True

        finally:
            kernel32.VirtualFreeEx(process_handle, alloc_addr, 0, MEM_RELEASE)

    def execute_batch(self, commands: list[str], delay_between: float = 0.01) -> int:
        """Execute multiple commands. Returns count of successful."""
        count = 0
        for cmd in commands:
            if self.execute(cmd):
                count += 1
            if delay_between > 0:
                time.sleep(delay_between)
        return count

    # ─── ConVar shortcuts ─────────────────────────────────

    def convar_get_int(self, name: str) -> int | None:
        return self.convar.get_int(name)

    def convar_set_int(self, name: str, value: int) -> bool:
        return self.convar.set_int(name, value)

    def convar_get_float(self, name: str) -> float | None:
        return self.convar.get_float(name)

    def convar_set_float(self, name: str, value: float) -> bool:
        return self.convar.set_float(name, value)

    # ─── Bot Farm Commands ────────────────────────────────

    def set_auto_attack(self, mode: int = 1):
        """Set auto-attack mode. 0=never, 1=standard, 2=always, 3=per setting."""
        self.convar_set_int("dota_player_units_auto_attack_mode", mode)

    # Item definition IDs (from items.json "ID" field)
    ITEM_DEF_IDS = {
        "item_blink": 1, "item_blades_of_attack": 12, "item_broadsword": 13,
        "item_chainmail": 14, "item_claymore": 15, "item_branches": 16,
        "item_gauntlets": 13, "item_slippers": 14, "item_mantle": 15,
        "item_boots": 29, "item_gem": 30, "item_cloak": 31,
        "item_talisman_of_evasion": 32, "item_cheese": 33, "item_magic_stick": 34,
        "item_recipe_magic_wand": 35, "item_magic_wand": 36,
        "item_ghost": 36, "item_clarity": 38, "item_flask": 39,
        "item_dust": 40, "item_bottle": 41, "item_ward_observer": 42,
        "item_ward_sentry": 43, "item_tango": 44, "item_tpscroll": 46,
        "item_recipe_travel_boots": 47, "item_travel_boots": 48,
        "item_phase_boots": 50, "item_demon_edge": 51,
        "item_eagle": 52, "item_reaver": 53, "item_sacred_relic": 54,
        "item_hyperstone": 55, "item_ring_of_health": 56,
        "item_void_stone": 57, "item_mystic_staff": 58,
        "item_energy_booster": 59, "item_point_booster": 60,
        "item_vitality_booster": 61, "item_power_treads": 63,
        "item_hand_of_midas": 65, "item_oblivion_staff": 67,
        "item_pers": 69, "item_poor_mans_shield": 70,
        "item_bracer": 73, "item_wraith_band": 75, "item_null_talisman": 77,
        "item_mekansm": 79, "item_vladmir": 81,
        "item_buckler": 83, "item_ring_of_basilius": 85,
        "item_pipe": 90, "item_urn_of_shadows": 92,
        "item_headdress": 94, "item_sheepstick": 96,
        "item_orchid": 98, "item_cyclone": 100,
        "item_force_staff": 102, "item_dagon": 104,
        "item_soul_ring": 117, "item_arcane_boots": 119,
        "item_quelling_blade": 121, "item_wind_lace": 244,
        "item_infused_raindrop": 265, "item_enchanted_mango": 216,
        "item_faerie_fire": 237, "item_blight_stone": 240,
        "item_circlet": 20, "item_belt_of_strength": 22,
        "item_boots_of_elves": 23, "item_robe": 24,
        "item_ogre_axe": 25, "item_blade_of_alacrity": 26,
        "item_staff_of_wizardry": 27, "item_ultimate_orb": 28,
    }

    def purchase_item(self, item_name: str) -> bool:
        """Buy an item via PrepareUnitOrders PURCHASE_ITEM with console fallback.

        Tries PrepareUnitOrders first (requires local_controller).
        Falls back to dota_purchase_item console command if order() fails.
        """
        item_id = self.ITEM_DEF_IDS.get(item_name)
        if item_id is None:
            # Try loading from GameDataDB
            try:
                from .game_data import GameDataDB
                db = GameDataDB()
                item_data = db.get_item(item_name)
                if item_data:
                    item_id = int(item_data.get("ID", 0))
            except:
                pass

        # Try PrepareUnitOrders first (works with local_controller)
        if item_id:
            try:
                ok = self.order(UnitOrder.PURCHASE_ITEM, ability_index=item_id)
                if ok:
                    return True
            except Exception:
                pass

        # Fallback: console command — always works, no local_controller needed
        return self.execute(f"dota_purchase_item {item_name}")

    def purchase_quickbuy(self) -> bool:
        """Buy all quickbuy items."""
        return self.execute("dota_purchase_quickbuy")

    def chatwheel(self, index: int) -> bool:
        """Use chat wheel (anti-AFK). index = 0-99."""
        return self.execute(f"chatwheel_say {index}")

    def camera_distance(self, distance: float = 1200.0):
        """Set camera distance (default 1200)."""
        self.convar_set_float("dota_camera_distance", distance)

    # ─── Hero Selection ───────────────────────────────────

    # Bot-friendly heroes (simple mechanics, low skill floor)
    BOT_HEROES = [
        "npc_dota_hero_skeleton_king",   # WK — internal name is skeleton_king!
        "npc_dota_hero_viper",           # Viper — right-click, tanky
        "npc_dota_hero_sniper",          # Sniper — range, simple
        "npc_dota_hero_drow_ranger",     # Drow — aura, right-click
        "npc_dota_hero_bloodseeker",     # BS — passive heavy
        "npc_dota_hero_abaddon",         # Abaddon — hard to die
        "npc_dota_hero_bristleback",     # BB — tanky, passive
        "npc_dota_hero_ogre_magi",       # Ogre — tanky support
        "npc_dota_hero_lich",            # Lich — simple support
        "npc_dota_hero_lion",            # Lion — disable + nuke
        "npc_dota_hero_crystal_maiden",  # CM — aura + disable
        "npc_dota_hero_tidehunter",      # Tide — tanky offlane
        "npc_dota_hero_centaur",         # Centaur — tanky initiator
        "npc_dota_hero_sven",            # Sven — cleave carry
        "npc_dota_hero_luna",            # Luna — aura carry
    ]

    def select_hero(self, hero_name: str = None) -> bool:
        """Pick a hero during hero selection phase.

        Args:
            hero_name: full internal name like 'npc_dota_hero_sniper'.
                       If None, picks a random bot-friendly hero.
        """
        import random
        if hero_name is None:
            hero_name = random.choice(self.BOT_HEROES)
        # Ensure proper prefix
        if not hero_name.startswith("npc_dota_hero_"):
            hero_name = f"npc_dota_hero_{hero_name}"
        print(f"[>] Picking hero: {hero_name}")
        return self.execute(f"dota_select_hero {hero_name}")

    def select_hero_by_index(self, hero_list: list[str] = None, index: int = 0) -> bool:
        """Pick hero by index from a list (for distributing heroes across bots)."""
        if hero_list is None:
            hero_list = self.BOT_HEROES
        hero = hero_list[index % len(hero_list)]
        return self.select_hero(hero)

    # ─── PrepareUnitOrders ─────────────────────────────────

    def _ensure_order_buf(self) -> int:
        """Get or create persistent RWX buffer for order shellcode."""
        if self._order_buf:
            return self._order_buf
        process_handle = self.mem.pm.process_handle
        addr = kernel32.VirtualAllocEx(
            process_handle, 0, 0x1000,
            MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE
        )
        if addr:
            self._order_buf = addr
        return addr

    def _patch_domain_check(self) -> bool:
        """Bypass RunScriptInPanelContext domain check entirely.

        Patches CMP RCX,RSI / JZ success (9 bytes) to NOP*3 / JMP success / NOP,
        so RunScriptInPanelContext ALWAYS skips domain/origin validation.
        Scans within the function body only (AOB not unique module-wide).
        """
        if self._domain_patched:
            return True

        panorama_base = self.mem.module_base("panorama.dll")
        func_addr = panorama_base + RUNSCRIPT_IN_PANEL_CONTEXT_RVA
        func_size = 0x600  # function is ~0x576 bytes

        # Read function body and search AOB within it only
        import re as _re
        try:
            func_data = self.mem.pm.read_bytes(func_addr, func_size)
        except Exception:
            print("[!] Failed to read RunScriptInPanelContext body")
            return False

        parts = DOMAIN_CHECK_AOB.split()
        regex_parts = []
        for p in parts:
            if p in ("?", "??"):
                regex_parts.append(b".")
            else:
                regex_parts.append(_re.escape(bytes([int(p, 16)])))
        match = _re.search(b"".join(regex_parts), func_data, _re.DOTALL)

        if not match:
            print("[!] Domain check AOB not found within RunScriptInPanelContext")
            return False

        # Patch starts at match (CMP RCX,RSI), 9 bytes total
        patch_addr = func_addr + match.start()
        process_handle = self.mem.pm.process_handle
        print(f"[*] AOB at func+0x{match.start():X}, patch at 0x{patch_addr:X} (RVA 0x{patch_addr - panorama_base:X})")

        # Read original 9 bytes (48 39 F1 0F 84 xx xx xx xx)
        orig_buf = ctypes.create_string_buffer(DOMAIN_CHECK_PATCH_SIZE)
        bytes_read = ctypes.c_size_t(0)
        if not kernel32.ReadProcessMemory(process_handle, patch_addr, orig_buf,
                                          DOMAIN_CHECK_PATCH_SIZE, ctypes.byref(bytes_read)):
            print("[!] Failed to read original bytes")
            return False
        self._domain_patch_orig = orig_buf.raw
        self._domain_patch_addr = patch_addr

        # Verify: CMP RCX,RSI (48 3B CE or 48 39 F1) + JZ near (0F 84)
        if self._domain_patch_orig[0] != 0x48:
            print(f"[!] Expected REX.W prefix (48), got {self._domain_patch_orig[0]:02x}")
            return False
        if self._domain_patch_orig[3:5] != b'\x0F\x84':
            print(f"[!] Expected 0F 84 (JZ near), got {self._domain_patch_orig[3:5].hex()}")
            return False

        # Build patch: NOP*3 + JMP near (same target) + NOP
        # Original JZ: at patch_addr+3, 6 bytes, target = patch_addr+9 + rel32
        # New JMP: at patch_addr+3, 5 bytes, target = patch_addr+8 + new_rel32
        # Same target => new_rel32 = rel32 + 1
        orig_rel32 = struct.unpack_from('<i', self._domain_patch_orig, 5)[0]
        new_rel32 = orig_rel32 + 1
        patch = b'\x90\x90\x90' + b'\xE9' + struct.pack('<i', new_rel32) + b'\x90'

        # VirtualProtectEx -> PAGE_EXECUTE_READWRITE
        old_protect = ctypes.c_ulong(0)
        if not kernel32.VirtualProtectEx(process_handle, patch_addr, DOMAIN_CHECK_PATCH_SIZE,
                                         PAGE_EXECUTE_READWRITE, ctypes.byref(old_protect)):
            print("[!] VirtualProtectEx failed")
            return False

        # Write patch
        if not _write_remote(process_handle, patch_addr, patch):
            kernel32.VirtualProtectEx(process_handle, patch_addr, DOMAIN_CHECK_PATCH_SIZE,
                                      old_protect.value, ctypes.byref(old_protect))
            print("[!] Failed to write domain check patch")
            return False

        # Restore original protection
        kernel32.VirtualProtectEx(process_handle, patch_addr, DOMAIN_CHECK_PATCH_SIZE,
                                  old_protect.value, ctypes.byref(old_protect))

        self._domain_patched = True
        print(f"[+] Domain check patched at 0x{patch_addr:X}: "
              f"{self._domain_patch_orig.hex(' ')} -> {patch.hex(' ')}")
        return True

    def _unpatch_domain_check(self):
        """Restore original domain check bytes."""
        if not self._domain_patched or not self._domain_patch_addr:
            return

        process_handle = self.mem.pm.process_handle
        addr = self._domain_patch_addr

        old_protect = ctypes.c_ulong(0)
        sz = DOMAIN_CHECK_PATCH_SIZE
        if not kernel32.VirtualProtectEx(process_handle, addr, sz, PAGE_EXECUTE_READWRITE, ctypes.byref(old_protect)):
            print("[!] VirtualProtectEx failed during unpatch")
            return

        _write_remote(process_handle, addr, self._domain_patch_orig)
        kernel32.VirtualProtectEx(process_handle, addr, sz, old_protect.value, ctypes.byref(old_protect))

        self._domain_patched = False
        print(f"[+] Domain check restored at 0x{addr:X}: {self._domain_patch_orig.hex(' ')}")

    def cleanup(self):
        """Free persistent buffers and restore patches. Call before detaching."""
        self._unpatch_domain_check()
        if self._order_buf:
            try:
                kernel32.VirtualFreeEx(
                    self.mem.pm.process_handle, self._order_buf, 0, MEM_RELEASE
                )
            except:
                pass
            self._order_buf = 0

    # ─── Panorama JS Execution ─────────────────────────────

    def init_panorama(self) -> bool:
        """Initialize panorama.dll addresses for JS execution via CUIEngine::RunScript.

        Call after init(). Reads CUIEngine* singleton and verifies V8 Isolate is alive.
        """
        panorama_base = self.mem.module_base("panorama.dll")
        if not panorama_base:
            print("[!] panorama.dll not found")
            return False

        engine_ptr_addr = panorama_base + CUIENGINE_PTR_RVA
        self._cuiengine = self.mem.read_ptr(engine_ptr_addr)
        if not self._cuiengine:
            print("[!] CUIEngine* is NULL — UI not initialized yet?")
            return False

        self._runscript_addr = panorama_base + RUNSCRIPT_RVA

        # Verify: V8 Isolate at CUIEngine+0x600 should be non-null
        isolate = self.mem.read_ptr(self._cuiengine + 0x600)
        if not isolate:
            print("[!] V8 Isolate is NULL — Panorama not ready")
            return False

        # Verify: default V8 context at CUIEngine+0x5D0 should be non-null
        default_ctx = self.mem.read_ptr(self._cuiengine + 0x5D0)
        if not default_ctx:
            print("[!] Default V8 context is NULL")
            return False

        self._panorama_ready = True
        print(f"[+] Panorama JS: CUIEngine=0x{self._cuiengine:X}, "
              f"Isolate=0x{isolate:X}, DefaultCtx=0x{default_ctx:X}")
        print(f"[+] RunScript @ 0x{self._runscript_addr:X}")
        return True

    def _get_any_panel(self) -> int:
        """Read first valid panel pointer from CUIEngine panel-context RBTree.

        The tree at engine+0x660 maps IUIPanel* ->v8::Global<Context>*.
        Element layout (0x20 each): [left:4][right:4][tag:4][pad:4][key:8][value:8]
        Returns panel pointer or 0.
        """
        engine = self._cuiengine
        cap = self.mem.read_u32(engine + 0x664) & 0x7FFFFFFF
        elements = self.mem.read_ptr(engine + 0x668)
        if not elements or cap == 0 or cap > 10000:
            return 0
        try:
            count = min(cap, 30)
            data = self.mem.pm.read_bytes(elements, count * 0x20)
            for i in range(count):
                key = struct.unpack_from('<Q', data, i * 0x20 + 0x10)[0]
                if 0x10000 < key < 0x800000000000:
                    return key
        except:
            pass
        return 0

    def run_panorama_js(self, js_code: str, panel: int = 0) -> bool:
        """Execute JavaScript in Panorama V8 context.

        Uses CUIEngine::RunScript(engine, panel, code, origin, flags=1).
        If panel=0, uses default V8 context (engine+0x5D0): has $ but
        GetContextPanel() is NULL. Pass a real panel for full UI tree access.

        Args:
            js_code: JavaScript source code (UTF-8). Has access to Panorama API ($).
            panel: CPanel2D* pointer (0 = default context).
        Returns:
            True if thread executed successfully.
        """
        if not self._panorama_ready:
            raise RuntimeError("Panorama not initialized — call init_panorama() first")

        process_handle = self.mem.pm.process_handle
        js_bytes = js_code.encode('utf-8') + b'\x00'
        origin = b'code://external\x00'

        # Allocate: 0x200 shellcode + js string + origin string
        alloc_size = 0x200 + len(js_bytes) + len(origin) + 0x10
        alloc_addr = kernel32.VirtualAllocEx(
            process_handle, 0, alloc_size,
            MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE
        )
        if not alloc_addr:
            return False

        try:
            js_addr = alloc_addr + 0x200
            origin_addr = js_addr + len(js_bytes)

            if not _write_remote(process_handle, js_addr, js_bytes):
                return False
            if not _write_remote(process_handle, origin_addr, origin):
                return False

            sc = _build_runscript_shellcode(
                self._cuiengine, panel, js_addr, origin_addr, self._runscript_addr
            )
            if not _write_remote(process_handle, alloc_addr, sc):
                return False

            thread_id = ctypes.c_ulong(0)
            thread = kernel32.CreateRemoteThread(
                process_handle, None, 0, alloc_addr, 0, 0, ctypes.byref(thread_id)
            )
            if not thread:
                return False

            result = kernel32.WaitForSingleObject(thread, 5000)
            kernel32.CloseHandle(thread)
            return result == 0  # WAIT_OBJECT_0
        finally:
            kernel32.VirtualFreeEx(process_handle, alloc_addr, 0, MEM_RELEASE)

    # JS helper: navigate from context panel to DotaDashboard root
    _JS_TO_ROOT = (
        "var p=$.GetContextPanel();"
        "if(!p)return;"
        "while(p.GetParent())p=p.GetParent();"
    )

    def _run_dashboard_js(self, js_body: str) -> bool:
        """Run JS in a real panel context, navigating to DotaDashboard root.

        Picks any panel from the CUIEngine RBTree and navigates up to root.
        Then executes js_body with `p` set to the root panel.
        """
        panel = self._get_any_panel()
        if not panel:
            print("[!] No panel found in RBTree")
            return False
        return self.run_panorama_js(
            "(function(){" + self._JS_TO_ROOT + js_body + "})()",
            panel=panel
        )

    def accept_party_invite_js(self) -> bool:
        """Accept party invite — DEPRECATED, use DotaGC.wait_and_accept_invite().

        DOTAAcceptPartyInvite is a native C++ handler, not callable from JS.
        This method kept for backward compat — delegates to GC approach if
        a DotaGC instance is available, otherwise falls back to SendInput Enter.
        """
        print("[!] accept_party_invite_js: use DotaGC.wait_and_accept_invite() instead")
        return self._click_accept_button_fallback()

    def decline_party_invite_js(self) -> bool:
        """Decline/dismiss party invite popup."""
        return self._run_dashboard_js(
            "var b=p.FindChildTraverse('DeclineButton');"
            "if(b)$.DispatchEvent('DOTADeclinePartyInvite',b);"
        )

    def close_popup_js(self, popup_id: str = 'PartyInvitePopup') -> bool:
        """Close a popup by ID (DeleteAsync)."""
        return self._run_dashboard_js(
            f"var popup=p.FindChildTraverse('{popup_id}');"
            f"if(popup)popup.DeleteAsync(0);"
        )

    def _find_dota_hwnd(self) -> int:
        """Find Dota 2 main window handle by PID via EnumWindows."""
        user32 = ctypes.windll.user32
        target_pid = self.mem.pid
        result = ctypes.c_void_p(0)

        WNDENUMPROC = ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.POINTER(ctypes.c_void_p))

        def enum_cb(hwnd, lparam):
            pid = ctypes.c_ulong(0)
            user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
            if pid.value == target_pid and user32.IsWindowVisible(hwnd):
                lparam[0] = hwnd
                return False  # stop enumeration
            return True

        cb = WNDENUMPROC(enum_cb)
        user32.EnumWindows(cb, ctypes.byref(result))
        return result.value or 0

    def _click_accept_button_fallback(self) -> bool:
        """Fallback: click AcceptButton via PostMessage (no focus required).

        Uses JS to read button layout coordinates, then sends WM_LBUTTONDOWN/UP.
        """
        user32 = ctypes.windll.user32
        WM_LBUTTONDOWN = 0x0201
        WM_LBUTTONUP = 0x0202
        MK_LBUTTON = 0x0001

        hwnd = self._find_dota_hwnd()
        if not hwnd:
            print("[!] Dota 2 window not found")
            return False

        # Try to get button coordinates via ConVar bridge:
        # Write JS that stores button center coords into a known ConVar
        # We'll use a simpler approach: just click center of window (popup is centered)
        # and the AcceptButton is the left button of the two-button layout.

        # Get window client rect
        class RECT(ctypes.Structure):
            _fields_ = [("left", ctypes.c_long), ("top", ctypes.c_long),
                        ("right", ctypes.c_long), ("bottom", ctypes.c_long)]

        rect = RECT()
        user32.GetClientRect(hwnd, ctypes.byref(rect))
        w = rect.right - rect.left
        h = rect.bottom - rect.top

        # Party invite popup: roughly centered, AcceptButton is left of center
        # Typical positions at 1920x1080: Accept ~(880, 580), Decline ~(1040, 580)
        # Scale proportionally
        btn_x = int(w * 0.458)  # ~880/1920
        btn_y = int(h * 0.537)  # ~580/1080

        lparam = btn_y << 16 | (btn_x & 0xFFFF)

        user32.PostMessageW(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, lparam)
        time.sleep(0.05)
        user32.PostMessageW(hwnd, WM_LBUTTONUP, 0, lparam)

        print(f"[+] Fallback click sent at ({btn_x}, {btn_y}) on hwnd=0x{hwnd:X}")
        return True

    def _order_via_panorama(self, order_type: int, x: float = 0, y: float = 0, z: float = 0,
                            target_index: int = 0, ability_index: int = 0,
                            queue: int = 0) -> bool:
        """Fallback: send unit order via Panorama JS Game.PrepareUnitOrders.
        Works without local_controller — Panorama knows the local player.
        """
        if not self._panorama_ready:
            try:
                self.init_panorama()
            except:
                return False
            if not self._panorama_ready:
                return False

        # Rate limit
        now = time.time()
        is_immediate = order_type in (
            UnitOrder.TRAIN_ABILITY, UnitOrder.CAST_NO_TARGET,
            UnitOrder.CAST_TARGET, UnitOrder.CAST_POSITION,
            UnitOrder.PURCHASE_ITEM, UnitOrder.SELL_ITEM,
            UnitOrder.BUYBACK, UnitOrder.STOP,
        )
        if not is_immediate and now - self._last_order_time < self._order_min_interval:
            return True
        self._last_order_time = now

        # Build JS call
        params = [f"OrderType: {order_type}"]
        if x != 0 or y != 0 or z != 0:
            params.append(f"Position: [{x}, {y}, {z}]")
        if target_index:
            params.append(f"TargetIndex: {target_index}")
        if ability_index:
            params.append(f"AbilityIndex: {ability_index}")
        if queue:
            params.append("Queue: true")
        params.append("ShowEffects: false")

        js = f"Game.PrepareUnitOrders({{{', '.join(params)}}});"
        return self.run_panorama_js(js)

    def order(self, order_type: int, x: float = 0, y: float = 0, z: float = 0,
              target_index: int = 0, ability_index: int = 0,
              issuer: int = OrderIssuer.HERO_ONLY, unit: int = 0,
              queue: int = QueueBehavior.DEFAULT, show_effects: bool = False) -> bool:
        """Send a unit order via PrepareUnitOrders shellcode.

        Uses persistent RWX buffer (no alloc/free per call) and rate limiting.

        Args:
            order_type: UnitOrder.* constant
            x, y, z: world position (for MOVE_TO_POSITION, ATTACK_MOVE, CAST_POSITION)
            target_index: entity index for target orders
            ability_index: entity index of ability for cast orders
            issuer: OrderIssuer.* constant
            unit: entity handle of unit to command (0 = auto)
            queue: QueueBehavior.* constant
            show_effects: show move indicator particle
        """
        if not self._initialized:
            raise RuntimeError("DotaCommands not initialized")

        # Fallback to Panorama JS if no local controller
        if not self.local_controller:
            return self._order_via_panorama(
                order_type, x, y, z, target_index, ability_index, queue)

        # Rate limit movement orders — avoid spamming CreateRemoteThread
        # Immediate actions (train, cast, buy) bypass rate limit
        now = time.time()
        is_immediate = order_type in (
            UnitOrder.TRAIN_ABILITY, UnitOrder.CAST_NO_TARGET,
            UnitOrder.CAST_TARGET, UnitOrder.CAST_POSITION,
            UnitOrder.PURCHASE_ITEM, UnitOrder.SELL_ITEM,
            UnitOrder.BUYBACK, UnitOrder.STOP,
        )
        if not is_immediate and now - self._last_order_time < self._order_min_interval:
            return True  # silently skip, not an error
        self._last_order_time = now

        # Validate controller is still readable (avoid crash on stale ptr)
        try:
            self.mem.pm.read_bytes(self.local_controller, 8)
        except Exception:
            return False

        process_handle = self.mem.pm.process_handle
        buf = self._ensure_order_buf()
        if not buf:
            return False

        vec3_addr = buf + 0x200
        vec3_data = struct.pack('<fff', x, y, z)
        _write_remote(process_handle, vec3_addr, vec3_data)

        sc = _build_prepare_orders_shellcode(
            self.local_controller, self.prepare_orders_addr, vec3_addr,
            order_type, target_index, ability_index, issuer, unit, queue,
            1 if show_effects else 0
        )
        _write_remote(process_handle, buf, sc)

        thread_id = ctypes.c_ulong(0)
        thread = kernel32.CreateRemoteThread(
            process_handle, None, 0, buf, 0, 0, ctypes.byref(thread_id)
        )
        if not thread:
            return False

        result = kernel32.WaitForSingleObject(thread, 3000)
        kernel32.CloseHandle(thread)
        return result == 0  # WAIT_OBJECT_0

    def move_to(self, x: float, y: float, z: float = 128.0, queue: bool = False) -> bool:
        """Move hero to world position."""
        return self.order(UnitOrder.MOVE_TO_POSITION, x, y, z,
                         queue=QueueBehavior.ALWAYS if queue else QueueBehavior.DEFAULT)

    def attack_move(self, x: float, y: float, z: float = 128.0) -> bool:
        """A-move to position."""
        return self.order(UnitOrder.ATTACK_MOVE, x, y, z)

    def stop(self) -> bool:
        """Stop current order."""
        return self.order(UnitOrder.STOP)

    def hold_position(self) -> bool:
        """Hold position (won't auto-attack)."""
        return self.order(UnitOrder.HOLD_POSITION)

    def attack_target(self, target_index: int) -> bool:
        """Attack specific entity by index."""
        return self.order(UnitOrder.ATTACK_TARGET, target_index=target_index)

    def cast_position(self, ability_index: int, x: float, y: float, z: float = 128.0) -> bool:
        """Cast ground-targeted ability."""
        return self.order(UnitOrder.CAST_POSITION, x, y, z, ability_index=ability_index)

    def cast_target(self, ability_index: int, target_index: int) -> bool:
        """Cast unit-targeted ability."""
        return self.order(UnitOrder.CAST_TARGET, target_index=target_index, ability_index=ability_index)

    def cast_no_target(self, ability_index: int) -> bool:
        """Cast ability with no target."""
        return self.order(UnitOrder.CAST_NO_TARGET, ability_index=ability_index)

    def train_ability(self, ability_index: int) -> bool:
        """Level up an ability by entity index (handle & 0x7FFF).
        NOTE: ability_index here is the entity index of the ability, NOT slot number.
        """
        return self.order(UnitOrder.TRAIN_ABILITY, ability_index=ability_index)

    def train_ability_slot(self, hero_entity: int, slot: int, game) -> bool:
        """Level up ability by slot number — resolves entity index automatically."""
        abilities = game.get_hero_abilities(hero_entity, getattr(game, '_handle_index', None))
        for ab in abilities:
            if ab.get("slot") == slot and ab.get("handle"):
                ent_idx = ab["handle"] & 0x7FFF
                return self.train_ability(ability_index=ent_idx)
        return False

    def buyback(self) -> bool:
        """Buy back from death."""
        return self.order(UnitOrder.BUYBACK)
