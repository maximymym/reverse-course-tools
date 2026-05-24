#!/usr/bin/env python3
"""
measure_ram.py — снапшот RAM-используемости dota2 farming pipeline для diff'а
между конфигурациями (baseline / WS-trim / -vulkan / -empty).

Output schema (JSON):
{
  "timestamp": "2026-05-13T03:21:00.123456Z",
  "dota2": [
    {
      "pid": 36584,
      "ws_mb": 3691.2,
      "pm_mb": 4863.7,            # private bytes (commit charge на процесс)
      "vm_mb": 19106.0,
      "handles": 19639,
      "threads": 81,
      "uptime_sec": 612.3,
      "cmdline_hash": "ab12cd"    # стабильный fp launch args (sanitizes login token)
    }
  ],
  "dota2_modules_top25": [          # из ПЕРВОГО dota2.exe (если несколько)
    {"name": "client.dll", "size_mb": 111.23},
    ...
  ],
  "steam_helpers": {
    "count": 7,
    "ws_mb": 1010.5
  },
  "steam_main": {
    "count": 1,
    "ws_mb": 250.0
  },
  "system": {
    "commit_used_mb": 29474.0,
    "commit_limit_mb": 45349.0,
    "available_mb": 12224.0,
    "total_mb": 16384.0
  },
  "renderer_loaded": {
    "dx11": false,
    "vulkan": false,
    "empty": false,
    "nvidia_user_driver": false  # nvwgf2umx.dll
  }
}

Usage:
  python measure_ram.py --json                  → stdout JSON one-line
  python measure_ram.py --json --pretty         → multiline
  python measure_ram.py                          → human readable
  python measure_ram.py --pid 36584              → only that PID
"""

from __future__ import annotations

import argparse
import ctypes
import ctypes.wintypes as wintypes
import datetime as dt
import hashlib
import json
import os
import sys
from typing import Iterable

try:
    import psutil
except ImportError:
    print("ERROR: psutil not installed; run `pip install psutil`", file=sys.stderr)
    sys.exit(2)


# ── Windows API: GlobalMemoryStatusEx ──────────────────────────
class MEMORYSTATUSEX(ctypes.Structure):
    _fields_ = [
        ("dwLength", wintypes.DWORD),
        ("dwMemoryLoad", wintypes.DWORD),
        ("ullTotalPhys", ctypes.c_ulonglong),
        ("ullAvailPhys", ctypes.c_ulonglong),
        ("ullTotalPageFile", ctypes.c_ulonglong),
        ("ullAvailPageFile", ctypes.c_ulonglong),
        ("ullTotalVirtual", ctypes.c_ulonglong),
        ("ullAvailVirtual", ctypes.c_ulonglong),
        ("sullAvailExtendedVirtual", ctypes.c_ulonglong),
    ]


def system_memory() -> dict:
    ms = MEMORYSTATUSEX()
    ms.dwLength = ctypes.sizeof(MEMORYSTATUSEX)
    if not ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(ms)):
        return {}
    vm = psutil.virtual_memory()
    # commit charge — TotalPageFile / AvailPageFile в нашей терминологии:
    # Windows commit limit = physical + pagefile; commit used = limit - avail.
    commit_limit_mb = ms.ullTotalPageFile / (1024 * 1024)
    commit_used_mb = (ms.ullTotalPageFile - ms.ullAvailPageFile) / (1024 * 1024)
    return {
        "commit_used_mb": round(commit_used_mb, 1),
        "commit_limit_mb": round(commit_limit_mb, 1),
        "available_mb": round(vm.available / (1024 * 1024), 1),
        "total_mb": round(vm.total / (1024 * 1024), 1),
        "memory_load_pct": ms.dwMemoryLoad,
    }


def _cmdline_hash(cmdline: list[str] | None) -> str:
    """Стабильный 6-байтный fp для diff'а — sanitize login token (после -login)."""
    if not cmdline:
        return ""
    parts: list[str] = []
    i = 0
    while i < len(cmdline):
        tok = cmdline[i]
        parts.append(tok)
        if tok == "-login" and i + 2 < len(cmdline):
            parts.append("<usr>")
            parts.append("<pwd>")
            i += 3
            continue
        i += 1
    h = hashlib.sha1(" ".join(parts).encode("utf-8", "ignore")).hexdigest()
    return h[:6]


def process_snapshot(proc: psutil.Process) -> dict | None:
    try:
        with proc.oneshot():
            mi = proc.memory_info()
            ws = mi.rss
            pm = getattr(mi, "private", None) or getattr(mi, "uss", None) or ws
            vm = mi.vms
            handles = proc.num_handles() if hasattr(proc, "num_handles") else 0
            threads = proc.num_threads()
            created = proc.create_time()
            uptime = max(0.0, dt.datetime.now().timestamp() - created)
            try:
                cmdline = proc.cmdline()
            except (psutil.AccessDenied, psutil.NoSuchProcess):
                cmdline = []
            return {
                "pid": proc.pid,
                "ws_mb": round(ws / (1024 * 1024), 1),
                "pm_mb": round(pm / (1024 * 1024), 1),
                "vm_mb": round(vm / (1024 * 1024), 1),
                "handles": handles,
                "threads": threads,
                "uptime_sec": round(uptime, 1),
                "cmdline_hash": _cmdline_hash(cmdline),
            }
    except (psutil.NoSuchProcess, psutil.AccessDenied):
        return None


# ── Module enumeration через ToolHelp32Snapshot (для топ-25 modules) ─────
TH32CS_SNAPMODULE   = 0x00000008
TH32CS_SNAPMODULE32 = 0x00000010
MAX_PATH = 260
MAX_MODULE_NAME32 = 255


class MODULEENTRY32W(ctypes.Structure):
    _fields_ = [
        ("dwSize", wintypes.DWORD),
        ("th32ModuleID", wintypes.DWORD),
        ("th32ProcessID", wintypes.DWORD),
        ("GlblcntUsage", wintypes.DWORD),
        ("ProccntUsage", wintypes.DWORD),
        ("modBaseAddr", ctypes.c_void_p),
        ("modBaseSize", wintypes.DWORD),
        ("hModule", ctypes.c_void_p),
        ("szModule", wintypes.WCHAR * (MAX_MODULE_NAME32 + 1)),
        ("szExePath", wintypes.WCHAR * MAX_PATH),
    ]


def enum_modules(pid: int) -> list[tuple[str, int]]:
    """Список (name, size_bytes). Может вернуть [] если access denied."""
    k32 = ctypes.windll.kernel32
    INVALID_HANDLE_VALUE = ctypes.c_void_p(-1).value
    snap = k32.CreateToolhelp32Snapshot(
        TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid
    )
    if snap == INVALID_HANDLE_VALUE or snap == 0:
        return []
    try:
        me = MODULEENTRY32W()
        me.dwSize = ctypes.sizeof(MODULEENTRY32W)
        if not k32.Module32FirstW(snap, ctypes.byref(me)):
            return []
        out: list[tuple[str, int]] = []
        while True:
            out.append((me.szModule, me.modBaseSize))
            if not k32.Module32NextW(snap, ctypes.byref(me)):
                break
        return out
    finally:
        k32.CloseHandle(snap)


def top_modules(pid: int, n: int = 25) -> tuple[list[dict], dict]:
    """Top-N modules + дешёвые fingerprints для renderer_loaded."""
    mods = enum_modules(pid)
    mods.sort(key=lambda m: m[1], reverse=True)
    top = [
        {"name": name, "size_mb": round(size / (1024 * 1024), 2)}
        for name, size in mods[:n]
    ]
    lowercase_names = {name.lower() for name, _ in mods}
    renderer = {
        "dx11":               "rendersystemdx11.dll" in lowercase_names,
        "vulkan":             "rendersystemvulkan.dll" in lowercase_names,
        "empty":              "rendersystemempty.dll" in lowercase_names,
        "nvidia_user_driver": "nvwgf2umx.dll" in lowercase_names,
        "nvidia_shader":      "nvgpucomp64.dll" in lowercase_names,
        "phonon_audio":       "phonon.dll" in lowercase_names,
        "ffmpeg_codec":       "libavcodec-58.dll" in lowercase_names,
        "video64":            "video64.dll" in lowercase_names,
        "panorama":           "panorama.dll" in lowercase_names,
        "v8":                 "v8.dll" in lowercase_names,
    }
    return top, renderer


# ── Sweep всех процессов ───────────────────────────────────────────
def collect(filter_pid: int | None = None) -> dict:
    dota_snaps: list[dict] = []
    steam_helper_ws = 0.0
    steam_helper_count = 0
    steam_main_ws = 0.0
    steam_main_count = 0
    first_dota_pid: int | None = None

    for proc in psutil.process_iter(["pid", "name"]):
        info = proc.info
        name = (info.get("name") or "").lower()
        if name == "dota2.exe":
            if filter_pid is not None and proc.pid != filter_pid:
                continue
            snap = process_snapshot(proc)
            if snap:
                dota_snaps.append(snap)
                if first_dota_pid is None:
                    first_dota_pid = proc.pid
        elif name == "steamwebhelper.exe":
            snap = process_snapshot(proc)
            if snap:
                steam_helper_ws += snap["ws_mb"]
                steam_helper_count += 1
        elif name == "steam.exe":
            snap = process_snapshot(proc)
            if snap:
                steam_main_ws += snap["ws_mb"]
                steam_main_count += 1

    dota2_modules: list[dict] = []
    renderer = {}
    if first_dota_pid is not None:
        dota2_modules, renderer = top_modules(first_dota_pid)

    return {
        "timestamp": dt.datetime.now(dt.timezone.utc).replace(microsecond=0).isoformat().replace("+00:00", "Z"),
        "dota2": dota_snaps,
        "dota2_modules_top25": dota2_modules,
        "steam_helpers": {
            "count": steam_helper_count,
            "ws_mb": round(steam_helper_ws, 1),
        },
        "steam_main": {
            "count": steam_main_count,
            "ws_mb": round(steam_main_ws, 1),
        },
        "system": system_memory(),
        "renderer_loaded": renderer,
        "snapshot_pid_for_modules": first_dota_pid,
    }


# ── Human formatter ────────────────────────────────────────────────
def human(out: dict) -> str:
    lines: list[str] = []
    lines.append(f"=== RAM snapshot @ {out['timestamp']} ===\n")

    if not out["dota2"]:
        lines.append("(no dota2.exe processes found)\n")
    else:
        lines.append("dota2.exe instances:")
        lines.append(
            f"  {'PID':>7}  {'WS_MB':>8}  {'PM_MB':>8}  {'VM_MB':>8}"
            f"  {'Handles':>8}  {'Thr':>4}  {'Uptime':>8}  CmdHash"
        )
        for d in out["dota2"]:
            lines.append(
                f"  {d['pid']:>7}  {d['ws_mb']:>8.1f}  {d['pm_mb']:>8.1f}"
                f"  {d['vm_mb']:>8.1f}  {d['handles']:>8}  {d['threads']:>4}"
                f"  {d['uptime_sec']:>7.0f}s  {d['cmdline_hash']}"
            )

    if out["dota2_modules_top25"]:
        lines.append("\nTop modules (first dota2.exe):")
        for m in out["dota2_modules_top25"]:
            lines.append(f"  {m['size_mb']:>8.2f} MB  {m['name']}")

    r = out["renderer_loaded"]
    if r:
        loaded = [k for k, v in r.items() if v]
        lines.append(f"\nKey modules loaded: {', '.join(loaded) if loaded else '(none)'}")

    sh = out["steam_helpers"]
    sm = out["steam_main"]
    lines.append(
        f"\nsteam.exe x{sm['count']}: {sm['ws_mb']:.1f} MB"
        f"   steamwebhelper.exe x{sh['count']}: {sh['ws_mb']:.1f} MB"
    )

    s = out["system"]
    if s:
        lines.append(
            f"\nSystem: commit {s['commit_used_mb']:.0f} / {s['commit_limit_mb']:.0f} MB"
            f"  ({s.get('memory_load_pct','?')}% load)"
            f"  available {s['available_mb']:.0f} MB"
        )

    return "\n".join(lines) + "\n"


def main() -> int:
    p = argparse.ArgumentParser(prog="measure_ram")
    p.add_argument("--json", action="store_true",
                   help="JSON output (default: human readable)")
    p.add_argument("--pretty", action="store_true",
                   help="Multiline JSON (only with --json)")
    p.add_argument("--pid", type=int, default=None,
                   help="Snapshot only this dota2.exe PID")
    p.add_argument("--out", type=str, default=None,
                   help="Write JSON to <path> (also prints to stdout unless --quiet)")
    p.add_argument("--quiet", action="store_true",
                   help="Suppress stdout when used with --out")
    p.add_argument("--label", type=str, default=None,
                   help="Annotate snapshot with .label field (e.g. 'baseline')")
    args = p.parse_args()

    if os.name != "nt":
        print("ERROR: Windows only (uses kernel32 toolhelp+meminfo)", file=sys.stderr)
        return 2

    out = collect(filter_pid=args.pid)
    if args.label:
        out["label"] = args.label

    if args.out:
        with open(args.out, "w", encoding="utf-8") as f:
            json.dump(out, f, ensure_ascii=False,
                      indent=(2 if args.pretty else None))
        if not args.quiet:
            if args.json:
                print(json.dumps(out, ensure_ascii=False,
                                 indent=(2 if args.pretty else None)))
            else:
                print(human(out))
                print(f"(also written to {args.out})")
    elif args.json:
        print(json.dumps(out, ensure_ascii=False,
                         indent=(2 if args.pretty else None)))
    else:
        print(human(out))

    return 0


if __name__ == "__main__":
    sys.exit(main())
