#!/usr/bin/env bash
set -euo pipefail
DEPLOY="$(cd "$(dirname "$0")/.." && pwd)/deploy.sh"
TEST_ROOT="$(mktemp -d)"
trap 'rm -rf "$TEST_ROOT"' EXIT
mkdir -p "$TEST_ROOT/source builds"
export TEST_SOURCE="$TEST_ROOT/source builds/test build.apk"
export TEST_EXPECTED="$TEST_ROOT/expected.apk"
export TEST_LOG="$TEST_ROOT/adb.log"
export TEST_MODE=success
export TEST_ARCHIVE_ROOT="$TEST_ROOT/archive space"
export TCVR_APK_BACKUP_DIR="$TEST_ARCHIVE_ROOT"
export ADB=tcvr_test_adb
printf 'original APK bytes\n' > "$TEST_SOURCE"
cp "$TEST_SOURCE" "$TEST_EXPECTED"
: > "$TEST_LOG"

tcvr_test_adb() {
  printf '%s\n' "$*" >> "$TEST_LOG"
  case "$1" in
    devices)
      [[ "$TEST_MODE" != devices-fail ]] || return 7
      ;;
    install)
      [[ "$2" == -r && "$3" == "$TEST_ARCHIVE_ROOT/"*/ToyCommanderVR.apk ]] || return 8
      if [[ "$TEST_MODE" == mutate ]]; then
        printf 'changed build output\n' > "$TEST_SOURCE"
      fi
      cmp "$3" "$TEST_EXPECTED" || return 9
      [[ "$TEST_MODE" != install-fail ]] || return 10
      ;;
  esac
}
export -f tcvr_test_adb

bash "$DEPLOY" "$TEST_SOURCE"
bash "$DEPLOY" "$TEST_SOURCE"
shopt -s nullglob
ENTRIES=("$TEST_ARCHIVE_ROOT"/*/)
[[ ${#ENTRIES[@]} -eq 2 ]]
for ENTRY in "${ENTRIES[@]}"; do
  [[ "$(cat "$ENTRY/install-status.txt")" == installed ]]
  [[ -s "$ENTRY/installed-at.txt" && -s "$ENTRY/deployment-checkout.txt" ]]
  cmp "$ENTRY/ToyCommanderVR.apk" "$TEST_EXPECTED"
  (cd "$ENTRY" && sha256sum -c SHA256SUMS)
done

export TEST_MODE=install-fail
export TEST_ARCHIVE_ROOT="$TEST_ROOT/failed install"
export TCVR_APK_BACKUP_DIR="$TEST_ARCHIVE_ROOT"
: > "$TEST_LOG"
if bash "$DEPLOY" "$TEST_SOURCE"; then exit 11; fi
ENTRIES=("$TEST_ARCHIVE_ROOT"/*/)
[[ ${#ENTRIES[@]} -eq 1 ]]
[[ "$(cat "${ENTRIES[0]}/install-status.txt")" == install-failed ]]
[[ ! -e "${ENTRIES[0]}/installed-at.txt" ]]
! grep -q '^shell ' "$TEST_LOG"
(cd "${ENTRIES[0]}" && sha256sum -c SHA256SUMS)

export TEST_MODE=devices-fail
export TEST_ARCHIVE_ROOT="$TEST_ROOT/no device"
export TCVR_APK_BACKUP_DIR="$TEST_ARCHIVE_ROOT"
if bash "$DEPLOY" "$TEST_SOURCE"; then exit 12; fi
ENTRIES=("$TEST_ARCHIVE_ROOT"/*/)
[[ "$(cat "${ENTRIES[0]}/install-status.txt")" == archived ]]
[[ ! -e "${ENTRIES[0]}/installed-at.txt" ]]

: > "$TEST_LOG"
if bash "$DEPLOY" "$TEST_ROOT/missing.apk"; then exit 13; fi
[[ ! -s "$TEST_LOG" ]]
printf 'not a directory\n' > "$TEST_ROOT/blocker"
export TCVR_APK_BACKUP_DIR="$TEST_ROOT/blocker/nested"
if bash "$DEPLOY" "$TEST_SOURCE"; then exit 14; fi
[[ ! -s "$TEST_LOG" ]]

export TEST_MODE=mutate
export TEST_ARCHIVE_ROOT="$TEST_ROOT/private hub/BACKUP"
export TCVR_PRIVATE_ROOT="$TEST_ROOT/private hub"
unset TCVR_APK_BACKUP_DIR
bash "$DEPLOY" "$TEST_SOURCE"
ENTRIES=("$TEST_ARCHIVE_ROOT"/*/)
[[ ${#ENTRIES[@]} -eq 1 ]]
[[ "$(cat "${ENTRIES[0]}/install-status.txt")" == installed ]]
cmp "${ENTRIES[0]}/ToyCommanderVR.apk" "$TEST_EXPECTED"
! cmp -s "$TEST_SOURCE" "$TEST_EXPECTED"
(cd "${ENTRIES[0]}" && sha256sum -c SHA256SUMS)
printf 'PASS: repeated success, install failure, unavailable device, missing APK, blocked archive, private-root override and mutable build output; no real ADB calls.\n'