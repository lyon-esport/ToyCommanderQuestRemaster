#!/usr/bin/env bash
# Scripted pad input for the running game (test aid, see injectedPad in core/rend/vr/xr_host.cpp).
# Usage: tcvr/tools/pad.sh <buttons> [ms] [stick_x] [stick_y]
#   buttons: A B X Y START UP DOWN LEFT RIGHT LT RT joined by +, or - for none
#   ms: how long they are held (default 150); stick: the left stick, -1..1, up positive
# Examples: pad.sh START ; pad.sh A 150 ; pad.sh - 3000 1 0   (steer right for 3 s)
# The headset must be running the game in front (worn, or its proximity sensor overridden:
#   adb shell am broadcast -a com.oculus.vrpowermanager.prox_close).
set -euo pipefail
cd "$(dirname "$0")/../.."
source tcvr/tools/env.sh
BUTTONS="${1:?buttons (or -)}"; MS="${2:-150}"; X="${3:-0}"; Y="${4:-0}"
"$ADB" shell "run-as $TCVR_PACKAGE sh -c 'echo \"$X $Y $BUTTONS $MS\" > files/pad.request'"
# wait for it to be taken and held
sleep "$(awk -v ms="$MS" 'BEGIN { printf "%.2f", ms / 1000 + 0.25 }')"
