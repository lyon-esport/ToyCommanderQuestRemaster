# Leak scan: no personal data in git

**Rule:** nothing personal or environment-specific is ever committed or pushed. That covers:

- full local paths (`X:\...`, `/x/...` in Git Bash, `/home/<user>`, `C:\Users\<user>`, shared-drive names);
- IP and MAC addresses (LAN, VPN or public), hostnames (`DESKTOP-XXXXXXX`, `*.local`);
- device serial numbers and adb ids (Quest serials, `adb -s <id>`, mDNS `adb-<serial>-...`);
- user names, personal e-mail addresses;
- keys, tokens, passwords, keystores and other secret files.

Use placeholders in code and docs (`<headset-ip>`, `<serial>`, `$ANDROID_HOME`, `~/...`,
the RFC 5737 documentation range `192.0.2.x`) and keep real values in untracked files:
`tcvr/tools/env.local.sh` (see `env.local.sh.example`) for the dev scripts.

The check is `tcvr/tools/leakscan/leakscan.pl`: plain regular expressions in Perl, which ships
with Git for Windows, so it costs nothing and needs no install. It reads only what is new
(added lines, new file names, commit messages), never the whole Flycast tree.

| Where | When | What it scans |
|---|---|---|
| git `pre-commit` hook | every `git commit` | staged added lines (plus the existing game-data/AI-file check) |
| git `pre-push` hook | every `git push` | every commit not yet on a remote, and its message |
| Claude Code `PreToolUse` hook | before Claude runs `git push` or a GitHub write tool | unpushed commits, or the text of the PR, comment or file it is about to send |
| GitHub Actions `Leak scan` | every push and PR | new commits, PR title and description (values masked in the log) |

## Install on Windows (once per clone)

In **Git Bash**, from the code checkout:

```bash
bash tcvr/tools/install-hooks.sh
```

This sets `git config core.hooksPath tcvr/tools/hooks`, so the hooks are versioned and update
with the repo (the old hand-copied `.git/hooks/pre-commit` is replaced by
`tcvr/tools/hooks/pre-commit`, which still runs `tcvr/tools/pre-commit`). It works for every
git client on the machine: Git Bash, VS Code, Android Studio, Copilot, Claude Code.

It also seeds a **personal denylist** in `.git/leakscan-denylist` (inside `.git`, so it can never
be committed): your Windows user name, machine name, `TCVR_HEADSET_IP` and the serials of the
headsets adb sees. Add any other exact value, one per line. `LEAKSCAN_DENYLIST=<file>` points
at an extra list.

Then move your machine values out of tracked files:

```bash
cp tcvr/tools/env.local.sh.example tcvr/tools/env.local.sh   # then edit it
```

Check it works: `bash tcvr/tools/leakscan/tests/run.sh` prints `leakscan self-test: OK`.

GUI clients that do not use Git Bash still run hooks through Git for Windows' `sh.exe`, which
finds Perl. If Perl is ever missing the hook refuses (fail closed) and says so.

### Claude Code on the laptop

The private hub's `.claude/settings.json` (not in this repo) runs the same scanner as a
`PreToolUse` hook, so Claude is stopped before it pushes or publishes, even through the GitHub
tools that do not go through git:

```json
{
  "hooks": {
    "PreToolUse": [
      {
        "matcher": "Bash|mcp__.*github.*__(push_files|create_or_update_file|create_pull_request|update_pull_request|add_issue_comment|update_issue_comment|issue_write|pull_request_review_write|add_comment_to_pending_review|add_reply_to_pull_request_comment)",
        "hooks": [
          {
            "type": "command",
            "command": "f=\"$CLAUDE_PROJECT_DIR/code/tcvr/tools/leakscan/leakscan.pl\"; if [ -f \"$f\" ]; then perl \"$f\" --claude-hook; fi"
          }
        ]
      }
    ]
  }
}
```

For `Bash` it only acts when the command contains `git ... push`. Exit code 2 blocks the call
and shows Claude the findings.

## When it refuses

It prints each finding with its rule and where it is (`<commit> <file>:<line>`). Then:

- **A real leak:** replace the value with a placeholder or an `env.local.sh` variable. If it is
  already in a commit, amend or rebase that commit: a later commit that deletes the line leaves
  it in history.
- **A false positive:** add `leakscan:allow` on that line, or a regex to `.leakscan-allow`
  (matched against the reported value; `path:<glob>` skips a file). Keep that file short.
- **Emergency only:** `git push --no-verify` skips the local hook. The CI job still runs.

`LEAKSCAN_STRICT_AUTHOR=1` also refuses commits whose author/committer e-mail is not a no-reply
address (by default it only warns). To stop publishing your e-mail in commit metadata:
`git config user.email <id>+<login>@users.noreply.github.com` and enable GitHub's
*Block command line pushes that expose my email*.

## Other modes

```bash
perl tcvr/tools/leakscan/leakscan.pl --range origin/main..HEAD   # commits of a branch
perl tcvr/tools/leakscan/leakscan.pl --unpushed                  # everything not pushed yet
echo "some text" | perl tcvr/tools/leakscan/leakscan.pl --text   # any text
perl tcvr/tools/leakscan/leakscan.pl --tree HEAD                 # audit every file (noisy: upstream code has e-mails)
```

Add `--mask` to hide the middle of each value in shared logs.
