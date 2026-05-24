"""
Multi-Instance Steam Setup — creates N Steam copies sharing one Dota install.

Each copy:
- Has its own Steam client + config (for separate account login)
- Symlinks steamapps -> original (shares game files, saves ~73GB per copy)
- Uses -master_ipc_name_override for IPC isolation

Usage:
    python setup_multi.py --count 2          # create Steam0, Steam1
    python setup_multi.py --count 5 --base D:\\MultiSteam  # custom base dir
    python setup_multi.py --status           # check existing instances
    python setup_multi.py --launch 0         # launch instance 0
    python setup_multi.py --launch-all       # launch all instances
"""
import os
import sys
import shutil
import json
import subprocess
import ctypes
import argparse
import time
from pathlib import Path

# ─── Config ───

STEAM_SRC = r"C:\Program Files (x86)\Steam"
DOTA_LIBRARY = r"D:\SteamLibrary"  # where Dota is installed
DEFAULT_BASE = r"C:\MultiSteam"    # where copies go
DEFAULT_COUNT = 2

# Dirs/files to SKIP when copying Steam (save space)
SKIP_DIRS = {"steamapps", "dumps", "depotcache", "logs", "appcache"}
# Only copy essential Steam files (not the full 2GB)
ESSENTIAL_TOP = {
    "Steam.exe", "steamservice.exe", "steamerrorreporter.exe", "steamerrorreporter64.exe",
    "crashhandler.dll", "crashhandler64.dll",
    "GameOverlayRenderer.dll", "GameOverlayRenderer64.dll",
    "gameoverlayui.exe",
    "tier0_s.dll", "tier0_s64.dll",
    "vstdlib_s.dll", "vstdlib_s64.dll",
    "steamclient.dll", "steamclient64.dll",
    "steam.dll",
    "CSERHelper.dll",
    "d3dcompiler_46.dll", "d3dcompiler_46_64.dll",
    "fossilize_engine_filters.json",
}
ESSENTIAL_DIRS = {"bin", "clientui", "config", "controller_base", "friends", "resource", "public", "tenfoot", "steam"}


def is_admin():
    try:
        return ctypes.windll.shell32.IsUserAnAdmin() != 0
    except:
        return False


def get_steam_size(src):
    """Estimate copy size (excluding steamapps)."""
    total = 0
    for item in os.listdir(src):
        full = os.path.join(src, item)
        if item.lower() in SKIP_DIRS:
            continue
        if os.path.isfile(full):
            total += os.path.getsize(full)
        elif os.path.isdir(full):
            for root, dirs, files in os.walk(full):
                for f in files:
                    total += os.path.getsize(os.path.join(root, f))
    return total


def copy_steam_instance(src: str, dest: str, instance_id: int):
    """Copy Steam client to dest, symlink steamapps."""
    if os.path.exists(dest):
        print(f"  [!] {dest} already exists, skipping copy")
        return True

    print(f"  [*] Creating Steam instance {instance_id} at {dest}")
    os.makedirs(dest, exist_ok=True)

    # Copy top-level files
    copied_files = 0
    for item in os.listdir(src):
        full_src = os.path.join(src, item)
        full_dst = os.path.join(dest, item)

        if item.lower() in SKIP_DIRS:
            continue

        if os.path.isfile(full_src):
            shutil.copy2(full_src, full_dst)
            copied_files += 1
        elif os.path.isdir(full_src):
            if item.lower() in {d.lower() for d in ESSENTIAL_DIRS}:
                shutil.copytree(full_src, full_dst, dirs_exist_ok=True)
                copied_files += 1

    # Create steamapps symlink -> original Steam library
    steamapps_src = os.path.join(src, "steamapps")
    steamapps_dst = os.path.join(dest, "steamapps")
    if not os.path.exists(steamapps_dst):
        os.symlink(steamapps_src, steamapps_dst, target_is_directory=True)
        print(f"  [+] Symlink: steamapps -> {steamapps_src}")

    # Also symlink the secondary library (D:\SteamLibrary)
    # Steam reads libraryfolders.vdf which points to D:\SteamLibrary

    # Create empty dirs that Steam expects
    for d in ["dumps", "logs", "depotcache"]:
        os.makedirs(os.path.join(dest, d), exist_ok=True)

    # Create appcache dir (Steam needs it)
    os.makedirs(os.path.join(dest, "appcache"), exist_ok=True)

    print(f"  [+] Copied {copied_files} items to {dest}")
    return True


def create_launcher_bat(dest: str, instance_id: int, ipc_name: str):
    """Create a .bat file to launch this Steam instance."""
    bat_path = os.path.join(dest, f"launch_steam{instance_id}.bat")
    content = f"""@echo off
echo Starting Steam instance {instance_id} (IPC: {ipc_name})...
cd /d "{dest}"
start "" "Steam.exe" -master_ipc_name_override {ipc_name} -noverifyfiles -nobootstrapupdate -skipinitialbootstrap
echo Steam {instance_id} launched. Log in with your account, then launch Dota 2.
echo.
echo To launch Dota directly (after login):
echo   Steam.exe -master_ipc_name_override {ipc_name} -applaunch 570 -novid -w 640 -h 480 -windowed +fps_max 15
pause
"""
    with open(bat_path, "w") as f:
        f.write(content)
    return bat_path


def create_dota_launcher_bat(dest: str, instance_id: int, ipc_name: str):
    """Create a .bat to launch Dota from this Steam instance."""
    bat_path = os.path.join(dest, f"launch_dota{instance_id}.bat")
    content = f"""@echo off
echo Launching Dota 2 from Steam instance {instance_id}...
cd /d "{dest}"
start "" "Steam.exe" -master_ipc_name_override {ipc_name} -applaunch 570 -novid -nojoy -w 640 -h 480 -windowed +fps_max 15 -console
"""
    with open(bat_path, "w") as f:
        f.write(content)
    return bat_path


def setup_instances(base_dir: str, count: int):
    """Main setup: create N Steam instances."""
    if not is_admin():
        print("[!] ERROR: Need admin rights for symlinks. Run as Administrator!")
        print("    Right-click cmd/terminal -> Run as Administrator")
        return False

    if not os.path.exists(STEAM_SRC):
        print(f"[!] Steam not found at {STEAM_SRC}")
        return False

    print(f"[*] Setting up {count} Steam instances in {base_dir}")
    print(f"[*] Source Steam: {STEAM_SRC}")

    # Estimate size
    est_size = get_steam_size(STEAM_SRC)
    print(f"[*] Estimated size per copy: {est_size / 1024 / 1024:.0f} MB")
    print(f"[*] Total estimated: {est_size * count / 1024 / 1024:.0f} MB")
    print()

    os.makedirs(base_dir, exist_ok=True)
    instances = []

    for i in range(count):
        dest = os.path.join(base_dir, f"Steam{i}")
        ipc_name = f"steam{i}"

        print(f"--- Instance {i} ---")
        copy_steam_instance(STEAM_SRC, dest, i)
        bat1 = create_launcher_bat(dest, i, ipc_name)
        bat2 = create_dota_launcher_bat(dest, i, ipc_name)
        print(f"  [+] Launchers: {bat1}")
        print()

        instances.append({
            "steam_path": dest,
            "ipc_name": ipc_name,
            "login": "",
            "note": f"account {i} — log in manually first"
        })

    # Update accounts.json
    config_dir = os.path.join(os.path.dirname(__file__), "config")
    os.makedirs(config_dir, exist_ok=True)
    config_path = os.path.join(config_dir, "accounts.json")
    config = {
        "instances": instances,
        "dota_app_id": 570,
        "launch_args": "-novid -nojoy -w 640 -h 480 -windowed +fps_max 15 -console",
        "base_dir": base_dir,
    }
    with open(config_path, "w") as f:
        json.dump(config, f, indent=4)
    print(f"[+] Config saved: {config_path}")

    # Print instructions
    print()
    print("=" * 60)
    print("SETUP COMPLETE")
    print("=" * 60)
    print()
    print("Next steps:")
    print(f"  1. Run {base_dir}\\Steam0\\launch_steam0.bat (as admin)")
    print(f"     -> Log in with account #0")
    print(f"  2. Run {base_dir}\\Steam1\\launch_steam1.bat (as admin)")
    print(f"     -> Log in with account #1")
    print(f"  3. After login, launch Dota from each:")
    print(f"     -> launch_dota0.bat, launch_dota1.bat")
    print(f"  4. Test from Python:")
    print(f"     from launcher import DotaLauncher")
    print(f"     DotaLauncher.find_running_dota_pids()")
    print()
    print("IMPORTANT:")
    print("  - First launch of each instance: Steam will update (~1 min)")
    print("  - Log in MANUALLY to each Steam instance with different account")
    print("  - After first login, subsequent launches are automatic")
    print("  - All instances share one Dota installation (no extra disk)")
    return True


def check_status(base_dir: str):
    """Show status of all instances."""
    if not os.path.exists(base_dir):
        print(f"[!] No instances found at {base_dir}")
        return

    # Find running processes
    from launcher import enum_processes
    steams = enum_processes("steam.exe")
    dotas = enum_processes("dota2.exe")

    for item in sorted(os.listdir(base_dir)):
        path = os.path.join(base_dir, item)
        if not os.path.isdir(path) or not item.startswith("Steam"):
            continue

        steam_exe = os.path.join(path, "Steam.exe")
        exists = os.path.exists(steam_exe)
        # Check if steamapps symlink works
        sa = os.path.join(path, "steamapps")
        sa_ok = os.path.islink(sa) and os.path.exists(sa)
        # Check if logged in (config/loginusers.vdf exists)
        login_file = os.path.join(path, "config", "loginusers.vdf")
        logged_in = os.path.exists(login_file)

        idx_str = item.replace("Steam", "")
        print(f"  {item}: exe={'OK' if exists else 'MISSING'}  steamapps={'OK' if sa_ok else 'BROKEN'}  logged_in={logged_in}")

    print(f"\n  Running Steam.exe: {len(steams)} processes")
    print(f"  Running dota2.exe: {len(dotas)} processes")
    for d in dotas:
        print(f"    PID {d['pid']} (parent {d['parent_pid']})")


def launch_instance(base_dir: str, idx: int):
    """Launch a single Steam+Dota instance."""
    dest = os.path.join(base_dir, f"Steam{idx}")
    ipc_name = f"steam{idx}"
    steam_exe = os.path.join(dest, "Steam.exe")

    if not os.path.exists(steam_exe):
        print(f"[!] Instance {idx} not found at {dest}")
        return

    cmd = [
        steam_exe,
        "-master_ipc_name_override", ipc_name,
        "-applaunch", "570",
        "-novid", "-nojoy", "-w", "640", "-h", "480", "-windowed",
        "+fps_max", "15", "-console"
    ]
    print(f"[*] Launching instance {idx}: {ipc_name}")
    subprocess.Popen(cmd, cwd=dest)


def launch_all(base_dir: str):
    """Launch all instances with 5s delay between each."""
    if not os.path.exists(base_dir):
        print(f"[!] No instances at {base_dir}")
        return

    indices = []
    for item in sorted(os.listdir(base_dir)):
        if item.startswith("Steam") and os.path.isdir(os.path.join(base_dir, item)):
            try:
                indices.append(int(item.replace("Steam", "")))
            except ValueError:
                pass

    print(f"[*] Launching {len(indices)} instances...")
    for i, idx in enumerate(indices):
        launch_instance(base_dir, idx)
        if i < len(indices) - 1:
            print(f"  [*] Waiting 5s before next launch...")
            time.sleep(5)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Multi-Instance Steam Setup")
    parser.add_argument("--count", type=int, default=DEFAULT_COUNT, help="Number of instances")
    parser.add_argument("--base", type=str, default=DEFAULT_BASE, help="Base directory for copies")
    parser.add_argument("--status", action="store_true", help="Show instance status")
    parser.add_argument("--launch", type=int, metavar="N", help="Launch instance N")
    parser.add_argument("--launch-all", action="store_true", help="Launch all instances")
    args = parser.parse_args()

    if args.status:
        check_status(args.base)
    elif args.launch is not None:
        launch_instance(args.base, args.launch)
    elif args.launch_all:
        launch_all(args.base)
    else:
        setup_instances(args.base, args.count)
