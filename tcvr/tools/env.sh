#!/usr/bin/env bash
# Toolchain locations. Machine-specific values (SDK path, headset IP, ...) go in the
# untracked tcvr/tools/env.local.sh, never in this file: see tcvr/docs/LeakScan.md.
_tcvr_env_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
[[ -f "$_tcvr_env_dir/env.local.sh" ]] && source "$_tcvr_env_dir/env.local.sh"
export JAVA_HOME="${JAVA_HOME:-/c/Program Files/Android/Android Studio/jbr}"
# Android Studio's default SDK location; override ANDROID_HOME in env.local.sh.
if [[ -z "${ANDROID_HOME:-}" && -n "${LOCALAPPDATA:-}" ]]; then
  export ANDROID_HOME="$(cygpath -u "$LOCALAPPDATA" 2>/dev/null || echo "$LOCALAPPDATA")/Android/Sdk"
fi
ADB_DEFAULT="/c/Program Files/Meta Quest Developer Hub/resources/bin/adb.exe"
[[ -n "${ANDROID_HOME:-}" && -x "$ANDROID_HOME/platform-tools/adb.exe" ]] && ADB_DEFAULT="$ANDROID_HOME/platform-tools/adb.exe"
command -v adb >/dev/null 2>&1 && [[ ! -x "$ADB_DEFAULT" ]] && ADB_DEFAULT="$(command -v adb)"
export ADB="${ADB:-$ADB_DEFAULT}"
export PATH="$JAVA_HOME/bin:$PATH"
export TCVR_PACKAGE="${TCVR_PACKAGE:-com.dreamcast4ever.toycommandervr}"
export TCVR_DEVICE_DIR="/sdcard/Download/ToyCommanderVR"
# Git Bash: stop MSYS from rewriting device paths like /sdcard
export MSYS_NO_PATHCONV=1
# Quest on the LAN (adb over TLS; its port changes, found by mDNS - see connect.sh).
# Set TCVR_HEADSET_IP in env.local.sh; empty = first headset advertised on the LAN.
export TCVR_HEADSET_IP="${TCVR_HEADSET_IP:-}"
unset _tcvr_env_dir
