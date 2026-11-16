# Sourced by the git hooks: sets $PERL. Git for Windows ships Perl in Git Bash (/usr/bin/perl).
PERL="$(command -v perl 2>/dev/null)"
if [[ -z "$PERL" ]]; then
  for p in /usr/bin/perl "/c/Program Files/Git/usr/bin/perl.exe"; do
    [[ -x "$p" ]] && { PERL="$p"; break; }
  done
fi
if [[ -z "$PERL" ]]; then
  echo "leakscan: perl not found, so the leak check cannot run. Refusing (fail closed)." >&2
  echo "Run git from Git Bash, or install Perl. Bypass once, at your own risk: --no-verify" >&2
  return 1 2>/dev/null || exit 1
fi
export PERL
