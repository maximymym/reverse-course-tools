#!/bin/bash
# Headless protector for DotaFarm package — VMProtect Console.
#
# Usage:
#   bash tools/dota2/protect/protect.sh <dist_dir>
#
# Why VMProtect (not Themida):
#   Themida 3.2.4 cracked silently игнорирует /q и fall back в GUI mode даже
#   с правильно загруженным /protect <tmd>. Headless невозможен без registered
#   license. VMProtect 3.8.1 в Ahuet/ — registered Personal License, fully
#   headless через VMProtect_Con.exe. Themida .tmd templates остались в
#   templates/ как backup на случай мануального протекта.
#
# Behavior:
#   - Для каждого binary в SPEC: клонирует template.vmp, подменяет
#     __INPUT_FILE_PLACEHOLDER__ на реальный path, запускает VMProtect_Con.
#     output = same path (in-place overwrite).
#   - Backup .preprotect перед каждой попыткой; restore при failure.
#   - Если template.vmp нет — graceful skip (бинари идут unprotected).
#   - Если VMProtect_Con.exe нет — graceful skip всего.
#   - Skip полностью при env DOTAFARM_NO_PROTECT=1.
#
# Exit codes:
#   0 — все запрошенные protections успешны (или graceful skip)
#   1 — invalid arguments / setup error
#   2 — at least one protection FAILED

set -u

if [ $# -lt 1 ]; then
    echo "usage: $0 <dist_dir>"
    exit 1
fi

DIST="$1"
PROTECT_DIR="$(cd "$(dirname "$0")" && pwd)"
TPL_VMP="$PROTECT_DIR/templates/template.vmp"

# VMProtect 3.8.1 (Ahuet build с registered license). Override через env VMP_DIR.
VMP_DIR="${VMP_DIR:-/c/Users/aleks/Downloads/test4/Ahuet}"
VMP_CON="$VMP_DIR/VMProtect_Con.exe"

if [ ! -d "$DIST" ]; then
    echo "[protect] FATAL: dist directory не существует: $DIST"
    exit 1
fi

if [ "${DOTAFARM_NO_PROTECT:-0}" = "1" ]; then
    echo "[protect] SKIP: DOTAFARM_NO_PROTECT=1"
    exit 0
fi

if [ ! -f "$VMP_CON" ]; then
    echo "[protect] WARN: VMProtect_Con.exe не найден: $VMP_CON"
    echo "[protect]       Бинари остаются unprotected. Set VMP_DIR=<path>."
    exit 0
fi

if [ ! -f "$TPL_VMP" ]; then
    echo "[protect] WARN: VMProtect template не найден: $TPL_VMP"
    echo "[protect]       Бинари остаются unprotected."
    exit 0
fi

# ── Spec: <relative_path_in_dist> ──
# VMProtect_Con автоматически детектит x86/x64 и EXE/DLL — отдельные templates
# не нужны как у Themida. Один template на всех.
SPEC=(
    "DotaFarm.exe"
    "ProxyHook.dll"
    "dist/HwidSpoofer.exe"
    "Andromeda-Dota2-Base.dll"
)

total=0
protected=0
skipped=0
failed=0
failed_list=""

# cygpath helper — VMProtect хочет Windows-style paths
to_win() {
    if command -v cygpath >/dev/null 2>&1; then
        cygpath -w "$1"
    else
        echo "$1" | sed -E 's|^/([a-zA-Z])/|\1:\\|; s|/|\\|g'
    fi
}

protect_one() {
    local rel="$1"
    local src="$DIST/$rel"
    local out="$DIST/$rel"

    total=$((total + 1))

    if [ ! -f "$src" ]; then
        echo "[protect] SKIP: $rel — не найден в bundle"
        skipped=$((skipped + 1))
        return 0
    fi

    # Подготовить per-binary .vmp project: клонировать template + подставить
    # input path. Output задаётся через CLI VMProtect_Con second arg.
    local tmp_vmp
    tmp_vmp=$(mktemp --suffix=.vmp 2>/dev/null) || tmp_vmp="/tmp/protect_$$_$(echo "$rel" | tr '/' '_').vmp"
    local src_win
    src_win=$(to_win "$src")
    # XML escape backslashes для XML attribute (на самом деле \ безопасен в
    # XML но на всякий случай делаем path-safe substitution через sed с | as
    # delimiter чтобы не конфликтовать с Windows '\'.
    sed "s|__INPUT_FILE_PLACEHOLDER__|${src_win//\\/\\\\}|g" "$TPL_VMP" > "$tmp_vmp"

    local backup="${src}.preprotect"
    cp "$src" "$backup"

    local out_win tmp_vmp_win
    out_win=$(to_win "$out")
    tmp_vmp_win=$(to_win "$tmp_vmp")

    echo "[protect] $rel → VMProtect..."
    # CLI: VMProtect_Con.exe <input> <output> -pf <project>
    # output = input → in-place overwrite (VMProtect создаёт temp + atomic rename)
    if "$VMP_CON" "$src_win" "$out_win" -pf "$tmp_vmp_win" 2>&1; then
        local new_size old_size
        new_size=$(stat -c '%s' "$out" 2>/dev/null || echo 0)
        old_size=$(stat -c '%s' "$backup" 2>/dev/null || echo 0)

        # VMProtect с pack=1 ОБЫЧНО уменьшает размер (compression). С pack=0
        # размер растёт. В обоих случаях файл должен ИЗМЕНИТЬСЯ vs backup.
        # Сравниваем через cmp.
        if ! cmp -s "$out" "$backup"; then
            echo "[protect]   OK: $old_size → $new_size bytes (изменён)"
            rm -f "$backup"
            protected=$((protected + 1))
        else
            echo "[protect]   FAIL: output identical to input — VMP не сработал"
            mv "$backup" "$src"
            failed=$((failed + 1))
            failed_list="$failed_list $rel"
        fi
    else
        local rc=$?
        echo "[protect]   FAIL: VMProtect_Con exit code $rc — restoring backup"
        mv "$backup" "$src"
        failed=$((failed + 1))
        failed_list="$failed_list $rel"
    fi

    rm -f "$tmp_vmp"
}

echo "=== VMProtect batch protector ==="
echo "  dist: $DIST"
echo "  vmprotect: $VMP_CON"
echo "  template: $TPL_VMP"
echo

for rel in "${SPEC[@]}"; do
    protect_one "$rel"
done

echo
echo "=== Summary ==="
echo "  total:     $total"
echo "  protected: $protected"
echo "  skipped:   $skipped"
echo "  failed:    $failed"
if [ -n "$failed_list" ]; then
    echo "  failed list:$failed_list"
fi

if [ "$failed" -gt 0 ]; then
    exit 2
fi
exit 0
