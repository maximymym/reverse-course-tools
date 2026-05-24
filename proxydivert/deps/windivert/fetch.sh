#!/bin/bash
# fetch WinDivert 2.2.2 release binaries (signed driver + import lib)
# run once after fresh clone; CI/dev box.
set -e

DIR="$(dirname "$0")"
cd "$DIR"

VERSION="2.2.2"
URL="https://github.com/basil00/WinDivert/releases/download/v${VERSION}/WinDivert-${VERSION}-A.zip"

if [ -f WinDivert.dll ] && [ -f WinDivert.lib ] && [ -f WinDivert64.sys ]; then
    echo "[ok] WinDivert already vendored — nothing to do."
    exit 0
fi

echo "Downloading $URL ..."
curl -L -o wd.zip "$URL"

echo "Extracting ..."
unzip -o wd.zip -d wd_tmp > /dev/null

cp "wd_tmp/WinDivert-${VERSION}-A/x64/WinDivert.dll" .
cp "wd_tmp/WinDivert-${VERSION}-A/x64/WinDivert.lib" .
cp "wd_tmp/WinDivert-${VERSION}-A/x64/WinDivert64.sys" .

rm -rf wd_tmp wd.zip
echo "[ok] vendored: WinDivert.dll, WinDivert.lib, WinDivert64.sys"
