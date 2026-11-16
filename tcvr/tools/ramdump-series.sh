#!/usr/bin/env bash
# A series of named RAM dumps from the running game, for finding the game camera (see
# tcvr/docs/RE/Camera360.md): between dumps the vehicle turns, by the player or by scripted
# input (steer = stick x held for <turn_ms> between dumps; 0 = don't steer, the player turns).
# Usage: tcvr/tools/ramdump-series.sh <name> [count=6] [turn_ms=700] [steer=1]
# Output (git-ignored, never commit): tcvr/docs/RE/dumps/<name>/ram_NN.bin + ram_NN.txt, eye0.ppm of the last.
set -euo pipefail
cd "$(dirname "$0")/../.."
source tcvr/tools/env.sh
NAME="${1:?name}"; COUNT="${2:-6}"; TURN_MS="${3:-700}"; STEER="${4:-1}"
D="tcvr/docs/RE/dumps/$NAME"; mkdir -p "$D"
take() {
  local tag="$1"
  "$ADB" shell "run-as $TCVR_PACKAGE sh -c 'rm -f files/ram_$tag.bin files/ram_$tag.txt; echo $tag > files/ram.request'"
  for _ in $(seq 1 50); do
    "$ADB" shell "run-as $TCVR_PACKAGE ls files/ram_$tag.txt" >/dev/null 2>&1 && break; sleep 0.2
  done
  sleep 0.5
  "$ADB" exec-out run-as "$TCVR_PACKAGE" cat "files/ram_$tag.bin" > "$D/ram_$tag.bin"
  "$ADB" exec-out run-as "$TCVR_PACKAGE" cat "files/ram_$tag.txt" > "$D/ram_$tag.txt"
  "$ADB" shell "run-as $TCVR_PACKAGE rm -f files/ram_$tag.bin files/vram_$tag.bin"
  echo "dump $tag: $(wc -c < "$D/ram_$tag.bin") bytes, $(tr '\n' ' ' < "$D/ram_$tag.txt")"
}
for i in $(seq -w 0 $((COUNT - 1))); do
  take "$i"
  if [[ "$i" -lt $((COUNT - 1)) && "$STEER" != "0" && "$TURN_MS" -gt 0 ]]; then
    bash tcvr/tools/pad.sh - "$TURN_MS" "$STEER" 0
    sleep 0.5   # let the camera settle behind the vehicle
  fi
done
echo "Next: python tcvr/re/find_camera.py $D/ram_*.bin"
