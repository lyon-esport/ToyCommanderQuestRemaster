#!/usr/bin/env perl
# leakscan: refuse personal or environment-specific data before it reaches git.
#
# Deterministic and cheap (Perl ships with Git for Windows and every Linux CI image).
# It looks for local absolute paths, IP/MAC addresses, device serials, hostnames,
# e-mail addresses, keys/tokens and secret files in what is about to be committed or pushed.
#
# Modes:
#   --staged               added lines of the index (pre-commit)
#   --pre-push REMOTE      commits being pushed; ref lines on stdin as git gives them (pre-push)
#   --unpushed             every local commit not on any remote (Claude Code hook before `git push`)
#   --range A..B           commits in a range (CI)
#   --text [LABEL]         plain text on stdin (PR title/body, MCP tool input)
#   --claude-hook          Claude Code PreToolUse hook: JSON on stdin, exit 2 blocks the tool call
#   --tree [REF]           every tracked file at REF (audit; noisy on upstream Flycast files)
# Options:
#   --mask                 print findings with the middle masked (CI logs)
#   --repo DIR             run git in DIR (default: current directory)
#
# Allow a real false positive:
#   - put `leakscan:allow` on the same line, or
#   - add a Perl regex (matched against the finding) to .leakscan-allow at the repo root,
#     or `path:<glob>` to skip a file.
# Personal denylist (exact strings, never committed): .git/leakscan-denylist, or the file
# named by $LEAKSCAN_DENYLIST. One value per line: headset IP, serial, Windows user name, ...
# Exit: 0 clean, 1 findings, 2 usage/git error.
use strict;
use warnings;
use utf8;

my $mode = '';
my @mode_args;
my $mask = 0;
my $repo;
while (@ARGV) {
    my $a = shift @ARGV;
    if    ($a eq '--mask') { $mask = 1 }
    elsif ($a eq '--repo') { $repo = shift @ARGV }
    elsif ($a =~ /^--(staged|unpushed|claude-hook)$/) { $mode = $1 }
    elsif ($a =~ /^--(pre-push|range|text|tree)$/) {
        $mode = $1;
        push @mode_args, shift @ARGV if @ARGV && $ARGV[0] !~ /^--/;
    }
    elsif ($a eq '-h' || $a eq '--help') { usage(0) }
    else { print STDERR "leakscan: unknown argument $a\n"; usage(2) }
}
usage(2) unless $mode;
if (defined $repo) { chdir $repo or die "leakscan: cannot cd to $repo: $!\n" }

sub usage {
    print STDERR "usage: leakscan.pl --staged | --pre-push REMOTE | --unpushed | --range A..B | --text [LABEL] | --claude-hook | --tree [REF]  [--mask] [--repo DIR]\n";
    exit $_[0];
}

sub git {
    my @cmd = ('git', @_);
    open(my $fh, '-|', @cmd) or die "leakscan: cannot run git: $!\n";
    binmode $fh;
    local $/;
    my $out = <$fh>;
    close $fh;
    return defined $out ? $out : '';
}

my $toplevel = '';
{
    my $t = `git rev-parse --show-toplevel 2>/dev/null`;
    chomp $t;
    $toplevel = $t;
}

# ---------------------------------------------------------------- allow / deny lists
my (@allow_re, @allow_path);
if ($toplevel ne '' && open(my $fh, '<', "$toplevel/.leakscan-allow")) {
    while (my $l = <$fh>) {
        chomp $l; $l =~ s/\r$//;
        next if $l =~ /^\s*(#|$)/;
        if ($l =~ /^path:(.+)$/) { push @allow_path, glob_to_re($1) }
        else { push @allow_re, qr/$l/ }
    }
    close $fh;
}

sub glob_to_re {
    my $g = shift;
    my $r = quotemeta $g;
    $r =~ s/\\\*\\\*/.*/g;
    $r =~ s/\\\*/[^\/]*/g;
    $r =~ s/\\\?/./g;
    return qr/^$r$/;
}

my @deny;   # [label, regex]
{
    my @files;
    push @files, $ENV{LEAKSCAN_DENYLIST} if $ENV{LEAKSCAN_DENYLIST};
    my $gd = `git rev-parse --git-common-dir 2>/dev/null`; chomp $gd;
    push @files, "$gd/leakscan-denylist" if $gd ne '';
    for my $f (@files) {
        open(my $fh, '<', $f) or next;
        while (my $l = <$fh>) {
            chomp $l; $l =~ s/\r$//; $l =~ s/^\s+|\s+$//g;
            next if $l eq '' || $l =~ /^#/;
            push @deny, ['denylist', qr/(?<![A-Za-z0-9])\Q$l\E(?![A-Za-z0-9])/i];
        }
        close $fh;
    }
    # The local OS user and machine name are personal by definition.
    my %generic = map { $_ => 1 } qw(user users username root admin administrator runner
        claude builder build ubuntu debian vagrant docker default guest localhost home test dev);
    for my $v ($ENV{USERNAME}, $ENV{USER}, $ENV{COMPUTERNAME}, $ENV{HOSTNAME}) {
        next unless defined $v && length($v) >= 4 && !$generic{lc $v};
        push @deny, ['local-identity', qr/(?<![A-Za-z0-9])\Q$v\E(?![A-Za-z0-9])/i];
    }
}

# ---------------------------------------------------------------- rules
my $OCT = qr/(?:25[0-5]|2[0-4]\d|1\d\d|[1-9]?\d)/;
my $IPV4 = qr/(?<![\w.])($OCT\.$OCT\.$OCT\.$OCT)(?![\w.]*\d)(?!\.\w)/;

sub ip_kind {
    my @o = split /\./, shift;
    return 'private-ip' if $o[0] == 10
        || ($o[0] == 172 && $o[1] >= 16 && $o[1] <= 31)
        || ($o[0] == 192 && $o[1] == 168)
        || ($o[0] == 169 && $o[1] == 254)
        || ($o[0] == 100 && $o[1] >= 64 && $o[1] <= 127);
    return undef if $o[0] == 127 || $o[0] == 0 || $o[0] >= 224;           # loopback, any, multicast/broadcast
    return undef if "$o[0].$o[1].$o[2]" =~ /^(192\.0\.2|198\.51\.100|203\.0\.113)$/;  # RFC 5737 docs
    my ($max) = sort { $b <=> $a } @o;
    return undef if $max < 32;                                             # looks like a version number
    return undef if join('.', @o) =~ /^(1\.1\.1\.1|1\.0\.0\.1|8\.8\.8\.8|8\.8\.4\.4|9\.9\.9\.9)$/;  # public DNS
    return 'public-ip';
}

my $PLACEHOLDER_USER = qr/^(?:user|username|me|you|name|example|runner|<[^>]*>|\$\{?\w+\}?|%\w+%|\.\.\.|xxx+|\*)$/i;
my $STD_WIN_DIR = qr/^(?:Program(?:Data)?|Program Files(?: \(x86\))?|Windows)\b/i;

my @rules = (
    # name, regex (capture 1 = value; whole match if no capture), optional filter returning false to drop
    ['private-key', qr/(-----BEGIN (?:[A-Z]+ )?PRIVATE KEY(?: BLOCK)?-----)/],
    ['token', qr/\b(gh[pousr]_[A-Za-z0-9]{30,}|github_pat_[A-Za-z0-9_]{40,})\b/],
    ['token', qr/\b(AKIA[0-9A-Z]{16})\b/],
    ['token', qr/\b(AIza[0-9A-Za-z_\-]{35})\b/],
    ['token', qr/\b(xox[abprs]-[A-Za-z0-9-]{10,})/],
    ['token', qr/\b(sk-ant-[A-Za-z0-9_\-]{20,}|sk-(?:proj-)?[A-Za-z0-9]{32,})/],
    ['secret-assignment',
        qr/(?i:(?:password|passwd|pwd|secret|api[_-]?key|access[_-]?token|auth[_-]?token|private[_-]?key|client[_-]?secret|store_?password|key_?password))["']?\s*[:=]\s*["']([^"'\s]{6,})["']/,
        sub { $_[0] !~ /^(?:\$|<|%|\{\{|changeme|password|secret|example|placeholder|xxxx|\*{3})/i }],
    ['windows-path', qr/(?<![\w\\\/.%-])([A-Za-z]:(?:\\\\|\\|\/)[^\s"'`<>|*?,;)\]}]+)/,
        sub { (my $rest = $_[0]) =~ s/^[A-Za-z]:(?:\\\\|\\|\/)//; $rest !~ $STD_WIN_DIR && $rest !~ /^(?:path|to|your|x|\.\.\.|<)/i && $rest !~ /^Users(?:\\\\|\\|\/)*$/i }],
    ['windows-path', qr/(?:^|[\s"'=(:-])(\/(?:mnt\/)?[a-zA-Z]\/[A-Za-z_][^\s"'`<>|*?,;)\]}]*)/,
        sub { (my $rest = $_[0]) =~ s/^\/(?:mnt\/)?[a-zA-Z]\///; $rest !~ $STD_WIN_DIR && $_[0] !~ /^\/[a-z]\/(?:[a-z]\/|[a-z]$)/ }],
    ['unc-path', qr/(?<![\\\w])(\\\\[A-Za-z][\w.-]+\\[\w\$.-]+)/],
    ['home-path', qr/((?:\/home|\/Users|(?i:\\Users|\\\\Users))[\\\/]+([^\\\/\s"'`<>|]+))/,  # leakscan:allow
        sub { $_[1] !~ $PLACEHOLDER_USER && $_[1] !~ /^(?:Public|Default|Shared|All Users)$/i }],
    ['shared-drive', qr/((?:Drive partag[ée]s|Shared drives|Mon Drive|My Drive)[\\\/])/i],
    ['mac-address', qr/(?<![\w:])((?:[0-9A-Fa-f]{2}[:-]){5}[0-9A-Fa-f]{2})(?![\w:])/,
        sub { $_[0] !~ /^(?:00[:-]){5}00$|^(?:ff[:-]){5}ff$/i }],
    ['device-serial', qr/\b((?:1WMH|2G0Y|340Y|1PAS|2G97)[A-Z0-9]{8,12})\b/],
    ['device-serial', qr/\badb(?:\.exe)?["']?\s+-s\s+["']?([A-Za-z0-9][\w.:-]{5,})/,
        sub { $_[0] !~ /^(?:emulator-\d+|localhost|127\.)/ }],
    ['device-serial', qr/\bANDROID_SERIAL\s*=\s*["']?([A-Za-z0-9][\w.:-]{5,})/],
    ['device-serial', qr/\badb-([A-Z0-9]{8,})-\w+\._adb/],
    ['device-serial', qr/\b(?i:serial(?:[ _-]?(?:no|number))?|ro\.serialno)\b["']?\s*[:=]\s*["']?([A-Z0-9]{8,})\b/],
    ['hostname', qr/\b((?:DESKTOP|LAPTOP)-[A-Z0-9]{6,8})\b/, sub { $_[0] !~ /-X+$/ }],
    ['hostname', qr/(?<![\w.\/-])([A-Za-z][A-Za-z0-9-]*\.(?:local|lan|home\.arpa))\b(?![.\w])/,
        sub { $_[0] !~ /^(?:localhost|example)\./i }],
    ['email', qr/(?<![\w.+-])([A-Za-z0-9._%+-]+@[A-Za-z0-9-]+(?:\.[A-Za-z0-9-]+)*\.[A-Za-z]{2,})\b/,
        sub { $_[0] !~ /(?:^(?:no-?reply|git)@|\@users\.noreply\.github\.com$|\@(?:example|test|localhost)\.|\@example$|\@[\w.-]*\.(?:png|jpg|svg|gif)$)/i
              && $_[0] !~ /\@\d+x\./ }],
);

my @secret_files = (
    qr/\.(?:jks|keystore|p12|pfx|pem|key|ppk|mobileprovision)$/i,
    qr/(?:^|\/)id_(?:rsa|dsa|ecdsa|ed25519)(?:\.pub)?$/,
    qr/(?:^|\/)(?:\.env(?:\.[\w.-]+)?|\.netrc|\.npmrc|\.pypirc|local\.properties|keystore\.properties|credentials(?:\.json)?|leakscan-denylist)$/i,
);

# ---------------------------------------------------------------- scanning core
my @findings;
my %seen;

sub path_allowed {
    my $p = shift;
    return 0 unless defined $p;
    for my $r (@allow_path) { return 1 if $p =~ $r }
    return 0;
}

sub value_allowed {
    my $v = shift;
    for my $r (@allow_re) { return 1 if $v =~ $r }
    return 0;
}

sub add_finding {
    my ($where, $rule, $value, $line) = @_;
    return if value_allowed($value);
    my $k = "$where\0$rule\0$value";
    return if $seen{$k}++;
    push @findings, [$where, $rule, $value, $line];
}

sub scan_line {
    my ($where, $line) = @_;
    return if $line =~ /leakscan:allow/;
    utf8::decode($line);
    for my $rule (@rules) {
        my ($name, $re, $filter) = @$rule;
        while ($line =~ /$re/g) {
            my $v = defined $1 ? $1 : $&;
            my @caps = ($v, $2);
            next if $filter && !$filter->(@caps);
            add_finding($where, $name, $v, $line);
        }
    }
    while ($line =~ /$IPV4/g) {
        my $ip = $1;
        my $kind = ip_kind($ip) or next;
        # Package/version pins such as `pkg==5.0.0.93` or `v10.2.0.1` are not addresses.
        next if $kind eq 'public-ip' && substr($line, 0, $-[1]) =~ /(?:[=<>~!]=|\bv|version[\s:=]*)$/i;
        add_finding($where, $kind, $ip, $line);
    }
    for my $d (@deny) {
        add_finding($where, $d->[0], $&, $line) if $line =~ $d->[1];
    }
}

sub scan_path {
    my ($where, $path) = @_;
    for my $r (@secret_files) {
        if ($path =~ $r) { add_finding($where, 'secret-file', $path, $path); last }
    }
    for my $d (@deny) {
        add_finding($where, $d->[0], $&, $path) if $path =~ $d->[1];
    }
}

# Parse `git diff`/`git log -p` output (with -U0); scan only added lines and new file names.
sub scan_patch {
    my ($patch, $prefix) = @_;
    my ($file, $ln, $skip) = (undef, 0, 0);
    my $commit = '';
    my $in_msg = 0;
    for my $l (split /\n/, $patch) {
        if ($l =~ /^commit ([0-9a-f]{7,40})/) { $commit = substr($1, 0, 8); $in_msg = 1; $file = undef; next }
        if ($in_msg) {
            if ($l =~ /^diff --git /) { $in_msg = 0 }
            elsif ($l =~ /^    (.*)$/) { scan_line("$commit (commit message)", $1); next }
            else { next }
        }
        if ($l =~ /^diff --git a\/(.*) b\/(.*)$/) { $file = $2; $skip = path_allowed($file); next }
        if ($l =~ /^new file mode/ && defined $file) { scan_path(where($commit, $file), $file) unless $skip; next }
        if ($l =~ /^rename to (.*)$/) { $file = $1; scan_path(where($commit, $file), $file); next }
        if ($l =~ /^\+\+\+ /) { next }
        if ($l =~ /^@@ -\S+ \+(\d+)/) { $ln = $1; next }
        next unless defined $file && !$skip;
        if ($l =~ /^\+(.*)$/) { scan_line(where($commit, $file, $ln), $1); $ln++ }
    }
}

sub where {
    my ($c, $f, $ln) = @_;
    my $w = defined $ln ? "$f:$ln" : $f;
    return $c ne '' ? "$c $w" : $w;
}

sub scan_commits {
    my @commits = @_;
    return unless @commits;
    # Scan in chunks to keep the command line short.
    while (my @chunk = splice(@commits, 0, 200)) {
        my $out = git('log', '--no-walk=unsorted', '-p', '-U0', '--no-color', '--no-ext-diff',
                      '--format=commit %H%n%n%w(0,4,4)%B', '--no-renames', @chunk);
        scan_patch($out);
    }
}

# Commit author/committer e-mails are pushed too. Warn (or refuse with LEAKSCAN_STRICT_AUTHOR=1)
# when they are not a no-reply address.
my %author_warned;
sub check_identities {
    my @commits = @_;
    while (my @chunk = splice(@commits, 0, 200)) {
        for my $e (split /\n/, git('log', '--no-walk=unsorted', '--format=%ae%n%ce', @chunk)) {
            next if $e eq '' || $author_warned{$e}++;
            next if $e =~ /(?:\@users\.noreply\.github\.com|^no-?reply\@[\w.-]+)$/i || value_allowed($e);
            if ($ENV{LEAKSCAN_STRICT_AUTHOR}) { add_finding('commit author/committer', 'author-email', $e, $e) }
            else {
                print STDERR "leakscan: warning: commits carry a personal e-mail (", ($mask ? masked($e) : $e), ").\n",
                    "  Use your GitHub no-reply address: git config user.email <id>+<login>\@users.noreply.github.com\n";
            }
        }
    }
}

sub rev_list { my $o = git('rev-list', @_); return grep { length } split /\n/, $o }

# ---------------------------------------------------------------- modes
my $exit_code_on_findings = 1;

if ($mode eq 'staged') {
    scan_patch(git('diff', '--cached', '-U0', '--no-color', '--no-ext-diff', '--no-renames'));
}
elsif ($mode eq 'pre-push') {
    my $remote = $mode_args[0] // 'origin';
    my @commits;
    while (my $l = <STDIN>) {
        my ($lref, $lsha, $rref, $rsha) = split ' ', $l;
        next unless defined $lsha && $lsha !~ /^0+$/;
        push @commits, rev_list($lsha, '--not', '--remotes');
    }
    my %u; @commits = grep { !$u{$_}++ } @commits;
    check_identities(@commits);
    scan_commits(@commits);
}
elsif ($mode eq 'unpushed') {
    my @c = rev_list('--branches', '--not', '--remotes');
    check_identities(@c);
    scan_commits(@c);
}
elsif ($mode eq 'range') {
    my $range = $mode_args[0] or usage(2);
    my @c = rev_list($range);
    check_identities(@c);
    scan_commits(@c);
}
elsif ($mode eq 'text') {
    my $label = $mode_args[0] // 'input';
    my $n = 0;
    while (my $l = <STDIN>) { chomp $l; $n++; scan_line("$label:$n", $l) }
}
elsif ($mode eq 'tree') {
    my $ref = $mode_args[0] // 'HEAD';
    for my $f (split /\n/, git('ls-tree', '-r', '--name-only', $ref)) {
        next if path_allowed($f);
        scan_path($f, $f);
        my $blob = git('cat-file', '-p', "$ref:$f");
        next if $blob =~ /\x00/;   # binary
        my $n = 0;
        for my $l (split /\n/, $blob) { $n++; scan_line("$f:$n", $l) }
    }
}
elsif ($mode eq 'claude-hook') {
    require JSON::PP;
    local $/;
    my $raw = <STDIN> // '';
    my $in = eval { JSON::PP->new->utf8->decode($raw) } || {};
    my $tool = $in->{tool_name} // '';
    my $ti = $in->{tool_input} // {};
    if ($tool eq 'Bash') {
        my $cmd = $ti->{command} // '';
        exit 0 unless $cmd =~ /\bgit\b[^\n;&|]*\bpush\b/;
        # Scan what the push would publish, in the repo the command targets.
        my $dir;
        if ($cmd =~ /\bgit\s+-C\s+("([^"]+)"|'([^']+)'|(\S+))/) { $dir = $2 // $3 // $4 }
        elsif ($cmd =~ /\bcd\s+("([^"]+)"|'([^']+)'|(\S+))\s*(?:&&|;)/) { $dir = $2 // $3 // $4 }
        if (defined $dir) {
            $dir = "$in->{cwd}/$dir" if $dir !~ m{^(?:[A-Za-z]:)?[\\/]} && $in->{cwd};
            chdir $dir;
        } elsif ($in->{cwd}) { chdir $in->{cwd} }
        if (system('git rev-parse --git-dir >/dev/null 2>&1') != 0) {
            for my $sub ('code', '.') { if (-d "$sub/.git") { chdir $sub; last } }
        }
        scan_commits(rev_list('--branches', '--not', '--remotes'));
    } else {
        # GitHub (or other) write tools: scan every string the tool would publish.
        my @strings;
        my $walk;
        $walk = sub {
            my $v = shift;
            if (ref $v eq 'HASH') { $walk->($v->{$_}) for sort keys %$v }
            elsif (ref $v eq 'ARRAY') { $walk->($_) for @$v }
            elsif (defined $v && !ref $v) { push @strings, $v }
        };
        $walk->($ti);
        my $n = 0;
        for my $s (@strings) {
            for my $l (split /\n/, $s) { $n++; my $b = $l; utf8::encode($b); scan_line("$tool input:$n", $b) }
        }
    }
    $exit_code_on_findings = 2;
}

# ---------------------------------------------------------------- report
exit 0 unless @findings;

sub masked {
    my $v = shift;
    return $v unless $mask;
    my $len = length $v;
    return '*' x $len if $len <= 4;
    my $k = $len >= 12 ? 3 : 1;
    return substr($v, 0, $k) . ('*' x ($len - 2 * $k)) . substr($v, -$k);
}

binmode STDERR, ':encoding(UTF-8)';
print STDERR "\nleakscan: personal or environment-specific data found. Nothing was ",
    ($mode eq 'staged' ? 'committed' : 'pushed'), ".\n\n";
for my $f (@findings) {
    my ($where, $rule, $value) = @$f;
    printf STDERR "  %-18s %s\n  %-18s %s\n", "[$rule]", masked($value), '', $where;
}
print STDERR <<'EOT';

Replace each value with a placeholder (<headset-ip>, <serial>, $ANDROID_HOME, ~/...) or read
it from an untracked local file (for example tcvr/tools/env.local.sh). For history, amend or
rebase the commit; a later commit that deletes the line does not remove it from history.
A real false positive: add `leakscan:allow` on that line, or a regex to .leakscan-allow.
Help: tcvr/docs/LeakScan.md
EOT
exit $exit_code_on_findings;
