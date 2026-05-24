"""
Dota 2 Multi-Instance via AppData Redirect + Mutex Kill.

Each instance gets its own USERPROFILE/APPDATA/LOCALAPPDATA/TEMP env vars,
so Steam's CEF cache doesn't conflict between instances.
Combined with -master_ipc_name_override for IPC isolation.
dota_singleton_mutex is closed after first Dota launch to allow second+.

No Windows user creation, no sandbox, no special permissions.
VAC sees a normal process — nothing to detect.

Usage:
    from sandbox import BotSandbox

    sb = BotSandbox(count=2)
    sb.setup()                         # create profile dirs
    sb.launch_steam(0)                 # launch Steam instance 0
    sb.launch_steam(1)                 # launch Steam instance 1
    sb.launch_dota(0)                  # launch Dota #0
    sb.launch_dota(1)                  # auto-kills mutex, launches Dota #1
    pids = sb.get_dota_pids()          # find running Dota PIDs
    sb.kill_all()                      # stop everything
"""
import os
import time
import subprocess
import json
import ctypes
import ctypes.wintypes
import struct
from pathlib import Path

DEFAULT_STEAM = r"C:\Program Files (x86)\Steam\steam.exe"
DEFAULT_PROFILES = r"C:\BotProfiles"
BOT_STEAM_DIR = r"C:\BotSteam"  # per-instance Steam copies (junctions + real config)
HANDLE_EXE = r"C:\temp\handle64.exe"
CONFIG_PATH = os.path.join(os.path.dirname(__file__), "config", "sandbox.json")

SINGLETON_MUTEX = "dota_singleton_mutex"


def close_dota_mutex(dota_pid: int = None) -> bool:
    """Close dota_singleton_mutex to allow multiple Dota instances.

    Uses Sysinternals handle64.exe to find and close the mutex.
    If dota_pid is None, finds it automatically.
    """
    if not os.path.exists(HANDLE_EXE):
        print(f"[!] handle64.exe not found at {HANDLE_EXE}")
        print("    Download: https://download.sysinternals.com/files/Handle.zip")
        return False

    # Find dota PID if not given
    if dota_pid is None:
        from launcher import enum_processes
        dotas = enum_processes("dota2.exe")
        if not dotas:
            return False  # no dota running, nothing to close
        dota_pid = dotas[0]["pid"]

    # Find the mutex handle
    try:
        r = subprocess.run(
            [HANDLE_EXE, "-accepteula", "-a", "-p", str(dota_pid)],
            capture_output=True, text=True, timeout=15
        )
    except Exception as e:
        print(f"[!] handle64.exe failed: {e}")
        return False

    for line in r.stdout.splitlines():
        if SINGLETON_MUTEX in line:
            # Parse handle ID: "  354: Mutant  ..."
            parts = line.strip().split(":")
            if len(parts) >= 2:
                handle_hex = parts[0].strip()
                # Close it
                r2 = subprocess.run(
                    [HANDLE_EXE, "-accepteula", "-c", handle_hex, "-p", str(dota_pid), "-y"],
                    capture_output=True, text=True, timeout=10
                )
                if "Handle closed" in r2.stdout:
                    print(f"[+] Closed {SINGLETON_MUTEX} (handle 0x{handle_hex}, PID {dota_pid})")
                    return True
                else:
                    print(f"[!] Failed to close handle: {r2.stdout.strip()}")
                    return False

    # Mutex not found (maybe already closed)
    return True


class BotSandbox:
    """Multi-instance Steam/Dota via AppData env redirect."""

    def __init__(self, count: int = 2, steam_exe: str = DEFAULT_STEAM,
                 profiles_dir: str = DEFAULT_PROFILES, config_path: str = CONFIG_PATH):
        self.count = count
        self.steam_exe = steam_exe
        self.steam_dir = os.path.dirname(steam_exe)
        self.profiles_dir = profiles_dir
        self.config_path = config_path
        self._load_config()

    def _load_config(self):
        if os.path.exists(self.config_path):
            with open(self.config_path, "r") as f:
                data = json.load(f)
                self.count = data.get("count", self.count)
                self.steam_exe = data.get("steam_exe", self.steam_exe)
                self.profiles_dir = data.get("profiles_dir", self.profiles_dir)
                self.steam_dir = os.path.dirname(self.steam_exe)

    def _save_config(self):
        os.makedirs(os.path.dirname(self.config_path), exist_ok=True)
        with open(self.config_path, "w") as f:
            json.dump({
                "count": self.count,
                "steam_exe": self.steam_exe,
                "profiles_dir": self.profiles_dir,
            }, f, indent=4)

    def _profile_dir(self, idx: int) -> str:
        return os.path.join(self.profiles_dir, f"bot{idx}")

    def _env_for(self, idx: int) -> dict:
        """Build environment with redirected AppData + TEMP for instance idx."""
        profile = self._profile_dir(idx)
        env = os.environ.copy()
        env["USERPROFILE"] = profile
        env["APPDATA"] = os.path.join(profile, "AppData", "Roaming")
        env["LOCALAPPDATA"] = os.path.join(profile, "AppData", "Local")
        env["TEMP"] = os.path.join(profile, "Temp")
        env["TMP"] = os.path.join(profile, "Temp")
        return env

    def _ipc_name(self, idx: int) -> str:
        return f"steam{idx}"

    # ─── Setup ───

    def setup(self) -> bool:
        """Create profile directories for all instances."""
        for i in range(self.count):
            profile = self._profile_dir(i)
            os.makedirs(os.path.join(profile, "AppData", "Roaming"), exist_ok=True)
            os.makedirs(os.path.join(profile, "AppData", "Local"), exist_ok=True)
            os.makedirs(os.path.join(profile, "Temp"), exist_ok=True)
            print(f"  [+] Profile {i}: {profile}")

        self._save_config()
        print(f"[+] {self.count} profiles ready at {self.profiles_dir}")
        return True

    # ─── Launch ───

    def _set_autologin(self, idx: int):
        """Set registry AutoLoginUser to the account saved in this instance's config."""
        import winreg
        bot_steam = os.path.join(BOT_STEAM_DIR, str(idx))
        vdf_path = os.path.join(bot_steam, "config", "loginusers.vdf")
        if not os.path.exists(vdf_path):
            return

        # Parse loginusers.vdf — find account with AllowAutoLogin=1
        username = None
        with open(vdf_path, "r") as f:
            lines = f.readlines()
        for i, line in enumerate(lines):
            if '"AccountName"' in line:
                username = line.split('"')[3]
            if '"AllowAutoLogin"' in line and '"1"' in line and username:
                break

        if not username:
            return

        # Set registry
        try:
            key = winreg.OpenKeyEx(winreg.HKEY_CURRENT_USER,
                                   r"Software\Valve\Steam", 0, winreg.KEY_SET_VALUE)
            winreg.SetValueEx(key, "AutoLoginUser", 0, winreg.REG_SZ, username)
            winreg.SetValueEx(key, "RememberPassword", 0, winreg.REG_DWORD, 1)
            # Also set SteamPath/SteamExe to this instance's dir
            bot_path = os.path.join(BOT_STEAM_DIR, str(idx)).replace("\\", "/").lower()
            winreg.SetValueEx(key, "SteamPath", 0, winreg.REG_SZ, bot_path)
            winreg.SetValueEx(key, "SteamExe", 0, winreg.REG_SZ, bot_path + "/steam.exe")
            winreg.CloseKey(key)
            print(f"  [+] Registry AutoLoginUser={username}")
        except Exception as e:
            print(f"  [!] Registry write failed: {e}")

    def _steam_exe_for(self, idx: int) -> tuple[str, str]:
        """Get Steam exe path and working dir for instance idx.

        Uses per-instance Steam copy from BOT_STEAM_DIR if available,
        falls back to main Steam install.
        """
        bot_steam = os.path.join(BOT_STEAM_DIR, str(idx))
        bot_exe = os.path.join(bot_steam, "steam.exe")
        if os.path.exists(bot_exe):
            return bot_exe, bot_steam
        return self.steam_exe, self.steam_dir

    def launch_steam(self, idx: int, with_dota: bool = False,
                     width: int = 640, height: int = 480, fps_max: int = 10) -> int:
        """Launch Steam instance from per-bot Steam copy + AppData redirect.

        Each bot has its own Steam copy (C:\\BotSteam\\N) with real config/ dir
        (separate login) and junctions for everything else (zero extra disk).

        If with_dota=True, also passes -applaunch 570 to start Dota immediately.
        """
        if idx >= self.count:
            print(f"[!] Instance {idx} not configured (count={self.count})")
            return 0

        exe, cwd = self._steam_exe_for(idx)
        env = self._env_for(idx)
        ipc = self._ipc_name(idx)

        # Set registry to this instance's account before launch
        self._set_autologin(idx)

        cmd = [exe, "-master_ipc_name_override", ipc]

        if with_dota:
            cmd += [
                "-applaunch", "570",
                "-novid", "-nojoy", "-noaafonts",
                "-w", str(width), "-h", str(height), "-sw",
                "+fps_max", str(fps_max),
                "+r_drawparticles", "0",
                "+cl_globallight_shadow_mode", "0",
                "+dota_cheap_water", "1",
                "+r_deferred_height_fog", "0",
                "-console",
            ]

        proc = subprocess.Popen(cmd, cwd=cwd, env=env)
        label = f"Steam+Dota" if with_dota else "Steam"
        print(f"[+] {label} #{idx} (IPC={ipc}, dir={cwd}): PID {proc.pid}")
        return proc.pid

    def launch_dota(self, idx: int, width: int = 640, height: int = 480,
                    fps_max: int = 10) -> int:
        """Launch Dota by (re)starting Steam+Dota together.

        Kills dota_singleton_mutex if another Dota is already running.
        NOTE: This kills and restarts the Steam instance for this idx.
        """
        if idx >= self.count:
            print(f"[!] Instance {idx} not configured")
            return 0

        # Kill singleton mutex if another Dota is already running
        from launcher import enum_processes
        existing_dotas = enum_processes("dota2.exe")
        if existing_dotas:
            close_dota_mutex(existing_dotas[0]["pid"])
            time.sleep(1)

        return self.launch_steam(idx, with_dota=True,
                                 width=width, height=height, fps_max=fps_max)

    def launch_all_steam(self, delay: float = 15) -> list[int]:
        """Launch all Steam instances with delay between."""
        pids = []
        for i in range(self.count):
            pid = self.launch_steam(i)
            pids.append(pid)
            if i < self.count - 1:
                print(f"  Waiting {delay}s...")
                time.sleep(delay)
        return pids

    def launch_all_dota(self, delay: float = 5) -> list[int]:
        """Launch Dota on all Steam instances."""
        pids = []
        for i in range(self.count):
            pid = self.launch_dota(i)
            pids.append(pid)
            if i < self.count - 1:
                time.sleep(delay)
        return pids

    def launch_all(self, steam_delay: float = 15, dota_delay: float = 45,
                   width: int = 640, height: int = 480, fps_max: int = 10) -> dict:
        """Full launch: Steam instances -> wait for login -> Dota instances.

        Returns {idx: dota_pid}.
        """
        print("=== Launching Steam instances ===")
        self.launch_all_steam(delay=steam_delay)

        print(f"\n[*] Waiting {dota_delay}s for Steam login...")
        time.sleep(dota_delay)

        print("\n=== Launching Dota instances ===")
        from launcher import enum_processes
        known = set(p["pid"] for p in enum_processes("dota2.exe"))

        for i in range(self.count):
            self.launch_dota(i, width=width, height=height, fps_max=fps_max)
            if i < self.count - 1:
                print("  Waiting 30s for Dota to start...")
                time.sleep(30)

        # Wait for Dotas to appear
        print("\n[*] Waiting for Dota processes...")
        result = {}
        for _ in range(30):
            dotas = enum_processes("dota2.exe")
            new = [d for d in dotas if d["pid"] not in known]
            if len(new) >= self.count:
                for i, d in enumerate(new[:self.count]):
                    result[i] = d["pid"]
                break
            time.sleep(3)

        print(f"[+] Running: {len(result)} Dota instances")
        return result

    # ─── Status ───

    def get_dota_pids(self) -> list[int]:
        """Return all running dota2.exe PIDs."""
        from launcher import enum_processes
        return [p["pid"] for p in enum_processes("dota2.exe")]

    def get_steam_count(self) -> int:
        """Count running Steam processes."""
        from launcher import enum_processes
        return len([p for p in enum_processes("steam.exe")
                    if p["name"].lower() == "steam.exe"])

    def status(self):
        """Print status."""
        from launcher import enum_processes
        steams = len([p for p in enum_processes("steam.exe")
                      if p["name"].lower() == "steam.exe"])
        dotas = enum_processes("dota2.exe")
        print(f"Configured: {self.count} instances")
        print(f"Profiles: {self.profiles_dir}")
        print(f"Running: {steams} Steam, {len(dotas)} Dota")
        for d in dotas:
            print(f"  dota2.exe PID {d['pid']}")

    # ─── Kill ───

    def kill_all(self):
        """Kill all Steam and Dota processes."""
        subprocess.run("taskkill /IM dota2.exe /F", shell=True,
                       capture_output=True, timeout=10)
        time.sleep(2)
        subprocess.run("taskkill /IM steam.exe /F", shell=True,
                       capture_output=True, timeout=10)
        print("[+] All killed")

    def cleanup(self):
        """Remove profile directories."""
        import shutil
        if os.path.exists(self.profiles_dir):
            shutil.rmtree(self.profiles_dir, ignore_errors=True)
            print(f"[+] Removed {self.profiles_dir}")


# ─── CLI ───
if __name__ == "__main__":
    import sys

    args = sys.argv[1:]
    count = 2
    for i, a in enumerate(args):
        if a == "--count" and i + 1 < len(args):
            count = int(args[i + 1])

    sb = BotSandbox(count=count)

    if "--setup" in args:
        sb.setup()
    elif "--steam" in args:
        idx = 0
        for i, a in enumerate(args):
            if a == "--steam" and i + 1 < len(args):
                try: idx = int(args[i + 1])
                except: pass
        sb.launch_steam(idx)
    elif "--steam-all" in args:
        sb.launch_all_steam()
    elif "--dota-all" in args:
        sb.launch_all_dota()
    elif "--status" in args:
        sb.status()
    elif "--kill" in args:
        sb.kill_all()
    elif "--cleanup" in args:
        sb.cleanup()
    else:
        print("Usage:")
        print("  sandbox.py --setup [--count N]    Create N profile dirs")
        print("  sandbox.py --steam N              Launch Steam instance N")
        print("  sandbox.py --steam-all            Launch all Steam instances")
        print("  sandbox.py --dota-all             Launch Dota on all instances")
        print("  sandbox.py --status               Show status")
        print("  sandbox.py --kill                 Kill everything")
        print("  sandbox.py --cleanup              Remove profiles")
