#!/usr/bin/env bash
# The HD texture review (tcvr/docs/TextureReview.md).
#   tcvr/tools/texcache-review.sh pull            copy the headset's cache and build the review page
#   tcvr/tools/texcache-review.sh reject <file>   send a list of rejected hashes (one per line) to the headset
# The review folder holds images made from the player's disc: it lives in the private hub
# (${TCVR_PRIVATE_ROOT:-..}/AI/TextureReview/<game id>/), never in git.
set -euo pipefail
cd "$(dirname "$0")/../.."
source tcvr/tools/env.sh
GAME="${TCVR_GAME_ID:-MK-5102050}"
DEV_DIR="/sdcard/Android/data/$TCVR_PACKAGE/files/data/texcache/$GAME"
OUT="${TCVR_PRIVATE_ROOT:-..}/AI/TextureReview/$GAME"
PY="${PYTHON:-python}"
case "${1:-}" in
  pull)
    mkdir -p "$OUT/cache"
    "$ADB" shell ls "$DEV_DIR" >/dev/null || { echo "No cache on the headset yet ($DEV_DIR)"; exit 1; }
    "$ADB" pull "$DEV_DIR/." "$OUT/cache" >/dev/null
    "$PY" tcvr/re/texcache_review.py "$OUT/cache" --out "$OUT/review"
    echo "Open $OUT/review/index.html"
    ;;
  reject)
    LIST="${2:?file with one hash per line}"
    # merged with what the headset already has; written into the existing folder (the app owns it)
    "$ADB" shell "cat $DEV_DIR/rejected.txt 2>/dev/null" > "$OUT/rejected.old" || true
    { echo "# HD textures rejected in review: the game uses the originals"; grep -hv '^#' "$OUT/rejected.old" "$LIST" | tr -d '\r' | grep -E '^[0-9a-fA-F]{1,8}$' | tr 'A-F' 'a-f' | sort -u; } > "$OUT/rejected.txt"
    "$ADB" shell "cat > $DEV_DIR/rejected.txt" < "$OUT/rejected.txt"
    while read -r h; do [[ "$h" == \#* || -z "$h" ]] && continue; "$ADB" shell rm -f "$DEV_DIR/$(printf '%08x' 0x$h).png"; done < "$OUT/rejected.txt"
    echo "$(grep -vc '^#' "$OUT/rejected.txt") rejected; restart the game to apply"
    ;;
  *) echo "usage: $0 pull | reject <file>"; exit 2 ;;
esac
