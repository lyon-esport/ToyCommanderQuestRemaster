#!/usr/bin/env bash
# DEV ONLY: copy your own disc image to the headset Download folder (never committed).
set -euo pipefail
cd "$(dirname "$0")/../.."
source tcvr/tools/env.sh
IMG="${1:-$(ls "L:/Drive partagés/04 Equipe Technique/20 VR/Games/DEV/Toy Commander Quest Remaster/Source/"*.chd | head -1)}"
"$ADB" shell mkdir -p "$TCVR_DEVICE_DIR"
"$ADB" push "$IMG" "$TCVR_DEVICE_DIR/"
