#!/usr/bin/env bash
# Toolchain locations on this dev machine. Override via environment if needed.
export JAVA_HOME="${JAVA_HOME:-/c/Program Files/Android/Android Studio/jbr}"
export ANDROID_HOME="${ANDROID_HOME:-/d/LOCAL_DATA/Documents/DEV_ANDROID/Sdk}"
ADB_DEFAULT="/c/Program Files/Meta Quest Developer Hub/resources/bin/adb.exe"
[[ -x "$ANDROID_HOME/platform-tools/adb.exe" ]] && ADB_DEFAULT="$ANDROID_HOME/platform-tools/adb.exe"
export ADB="${ADB:-$ADB_DEFAULT}"
export PATH="$JAVA_HOME/bin:$PATH"
export TCVR_PACKAGE="${TCVR_PACKAGE:-com.toycommandervr.app}"
export TCVR_DEVICE_DIR="/sdcard/Download/ToyCommanderVR"
# Git Bash: stop MSYS from rewriting device paths like /sdcard
export MSYS_NO_PATHCONV=1
# Quest 3 on the LAN (adb over TLS; its port changes, found by mDNS - see connect.sh)
export TCVR_HEADSET_IP="${TCVR_HEADSET_IP:-192.168.1.243}"
