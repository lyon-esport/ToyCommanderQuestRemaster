#!/usr/bin/env bash
# Dump the running game's RAM/VRAM (+ left eye image) from the headset into tcvr/docs/RE/dumps/<name>/.
# Usage: tcvr/tools/ramdump.sh <name> [package]   (git-ignored output; never commit dumps)
set -euo pipefail
cd "$(dirname "$0")/../.."
source tcvr/tools/env.sh
NAME="${1:?name}"; PKG="${2:-com.dreamcast4ever.toycommandervr}"
export ANDROID_SERIAL="${ANDROID_SERIAL:-$(bash tcvr/tools/connect.sh | sed -n 's/export ANDROID_SERIAL=//p')}"
"$ADB" shell "run-as $PKG sh -c 'rm -f files/ram.bin files/vram.bin; touch files/ram.request files/capture.request'"
for i in $(seq 1 30); do
  "$ADB" shell "run-as $PKG ls files/ram.bin" >/dev/null 2>&1 && break; sleep 0.2
done
sleep 1
D="tcvr/docs/RE/dumps/$NAME"; mkdir -p "$D"
for f in ram.bin vram.bin eye0.ppm scene.txt polys.txt; do "$ADB" exec-out run-as "$PKG" cat "files/$f" > "$D/$f"; done
ls -la "$D" | tail -n +2
