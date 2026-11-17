#!/usr/bin/env bash
# Toolchain and headset settings. Machine- and network-specific values (SDK path, headset IP)
# never go in git: put them in the private hub's tools/local.env (next to this checkout,
# TCVR_PRIVATE_ROOT overrides), e.g.
#   ANDROID_HOME=/x/path/to/Android/Sdk
#   TCVR_HEADSET_IP=<the headset's LAN address>
#   TCVR_DECOMP=/x/path/to/decompile/workspace   (outside this checkout, see tcvr/docs/Decomp.md)
# or set them in the environment.
_tcvr_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
_tcvr_local="${TCVR_PRIVATE_ROOT:-$_tcvr_root/..}/tools/local.env"
if [[ -f "$_tcvr_local" ]]; then
  set -a; source "$_tcvr_local"; set +a
fi
unset _tcvr_root _tcvr_local
# Builds use Android Studio's bundled Java (the one v1.0.0 was built and tested with), not the
# system JAVA_HOME, which may point to a newer Java installed for other tools (Ghidra needs 25).
# TCVR_JAVA_HOME overrides it.
_tcvr_jbr="/c/Program Files/Android/Android Studio/jbr"
if [[ -n "${TCVR_JAVA_HOME:-}" ]]; then
  export JAVA_HOME="$TCVR_JAVA_HOME"
elif [[ -d "$_tcvr_jbr" ]]; then
  export JAVA_HOME="$_tcvr_jbr"
else
  export JAVA_HOME="${JAVA_HOME:-$_tcvr_jbr}"
fi
unset _tcvr_jbr
export ANDROID_HOME="${ANDROID_HOME:-$LOCALAPPDATA/Android/Sdk}"
ADB_DEFAULT="/c/Program Files/Meta Quest Developer Hub/resources/bin/adb.exe"
[[ -x "$ANDROID_HOME/platform-tools/adb.exe" ]] && ADB_DEFAULT="$ANDROID_HOME/platform-tools/adb.exe"
export ADB="${ADB:-$ADB_DEFAULT}"
export PATH="$JAVA_HOME/bin:$PATH"
export TCVR_PACKAGE="${TCVR_PACKAGE:-com.dreamcast4ever.toycommandervr}"
export TCVR_DEVICE_DIR="/sdcard/Download/ToyCommanderVR"
# Git Bash: stop MSYS from rewriting device paths like /sdcard
export MSYS_NO_PATHCONV=1
# The headset on the LAN (adb over TLS; its port changes, found by mDNS - see connect.sh).
# Empty: connect.sh takes the first headset mDNS finds.
export TCVR_HEADSET_IP="${TCVR_HEADSET_IP:-}"
# Decompile workspace (game code and data, never in git): see tcvr/docs/Decomp.md.
export TCVR_DECOMP="${TCVR_DECOMP:-}"
