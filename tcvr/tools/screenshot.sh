#!/usr/bin/env bash
# Capture the headset screen to Docs/../build/screens/<timestamp>.png
set -euo pipefail
cd "$(dirname "$0")/../.."
source tcvr/tools/env.sh
mkdir -p build/screens
OUT="build/screens/$(date +%Y%m%d_%H%M%S).png"
"$ADB" exec-out screencap -p > "$OUT"
echo "$OUT"
