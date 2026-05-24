"""
Dota 2 Multi-Instance Launcher — runs N Steam instances from ONE Steam install.

Uses -master_ipc_name_override for IPC isolation. No Steam copies needed.
Second+ instances use -userchooser to select a different account.

Usage:
    from launcher import DotaLauncher, enum_processes

    launcher = DotaLauncher()                    # loads config/accounts.json
    launcher.launch_steam()                      # launch all Steam instances
    launcher.launch_dota()                       # launch Dota on all instances
    pids = DotaLauncher.find_running_dota_pids() # discover running Dota PIDs
"""
import json
import os
import time
import subprocess
import ctypes
from pathlib import Path

# ─── Process enumeration via Win32 API ───
import ctypes.wintypes

TH32CS_SNAPPROCESS = 0x2
INVALID_HANDLE_VALUE = ctypes.c_void_p(-1).value


class PROCESSENTRY32(ctypes.Structure):
    _fields_ = [
        ("dwSize", ctypes.wintypes.DWORD),
        ("cntUsage", ctypes.wintypes.DWORD),
        ("th32ProcessID", ctypes.wintypes.DWORD),
        ("th32DefaultHeapID", ctypes.POINTER(ctypes.c_ulong)),
        ("th32ModuleID", ctypes.wintypes.DWORD),
        ("cntThreads", ctypes.wintypes.DWORD),
        ("th32ParentProcessID", ctypes.wintypes.DWORD),
        ("pcPriClassBase", ctypes.c_long),
        ("dwFlags", ctypes.wintypes.DWORD),
        ("szExeFile", ctypes.c_char * 260),
    ]


def enum_processes(name_filter: str = None) -> list[dict]:
    """Enumerate running processes. Returns [{pid, name, parent_pid}, ...]."""
    kernel32 = ctypes.windll.kernel32
    snap = kernel32.CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)
    if snap == INVALID_HANDLE_VALUE:
        return []

    pe = PROCESSENTRY32()
    pe.dwSize = ctypes.sizeof(PROCESSENTRY32)
    result = []

    if kernel32.Process32First(snap, ctypes.byref(pe)):
        while True:
            pname = pe.szExeFile.decode("utf-8", errors="replace")
            if name_filter is None or name_filter.lower() in pname.lower():
                result.append({
                    "pid": pe.th32ProcessID,
                    "name": pname,
                    "parent_pid": pe.th32ParentProcessID,
                })
            if not kernel32.Process32Next(snap, ctypes.byref(pe)):
                break

    kernel32.CloseHandle(snap)
    return result


def get_process_cmdline(pid: int) -> str:
    """Get command line of a process via WMI (slow but reliable)."""
    try:
        out = subprocess.check_output(
            ["wmic", "process", "where", f"ProcessId={pid}", "get", "CommandLine", "/format:list"],
            timeout=5, stderr=subprocess.DEVNULL
        ).decode("utf-8", errors="replace")
        for line in out.splitlines():
            if line.startswith("CommandLine="):
                return line[len("CommandLine="):]
    except Exception:
        pass
    return ""


# ─── Launcher ───

CONFIG_PATH = os.path.join(os.path.dirname(__file__), "config", "accounts.json")


class DotaLauncher:
    def __init__(self, config_path: str = CONFIG_PATH):
        with open(config_path, "r") as f:
            self.config = json.load(f)
        self.instances = self.config["instances"]
        self.steam_exe = self.config.get("steam_exe", r"C:\Program Files (x86)\Steam\steam.exe")
        self.app_id = self.config.get("dota_app_id", 570)
        self.launch_args = self.config.get("launch_args", "-novid -w 640 -h 480")

    def launch_steam(self, indices: list[int] = None, delay: float = 10) -> list[int]:
        """Launch Steam instances (login only, no game). Returns launched indices.

        First instance auto-logs in with saved credentials.
        Subsequent instances show -userchooser popup.
        """
        if indices is None:
            indices = list(range(len(self.instances)))

        if not os.path.exists(self.steam_exe):
            print(f"[!] Steam not found: {self.steam_exe}")
            return []

        steam_dir = os.path.dirname(self.steam_exe)
        launched = []

        for i, idx in enumerate(indices):
            inst = self.instances[idx]
            ipc_name = inst["ipc_name"]

            cmd = [self.steam_exe, "-master_ipc_name_override", ipc_name]
            if inst.get("userchooser", False):
                cmd.append("-userchooser")

            print(f"[*] Launching Steam #{idx}: {ipc_name}" +
                  (" (userchooser)" if "-userchooser" in cmd else ""))
            subprocess.Popen(cmd, cwd=steam_dir)
            launched.append(idx)

            if i < len(indices) - 1:
                print(f"  Waiting {delay}s before next...")
                time.sleep(delay)

        return launched

    def launch_dota(self, indices: list[int] = None, wait_for_dota: bool = True,
                    wait_timeout: float = 120) -> dict[int, int]:
        """Launch Dota 2 on already-running Steam instances. Returns {index: dota_pid}."""
        if indices is None:
            indices = list(range(len(self.instances)))

        steam_dir = os.path.dirname(self.steam_exe)
        known_pids = set(p["pid"] for p in enum_processes("dota2.exe"))
        launched = {}

        for idx in indices:
            inst = self.instances[idx]
            ipc_name = inst["ipc_name"]

            cmd = [
                self.steam_exe,
                "-master_ipc_name_override", ipc_name,
                "-applaunch", str(self.app_id),
            ] + self.launch_args.split()

            print(f"[*] Launching Dota on #{idx}: {ipc_name}")
            subprocess.Popen(cmd, cwd=steam_dir)
            time.sleep(3)

        if not wait_for_dota:
            return {}

        print(f"[*] Waiting for {len(indices)} dota2.exe processes (timeout={wait_timeout}s)...")
        start = time.time()

        while time.time() - start < wait_timeout:
            current = enum_processes("dota2.exe")
            new_pids = [p for p in current if p["pid"] not in known_pids]

            for p in new_pids:
                for idx in indices:
                    if idx not in launched:
                        launched[idx] = p["pid"]
                        known_pids.add(p["pid"])
                        print(f"  [+] Instance {idx}: dota2.exe PID {p['pid']}")
                        break

            if len(launched) >= len(indices):
                break
            time.sleep(3)

        if len(launched) < len(indices):
            missing = [i for i in indices if i not in launched]
            print(f"[!] Timeout: instances {missing} did not start dota2.exe")

        return launched

    def launch_all(self, steam_delay: float = 10, **kwargs) -> dict[int, int]:
        """Launch all Steam instances, then all Dota instances."""
        self.launch_steam(delay=steam_delay)
        print("[*] Waiting 15s for Steam login...")
        time.sleep(15)
        return self.launch_dota(**kwargs)

    @staticmethod
    def find_running_dota_pids() -> list[int]:
        """Find all running dota2.exe PIDs."""
        procs = enum_processes("dota2.exe")
        return [p["pid"] for p in procs]

    @staticmethod
    def find_running_steam_pids() -> list[int]:
        """Find all running Steam.exe PIDs."""
        procs = enum_processes("steam.exe")
        return [p["pid"] for p in procs if p["name"].lower() == "steam.exe"]


# ─── CLI ───
if __name__ == "__main__":
    import sys

    if "--find" in sys.argv:
        pids = DotaLauncher.find_running_dota_pids()
        print(f"Running dota2.exe: {pids}")
    elif "--steam" in sys.argv:
        launcher = DotaLauncher()
        launcher.launch_steam()
    elif "--dota" in sys.argv:
        launcher = DotaLauncher()
        launcher.launch_dota()
    elif "--all" in sys.argv:
        launcher = DotaLauncher()
        result = launcher.launch_all()
        print(f"Launched: {result}")
    else:
        dota_pids = DotaLauncher.find_running_dota_pids()
        steam_pids = DotaLauncher.find_running_steam_pids()
        print(f"Steam PIDs: {steam_pids}")
        print(f"Dota 2 PIDs: {dota_pids}")
