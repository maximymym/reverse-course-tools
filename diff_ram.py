#!/usr/bin/env python3
"""
diff_ram.py — сравнить два measure_ram.py JSON-снапшота.

Сравнивает:
  - dota2 (первый PID в каждом snap): ws_mb, pm_mb, vm_mb, handles, threads, uptime
  - dota2_modules_top25: исчезнувшие / появившиеся / изменившие size
  - steam_helpers / steam_main aggregate ws_mb
  - system commit_used_mb, available_mb
  - renderer_loaded flags — diff true/false

Usage:
  python diff_ram.py baseline.json test2.json
  python diff_ram.py baseline.json test2.json --json
"""
from __future__ import annotations

import argparse
import json
import sys


def fmt_delta(new: float, old: float, suffix: str = "MB") -> str:
    d = new - old
    pct = (d / old * 100.0) if old else 0.0
    sign = "+" if d >= 0 else ""
    return f"{new:.1f} {suffix} ({sign}{d:.1f}, {sign}{pct:.1f}%)"


def fmt_delta_int(new: int, old: int, suffix: str = "") -> str:
    d = new - old
    pct = (d / old * 100.0) if old else 0.0
    sign = "+" if d >= 0 else ""
    return f"{new}{suffix} ({sign}{d}, {sign}{pct:.1f}%)"


def get_dota_summary(snap: dict) -> dict:
    d = snap.get("dota2") or []
    if not d:
        return {}
    return d[0]


def get_modules(snap: dict) -> dict[str, float]:
    return {m["name"].lower(): m["size_mb"]
            for m in snap.get("dota2_modules_top25", [])}


def diff(a: dict, b: dict) -> dict:
    """a = baseline, b = candidate"""
    da = get_dota_summary(a)
    db = get_dota_summary(b)
    ma = get_modules(a)
    mb = get_modules(b)

    only_a = sorted(ma.keys() - mb.keys())
    only_b = sorted(mb.keys() - ma.keys())
    common = ma.keys() & mb.keys()
    changed = []
    for name in common:
        diff_mb = mb[name] - ma[name]
        if abs(diff_mb) >= 0.5:
            changed.append((name, ma[name], mb[name], diff_mb))
    changed.sort(key=lambda x: x[3])

    return {
        "label_a": a.get("label") or a.get("timestamp", "?"),
        "label_b": b.get("label") or b.get("timestamp", "?"),
        "dota_summary": {
            "pid_a": da.get("pid"),
            "pid_b": db.get("pid"),
            "ws_mb_a": da.get("ws_mb"),
            "ws_mb_b": db.get("ws_mb"),
            "pm_mb_a": da.get("pm_mb"),
            "pm_mb_b": db.get("pm_mb"),
            "vm_mb_a": da.get("vm_mb"),
            "vm_mb_b": db.get("vm_mb"),
            "handles_a": da.get("handles"),
            "handles_b": db.get("handles"),
            "threads_a": da.get("threads"),
            "threads_b": db.get("threads"),
            "uptime_a": da.get("uptime_sec"),
            "uptime_b": db.get("uptime_sec"),
        },
        "modules_only_in_baseline": only_a,
        "modules_only_in_candidate": only_b,
        "modules_changed_mb": [
            {"name": n, "old_mb": old, "new_mb": new, "delta_mb": d}
            for n, old, new, d in changed
        ],
        "system": {
            "commit_used_mb_a": (a.get("system") or {}).get("commit_used_mb"),
            "commit_used_mb_b": (b.get("system") or {}).get("commit_used_mb"),
            "available_mb_a":   (a.get("system") or {}).get("available_mb"),
            "available_mb_b":   (b.get("system") or {}).get("available_mb"),
        },
        "steam_helpers": {
            "ws_mb_a": (a.get("steam_helpers") or {}).get("ws_mb"),
            "ws_mb_b": (b.get("steam_helpers") or {}).get("ws_mb"),
            "count_a": (a.get("steam_helpers") or {}).get("count"),
            "count_b": (b.get("steam_helpers") or {}).get("count"),
        },
        "renderer_loaded_a": a.get("renderer_loaded") or {},
        "renderer_loaded_b": b.get("renderer_loaded") or {},
    }


def human(d: dict) -> str:
    out: list[str] = []
    out.append(f"=== DIFF: '{d['label_a']}' -> '{d['label_b']}' ===\n")

    s = d["dota_summary"]
    out.append("dota2.exe (first PID):")
    if s["pid_a"] != s["pid_b"]:
        out.append(f"  PID:     {s['pid_a']} -> {s['pid_b']}")
    if s["ws_mb_a"] is not None and s["ws_mb_b"] is not None:
        out.append(f"  WS:      {s['ws_mb_a']:.1f} MB -> {fmt_delta(s['ws_mb_b'], s['ws_mb_a'])}")
    if s["pm_mb_a"] is not None and s["pm_mb_b"] is not None:
        out.append(f"  PM:      {s['pm_mb_a']:.1f} MB -> {fmt_delta(s['pm_mb_b'], s['pm_mb_a'])}  <-- KEY METRIC")
    if s["vm_mb_a"] is not None and s["vm_mb_b"] is not None:
        out.append(f"  VM:      {s['vm_mb_a']:.1f} MB -> {fmt_delta(s['vm_mb_b'], s['vm_mb_a'])}")
    if s["handles_a"] is not None and s["handles_b"] is not None:
        out.append(f"  Handles: {fmt_delta_int(s['handles_b'], s['handles_a'])}")
    if s["threads_a"] is not None and s["threads_b"] is not None:
        out.append(f"  Threads: {fmt_delta_int(s['threads_b'], s['threads_a'])}")
    if s["uptime_a"] is not None and s["uptime_b"] is not None:
        out.append(f"  Uptime:  {s['uptime_a']:.0f}s -> {s['uptime_b']:.0f}s  (для контекста — больший uptime обычно больше WS)")

    out.append("\nRenderer loaded:")
    keys = sorted(set(d["renderer_loaded_a"]) | set(d["renderer_loaded_b"]))
    for k in keys:
        a = d["renderer_loaded_a"].get(k)
        b = d["renderer_loaded_b"].get(k)
        if a == b:
            continue
        out.append(f"  {k:30s}  {a} -> {b}")

    if d["modules_only_in_baseline"]:
        out.append("\nМодули БОЛЬШЕ НЕ загружены (gain):")
        for name in d["modules_only_in_baseline"]:
            out.append(f"  - {name}")
    if d["modules_only_in_candidate"]:
        out.append("\nМодули ВНОВЬ загружены:")
        for name in d["modules_only_in_candidate"]:
            out.append(f"  + {name}")

    if d["modules_changed_mb"]:
        out.append("\nИзменившие размер модули (>=0.5 MB):")
        for m in d["modules_changed_mb"]:
            sign = "+" if m["delta_mb"] >= 0 else ""
            out.append(f"  {m['name']}: {m['old_mb']:.2f} -> {m['new_mb']:.2f} ({sign}{m['delta_mb']:.2f})")

    sys_a = d["system"]["commit_used_mb_a"]
    sys_b = d["system"]["commit_used_mb_b"]
    av_a = d["system"]["available_mb_a"]
    av_b = d["system"]["available_mb_b"]
    if sys_a is not None and sys_b is not None:
        out.append(f"\nSystem commit_used:  {fmt_delta(sys_b, sys_a)}")
    if av_a is not None and av_b is not None:
        out.append(f"System available:    {fmt_delta(av_b, av_a)}")

    sh_a = d["steam_helpers"]["ws_mb_a"]
    sh_b = d["steam_helpers"]["ws_mb_b"]
    if sh_a is not None and sh_b is not None:
        out.append(f"steam_helpers WS:    {fmt_delta(sh_b, sh_a)}  "
                   f"(count: {d['steam_helpers']['count_a']} -> {d['steam_helpers']['count_b']})")

    return "\n".join(out) + "\n"


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument("baseline", help="Baseline JSON path")
    p.add_argument("candidate", help="Candidate JSON path")
    p.add_argument("--json", action="store_true",
                   help="JSON output instead of human")
    p.add_argument("--pretty", action="store_true")
    args = p.parse_args()

    with open(args.baseline, "r", encoding="utf-8") as f:
        a = json.load(f)
    with open(args.candidate, "r", encoding="utf-8") as f:
        b = json.load(f)

    d = diff(a, b)
    if args.json:
        print(json.dumps(d, ensure_ascii=False,
                         indent=(2 if args.pretty else None)))
    else:
        print(human(d))
    return 0


if __name__ == "__main__":
    sys.exit(main())
