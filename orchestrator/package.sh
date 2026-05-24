#!/bin/bash
# Package DotaFarm for distribution
# Run from orchestrator/ directory

set -e

OUT="/c/temp/DotaFarm"
TOOLS_DIR="$(dirname "$0")/.."

# Version tag — first arg or timestamp. Пишется в scripts/bots/.dist_version
# чтобы DotaFarm.exe на стороне юзера мог определить "нужен ли sync в andromeda"
# и не перезаписывать правки молча.
DIST_VERSION="${1:-$(date +%Y.%m.%d.%H%M)}"

echo "=== Packaging DotaFarm v${DIST_VERSION} ==="

# ── Pre-step: sync scripts_source → live ────────────────────────
# scripts_source/ is git-tracked master; C:\temp\andromeda\scripts\bots is live.
# This keeps live in sync before packaging so we don't ship stale code.
#
# Orphan-protection: full `rm -rf` гарантирует что legacy/тестовые файлы
# (FretBots.lua, Buff/, ability_item_usage_generic.lua, --mode_item_generic.lua,
# и т.п.) не переживают sync. Если когда-нибудь rm-rf будет ослаблен до per-file
# sync — explicit-list ниже остаётся как safety net.
if [ -d "$TOOLS_DIR/scripts_source/bots" ]; then
    # Defense-in-depth: явный список known orphans (на случай если rm-rf
    # ниже когда-нибудь будет заменён на rsync-style merge).
    rm -rf \
        /c/temp/andromeda/scripts/bots/FretBots.lua \
        /c/temp/andromeda/scripts/bots/Buff \
        /c/temp/andromeda/scripts/bots/ability_item_usage_generic.lua \
        /c/temp/andromeda/scripts/bots/--mode_item_generic.lua \
        2>/dev/null || true
    rm -rf /c/temp/andromeda/scripts/bots
    cp -r "$TOOLS_DIR/scripts_source/bots" /c/temp/andromeda/scripts/
    echo "[+] Synced scripts_source/bots → C:/temp/andromeda/scripts/bots"
fi

# Clean and create output dir
rm -rf "$OUT"
mkdir -p "$OUT/config" "$OUT/data" "$OUT/scripts" "$OUT/docs"

# Copy built EXE
cp build/Release/DotaFarm.exe "$OUT/"
echo "[+] DotaFarm.exe"

# Copy assets (fonts for theme — Cinzel + fallbacks)
ASSETS_SRC="$(dirname "$0")/assets"
if [ -d "$ASSETS_SRC" ]; then
    mkdir -p "$OUT/assets"
    cp -r "$ASSETS_SRC"/* "$OUT/assets/"
    echo "[+] assets/ (fonts + ...)"
fi

# MAPPER-FIX 2026-05-18: manual map снова работает (TLS callbacks + LoadConfig
# SecurityCookie init + section perms добавлены). Andromeda + ProxyHook
# живут как encrypted RT_RCDATA внутри DotaFarm.exe, на диск не падают.

# Copy game data and scripts
if [ -d "/c/temp/andromeda/data" ]; then
    cp -r /c/temp/andromeda/data/* "$OUT/data/"
    echo "[+] data/"
fi

if [ -d "/c/temp/andromeda/scripts" ]; then
    cp -r /c/temp/andromeda/scripts/* "$OUT/scripts/"
    # Version marker — используется DotaFarm.exe для sync с C:\temp\andromeda\
    # при обновлении. Сравнение .dist_version с .installed_version в andromeda.
    mkdir -p "$OUT/scripts/bots"
    echo "$DIST_VERSION" > "$OUT/scripts/bots/.dist_version"
    echo "[+] scripts/ (.dist_version=$DIST_VERSION)"
fi

# ── Lua → bytecode compile (anti-RE: ship .luac, удалить .lua plain) ──
# OPT-IN флаг: DOTAFARM_LUAC_ENABLE=1 включает compile в .luac. По умолчанию
# OFF — .lua едут plain (нужно для debug'а / reading'а у юзера).
# Когда нужно production обфускацию — `DOTAFARM_LUAC_ENABLE=1 bash deploy.sh`.
if [ "${DOTAFARM_LUAC_ENABLE:-0}" != "1" ]; then
    echo "[i] Lua bytecode compile DISABLED (DOTAFARM_LUAC_ENABLE != 1) — shipping plain .lua"
else
    # Andromeda использует lua54.lib → совместимо с системным luac.exe 5.4.
    # Source: tools/dota2/scripts_source/bots/ (git-tracked master).
    # Target: $OUT/scripts/bots/*.luac (без plain .lua).
    # Все .lua под bots/ компилируются; иерархия (heroes/, states/, util/)
    # сохраняется. Файл с ошибкой парсинга — fatal-warn + skip.
    LUAC=""
    for cand in \
        "/c/Users/aleks/AppData/Local/Programs/Lua/bin/luac.exe" \
        "/c/Program Files/Lua/bin/luac.exe" \
        "$(command -v luac 2>/dev/null || true)"
    do
        if [ -n "$cand" ] && [ -f "$cand" ]; then LUAC="$cand"; break; fi
    done

    if [ -n "$LUAC" ] && [ -d "$TOOLS_DIR/scripts_source/bots" ]; then
        echo "[+] Lua bytecode compile via $LUAC"
        # Сначала сносим plain .lua скопированные из /c/temp/andromeda/scripts/ —
        # их теперь заменим скомпилированными .luac
        find "$OUT/scripts/bots" -name "*.lua" -type f -delete 2>/dev/null || true

        COMPILED=0
        FAILED=0
        SRC_BOTS="$TOOLS_DIR/scripts_source/bots"
        pushd "$SRC_BOTS" >/dev/null
        while IFS= read -r -d '' luafile; do
            rel="${luafile#./}"
            out_luac="$OUT/scripts/bots/${rel%.lua}.luac"
            mkdir -p "$(dirname "$out_luac")"
            if "$LUAC" -o "$out_luac" "$luafile" 2>/tmp/dotafarm_luac_err; then
                COMPILED=$((COMPILED+1))
            else
                FAILED=$((FAILED+1))
                echo "[!] luac FAILED: $rel"
                cat /tmp/dotafarm_luac_err | sed 's/^/    /'
                # Fallback: оставляем plain .lua чтобы рантайм не упал
                cp "$luafile" "$OUT/scripts/bots/$rel"
            fi
        done < <(find . -name "*.lua" -type f -print0)
        popd >/dev/null
        rm -f /tmp/dotafarm_luac_err

        echo "[+] Lua compile: $COMPILED ok, $FAILED failed"
        if [ "$FAILED" -gt 0 ]; then
            echo "[!] $FAILED .lua файл(ов) shipped plain как fallback — runtime будет искать .lua"
        fi
    else
        if [ -z "$LUAC" ]; then
            echo "[!] luac.exe не найден ни в одном источнике — .lua скрипты shipped plain"
            echo "    TODO: install Lua 5.4 → choco install lua или https://luabinaries.sourceforge.net/"
        fi
    fi
fi

# Copy docs — prefer scripts_source/docs (new canonical), fallback to legacy docs/
NEW_DOCS_SRC="$TOOLS_DIR/scripts_source/docs"
LEGACY_DOCS_SRC="$TOOLS_DIR/docs"
if [ -d "$NEW_DOCS_SRC" ] && [ "$(ls -A "$NEW_DOCS_SRC" 2>/dev/null)" ]; then
    cp -r "$NEW_DOCS_SRC"/* "$OUT/docs/"
    echo "[+] docs/ (from scripts_source/docs)"
elif [ -d "$LEGACY_DOCS_SRC" ]; then
    cp -r "$LEGACY_DOCS_SRC"/* "$OUT/docs/"
    echo "[+] docs/ (legacy)"
fi
# NOTE: BOT_SCRIPTING_API.md (Valve's verbose reference) is no longer shipped —
# our API_REFERENCE.md is the authoritative list of what REAL works vs STUB.

# Bundle H: handle64.exe NOT bundled (Sysinternals не разрешает redistribution).
# DotaLauncher::KillDotaMutex использует native NtQuerySystemInformation как
# primary path — handle64.exe был fallback. Если на тестовой машине проявится
# случай где native не справляется — handle64.exe можно положить в bundle root
# вручную (см. dist_readme.txt → "Опциональные дополнения").

# ProxyHook.dll — больше НЕ копируется отдельно. Stream B запекает её
# как encrypted resource в DotaFarm.exe (как и Andromeda DLL); orchestrator
# мануал-мапит из памяти когда нужен user-mode SOCKS5 fallback.

# Copy WinDivert (legacy kernel-mode TCP+UDP redirect — dead path, kept for UDP scenarios)
WINDIVERT_DIR="$TOOLS_DIR/proxydivert/deps/windivert"
for f in WinDivert.dll WinDivert64.sys; do
    if [ -f "$WINDIVERT_DIR/$f" ]; then
        cp "$WINDIVERT_DIR/$f" "$OUT/$f"
        echo "[+] $f"
    else
        echo "[!] $f missing — kernel redirect will fail on tester machine"
    fi
done

# Copy sing-box + wintun (tun2socks — основной per-account SOCKS5 path)
SINGBOX_DIR="$TOOLS_DIR/singbox/deps"
for f in sing-box.exe wintun.dll; do
    if [ -f "$SINGBOX_DIR/$f" ]; then
        cp "$SINGBOX_DIR/$f" "$OUT/$f"
        echo "[+] $f"
    else
        echo "[!] $f missing — tun2socks will fail on tester machine. Run: (cd $SINGBOX_DIR && bash fetch.sh)"
    fi
done

# ── HWID spoofer artifacts (antiban) — БОЛЬШЕ НЕ КОПИРУЮТСЯ ──
# HwidSpoofer.exe, spoofer.sys, kdu.exe ушли в encrypted resources внутри
# DotaFarm.exe (Stream B). При выборе spoof_mode=full_pc|both orchestrator
# распаковывает их в %TEMP%\df_<GUID>\, запускает kdu, удаляет файлы,
# зачищает буферы памяти. На диске у юзера их быть не должно — иначе
# Defender ловит spoofer.sys как HackTool, а kdu.exe как Bearfoos.
# См. src/hwid_spoof.cpp (in-process flow).

# Copy config templates
cp "$TOOLS_DIR/config/accounts.json" "$OUT/config/" 2>/dev/null || true
cp "$TOOLS_DIR/config/farm.json" "$OUT/config/" 2>/dev/null || true
echo "[+] config/"

# Bundle G2/H — dota2_minify_wrapper PyInstaller bundle + vendor dota2_minify/
#
# Bundle H portable: предпочитаем PyInstaller standalone .exe (zero-install).
# Wrapper.py остаётся в bundle как fallback для тех у кого Python+vpk в PATH.
# Vendor dota2_minify/ нужен целиком рядом — wrapper читает mods/blacklist.txt
# и bin/blank-files в runtime.

WRAPPER_EXE="$TOOLS_DIR/dist/dota2_minify_wrapper.exe"
WRAPPER_PY="$TOOLS_DIR/dota2_minify_wrapper.py"

if [ -f "$WRAPPER_EXE" ]; then
    cp "$WRAPPER_EXE" "$OUT/scripts/dota2_minify_wrapper.exe"
    echo "[+] scripts/dota2_minify_wrapper.exe (PyInstaller bundle, zero-install)"
else
    echo "[!] $WRAPPER_EXE not found — VPK minifier потребует Python+vpk на target"
    echo "    Build: cd tools/dota2 && pyinstaller --clean dota2_minify_wrapper_build/wrapper.spec --distpath dist --workpath build_pyi"
fi

if [ -f "$WRAPPER_PY" ]; then
    cp "$WRAPPER_PY" "$OUT/scripts/dota2_minify_wrapper.py"
    echo "[+] scripts/dota2_minify_wrapper.py (fallback для dev машины с Python в PATH)"
fi

if [ -d "$TOOLS_DIR/dota2_minify" ]; then
    # Достаточно Minify/{mods,bin/blank-files} — остальное (UI/build.py) не нужно.
    mkdir -p "$OUT/scripts/dota2_minify/Minify/bin"
    cp -r "$TOOLS_DIR/dota2_minify/Minify/mods" "$OUT/scripts/dota2_minify/Minify/"
    cp -r "$TOOLS_DIR/dota2_minify/Minify/bin/blank-files" "$OUT/scripts/dota2_minify/Minify/bin/"
    cp "$TOOLS_DIR/dota2_minify/LICENSE" "$OUT/scripts/dota2_minify/" 2>/dev/null || true
    cp "$TOOLS_DIR/dota2_minify/README.md" "$OUT/scripts/dota2_minify/" 2>/dev/null || true
    echo "[+] scripts/dota2_minify/Minify/{mods,bin/blank-files} (Egezenn vendor, GPL-3.0)"
fi

# Bundle H — README для конечного пользователя
DIST_README="$(dirname "$0")/dist_readme.txt"
if [ -f "$DIST_README" ]; then
    cp "$DIST_README" "$OUT/README.txt"
    echo "[+] README.txt"
fi

# ── Themida protection (anti-RE) ──
# Применяется in-place к собранным бинарям в $OUT/. Нужны .tmd templates в
# tools/dota2/protect/templates/ (создаются один раз через GUI Themida — см.
# README_HOWTO.md). Если templates нет → graceful skip (бинари unprotected).
# Skip полностью: env DOTAFARM_NO_PROTECT=1 или флаг --no-protect.
#
# Исключения (НЕ протектим):
#   - kdu.exe (third-party utility, чужая подпись)
#   - spoofer.sys (kernel driver, Themida сломает signing → driver не загрузится)
#   - sing-box.exe / wintun.dll / WinDivert* (third-party)
#   - PyInstaller-bundle dota2_minify_wrapper.exe (Themida на PyInstaller wrap
#     обычно ломает Python rt — отдельная проблема)
PROTECT_SCRIPT="$TOOLS_DIR/protect/protect.sh"
if [ "${DOTAFARM_NO_PROTECT:-0}" = "1" ] || [ "${1:-}" = "--no-protect" ] || [ "${2:-}" = "--no-protect" ]; then
    echo "[!] Themida protection пропущен (DOTAFARM_NO_PROTECT=1 или --no-protect)"
elif [ -f "$PROTECT_SCRIPT" ]; then
    echo ""
    echo "=== Applying Themida protection ==="
    if bash "$PROTECT_SCRIPT" "$OUT"; then
        echo "[+] Protection complete"
    else
        rc=$?
        echo "[!] Protection script exit code $rc — bundle есть, но не все бинари защищены"
        # Не валим build целиком — bundle юзабелен, просто часть unprotected.
    fi
else
    echo "[!] Protection script not found at $PROTECT_SCRIPT — skip"
fi

echo ""
echo "=== Package complete: $OUT ==="
ls -la "$OUT/"
echo ""
echo "Size: $(du -sh "$OUT" | cut -f1)"

# ── Sanity check: forbidden artifacts (раздаём только то что должно быть) ──
# Allowed DLL'ы: только system / third-party (sing-box, wintun, WinDivert).
# Запрещённые: Andromeda*.dll, ProxyHook.dll — обязаны быть embedded.
# Запрещённые EXE: HwidSpoofer.exe, kdu.exe — обязаны быть embedded.
# Запрещённые .sys: spoofer.sys — embedded.
# Запрещённые plain .lua под bots/ — должны быть .luac (если luac доступен).
echo ""
echo "=== Sanity check ==="
SANITY_FAIL=0
while IFS= read -r forbidden; do
    if [ -n "$forbidden" ]; then
        echo "[FAIL] Forbidden artifact on disk: $forbidden"
        SANITY_FAIL=$((SANITY_FAIL+1))
    fi
done < <(
    find "$OUT" -type f \( \
        -iname "Andromeda*.dll" -o \
        -iname "ProxyHook.dll"  -o \
        -iname "HwidSpoofer.exe" -o \
        -iname "kdu.exe"        -o \
        -iname "spoofer.sys"    \
    \) 2>/dev/null
)

# Plain .lua под bots/ — допустим только если luac compile выключен (default off
# для debug) или partial fallback. Если LUAC_ENABLE=1 и есть .lua — partial fail.
PLAIN_LUA=$(find "$OUT/scripts/bots" -name "*.lua" -type f 2>/dev/null | wc -l)
LUAC_FILES=$(find "$OUT/scripts/bots" -name "*.luac" -type f 2>/dev/null | wc -l)
if [ "${DOTAFARM_LUAC_ENABLE:-0}" = "1" ]; then
    if [ "$PLAIN_LUA" -gt 0 ] && [ "$LUAC_FILES" -gt 0 ]; then
        echo "[WARN] $PLAIN_LUA plain .lua + $LUAC_FILES .luac (partial compile)"
    elif [ "$PLAIN_LUA" -gt 0 ] && [ "$LUAC_FILES" -eq 0 ]; then
        echo "[WARN] $PLAIN_LUA plain .lua (luac unavailable, no bytecode protection)"
    fi
else
    # Default mode — plain .lua это namepe, не WARN.
    echo "[i] Lua scripts: $PLAIN_LUA plain .lua (bytecode disabled — debug mode)"
fi

if [ "$SANITY_FAIL" -gt 0 ]; then
    echo ""
    echo "=== SANITY FAILED: $SANITY_FAIL forbidden artifact(s) on disk ==="
    echo "    Эти файлы должны быть embedded resources в DotaFarm.exe, не на диске."
    exit 1
fi
echo "[+] Sanity OK — no forbidden artifacts on disk"
