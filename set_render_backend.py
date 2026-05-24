#!/usr/bin/env python3
"""
set_render_backend.py — атомарно подменить minifier.render_backend в farm.json.

Используется для Test 2 / Test 3 — переключение DX11 / Vulkan / empty без
ручной редактуры JSON (риск unclosed brace → DotaFarm не парсит конфиг и
запустится с defaults).

Usage:
  python set_render_backend.py vulkan
  python set_render_backend.py empty
  python set_render_backend.py dx11
  python set_render_backend.py --config C:\\temp\\DotaFarm\\config\\farm.json vulkan

Default --config = C:\\temp\\DotaFarm\\config\\farm.json (production deploy).
Бекап: <path>.bak (overwrite каждый запуск).
"""
from __future__ import annotations

import argparse
import json
import os
import shutil
import sys

ALLOWED = ("dx11", "vulkan", "empty")
DEFAULT_CONFIG = r"C:\temp\DotaFarm\config\farm.json"


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument("backend", choices=ALLOWED)
    p.add_argument("--config", default=DEFAULT_CONFIG)
    p.add_argument("--ws-trim", choices=("on", "off", "keep"), default="keep",
                   help="Toggle periodic_ws_trim concurrently")
    args = p.parse_args()

    if not os.path.isfile(args.config):
        print(f"ERROR: config not found: {args.config}", file=sys.stderr)
        return 1

    with open(args.config, "r", encoding="utf-8") as f:
        data = json.load(f)

    mn = data.setdefault("minifier", {})
    old_backend = mn.get("render_backend", "(unset)")
    mn["render_backend"] = args.backend
    if args.ws_trim != "keep":
        mn["periodic_ws_trim"] = (args.ws_trim == "on")

    shutil.copy2(args.config, args.config + ".bak")
    with open(args.config, "w", encoding="utf-8") as f:
        json.dump(data, f, ensure_ascii=False, indent=4)

    print(f"render_backend: {old_backend} -> {args.backend}")
    print(f"periodic_ws_trim: {mn.get('periodic_ws_trim')}")
    print(f"config: {args.config}")
    print(f"backup: {args.config}.bak")
    return 0


if __name__ == "__main__":
    sys.exit(main())
