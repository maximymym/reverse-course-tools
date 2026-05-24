"""
Test hero movement via:
1. SendInput mouse right-click (game needs to be foreground)
2. mc_move + unit order console commands
3. Script-based approach
"""
import sys, time, ctypes, struct
sys.path.insert(0, ".")
from cheat.memory import DotaMemory
from cheat.game_state import DotaGame
from cheat.commands import DotaCommands

# SendInput structures
INPUT_MOUSE = 0
MOUSEEVENTF_RIGHTDOWN = 0x0008
MOUSEEVENTF_RIGHTUP = 0x0010
MOUSEEVENTF_MOVE = 0x0001
MOUSEEVENTF_ABSOLUTE = 0x8000

class MOUSEINPUT(ctypes.Structure):
    _fields_ = [
        ("dx", ctypes.c_long),
        ("dy", ctypes.c_long),
        ("mouseData", ctypes.c_ulong),
        ("dwFlags", ctypes.c_ulong),
        ("time", ctypes.c_ulong),
        ("dwExtraInfo", ctypes.POINTER(ctypes.c_ulong)),
    ]

class INPUT(ctypes.Structure):
    class _INPUT(ctypes.Union):
        _fields_ = [("mi", MOUSEINPUT)]
    _fields_ = [
        ("type", ctypes.c_ulong),
        ("input", _INPUT),
    ]

user32 = ctypes.windll.user32

def send_mouse_click(x, y, right=True):
    """Send mouse move + right click at screen position (absolute coords)."""
    # Convert to absolute coords (0-65535 range)
    screen_w = user32.GetSystemMetrics(0)
    screen_h = user32.GetSystemMetrics(1)
    abs_x = int(x * 65536 / screen_w)
    abs_y = int(y * 65536 / screen_h)

    # Move mouse
    move_input = INPUT()
    move_input.type = INPUT_MOUSE
    move_input.input.mi.dx = abs_x
    move_input.input.mi.dy = abs_y
    move_input.input.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE
    user32.SendInput(1, ctypes.byref(move_input), ctypes.sizeof(INPUT))
    time.sleep(0.05)

    # Right-click down
    down = MOUSEEVENTF_RIGHTDOWN if right else 0x0002  # LEFT_DOWN
    up = MOUSEEVENTF_RIGHTUP if right else 0x0004  # LEFT_UP

    click_input = INPUT()
    click_input.type = INPUT_MOUSE
    click_input.input.mi.dwFlags = down
    user32.SendInput(1, ctypes.byref(click_input), ctypes.sizeof(INPUT))
    time.sleep(0.02)

    click_input.input.mi.dwFlags = up
    user32.SendInput(1, ctypes.byref(click_input), ctypes.sizeof(INPUT))

def main():
    mem = DotaMemory()
    game = DotaGame(mem)
    cmd = DotaCommands(mem)

    game.init()
    cmd.init()

    print(f"[+] Game state: {game.get_game_state_name()}")

    heroes = game.find_all_heroes()
    if not heroes:
        print("[!] No heroes found")
        return

    hero_addr, hero_name = heroes[0]
    pos = game.read_position(hero_addr)
    print(f"[+] Hero: {hero_name} at ({pos[0]:.0f}, {pos[1]:.0f}, {pos[2]:.0f})")

    # Method 1: Try various console commands first
    print("\n=== Method 1: Console commands ===")
    test_cmds = [
        "dota_dev hero_teleport -2000 -1500 128",
        "setpos -2000 -1500 128",
        "setpos_exact -2000 -1500 128",
        "ent_fire !player SetLocalOrigin \"-2000 -1500 128\"",
    ]
    for tc in test_cmds:
        cmd.execute(tc)
        time.sleep(0.3)
        pos_new = game.read_position(hero_addr)
        moved = abs(pos_new[0] - pos[0]) > 10 or abs(pos_new[1] - pos[1]) > 10
        if moved:
            print(f"  '{tc}' -> MOVED to ({pos_new[0]:.0f}, {pos_new[1]:.0f})")
            pos = pos_new
            break
        else:
            print(f"  '{tc}' -> no movement")

    # Method 2: dota_execute_order (might work for unit orders)
    print("\n=== Method 2: Script/order commands ===")
    order_cmds = [
        # Protobuf order: DOTA_UNIT_ORDER_MOVE_TO_POSITION = 1
        "script_debug_unit_order_move_to_position -2000 -1500",
        "dota_unit_order_move_to_position -2000 -1500",
        "script EntFire(\"npc_dota_hero_monkey_king\", \"SetAbsOrigin\", \"-2000 -1500 128\")",
    ]
    for oc in order_cmds:
        cmd.execute(oc)
        time.sleep(0.5)
        pos_new = game.read_position(hero_addr)
        moved = abs(pos_new[0] - pos[0]) > 10 or abs(pos_new[1] - pos[1]) > 10
        if moved:
            print(f"  '{oc[:50]}' -> MOVED!")
            pos = pos_new
            break
        else:
            print(f"  '{oc[:50]}' -> no movement")

    # Method 3: Mouse simulation (requires Dota window to be foreground)
    print("\n=== Method 3: Mouse right-click (needs foreground Dota window) ===")
    print("  Switching to Dota 2 window...")

    # Find Dota 2 window
    hwnd = user32.FindWindowW(None, "Dota 2")
    if not hwnd:
        # Try other window titles
        hwnd = user32.FindWindowW(None, "dota 2")
    if hwnd:
        print(f"  Dota 2 window: {hwnd}")
        # Bring to foreground
        user32.SetForegroundWindow(hwnd)
        time.sleep(0.5)

        # Get window rect
        rect = ctypes.wintypes.RECT()
        user32.GetWindowRect(hwnd, ctypes.byref(rect))
        w = rect.right - rect.left
        h = rect.bottom - rect.top
        print(f"  Window: {rect.left},{rect.top} {w}x{h}")

        # Right-click at center-right of game view (should order move to the right)
        click_x = rect.left + w * 3 // 4
        click_y = rect.top + h // 2
        print(f"  Right-clicking at screen ({click_x}, {click_y})")

        pos_before = game.read_position(hero_addr)
        print(f"  Position before: ({pos_before[0]:.0f}, {pos_before[1]:.0f})")

        send_mouse_click(click_x, click_y, right=True)
        time.sleep(3.0)  # Wait for hero to walk

        pos_after = game.read_position(hero_addr)
        dist = ((pos_after[0]-pos_before[0])**2 + (pos_after[1]-pos_before[1])**2)**0.5
        print(f"  Position after: ({pos_after[0]:.0f}, {pos_after[1]:.0f})")
        print(f"  Distance moved: {dist:.0f}")

        if dist > 20:
            print("  [OK] Hero moved via mouse right-click!")
        else:
            print("  [?] No significant movement detected")
            print("  Make sure hero is selected and visible on screen")
    else:
        print("  [!] Dota 2 window not found")

    mem.close()
    print("\n[+] Done")

if __name__ == "__main__":
    main()
