#!/bin/bash
# Desktop test of the "Unlock all levels" patcher (VrSaveUnlock.java) with a JDK (javac/java on PATH,
# or JAVA_HOME set). Extra arguments: real VMU images to check read-only.
#   bash tcvr/tools/tests/unlock-test.sh [card.bin ...]
set -e
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../../.." && pwd)"
src="$root/shell/android-studio/flycast/src/main/java/com/flycast/emulator/VrSaveUnlock.java"
jbin="${JAVA_HOME:+$JAVA_HOME/bin/}"
out="$(mktemp -d)"
trap 'rm -rf "$out"' EXIT
"${jbin}javac" -d "$out" "$src" "$here/VrSaveUnlockTest.java"
"${jbin}java" -cp "$out" VrSaveUnlockTest "$@"
