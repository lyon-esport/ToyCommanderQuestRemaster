#!/usr/bin/env bash
# The HD texture review (tcvr/docs/TextureReview.md).
#   tcvr/tools/texcache-review.sh pull            copy the headset's cache and build the review page
#   tcvr/tools/texcache-review.sh reject <file>   send a list of rejected hashes (one per line) to the headset
#   tcvr/tools/texcache-review.sh clear           empty the headset's HD cache (at the game's next start)
# The review folder holds images made from the player's disc: it lives in the private hub
# (${TCVR_PRIVATE_ROOT:-..}/AI/TextureReview/<game id>/), never in git.
set -euo pipefail
cd "$(dirname "$0")/../.."
source tcvr/tools/env.sh
GAME="${TCVR_GAME_ID:-MK-5102050}"
DEV_DIR="/sdcard/Android/data/$TCVR_PACKAGE/files/data/texcache/v2/$GAME"
OUT="${TCVR_PRIVATE_ROOT:-..}/AI/TextureReview/$GAME"
INBOX="/sdcard/Download/ToyCommanderVR/TextureReview"
PY="${PYTHON:-python}"
case "${1:-}" in
  pull)
    mkdir -p "$OUT/cache" "$OUT/cache-orig"
    "$ADB" shell ls "$DEV_DIR" >/dev/null || { echo "No cache on the headset yet ($DEV_DIR)"; exit 1; }
    "$ADB" pull "$DEV_DIR/." "$OUT/cache" >/dev/null
    # the originals, kept next to the cache since 2026-11-16 (older caches have none)
    "$ADB" pull "$DEV_DIR-orig/." "$OUT/cache-orig" >/dev/null 2>&1 || true
    "$PY" tcvr/re/texcache_review.py "$OUT/cache" --out "$OUT/review"
    echo "Open $OUT/review/index.html"
    ;;
  reject)
    LIST="${2:?file with one hash per line}"
    # The cache folder is the app's own (adb can't write or delete there): the list goes to the
    # review inbox in Download, which the game reads at its next start (it adds the hashes to the
    # cache's rejected.txt and deletes those HD files itself). The hub keeps the full list.
    mkdir -p "$OUT"
    { grep -hv '^#' "$OUT/rejected.txt" "$LIST" 2>/dev/null || true; } | tr -d '\r' | grep -E '^[0-9a-fA-F]{1,8}$' | tr 'A-F' 'a-f' | sort -u > "$OUT/rejected.new"
    mv "$OUT/rejected.new" "$OUT/rejected.txt"
    "$ADB" shell mkdir -p "$INBOX"
    tr -d '\r' < "$LIST" | "$ADB" shell "cat > $INBOX/rejected.txt"
    echo "$(grep -c . "$OUT/rejected.txt") rejected in total; restart the game to apply"
    ;;
  clear)
    # empties the HD cache (HD and original copies); it fills again as textures appear
    "$ADB" shell "mkdir -p $INBOX && echo clear > $INBOX/clear.request"
    echo "The cache is cleared at the game's next start"
    ;;
  *) echo "usage: $0 pull | reject <file> | clear"; exit 2 ;;
esac
