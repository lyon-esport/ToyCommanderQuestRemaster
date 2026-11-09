#!/usr/bin/env bash
# Prints/exports the build version: YYYYMMDD_HHMMSS (local time).
# Usage: source tools/version.sh  -> $TCVR_VERSION ; or ./tools/version.sh -> prints it
TCVR_VERSION="$(date +%Y%m%d_%H%M%S)"
export TCVR_VERSION
# versionCode must be an int <= 2100000000: use minutes since 2026-01-01
TCVR_VERSION_CODE=$(( ( $(date +%s) - 1767225600 ) / 60 ))
export TCVR_VERSION_CODE
[[ "${BASH_SOURCE[0]}" == "$0" ]] && echo "$TCVR_VERSION ($TCVR_VERSION_CODE)"
