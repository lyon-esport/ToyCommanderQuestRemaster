#!/usr/bin/env bash
# DEV ONLY: copy your own disc image to the headset Download folder (never committed).
set -euo pipefail
cd "$(dirname "$0")/../.."
source tcvr/tools/env.sh
IMG="${1:-}"
if [[ -z "$IMG" ]]; then
	PRIVATE_ROOT="${TCVR_PRIVATE_ROOT:-..}"
	shopt -s nullglob
	IMAGES=("$PRIVATE_ROOT"/Source/*.chd)
	[[ ${#IMAGES[@]} -gt 0 ]] || { echo "No CHD found in $PRIVATE_ROOT/Source; provide an image path or set TCVR_PRIVATE_ROOT." >&2; exit 1; }
	IMG="${IMAGES[0]}"
fi
[[ -f "$IMG" ]] || { echo "Image not found: $IMG" >&2; exit 1; }
"$ADB" shell mkdir -p "$TCVR_DEVICE_DIR"
"$ADB" push "$IMG" "$TCVR_DEVICE_DIR/"
