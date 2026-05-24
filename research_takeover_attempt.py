"""
Bot takeover attempt:
1. Check sv_cheats in this proper bot game
2. Try VScript to attach bot AI to our hero
3. Try flipping controller flags
4. Monitor if our hero starts moving
"""
import sys, os, time, math, struct
sys.path.insert(0, os.path.dirname(__file__))
from cheat.memory import DotaMemory
from cheat.commands import DotaCommands
from cheat.game_state import DotaGame
from cheat.offsets import GameState, BaseEntity, GameSceneNode, BaseNPC, EntitySystem, EntityIdentity


def find_our_hero(game, mem):
    """Find local player's hero entity."""
    seen = set()
    for list_off in EntitySystem.CHUNK_LISTS:
        chunk_list = mem.read_ptr(game.entity_system + list_off)
        if not chunk_list or chunk_list < 0x10000:
            continue
        for ci in range(EntitySystem.MAX_CHUNKS):
            try:
                chunk_ptr = mem.read_ptr(chunk_list + ci * 8)
            except:
                continue
            if not chunk_ptr or chunk_ptr < 0x10000:
                continue
            try:
                chunk_data = mem.pm.read_bytes(chunk_ptr, EntitySystem.SLOTS_PER_CHUNK * EntityIdentity.STRIDE)
            except:
                continue
            for si in range(EntitySystem.SLOTS_PER_CHUNK):
                off = si * EntityIdentity.STRIDE
                ent_ptr = struct.unpack("<Q", chunk_data[off:off+8])[0]
                if not ent_ptr or ent_ptr < 0x10000 or ent_ptr in seen:
                    continue
                seen.add(ent_ptr)
                try:
                    hp = mem.read_i32(ent_ptr + BaseEntity.HEALTH)
                    team = mem.read_u8(ent_ptr + BaseEntity.TEAM_NUM)
                    if team not in (2, 3) or hp <= 0 or hp > 10000:
                        continue
                    uname_ptr = mem.read_ptr(ent_ptr + BaseNPC.UNIT_NAME)
                    if not uname_ptr or uname_ptr < 0x10000:
                        continue
                    uname = mem.read_string(uname_ptr, 64)
                    if "hero" in uname:
                        scene = mem.read_ptr(ent_ptr + BaseEntity.GAME_SCENE_NODE)
                        x = mem.read_f32(scene + GameSceneNode.VEC_ABS_ORIGIN)
                        y = mem.read_f32(scene + GameSceneNode.VEC_ABS_ORIGIN + 4)
                        yield ent_ptr, uname, hp, team, x, y
                except:
                    continue


def main():
    mem = DotaMemory()
    print(f"[+] PID {mem.pid}")
    game = DotaGame(mem)
    game.init()
    cmd = DotaCommands(mem)
    cmd.init(game)
    print(f"[+] State: {game.get_game_state()}")

    # === 1. Check sv_cheats ===
    print("\n=== STEP 1: sv_cheats check ===")
    sv = cmd.convar.get_int("sv_cheats")
    print(f"  sv_cheats = {sv}")
    if sv != 1:
        print("  Setting sv_cheats = 1 via memory...")
        cmd.convar.set_int("sv_cheats", 1)
        cmd.execute("sv_cheats 1")
        time.sleep(1)
        sv = cmd.convar.get_int("sv_cheats")
        print(f"  sv_cheats after set: {sv}")

    # === 2. Test VScript ===
    print("\n=== STEP 2: VScript test ===")
    # Give gold (visible effect)
    cmd.execute('script PlayerResource:SetGold(0, 99999, true)')
    time.sleep(2)
    # Set time (visible)
    cmd.execute('script GameRules:SetTimeOfDay(0.0)')
    time.sleep(2)

    print("  [CHECK DOTA] Did gold change? Did time become night?")

    # === 3. Find our controller and hero ===
    print("\n=== STEP 3: Find our controller ===")
    our_ctrl = None
    seen = set()
    for list_off in EntitySystem.CHUNK_LISTS:
        chunk_list = mem.read_ptr(game.entity_system + list_off)
        if not chunk_list or chunk_list < 0x10000:
            continue
        for ci in range(EntitySystem.MAX_CHUNKS):
            try:
                chunk_ptr = mem.read_ptr(chunk_list + ci * 8)
            except:
                continue
            if not chunk_ptr or chunk_ptr < 0x10000:
                continue
            try:
                chunk_data = mem.pm.read_bytes(chunk_ptr, EntitySystem.SLOTS_PER_CHUNK * EntityIdentity.STRIDE)
            except:
                continue
            for si in range(EntitySystem.SLOTS_PER_CHUNK):
                off = si * EntityIdentity.STRIDE
                ent_ptr = struct.unpack("<Q", chunk_data[off:off+8])[0]
                if not ent_ptr or ent_ptr < 0x10000 or ent_ptr in seen:
                    continue
                seen.add(ent_ptr)
                try:
                    ident_addr = chunk_ptr + off
                    dname_ptr = mem.read_ptr(ident_addr + EntityIdentity.DESIGNER_NAME)
                    if not dname_ptr or dname_ptr < 0x10000:
                        continue
                    dname = mem.read_string(dname_ptr, 64)
                    if dname == "dota_player_controller":
                        slot = mem.read_i32(ent_ptr + 0x908)
                        if slot == 0:
                            our_ctrl = ent_ptr
                except:
                    continue

    if our_ctrl:
        print(f"  Our controller: {hex(our_ctrl)}")
        pname = mem.read_string(our_ctrl + 0x6E0, 32)
        print(f"  Name: {pname}")

        # Read current flags
        flag_a68 = mem.read_u8(our_ctrl + 0x0A68)
        flag_a80 = mem.read_u8(our_ctrl + 0x0A80)
        print(f"  +0x0A68 = {flag_a68} (human=1, bot=0)")
        print(f"  +0x0A80 = {flag_a80} (human=1, bot=0)")

    # === 4. List heroes and their positions ===
    print("\n=== STEP 4: Current heroes ===")
    heroes = list(find_our_hero(game, mem))
    for ent, name, hp, team, x, y in heroes:
        t = {2: "Rad", 3: "Dire"}.get(team, "?")
        print(f"  {name:40s} {t} hp={hp} pos=({x:.0f},{y:.0f})")

    # === 5. Try VScript to add bot AI to our player ===
    print("\n=== STEP 5: VScript bot attach attempts ===")

    vscript_cmds = [
        # Enable bot thinking
        'script GameRules:GetGameModeEntity():SetBotThinkingEnabled(true)',
        # Try SetBotDifficulty for player 0
        'script GameRules:SetBotDifficulty(0, 2)',
        # Try AddBotPlayerWithEntityScript to add a bot on our slot
        # (might fail because slot 0 is taken)
        # Try PlayerResource to flag us as bot
        'script PlayerResource:SetFakeClient(0, true)',
        'script PlayerResource:SetHaveBot(0, true)',
        # Try generic Think system
        'script Think(function() print("THINK_WORKS") end, "test", 1)',
    ]
    for c in vscript_cmds:
        print(f"  [>] {c}")
        cmd.execute(c)
        time.sleep(1)

    # === 6. Try flipping controller flags ===
    print("\n=== STEP 6: Flip controller flags ===")
    if our_ctrl:
        print(f"  Setting +0x0A68 = 0 (was {flag_a68})")
        mem.pm.write_bytes(our_ctrl + 0x0A68, b'\x00', 1)

        print(f"  Setting +0x0A80 = 0 (was {flag_a80})")
        mem.pm.write_bytes(our_ctrl + 0x0A80, b'\x00', 1)

        # Verify
        print(f"  After write: +0x0A68 = {mem.read_u8(our_ctrl + 0x0A68)}")
        print(f"  After write: +0x0A80 = {mem.read_u8(our_ctrl + 0x0A80)}")

    # === 7. Wait and check if our hero moves ===
    print("\n=== STEP 7: Movement check (30s) ===")
    # Find our hero (should be the one matching our team)
    our_hero = None
    for ent, name, hp, team, x, y in find_our_hero(game, mem):
        if team == 2:  # We're Radiant (slot 0)
            # Could be our hero or a bot's hero
            our_hero = (ent, name, x, y)
            # Take the first Radiant hero for now

    if our_hero:
        ent, name, x0, y0 = our_hero
        print(f"  Tracking: {name} at ({x0:.0f}, {y0:.0f})")
        time.sleep(30)
        scene = mem.read_ptr(ent + BaseEntity.GAME_SCENE_NODE)
        x1 = mem.read_f32(scene + GameSceneNode.VEC_ABS_ORIGIN)
        y1 = mem.read_f32(scene + GameSceneNode.VEC_ABS_ORIGIN + 4)
        dist = math.sqrt((x1-x0)**2 + (y1-y0)**2)

        if dist > 50:
            print(f"\n  >>> OUR HERO MOVED {dist:.0f} units! BOT AI TAKEOVER SUCCESSFUL!")
        else:
            print(f"\n  >>> Hero stationary ({dist:.1f} units). Flags alone don't trigger bot AI.")
            print("  Bot AI is managed by BotManager server-side.")

    # === 8. Restore flags ===
    print("\n=== STEP 8: Restore flags ===")
    if our_ctrl:
        mem.pm.write_bytes(our_ctrl + 0x0A68, bytes([flag_a68]), 1)
        mem.pm.write_bytes(our_ctrl + 0x0A80, bytes([flag_a80]), 1)
        print(f"  Restored +0x0A68 = {flag_a68}, +0x0A80 = {flag_a80}")

    print("\nDone!")


if __name__ == "__main__":
    main()
