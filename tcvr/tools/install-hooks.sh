#!/usr/bin/env bash
# One-time setup per clone: enable the versioned git hooks (pre-commit + pre-push leak check)
# and seed the personal denylist with this machine's identifiers. Safe to run again.
# Usage (Git Bash, from the code checkout): bash tcvr/tools/install-hooks.sh
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"
chmod +x tcvr/tools/hooks/* tcvr/tools/pre-commit 2>/dev/null || true
git config core.hooksPath tcvr/tools/hooks
echo "core.hooksPath = tcvr/tools/hooks (pre-commit and pre-push enabled)"

# Old copies installed by hand in .git/hooks are now ignored by git; say so.
for h in pre-commit pre-push; do
  [[ -f .git/hooks/$h ]] && echo "note: .git/hooks/$h is no longer used (core.hooksPath wins)"
done

# Personal denylist: exact values that must never appear in a commit. It lives inside .git,
# so it can never be committed itself. Add your headset IP, serials, user and machine names.
deny="$(git rev-parse --git-common-dir)/leakscan-denylist"
touch "$deny"
add() { local v="$1"; [[ -n "$v" && ${#v} -ge 4 ]] || return 0; grep -qixF -- "$v" "$deny" || { echo "$v" >> "$deny"; echo "  denylist += $2"; }; }
echo "Seeding $deny (values are not printed):"
add "${USERNAME:-${USER:-}}" "OS user name"
add "${COMPUTERNAME:-$(hostname 2>/dev/null || true)}" "machine name"
if [[ -f tcvr/tools/env.sh ]]; then source tcvr/tools/env.sh >/dev/null 2>&1 || true; fi
add "${TCVR_HEADSET_IP:-}" "headset IP (TCVR_HEADSET_IP)"
if [[ -n "${ADB:-}" && -x "${ADB}" ]]; then
  while read -r id state _; do
    [[ "$state" == device ]] || continue
    v="${id%%:*}"                                   # <ip>:<port> -> <ip>
    [[ "$v" == adb-* ]] && { v="${v#adb-}"; v="${v%%-*}"; }   # adb-<serial>-<rnd>._adb-tls-connect._tcp
    add "$v" "connected device id"
    add "$("$ADB" -s "$id" shell getprop ro.serialno 2>/dev/null | tr -d '\r' || true)" "device serial"
  done < <("$ADB" devices 2>/dev/null | tail -n +2)
fi
echo "Done. Edit $deny to add more values (one per line)."
