#!/usr/bin/env bash
# Build (optional), install and launch the APK on the Quest, then follow logcat.
# Usage: tools/deploy.sh [path/to.apk]   (default: newest APK under dist/)
set -euo pipefail
cd "$(dirname "$0")/../.."
source tcvr/tools/env.sh
APK="${1:-$(ls -t dist/*.apk 2>/dev/null | head -1 || true)}"
[[ -f "$APK" ]] || { echo "No APK found (build first)"; exit 1; }
BACKUP_ROOT="${TCVR_APK_BACKUP_DIR:-${TCVR_PRIVATE_ROOT:-..}/BACKUP}"
mkdir -p "$BACKUP_ROOT"
BACKUP_DIR="$(mktemp -d "$BACKUP_ROOT/$(date +%Y%m%d_%H%M%S)_XXXXXX")"
cp "$APK" "$BACKUP_DIR/ToyCommanderVR.apk"
(
	cd "$BACKUP_DIR"
	sha256sum ToyCommanderVR.apk > SHA256SUMS
)
printf 'Archived: %s\nSource APK: %s\nPackage: %s\nConfigured target: %s\n' \
	"$(date -Iseconds)" "$APK" "$TCVR_PACKAGE" "${ANDROID_SERIAL:-adb-default}" > "$BACKUP_DIR/deployment.txt"
git rev-parse HEAD > "$BACKUP_DIR/deployment-checkout.txt"
printf 'archived\n' > "$BACKUP_DIR/install-status.txt"
echo "APK rollback archive: $BACKUP_DIR"
"$ADB" devices
if "$ADB" install -r "$BACKUP_DIR/ToyCommanderVR.apk"; then
	printf 'installed\n' > "$BACKUP_DIR/install-status.txt"
	date -Iseconds > "$BACKUP_DIR/installed-at.txt"
else
	printf 'install-failed\n' > "$BACKUP_DIR/install-status.txt"
	exit 1
fi
"$ADB" shell monkey -p "$TCVR_PACKAGE" -c android.intent.category.LAUNCHER 1 >/dev/null
"$ADB" logcat -c
"$ADB" logcat -s flycast:* AndroidRuntime:E DEBUG:* OpenXR:* tcvr:*
