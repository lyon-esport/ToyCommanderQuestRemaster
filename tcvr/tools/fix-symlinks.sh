#!/usr/bin/env bash
# Windows without symlink support: git checks out symlinks as small text files holding
# the target path. Replace each one (in the repo and all submodules) with a real copy.
# Run after a clone or a submodule update. Safe to run several times.
set -uo pipefail
cd "$(dirname "$0")/../.."
fix_repo() {  # $1 = repo dir relative to root
  local base="$1"
  git -C "$base" ls-files -s | awk '$1==120000{print $4}' | while read -r link; do
    local p="$base/$link"
    [[ -f "$p" && ! -L "$p" ]] || continue
    local tgt; tgt=$(cat "$p")
    local src; src="$(dirname "$p")/$tgt"
    if [[ -e "$src" && ! ( -f "$src" && $(wc -c < "$src") -lt 200 && "$(head -c 3 "$src")" == "../" ) ]]; then
      rm -f "$p"; cp -r "${src%/}" "$p" && echo "fixed: $p"
    else
      echo "skip (target missing): $p -> $tgt"
    fi
  done
  # mark them unchanged so git status stays clean
  git -C "$base" ls-files -s | awk '$1==120000{print $4}' | xargs -r git -C "$base" update-index --assume-unchanged 2>/dev/null
}
for pass in 1 2; do
  fix_repo .
  git submodule foreach --quiet --recursive 'echo "$displaypath"' | while read -r sm; do fix_repo "$sm"; done
done
