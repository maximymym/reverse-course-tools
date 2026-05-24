#!/usr/bin/env bash
# fetch_hero_portraits.sh — pull all Dota 2 hero portraits (124 PNG, 124x70 small box).
#
# Source: OpenDota /api/heroes (canonical hero list with `name`/`localized_name`/
# `id`/`primary_attr`) → JSON saved to assets/heroes/heroes.json.
# Then for each hero, pull `<short_name>.png` from Valve CDN (dota_react first,
# then `_sb.png` legacy fallback if 404). Files land in assets/heroes/.
#
# Resumable: skip if local file already exists and is non-empty PNG.
# Parallel: xargs -P 8.
#
# Usage:
#   bash scripts/fetch_hero_portraits.sh
set -uo pipefail

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
ROOT_DIR="$( cd "$SCRIPT_DIR/.." && pwd )"
HEROES_DIR="$ROOT_DIR/assets/heroes"
HEROES_JSON="$HEROES_DIR/heroes.json"

mkdir -p "$HEROES_DIR"

URL_PRIMARY="https://cdn.cloudflare.steamstatic.com/apps/dota2/images/dota_react/heroes"
URL_FALLBACK="https://cdn.cloudflare.steamstatic.com/apps/dota2/images/heroes"

echo "[1/3] fetching hero list from OpenDota…"
if ! curl -fsSL "https://api.opendota.com/api/heroes" -o "$HEROES_JSON.tmp"; then
    echo "ERROR: failed to fetch https://api.opendota.com/api/heroes" >&2
    exit 1
fi
mv "$HEROES_JSON.tmp" "$HEROES_JSON"

# Extract short_name list (everything after npc_dota_hero_).
# We use grep+sed instead of jq to avoid extra dep.
mapfile -t SHORTS < <(
    grep -oE '"name"[[:space:]]*:[[:space:]]*"npc_dota_hero_[^"]+"' "$HEROES_JSON" \
        | sed -E 's/.*"npc_dota_hero_([^"]+)".*/\1/'
)

TOTAL=${#SHORTS[@]}
echo "[2/3] hero list parsed: $TOTAL heroes"
if [[ "$TOTAL" -lt 100 ]]; then
    echo "ERROR: only $TOTAL heroes parsed — JSON shape changed? bailing." >&2
    exit 1
fi

# Worker function — invoked by xargs.
fetch_one() {
    local short="$1"
    local out="$HEROES_DIR/$short.png"

    # Skip if already a valid PNG (≥1KB).
    if [[ -s "$out" ]]; then
        local sz
        sz=$(stat -c '%s' "$out" 2>/dev/null || stat -f '%z' "$out" 2>/dev/null || echo 0)
        if [[ "$sz" -ge 1024 ]]; then
            # Magic check: PNG starts with 89 50 4E 47.
            local magic
            magic=$(head -c 4 "$out" | od -An -tx1 | tr -d ' \n')
            if [[ "$magic" == "89504e470d0a1a0a" || "$magic" == "89504e47" ]]; then
                echo "  skip $short (cached)"
                return 0
            fi
        fi
        rm -f "$out"
    fi

    # Try dota_react first.
    local url1="$URL_PRIMARY/$short.png"
    if curl -fsSL --max-time 15 "$url1" -o "$out.tmp" 2>/dev/null; then
        local magic
        magic=$(head -c 4 "$out.tmp" | od -An -tx1 | tr -d ' \n')
        if [[ "$magic" == "89504e47" ]]; then
            mv "$out.tmp" "$out"
            echo "  ok   $short (dota_react)"
            return 0
        fi
        rm -f "$out.tmp"
    fi

    # Fallback to legacy _sb.png.
    local url2="$URL_FALLBACK/${short}_sb.png"
    if curl -fsSL --max-time 15 "$url2" -o "$out.tmp" 2>/dev/null; then
        local magic
        magic=$(head -c 4 "$out.tmp" | od -An -tx1 | tr -d ' \n')
        if [[ "$magic" == "89504e47" ]]; then
            mv "$out.tmp" "$out"
            echo "  ok   $short (legacy _sb)"
            return 0
        fi
        rm -f "$out.tmp"
    fi

    echo "  FAIL $short (both URLs 404 / not-PNG)" >&2
    return 1
}

export HEROES_DIR URL_PRIMARY URL_FALLBACK
export -f fetch_one

echo "[3/3] downloading $TOTAL portraits (xargs -P 8)…"
printf '%s\n' "${SHORTS[@]}" | xargs -n 1 -P 8 -I {} bash -c 'fetch_one "$@"' _ {}

# Tally.
GOT=$(ls -1 "$HEROES_DIR"/*.png 2>/dev/null | wc -l)
SIZE=$(du -sh "$HEROES_DIR" 2>/dev/null | awk '{print $1}')
echo
echo "DONE: $GOT/$TOTAL hero portraits in $HEROES_DIR (total $SIZE)"
