#!/usr/bin/env bash
# Build (optional), install and launch the APK on the Quest, then follow logcat.
# Usage: tools/deploy.sh [path/to.apk]   (default: newest APK under dist/)
set -euo pipefail
cd "$(dirname "$0")/../.."
source tcvr/tools/env.sh
APK="${1:-$(ls -t dist/*.apk 2>/dev/null | head -1 || true)}"
[[ -f "$APK" ]] || { echo "No APK found (build first)"; exit 1; }
"$ADB" devices
"$ADB" install -r "$APK"
"$ADB" shell monkey -p "$TCVR_PACKAGE" -c android.intent.category.LAUNCHER 1 >/dev/null
"$ADB" logcat -c
"$ADB" logcat -s flycast:* AndroidRuntime:E DEBUG:* OpenXR:* tcvr:*
