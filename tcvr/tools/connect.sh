#!/usr/bin/env bash
# Connect adb to the Quest over Wi-Fi (TLS). The TLS port changes, so look it up by mDNS.
set -euo pipefail
cd "$(dirname "$0")/../.."
source tcvr/tools/env.sh
EP=$("$ADB" mdns services | awk -v ip="$TCVR_HEADSET_IP" '/_adb-tls-connect/ && index($3, ip":")==1 {print $3; exit}')
[[ -n "$EP" ]] || { echo "Headset $TCVR_HEADSET_IP not found by mDNS (wireless debugging on? same LAN?)"; exit 1; }
"$ADB" connect "$EP"
echo "export ANDROID_SERIAL=$EP"
