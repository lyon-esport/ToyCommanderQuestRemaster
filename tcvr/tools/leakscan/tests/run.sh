#!/usr/bin/env bash
# Self-test for leakscan.pl. Usage: bash tcvr/tools/leakscan/tests/run.sh
set -uo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
scan="$here/../leakscan.pl"
fail=0
export USERNAME= USER= COMPUTERNAME= HOSTNAME= LEAKSCAN_DENYLIST=   # no local identity in the test
check() { # expected-exit file
  local want="$1" n=0
  while IFS= read -r line; do
    n=$((n+1)); [[ -z "$line" || "$line" == \#* ]] && continue
    printf '%s\n' "$line" | perl "$scan" --text t >/dev/null 2>&1; got=$?
    [[ $got -eq $want ]] || { echo "FAIL $(basename "$2"):$n (exit $got, want $want): $line"; fail=1; }
  done < "$2"
}
check 1 "$here/positives.txt"
check 0 "$here/negatives.txt"

# Integration: a throwaway repo with a fake remote, through the real hooks.
tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT
git init -q --bare "$tmp/remote.git"
git init -q "$tmp/w" && cd "$tmp/w"
git config user.email "1+test@users.noreply.github.com"; git config user.name test
mkdir -p tcvr/tools; cp -r "$here/../../leakscan" tcvr/tools/; rm -rf tcvr/tools/leakscan/tests
cp -r "$here/../../hooks" tcvr/tools/; printf '#!/usr/bin/env bash\nexit 0\n' > tcvr/tools/pre-commit
git config core.hooksPath tcvr/tools/hooks
git add -A && git commit -qm init && git remote add origin "$tmp/remote.git" && git push -q origin HEAD:main 2>/dev/null \
  || { echo "FAIL clean push refused"; fail=1; }
echo 'IP=10.9.8.7' > leak.txt; git add leak.txt
git commit -qm leak 2>/dev/null && { echo "FAIL pre-commit let a leak through"; fail=1; }
git commit -qm leak --no-verify
git push -q origin HEAD:main 2>/dev/null && { echo "FAIL pre-push let a leak through"; fail=1; }
echo 'secret value: myname' > d.txt; git add d.txt; echo myname > .git/leakscan-denylist
git commit -qm d 2>/dev/null && { echo "FAIL denylist ignored"; fail=1; }
# Claude Code hook: JSON in, exit 2 = blocked.
printf '{"tool_name":"mcp__github__create_pull_request","tool_input":{"body":"tested on 192.168.0.12"}}' \
  | perl "$scan" --claude-hook 2>/dev/null; [[ $? -eq 2 ]] || { echo "FAIL claude hook (MCP)"; fail=1; }
printf '{"tool_name":"Bash","tool_input":{"command":"ls"},"cwd":"%s"}' "$tmp/w" \
  | perl "$scan" --claude-hook 2>/dev/null || { echo "FAIL claude hook blocked a non-push"; fail=1; }
git reset -q --hard HEAD~2 2>/dev/null; echo 'IP=10.9.8.6' > l2.txt; git add l2.txt; git commit -qm l2 --no-verify
printf '{"tool_name":"Bash","tool_input":{"command":"git push origin HEAD"},"cwd":"%s"}' "$tmp/w" \
  | perl "$scan" --claude-hook 2>/dev/null; [[ $? -eq 2 ]] || { echo "FAIL claude hook (git push)"; fail=1; }

[[ $fail -eq 0 ]] && echo "leakscan self-test: OK"
exit $fail
