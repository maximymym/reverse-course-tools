#!/bin/bash
# Deploy DotaFarm package to server
# Runs package.sh with the SAME version → .dist_version inside zip matches
# version.txt on server. Uploads to eft-deploy.
#
# Usage:
#   bash tools/dota2/deploy.sh                  → auto-version (YYYY.MM.DD.HHMM)
#   bash tools/dota2/deploy.sh my-feature-fix   → explicit version tag
#
# RULE: каждый deploy получает новую version. Иначе MessageBox "scripts update"
# у юзера не покажется (он увидит равные .dist_version == .installed_version).

set -e

PACKAGE_DIR="/c/temp/DotaFarm"
ZIP_PATH="/c/temp/DotaFarm_deploy.zip"
ZIP_PATH_WIN="C:\\temp\\DotaFarm_deploy.zip"
REMOTE_DIR="/data/static/dota"
VERSION="${1:-$(date +%Y.%m.%d.%H%M)}"
SKIP_REPACKAGE="${SKIP_REPACKAGE:-0}"

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

# ── Auto-repackage ──────────────────────────────────────────────
# По умолчанию deploy сам делает package.sh с этой версией → .dist_version
# внутри zip совпадает с version.txt на сервере. Пропустить можно через
# SKIP_REPACKAGE=1 bash deploy.sh ... (если ты уверен что package свежий).
if [ "$SKIP_REPACKAGE" != "1" ]; then
    echo "=== Packaging DotaFarm v${VERSION} ==="
    ( cd "$SCRIPT_DIR/orchestrator" && bash package.sh "$VERSION" ) >/dev/null
fi

if [ ! -d "$PACKAGE_DIR" ]; then
    echo "[!] Package dir not found: $PACKAGE_DIR"
    echo "    Run: cd tools/dota2/orchestrator && bash package.sh $VERSION"
    exit 1
fi

if [ ! -f "$PACKAGE_DIR/DotaFarm.exe" ]; then
    echo "[!] DotaFarm.exe not found in $PACKAGE_DIR"
    exit 1
fi

# Sanity check: .dist_version в zip должен совпадать с $VERSION
DIST_VER_FILE="$PACKAGE_DIR/scripts/bots/.dist_version"
if [ -f "$DIST_VER_FILE" ]; then
    ACTUAL_VER="$(cat "$DIST_VER_FILE" | tr -d '\r\n ')"
    if [ "$ACTUAL_VER" != "$VERSION" ]; then
        echo "[!] WARNING: .dist_version in package = '$ACTUAL_VER', but deploying as '$VERSION'"
        echo "    → script sync MessageBox on user side may not trigger."
        echo "    Re-run WITHOUT SKIP_REPACKAGE=1 to fix."
    fi
fi

echo "=== Deploying DotaFarm v${VERSION} ==="

# Create zip (contents only, exclude config/logs/ini/license)
rm -f "$ZIP_PATH"
cd "$PACKAGE_DIR"
powershell.exe -NoProfile -Command "
    \$items = @(
        'DotaFarm.exe',
        'Andromeda-Dota2-Base.dll',
        'ProxyHook.dll',
        'WinDivert.dll',
        'WinDivert64.sys',
        'sing-box.exe',
        'wintun.dll',
        'handle64.exe',
        'README.txt',
        'data',
        'scripts',
        'docs',
        'assets',
        'config',
        'dist'
    ) | Where-Object { Test-Path \$_ }
    Compress-Archive -Path \$items -DestinationPath '$ZIP_PATH_WIN' -Force
"

ZIP_SIZE=$(stat -c '%s' "$ZIP_PATH" 2>/dev/null || stat -f '%z' "$ZIP_PATH")
echo "[+] Created zip: $(( ZIP_SIZE / 1024 / 1024 ))MB"

# Ensure remote dir exists
ssh eft-deploy "mkdir -p $REMOTE_DIR"

# Upload
echo "[*] Uploading..."
scp "$ZIP_PATH" "eft-deploy:${REMOTE_DIR}/DotaFarm.zip"
echo "$VERSION" | ssh eft-deploy "cat > ${REMOTE_DIR}/version.txt"

# Verify
REMOTE_SIZE=$(ssh eft-deploy "stat -c '%s' ${REMOTE_DIR}/DotaFarm.zip")
echo "[+] Remote: ${REMOTE_SIZE} bytes, version: ${VERSION}"

if [ "$ZIP_SIZE" != "$REMOTE_SIZE" ]; then
    echo "[!] WARNING: size mismatch! Local=$ZIP_SIZE Remote=$REMOTE_SIZE"
    exit 1
fi

echo "=== Deploy complete ==="
echo "  URL: https://v1per.tech/dota/DotaFarm.zip"
echo "  Ver: https://v1per.tech/dota/version.txt"
