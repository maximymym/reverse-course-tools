#!/usr/bin/env python3
"""
dota2_minify_wrapper.py — headless CLI обёртка для применения подмножества модов
из github.com/Egezenn/dota2-minify (vendor'ан в tools/dota2/dota2_minify/).

Зачем не их `Minify/__main__.py`: их entry-point — DearPyGui приложение,
требует window context, читает чекбоксы через `dpg.get_value()`. Нам нужен
silent CLI для оркестратора в self-play ферме.

Что делаем:
  - apply <preset>  — строим pak66_dir.vpk из blacklist.txt + files/ модов в preset,
                      заливаем в <dota>/game/dota_minify/
  - revert          — удаляем все наши pak NN _dir.vpk из dota_minify/ и lang dirs,
                      убираем -language minify из Steam launch options
  - status          — JSON со статусом (applied/preset/paks_count/dota_minify_dir)

Output: stdout = JSON одной строкой; stderr = свободный лог.
Exit code: 0 OK, 1 user error, 2 internal failure.

Vendor: dota2_minify/ (commit pinned в README рядом).
Зависимости: pip install vpk vdf  (часть и так требуется их репо).
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
from typing import Iterable

def _locate_vendor() -> str:
    """
    Vendor может лежать:
      - рядом с wrapper (dev: tools/dota2/dota2_minify/)
      - на уровень выше (packaged: <root>/scripts/wrapper.py + <root>/dota2_minify/)
      - в DOTA2_MINIFY_VENDOR env var (override для CI/тестов)
    """
    env = os.environ.get("DOTA2_MINIFY_VENDOR")
    if env and os.path.isdir(env):
        return env
    here = os.path.dirname(os.path.abspath(__file__))
    candidates = [
        os.path.join(here, "dota2_minify"),
        os.path.join(here, "..", "dota2_minify"),
    ]
    for c in candidates:
        if os.path.isdir(c) and os.path.isdir(os.path.join(c, "Minify", "mods")):
            return os.path.abspath(c)
    return os.path.abspath(candidates[0])  # fallback (apply упадёт с вменяемой ошибкой)


VENDOR_DIR = _locate_vendor()
MINIFY_DIR = os.path.join(VENDOR_DIR, "Minify")
MODS_DIR = os.path.join(MINIFY_DIR, "mods")
BLANK_FILES_DIR = os.path.join(MINIFY_DIR, "bin", "blank-files")

# Маркер чтобы revert знал что pak — наш (и не трогал чужие mod'ы)
MARKER_FILENAME = "minify_orchestrator.txt"
MARKER_VERSION = "1"

# Presets — name → list of mod folder names. Только те что используют blacklist.txt
# либо files/ (без styling.css/xml_mod/script.py — это требует Workshop Tools и
# Source 2 Viewer, мы их не вытягиваем).
PRESETS: dict[str, list[str]] = {
    # AGGRESSIVE — всё что wrapper умеет применить (blacklist+files моды).
    # Оценка размера blacklist'ов: Spells&Items=5365, Foilage=581, BaseAttacks=247,
    # MiscOpt=191, MuteAmbient=17, Weather=10, River=8 → ~6400 файлов под blank.
    # + Tree Mod/Dark Terrain подменяют terrain через files/.
    # Для бот-фермы (нет визуального восприятия) = максимально безопасный максимум.
    "minify_aggressive": [
        "Misc Optimization",
        "Minify Base Attacks",
        "Minify Spells & Items",
        "Mute Ambient Sounds",
        "Mute Default Announcer",
        "Mute Taunt Sounds",
        "Mute Voice Line Sounds",
        "Remove Foilage",
        "Remove Pings",
        "Remove Sprays",
        "Remove Weather Effects",
        "Remove River",          # 8 vmat/vmdl _c → blank, river gone
        "Tree Mod",              # files/ override → simplified trees
        "Dark Terrain",          # files/ override → ascetic terrain skin
    ],
    "minimal_visuals": [
        "Misc Optimization",
        "Minify Base Attacks",
        "Minify Spells & Items",
        "Mute Ambient Sounds",
        "Mute Default Announcer",
        "Mute Taunt Sounds",
        "Mute Voice Line Sounds",
        "Remove Foilage",
        "Remove Pings",
        "Remove Sprays",
        "Remove Weather Effects",
        "Tree Mod",
        "Dark Terrain",
    ],
    "performance_only": [
        "Misc Optimization",
        "Minify Base Attacks",
        "Minify Spells & Items",
        "Mute Ambient Sounds",
        "Remove Weather Effects",
    ],
    "audio_mute_only": [
        "Mute Ambient Sounds",
        "Mute Default Announcer",
        "Mute Taunt Sounds",
        "Mute Voice Line Sounds",
    ],
}

# Поддерживаемые расширения blank-файлов (соответствует bin/blank-files/)
BLANK_EXTENSIONS = {".vsnd_c", ".vtex_c", ".vmdl_c", ".vmat_c", ".vpcf_c",
                    ".vcss_c", ".vjs_c", ".vxml_c", ".txt"}


def log(msg: str) -> None:
    print(f"[wrapper] {msg}", file=sys.stderr, flush=True)


def emit_json(obj: dict) -> None:
    print(json.dumps(obj, ensure_ascii=False), flush=True)


def detect_steam_library() -> str:
    """Найти Steam library с установленной Dota 2 через registry + libraryfolders.vdf"""
    try:
        import winreg
        with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE,
                            r"SOFTWARE\WOW6432Node\Valve\Steam") as key:
            steam_root, _ = winreg.QueryValueEx(key, "InstallPath")
    except OSError:
        steam_root = r"C:\Program Files (x86)\Steam"

    libraryfolders = os.path.join(steam_root, "config", "libraryfolders.vdf")
    candidates: list[str] = [steam_root]
    if os.path.exists(libraryfolders):
        # Минимальный VDF parser ровно под "path" в libraryfolders — без зависимости от vdf
        try:
            with open(libraryfolders, "r", encoding="utf-8") as f:
                content = f.read()
            for m in re.finditer(r'"path"\s+"([^"]+)"', content):
                p = m.group(1).replace("\\\\", "\\")
                if p and p not in candidates:
                    candidates.append(p)
        except Exception as e:
            log(f"libraryfolders.vdf parse failed: {e}")

    for lib in candidates:
        dota_exe = os.path.join(lib, "steamapps", "common", "dota 2 beta",
                                "game", "bin", "win64", "dota2.exe")
        if os.path.exists(dota_exe):
            return lib

    raise RuntimeError("Steam library с Dota 2 не найдена")


def dota_minify_dir(steam_lib: str) -> str:
    """<lib>/steamapps/common/dota 2 beta/game/dota_minify"""
    return os.path.join(steam_lib, "steamapps", "common", "dota 2 beta",
                        "game", "dota_minify")


def all_lang_dirs(steam_lib: str) -> list[str]:
    """Все возможные lang-overlay директории — для cleanup"""
    base = os.path.join(steam_lib, "steamapps", "common", "dota 2 beta", "game")
    return [
        os.path.join(base, name) for name in [
            "dota_minify", "dota_brazilian", "dota_bulgarian", "dota_czech",
            "dota_danish", "dota_dutch", "dota_finnish", "dota_french",
            "dota_german", "dota_greek", "dota_hungarian", "dota_italian",
            "dota_japanese", "dota_koreana", "dota_latam", "dota_norwegian",
            "dota_polish", "dota_portuguese", "dota_romanian", "dota_russian",
            "dota_schinese", "dota_spanish", "dota_swedish", "dota_tchinese",
            "dota_thai", "dota_turkish", "dota_ukrainian", "dota_vietnamese",
        ]
    ]


def list_dota_pak_contents(steam_lib: str) -> list[str]:
    """Перечислить все пути в pak01_dir.vpk (для resolve glob-паттернов)"""
    import vpk
    pak_path = os.path.join(steam_lib, "steamapps", "common", "dota 2 beta",
                            "game", "dota", "pak01_dir.vpk")
    if not os.path.exists(pak_path):
        raise RuntimeError(f"pak01_dir.vpk не найден: {pak_path}")
    pak = vpk.open(pak_path)
    return list(pak)  # iterable -> list of paths


def resolve_blacklist(blacklist_path: str, all_paks: Iterable[str]) -> set[str]:
    """
    Преобразовать blacklist.txt мода в plain set путей.
    Поддерживаем подмножество синтаксиса оригинала:
      - empty / '#'  — comment, skip
      - '>> path/'   — recursive prefix (все файлы под этим путём)
      - '** regex'   — regex match по pak listing
      - '*- regex'   — regex EXCLUSION
      - '-- path'    — exact-path EXCLUSION
      - '<path>'     — explicit single file
    """
    pak_list = list(all_paks)
    include: set[str] = set()
    exclude: set[str] = set()

    with open(blacklist_path, "r", encoding="utf-8") as f:
        for raw in f:
            line = raw.strip()
            if not line or line.startswith("#"):
                continue
            if line.startswith(">>"):
                prefix = line[2:].strip().lstrip("/")
                # ">>foo/bar" → все p in pak_list начинающиеся с "foo/bar"
                for p in pak_list:
                    if p.startswith(prefix):
                        include.add(p)
            elif line.startswith("**"):
                pattern = line[2:].strip()
                try:
                    rx = re.compile(pattern)
                except re.error as e:
                    log(f"  bad regex in {blacklist_path}: {pattern} ({e})")
                    continue
                for p in pak_list:
                    if rx.search(p):
                        include.add(p)
            elif line.startswith("*-"):
                pattern = line[2:].strip()
                try:
                    rx = re.compile(pattern)
                except re.error as e:
                    log(f"  bad exclude regex in {blacklist_path}: {pattern} ({e})")
                    continue
                for p in pak_list:
                    if rx.search(p):
                        exclude.add(p)
            elif line.startswith("--"):
                exclude.add(line[2:].strip())
            else:
                include.add(line)

    return include - exclude


def write_blank_for_path(target_path: str, dest_root: str) -> bool:
    """Положить blank.<ext> в dest_root/<target_path>"""
    _, ext = os.path.splitext(target_path)
    blank_src = os.path.join(BLANK_FILES_DIR, f"blank{ext}")
    if not os.path.exists(blank_src):
        # Не наш ext — пропускаем (кейс: .vmap_c и т.п.)
        return False
    dest = os.path.join(dest_root, target_path)
    os.makedirs(os.path.dirname(dest), exist_ok=True)
    shutil.copy2(blank_src, dest)
    return True


def copy_mod_files_dir(mod_dir: str, dest_root: str) -> int:
    """Если у мода есть files/ — copy_tree'ом в dest_root."""
    files_src = os.path.join(mod_dir, "files")
    if not os.path.isdir(files_src):
        return 0
    count = 0
    for root, _, names in os.walk(files_src):
        rel = os.path.relpath(root, files_src)
        for name in names:
            if name.endswith(".gitkeep"):
                continue
            src = os.path.join(root, name)
            dst_dir = os.path.join(dest_root, rel) if rel != "." else dest_root
            os.makedirs(dst_dir, exist_ok=True)
            shutil.copy2(src, os.path.join(dst_dir, name))
            count += 1
    return count


def build_pak(staging_dir: str, output_pak_path: str, preset: str,
              mods_used: list[str]) -> None:
    """Создать pak XX_dir.vpk из staging_dir и положить в output_pak_path."""
    import vpk
    # Marker файл с метаданными — чтобы revert опознал «свой» pak
    marker_path = os.path.join(staging_dir, MARKER_FILENAME)
    with open(marker_path, "w", encoding="utf-8") as f:
        json.dump({
            "version": MARKER_VERSION,
            "preset": preset,
            "mods": mods_used,
            "created_at": int(time.time()),
            "tool": "dota2_minify_wrapper.py",
            "vendor": "Egezenn/dota2-minify",
        }, f, indent=2)

    new_pak = vpk.new(staging_dir)
    os.makedirs(os.path.dirname(output_pak_path), exist_ok=True)
    new_pak.save(output_pak_path)


def fix_steam_launch_options(steam_lib: str, lang: str = "minify") -> int:
    """
    Добавить '-language minify' в Steam launch options для всех users у которых
    есть Dota 2 data. Возвращает кол-во обновлённых users.
    Steam должен быть закрыт перед этим (иначе localconfig.vdf перезапишется).
    """
    import vdf
    # Steam root = parent of steamapps
    steam_root = os.path.dirname(os.path.dirname(steam_lib)) \
        if "steamapps" in steam_lib else steam_lib
    # На самом деле steam_lib часто == steam_root для default install,
    # либо путь к extra library без userdata. Используем registry для root:
    try:
        import winreg
        with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE,
                            r"SOFTWARE\WOW6432Node\Valve\Steam") as key:
            steam_root, _ = winreg.QueryValueEx(key, "InstallPath")
    except OSError:
        steam_root = r"C:\Program Files (x86)\Steam"

    userdata = os.path.join(steam_root, "userdata")
    if not os.path.isdir(userdata):
        return 0

    updated = 0
    for uid in os.listdir(userdata):
        if not uid.isdigit():
            continue
        localconfig = os.path.join(userdata, uid, "config", "localconfig.vdf")
        if not os.path.exists(localconfig):
            continue
        try:
            with open(localconfig, "r", encoding="utf-8") as f:
                data = vdf.load(f)
            apps = data.get("UserLocalConfigStore", {}).get("Software", {}) \
                .get("Valve", {}).get("Steam", {}).get("apps", {})
            dota_app = apps.get("570")
            if not isinstance(dota_app, dict):
                continue
            opts = dota_app.get("LaunchOptions", "")
            tokens = opts.split() if opts else []
            cleaned: list[str] = []
            skip = False
            for i, t in enumerate(tokens):
                if skip:
                    skip = False
                    continue
                if t == "-language":
                    if i + 1 < len(tokens) and not tokens[i + 1].startswith(("-", "+")):
                        skip = True
                    continue
                cleaned.append(t)
            new_opts = f"-language {lang} " + " ".join(cleaned)
            new_opts = new_opts.strip()
            if new_opts != opts:
                dota_app["LaunchOptions"] = new_opts
                with open(localconfig, "w", encoding="utf-8") as f:
                    vdf.dump(data, f, pretty=True)
                updated += 1
                log(f"  updated launch options for user {uid}: '{new_opts}'")
        except Exception as e:
            log(f"  user {uid}: localconfig.vdf rewrite failed: {e}")

    return updated


def remove_steam_lang_option(lang: str = "minify") -> int:
    """Убрать '-language minify' из Steam launch options всех users. Idempotent."""
    import vdf
    try:
        import winreg
        with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE,
                            r"SOFTWARE\WOW6432Node\Valve\Steam") as key:
            steam_root, _ = winreg.QueryValueEx(key, "InstallPath")
    except OSError:
        steam_root = r"C:\Program Files (x86)\Steam"

    userdata = os.path.join(steam_root, "userdata")
    if not os.path.isdir(userdata):
        return 0

    updated = 0
    for uid in os.listdir(userdata):
        if not uid.isdigit():
            continue
        localconfig = os.path.join(userdata, uid, "config", "localconfig.vdf")
        if not os.path.exists(localconfig):
            continue
        try:
            with open(localconfig, "r", encoding="utf-8") as f:
                data = vdf.load(f)
            apps = data.get("UserLocalConfigStore", {}).get("Software", {}) \
                .get("Valve", {}).get("Steam", {}).get("apps", {})
            dota_app = apps.get("570")
            if not isinstance(dota_app, dict):
                continue
            opts = dota_app.get("LaunchOptions", "")
            if not opts:
                continue
            tokens = opts.split()
            cleaned: list[str] = []
            skip = False
            removed = False
            for i, t in enumerate(tokens):
                if skip:
                    skip = False
                    continue
                if t == "-language" and i + 1 < len(tokens) \
                        and tokens[i + 1] == lang:
                    skip = True
                    removed = True
                    continue
                cleaned.append(t)
            if removed:
                dota_app["LaunchOptions"] = " ".join(cleaned)
                with open(localconfig, "w", encoding="utf-8") as f:
                    vdf.dump(data, f, pretty=True)
                updated += 1
                log(f"  removed -language {lang} for user {uid}")
        except Exception as e:
            log(f"  user {uid}: localconfig.vdf cleanup failed: {e}")

    return updated


def is_our_pak(pak_path: str) -> bool:
    """Проверить что pak содержит наш marker файл."""
    try:
        import vpk
        pak = vpk.open(pak_path)
        return MARKER_FILENAME in pak
    except Exception:
        return False


def cmd_apply(args: argparse.Namespace) -> int:
    preset = args.preset
    if preset not in PRESETS:
        log(f"unknown preset: {preset}; available: {list(PRESETS)}")
        emit_json({"ok": False, "error": "unknown_preset",
                   "available": list(PRESETS)})
        return 1

    if not os.path.isdir(VENDOR_DIR):
        emit_json({"ok": False, "error": "vendor_missing", "path": VENDOR_DIR})
        log(f"vendor dir not found: {VENDOR_DIR}")
        return 2
    if not os.path.isdir(BLANK_FILES_DIR):
        emit_json({"ok": False, "error": "blank_files_missing"})
        return 2

    try:
        steam_lib = detect_steam_library()
    except Exception as e:
        emit_json({"ok": False, "error": "steam_lib_detect", "msg": str(e)})
        return 2
    log(f"steam library: {steam_lib}")

    try:
        all_paks = list_dota_pak_contents(steam_lib)
    except Exception as e:
        emit_json({"ok": False, "error": "pak_listing", "msg": str(e)})
        return 2
    log(f"pak01_dir.vpk listing: {len(all_paks)} entries")

    selected_mods = PRESETS[preset]
    log(f"preset '{preset}' → {len(selected_mods)} mods")

    # Все ли mods доступны?
    missing = [m for m in selected_mods
               if not os.path.isdir(os.path.join(MODS_DIR, m))]
    if missing:
        emit_json({"ok": False, "error": "mod_missing", "mods": missing})
        return 1

    blank_targets: set[str] = set()
    file_copy_mods: list[str] = []

    for mod in selected_mods:
        mod_dir = os.path.join(MODS_DIR, mod)
        bl = os.path.join(mod_dir, "blacklist.txt")
        if os.path.exists(bl):
            paths = resolve_blacklist(bl, all_paks)
            log(f"  {mod}: blacklist resolved → {len(paths)} files")
            blank_targets.update(paths)
        if os.path.isdir(os.path.join(mod_dir, "files")):
            file_copy_mods.append(mod)

    log(f"total unique blank-targets: {len(blank_targets)}")

    with tempfile.TemporaryDirectory(prefix="minify_stage_") as staging:
        # 1. blanks
        applied_blank = 0
        skipped_unknown_ext = 0
        for tgt in blank_targets:
            if write_blank_for_path(tgt, staging):
                applied_blank += 1
            else:
                skipped_unknown_ext += 1
        log(f"applied {applied_blank} blanks ({skipped_unknown_ext} skipped — unknown ext)")

        # 2. files/
        copied = 0
        for mod in file_copy_mods:
            n = copy_mod_files_dir(os.path.join(MODS_DIR, mod), staging)
            log(f"  {mod}: {n} files copied")
            copied += n
        log(f"applied {copied} replacement files")

        # 3. build pak66
        out_dir = dota_minify_dir(steam_lib)
        out_pak = os.path.join(out_dir, "pak66_dir.vpk")
        build_pak(staging, out_pak, preset, selected_mods)
        size_mb = os.path.getsize(out_pak) / 1024.0 / 1024.0
        log(f"built {out_pak} ({size_mb:.2f} MB)")

    # 4. Steam launch options
    fixed = 0
    if args.fix_launch_options:
        fixed = fix_steam_launch_options(steam_lib, "minify")
        log(f"updated launch options for {fixed} users")

    emit_json({
        "ok": True,
        "preset": preset,
        "mods": selected_mods,
        "pak_path": out_pak,
        "pak_size_bytes": os.path.getsize(out_pak),
        "blanks_applied": applied_blank,
        "files_copied": copied,
        "users_launch_options_updated": fixed,
        "marker_filename": MARKER_FILENAME,
    })
    return 0


def cmd_revert(args: argparse.Namespace) -> int:
    try:
        steam_lib = detect_steam_library()
    except Exception as e:
        emit_json({"ok": False, "error": "steam_lib_detect", "msg": str(e)})
        return 2

    pak_pat = re.compile(r"^pak\d{2}_dir\.vpk$")
    removed_paks: list[str] = []
    foreign_paks: list[str] = []

    for ldir in all_lang_dirs(steam_lib):
        if not os.path.isdir(ldir):
            continue
        for entry in os.listdir(ldir):
            if not pak_pat.fullmatch(entry):
                continue
            full = os.path.join(ldir, entry)
            if is_our_pak(full):
                try:
                    os.remove(full)
                    removed_paks.append(full)
                    log(f"  removed (ours): {full}")
                except OSError as e:
                    log(f"  remove failed {full}: {e}")
            else:
                foreign_paks.append(full)

    # Cleanup pak\d{2}_\d{3}.vpk сегменты тоже (vpk пакет split'ит >2GB на части)
    seg_pat = re.compile(r"^pak(\d{2})_\d{3}\.vpk$")
    for ldir in all_lang_dirs(steam_lib):
        if not os.path.isdir(ldir):
            continue
        for entry in os.listdir(ldir):
            m = seg_pat.fullmatch(entry)
            if not m:
                continue
            # Удаляем сегменты только если соответствующий dir был наш
            # (если dir-pak уже удалён — orphan-segments тоже надо снести)
            num = m.group(1)
            dir_pak_name = f"pak{num}_dir.vpk"
            dir_pak_full = os.path.join(ldir, dir_pak_name)
            # Если dir_pak только что удалён ИЛИ его нет вообще — это orphan
            if not os.path.exists(dir_pak_full):
                full = os.path.join(ldir, entry)
                try:
                    os.remove(full)
                    log(f"  removed orphan segment: {full}")
                except OSError as e:
                    log(f"  segment remove failed {full}: {e}")

    # Steam launch options
    cleaned = 0
    if args.cleanup_launch_options:
        cleaned = remove_steam_lang_option("minify")

    emit_json({
        "ok": True,
        "removed_paks": removed_paks,
        "removed_count": len(removed_paks),
        "foreign_paks_left": foreign_paks,
        "users_launch_options_cleaned": cleaned,
    })
    return 0


def cmd_status(_args: argparse.Namespace) -> int:
    try:
        steam_lib = detect_steam_library()
    except Exception as e:
        emit_json({"ok": False, "error": "steam_lib_detect", "msg": str(e)})
        return 2

    pak_pat = re.compile(r"^pak\d{2}_dir\.vpk$")
    our_paks: list[dict] = []
    foreign_paks: list[str] = []
    for ldir in all_lang_dirs(steam_lib):
        if not os.path.isdir(ldir):
            continue
        for entry in os.listdir(ldir):
            if not pak_pat.fullmatch(entry):
                continue
            full = os.path.join(ldir, entry)
            if is_our_pak(full):
                meta: dict = {"path": full,
                              "size": os.path.getsize(full)}
                try:
                    import vpk
                    pak = vpk.open(full)
                    f = pak.get_file(MARKER_FILENAME)
                    if f:
                        meta["marker"] = json.loads(f.read().decode("utf-8"))
                except Exception as e:
                    meta["marker_read_error"] = str(e)
                our_paks.append(meta)
            else:
                foreign_paks.append(full)

    emit_json({
        "ok": True,
        "applied": len(our_paks) > 0,
        "our_paks": our_paks,
        "foreign_paks": foreign_paks,
        "dota_minify_dir": dota_minify_dir(steam_lib),
        "presets_available": list(PRESETS),
    })
    return 0


def cmd_list_presets(_args: argparse.Namespace) -> int:
    out = {p: m for p, m in PRESETS.items()}
    emit_json({"ok": True, "presets": out})
    return 0


def main() -> int:
    p = argparse.ArgumentParser(prog="dota2_minify_wrapper")
    sub = p.add_subparsers(dest="cmd", required=True)

    p_apply = sub.add_parser("apply", help="Build & install pak66")
    p_apply.add_argument("--preset", default="minimal_visuals")
    p_apply.add_argument("--fix-launch-options", action="store_true",
                         help="Add '-language minify' to Steam users")
    p_apply.set_defaults(func=cmd_apply)

    p_revert = sub.add_parser("revert", help="Remove our pak files")
    p_revert.add_argument("--cleanup-launch-options", action="store_true",
                          help="Remove '-language minify' from Steam users")
    p_revert.set_defaults(func=cmd_revert)

    sub.add_parser("status", help="Print current state").set_defaults(func=cmd_status)
    sub.add_parser("list-presets", help="Print presets").set_defaults(func=cmd_list_presets)

    args = p.parse_args()
    try:
        return args.func(args)
    except KeyboardInterrupt:
        emit_json({"ok": False, "error": "interrupted"})
        return 130
    except Exception as e:
        log(f"FATAL: {type(e).__name__}: {e}")
        import traceback
        traceback.print_exc(file=sys.stderr)
        emit_json({"ok": False, "error": "internal", "msg": str(e)})
        return 2


if __name__ == "__main__":
    sys.exit(main())
