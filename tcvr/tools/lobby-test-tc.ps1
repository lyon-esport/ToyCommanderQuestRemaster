<#
  Toy Commander multiplayer lobby test on one PC (tcvr, multiplayer wave 2, tasks 3.1 to 3.3).

  Two to four copies of the Windows build, each started like a headset copy in single player. Then, driven
  through the copies' "MP" commands on their pad UDP ports (core/network/tcvr_match.cpp), exactly what the
  VR menu's Multiplayer pages do:

    1. copy p1 creates a game (MP HOST), the others find it on the network (UDP broadcast, no server)
       and join it (MP JOIN: the time to find it is measured, it must be under 3 s), all press Ready;
    2. the host presses Start (MP START): every copy writes the netplay options (network:GGPOPeers,
       GGPOPlayer, ...) into its emu.cfg and asks for a restart (the line "MP RESTART" in its log; on the
       headset the app starts itself again). The script restarts the copies, like the app does;
    3. the restarted copies are one netplay session (the safety gate of a fresh start marker): the automatic
       setup takes every copy to the team and vehicle page with the host's settings, and the per-frame memory hash of every copy is compared with p1's
       (main RAM and video/sound RAM, HOTD2VR_SYNCLOG), as in netplay-test-tc.ps1;
    4. every copy leaves the match (MP LEAVEMATCH): netplay is off in emu.cfg again, the copy is restarted
       and a game start finds no match (MP STATE match=0) and a "match over" mark for the menu (MP RETURN).

  .\tcvr\tools\lobby-test-tc.ps1                       2 copies
  .\tcvr\tools\lobby-test-tc.ps1 -Players 4            4 copies
  .\tcvr\tools\lobby-test-tc.ps1 -NetDelay 80 -Loss 2  the match with simulated delay and loss
  .\tcvr\tools\lobby-test-tc.ps1 -InputDelay 0         the host starts with GGPO input delay 0 (MP START 0; the VR menu
                                                       starts with 2)
  .\tcvr\tools\lobby-test-tc.ps1 -Mode 2 -Room 7 -Points 40   other match settings
  .\tcvr\tools\lobby-test-tc.ps1 -Players 4 -Handover        the host (p1) leaves the lobby before the match: p2 must
                                                       host the same game in its own slot, the others come back to
                                                       their slots; the match is then played by the 3 others
  .\tcvr\tools\lobby-test-tc.ps1 -KillAt 70 -Seconds 100      the last copy is killed 70 s into the match (task 1.4): with
                                                       2 players the other one ends the match and the menu says who
                                                       left; with 3 or 4 the others go on and are compared

  In the match nobody touches a pad: the automatic setup (task 3.4, tcvr_match.cpp setupButtons) takes every
  copy to the game's team and vehicle page with the host's Mode, Room and Points; the test checks its log lines
  and, through PEEK, the page, the players' slots and the options in every copy's memory.

  Windows Firewall asks once for each new flycast.exe path (one per copy folder); a prompt left unanswered
  makes a Block rule, and the copies then never connect. -WorkDir can point to folders already allowed.

  Paths, build and output as netplay-test-tc.ps1 (needs build-win\flycast.exe, or -BuildDir /
  $env:TCVR_BUILD_WIN; the game image from -GamePath or $env:TCVR_PRIVATE_ROOT\Source). Pad ports
  27241+n, netplay ports 19913+n (apart from the other tests'), discovery port 19790.
  Nothing personal goes in the report: no addresses, paths or names.
#>
param(
	[string]$GamePath = '',
	[ValidateRange(2, 4)][int]$Players = 2,
	[int]$Seconds = 75,			# the match, from its first frame
	[int]$NetDelay = 0,			# GGPO's simulated network delay (ms each way)
	[int]$Loss = 0,				# % packets lost
	[int]$InputDelay = 2,			# GGPO input delay (frames) the host starts the match with (MP START <delay>; 2 as the VR menu)
	[ValidateRange(0, 2)][int]$Mode = 0,	# the match settings the host creates the game with (MP HOST): 0 Deathmatch, 1 Cat and Mice, 2 Capture the Flag
	[ValidateRange(0, 7)][int]$Room = 3,	# arena 0..7
	[int]$Points = 15,
	[string]$BuildDir = '',
	[string]$WorkDir = '',
	[string]$OutDir = '',
	[string]$Name = '',
	[switch]$Handover,			# p1 (the host) leaves the lobby before the match (host handover; 3 or 4 players)
	[double]$KillAt = 0,		# >0: the last copy is killed this many seconds into the match (a player who drops)
	[int]$PortOffset = 0		# added to the pad (27241+), GGPO (19913+) and discovery (19790) ports: two tests on one PC at once
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$private = if ($env:TCVR_PRIVATE_ROOT) { $env:TCVR_PRIVATE_ROOT } else { Split-Path -Parent $repo }
$build = if ($BuildDir) { $BuildDir } elseif ($env:TCVR_BUILD_WIN) { $env:TCVR_BUILD_WIN } else { Join-Path $repo 'build-win' }
if (-not (Test-Path (Join-Path $build 'flycast.exe'))) { throw "No flycast.exe in $build (give -BuildDir)" }
if (-not $GamePath) {
	$GamePath = (Get-ChildItem (Join-Path $private 'Source') -Include *.chd, *.gdi, *.cue -Recurse -ErrorAction SilentlyContinue |
		Select-Object -First 1).FullName
}
if (-not $GamePath -or -not (Test-Path $GamePath)) { throw 'No Toy Commander image (give -GamePath)' }
if (-not $WorkDir) { $WorkDir = Join-Path $env:TEMP 'tcvr-lobby' }
if (-not $Name) { $Name = "lobby$($Players)p_d$($NetDelay)_l$($Loss)" + $(if ($Handover) { '_handover' } else { '' }) + $(if ($KillAt) { '_kill' } else { '' }) }
if ($Handover -and $Players -lt 3) { throw '-Handover needs 3 or 4 players (one leaves, two play)' }
if (-not $OutDir) { $OutDir = Join-Path $private ("multiplayer\lobby\{0}_{1}" -f (Get-Date -Format 'yyyyMMdd_HHmmss'), $Name) }
New-Item -ItemType Directory -Force $WorkDir, $OutDir | Out-Null

$report = [System.Collections.Generic.List[string]]::new()
function Say([string]$s) { $report.Add($s); Write-Output $s }
$script:failures = 0
function Check([bool]$ok, [string]$what) {
	Say ($(if ($ok) { 'ok    ' } else { 'FAIL  ' }) + $what)
	if (-not $ok) { $script:failures++ }
}

# Dreamcast pad bits (core/input/gamepad.h)
$BTN = @{ B = 2; A = 4; START = 8; UP = 0x10; DOWN = 0x20; LEFT = 0x40; RIGHT = 0x80 }
# ---- The copies ------------------------------------------------------------------------------
$peers = @(for ($i = 0; $i -lt $Players; $i++) {
	[pscustomobject]@{ Name = "p$($i + 1)"; Index = $i; Port = 27241 + $PortOffset + $i; GgpoPort = 19913 + $PortOffset + $i }
})
foreach ($peer in $peers) {
	$dir = Join-Path $WorkDir $peer.Name
	if (Test-Path $dir) { Remove-Item $dir -Recurse -Force }
	New-Item -ItemType Directory -Force (Join-Path $dir 'data') | Out-Null
	Copy-Item (Join-Path $build 'flycast.exe') $dir
	Get-ChildItem $build -Filter *.dll -ErrorAction SilentlyContinue | Copy-Item -Destination $dir
	$peer | Add-Member Dir $dir
	$peer | Add-Member Proc $null -Force
}
$base = 'config:pvr.rend=0,config:rend.NativeDepthInterpolation=yes,config:aica.Volume=0,log:LogToFile=yes' +
	',config:vr.Reproject=yes,config:vr.HeadCamera=0,config:vr.Game60=no'
for ($i = 1; $i -le 4; $i++) { $base += ",input:device$i=" + $(if ($i -le $Players) { 0 } else { 10 }) }

function Start-Copy($peer, [bool]$sync) {
	$env:TCVR_MP_PORT = $peer.GgpoPort
	if ($PortOffset) { $env:TCVR_MP_DISCOVERY_PORT = 19790 + $PortOffset }
	if ($sync) { $env:HOTD2VR_SYNCLOG = Join-Path $peer.Dir 'sync.txt' } else { Remove-Item Env:HOTD2VR_SYNCLOG -ErrorAction SilentlyContinue }
	if ($NetDelay) { $env:GGPO_NETWORK_DELAY = $NetDelay }
	if ($Loss) { $env:GGPO_PACKET_LOSS = $Loss }
	$env:TCVR_NET_LOOPBACK = '1'	# all copies talk over 127.0.0.1: listen there only (no firewall prompt for each new copy)
	$cfg = "$base,config:vr.UdpPort=$($peer.Port)"
	$peer.Proc = Start-Process -FilePath (Join-Path $peer.Dir 'flycast.exe') `
		-ArgumentList @('-config', $cfg, "`"$GamePath`"") -WorkingDirectory $peer.Dir -PassThru
	foreach ($k in 'TCVR_MP_PORT', 'TCVR_MP_DISCOVERY_PORT', 'HOTD2VR_SYNCLOG', 'GGPO_NETWORK_DELAY', 'GGPO_PACKET_LOSS') { Remove-Item "Env:$k" -ErrorAction SilentlyContinue }
}
function Stop-Copy($peer) {
	if ($peer.Proc -and -not $peer.Proc.HasExited) { Stop-Process -Id $peer.Proc.Id -Force }
	if ($peer.Proc) { $peer.Proc.WaitForExit(5000) | Out-Null }
}

# "MP <command>" to a copy's pad port, the answer text ("" when none within the time)
$cmdUdp = New-Object System.Net.Sockets.UdpClient 0
function Send-Mp($peer, [string]$cmd, [int]$timeoutMs = 8000) {
	$cmdUdp.Client.ReceiveTimeout = $timeoutMs
	$m = [Text.Encoding]::ASCII.GetBytes("MP $cmd")
	[void]$cmdUdp.Send($m, $m.Length, '127.0.0.1', $peer.Port)
	$ep = New-Object System.Net.IPEndPoint([System.Net.IPAddress]::Any, 0)
	try { return [Text.Encoding]::ASCII.GetString($cmdUdp.Receive([ref]$ep)) } catch { return '' }
}
# "PEEK <address> <length>": main RAM bytes of a running copy (core/input/udp_lightgun.cpp), $null when no answer
function Peek($peer, [int64]$addr, [int]$len) {
	$cmdUdp.Client.ReceiveTimeout = 1000
	$m = [Text.Encoding]::ASCII.GetBytes(('PEEK {0:X8} {1}' -f $addr, $len))
	[void]$cmdUdp.Send($m, $m.Length, '127.0.0.1', $peer.Port)
	$ep = New-Object System.Net.IPEndPoint([System.Net.IPAddress]::Any, 0)
	try { $f = [Text.Encoding]::ASCII.GetString($cmdUdp.Receive([ref]$ep)).Split(' ') } catch { return $null }
	if ($f.Count -lt 3 -or $f[0] -ne 'PEEKR' -or $f[2] -eq 'ERR') { return $null }
	$b = [byte[]]::new($f[2].Length / 2)
	for ($i = 0; $i -lt $b.Length; $i++) { $b[$i] = [Convert]::ToByte($f[2].Substring($i * 2, 2), 16) }
	return ,$b
}
function Wait-Mp($peer, [string]$cmd, [scriptblock]$ok, [int]$seconds) {
	$end = (Get-Date).AddSeconds($seconds)
	$r = ''
	while ((Get-Date) -lt $end) {
		$r = Send-Mp $peer $cmd 1500
		if ($r -and (& $ok $r)) { return $r }
		Start-Sleep -Milliseconds 300
	}
	return $r
}
# a file a running copy still has open for writing (its log, sync.txt): File.ReadAllText cannot open those
function Read-Shared([string]$path) {
	try {
		$s = New-Object IO.FileStream($path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite)
		try { return (New-Object IO.StreamReader($s)).ReadToEnd() } finally { $s.Dispose() }
	} catch { return '' }
}
function Cfg-Text($peer) {
	# (on the PC emu.cfg is in the copy's own folder, beside flycast.exe)
	$f = Get-ChildItem $peer.Dir -Filter emu.cfg -Recurse -ErrorAction SilentlyContinue | Select-Object -First 1
	if ($f) { return Read-Shared $f.FullName } else { return '' }
}
function Log-Text($peer) {
	$f = Get-ChildItem $peer.Dir -Filter *.log -Recurse -ErrorAction SilentlyContinue | Sort-Object LastWriteTime | Select-Object -Last 1
	if ($f) { return Read-Shared $f.FullName } else { return '' }
}

try {
	Say ("lobby test {0}: {1} copies, match {2} s, network delay {3} ms, loss {4}%" -f $Name, $Players, $Seconds, $NetDelay, $Loss)

	# ---- 1. start the copies (single player), wait for each one's command listener
	foreach ($peer in $peers) { Start-Copy $peer $false; Start-Sleep -Seconds 1 }
	foreach ($peer in $peers) {
		$r = Wait-Mp $peer 'STATE' { param($x) $x -like 'MPR idle*' } 120
		Check ($r -like 'MPR idle*') "$($peer.Name) is running and answers the MP commands ($r)"
		if ($r -notlike 'MPR idle*') { throw "$($peer.Name) did not start" }
	}

	# ---- 2. host, find, join, ready
	$r = Send-Mp $peers[0] "HOST P1 $Mode $Room $Points"
	Check ($r -like 'MPR lobby players=1 slot=0*') "p1 creates a game: $r"
	$findTimes = @()
	for ($i = 1; $i -lt $Players; $i++) {
		$sw = [Diagnostics.Stopwatch]::StartNew()
		$r = Send-Mp $peers[$i] ("JOIN P{0}" -f ($i + 1)) 12000
		$sw.Stop()
		Check ($r -like "MPR lobby*slot=$i *") ("{0} finds the game and joins it as slot {1}, in {2:0.0} s: {3}" -f $peers[$i].Name, $i, $sw.Elapsed.TotalSeconds, $r)
		$findTimes += $sw.Elapsed.TotalSeconds
	}
	Check (($findTimes | Measure-Object -Maximum).Maximum -lt 3.5) ("every joiner found and joined within 3.5 s (slowest {0:0.0} s)" -f ($findTimes | Measure-Object -Maximum).Maximum)
	$r = Wait-Mp $peers[0] 'STATE' { param($x) $x -like "MPR lobby players=$Players *" } 10
	Check ($r -like "MPR lobby players=$Players *") "the host sees all $Players players: $r"
	$want = "mode=$Mode room=$Room points=$Points"
	Check ($r -like "*$want*") "the host settings ($want) are in the lobby"
	$r = Send-Mp $peers[1] 'STATE'
	Check ($r -like "*$want*") "a joiner sees the host's settings: $r"

	if ($Handover) {
		# ---- 2b. the host leaves before the match: the next player in slot order hosts the same game
		[void](Send-Mp $peers[2] 'READY 1')		# (a ready mark is kept through the handover)
		Start-Sleep -Milliseconds 500
		$sw = [Diagnostics.Stopwatch]::StartNew()
		$r = Send-Mp $peers[0] 'LEAVE'
		Check ($r -like 'MPR idle*') "p1 (the host) leaves the lobby: $r"
		$r = Wait-Mp $peers[1] 'STATE' { param($x) $x -like 'MPR lobby*role=host*' } 15
		Check ($r -like "MPR lobby players=$($Players - 1) slot=1 *host=1 role=host*") ("p2 hosts the game now, in its own slot, after {0:0.0} s: {1}" -f $sw.Elapsed.TotalSeconds, $r)
		for ($i = 2; $i -lt $Players; $i++) {
			$r = Wait-Mp $peers[$i] 'STATE' { param($x) $x -like 'MPR lobby*host=1 role=joiner*' } 15
			Check ($r -like "MPR lobby players=$($Players - 1) slot=$i *host=1 role=joiner*") ("{0} is back in slot {1} with the new host, after {2:0.0} s: {3}" -f $peers[$i].Name, $i, $sw.Elapsed.TotalSeconds, $r)
			Check ($r -like "*$want*") "$($peers[$i].Name): the settings are kept"
		}
		$r = Send-Mp $peers[1] 'STATE'
		Check ($r -like '*ready=-11*') "p3's ready mark is kept: $r"
		for ($i = 2; $i -lt $Players; $i++) { [void](Send-Mp $peers[$i] 'READY 0') }
		Stop-Copy $peers[0]
		$peers = @($peers | Select-Object -Skip 1)
		for ($i = 0; $i -lt $peers.Count; $i++) { $peers[$i].Index = $i }
		$Players = $peers.Count
		Say "the match is now played by $Players copies (p2 hosts)"
	}
	$r = Send-Mp $peers[0] "START $InputDelay"
	Check ($r -like 'MPR error*') "Start is refused while the joiners are not ready: $r"
	for ($i = 1; $i -lt $Players; $i++) { [void](Send-Mp $peers[$i] 'READY 1') }
	$allReady = { param($x) ([regex]::Match($x, 'ready=([01-]{4})').Groups[1].Value -replace '[^1]', '').Length -eq $Players }
	$r = Wait-Mp $peers[0] 'STATE' $allReady 10
	Check (& $allReady $r) "the ready marks reach the host: $r"

	# ---- 3. start: options written, restart asked
	$r = Send-Mp $peers[0] "START $InputDelay"
	Check ($r -like 'MPR started*index=0*') "the host starts the match: $r"
	foreach ($peer in $peers) {
		$r = Wait-Mp $peer 'STATE' { param($x) $x -like 'MPR started*' } 10
		Check ($r -like "MPR started*index=$($peer.Index) of=$Players*") "$($peer.Name) got START as player $($peer.Index + 1) of $Players"
	}
	Start-Sleep -Seconds 1
	foreach ($peer in $peers) {
		$cfg = Cfg-Text $peer
		Check ($cfg -match 'GGPOPeers' -and $cfg -match 'GGPOPlayer\s*=\s*' + $peer.Index) "$($peer.Name): netplay options and player number in emu.cfg"
		Check ((Log-Text $peer) -match 'MP RESTART') "$($peer.Name): asked for a restart"
	}

	# ---- 4. restart the copies (what the app does), the match
	foreach ($peer in $peers) { Stop-Copy $peer }
	Start-Sleep -Seconds 2
	foreach ($peer in $peers) { Start-Copy $peer $true; Start-Sleep -Seconds 1 }
	foreach ($peer in $peers) {
		$r = Wait-Mp $peer 'STATE' { param($x) $x -like '*match=1*' } 120
		Check ($r -like '*match=1*') "$($peer.Name): the restarted game is a match: $r"
	}
	$udp = New-Object System.Net.Sockets.UdpClient
	$launch = Get-Date
	$t0 = $null
	$killed = $null
	while ($true) {
		foreach ($peer in $peers) { if ($peer -ne $killed -and $peer.Proc.HasExited) { throw "$($peer.Name) closed during the match (code $($peer.Proc.ExitCode))" } }
		if ($null -eq $t0) {
			$f = Join-Path $peers[0].Dir 'sync.txt'
			if ((Test-Path $f) -and (Get-Item $f).Length -gt 0) { $t0 = Get-Date }
			elseif (((Get-Date) - $launch).TotalSeconds -gt 120) { throw 'the copies did not start a netplay session within 120 s' }
			Start-Sleep -Milliseconds 100
			continue
		}
		$t = ((Get-Date) - $t0).TotalSeconds
		if ($t -ge $Seconds) { break }
		if ($KillAt -and $null -eq $killed -and $t -ge $KillAt) {
			$killed = $peers[-1]
			Stop-Copy $killed
			Say ("{0:0.0} s into the match: {1} killed" -f $t, $killed.Name)
			if ($Players -eq 2) { break }
		}
		foreach ($peer in $peers) {
			if ($peer -eq $killed) { continue }
			# (nobody presses anything: the automatic setup does)
			$msg = [Text.Encoding]::ASCII.GetBytes('PAD 0 0 0 0 0 0')
			[void]$udp.Send($msg, $msg.Length, '127.0.0.1', $peer.Port)
		}
		Start-Sleep -Milliseconds 30
	}
	$udp.Close()

	if ($killed -and $Players -eq 2) {
		# ---- 4b. two players, one gone: the other's match ends, its menu says who left
		$alone = $peers[0]
		$sw = [Diagnostics.Stopwatch]::StartNew()
		# (the netplay waits 10 s for a silent player; then the match ends. On the PC the netplay's error then
		# stops the game, so the log and emu.cfg are checked rather than the copy's answers)
		$line = $null
		while ($sw.Elapsed.TotalSeconds -lt 40) {
			$line = [regex]::Match((Log-Text $alone), 'MP match: [^\r\n]*left[^\r\n]*')
			if ($line.Success) { break }
			Start-Sleep -Milliseconds 500
		}
		Check $line.Success ("{0}: the match is over {1:0.0} s after {2} left: {3}" -f $alone.Name, $sw.Elapsed.TotalSeconds, $killed.Name, $line.Value)
		Start-Sleep -Seconds 2
		Check ((Cfg-Text $alone) -notmatch 'GGPOPeers[ \t]*=[ \t]*\S') "$($alone.Name): netplay options cleared in emu.cfg"
		Stop-Copy $alone
		Start-Sleep -Seconds 2
		Start-Copy $alone $false
		$r = Wait-Mp $alone 'STATE' { param($x) $x -like 'MPR idle*' } 120
		Check ($r -like 'MPR idle*match=0*') "$($alone.Name): the game is single player again: $r"
		$r = Send-Mp $alone 'RETURN'
		Check ($r -like '*return=1 why=P2 left: the match is over*') "$($alone.Name): the menu opens on the Multiplayer page and says who left: $r"
		throw 'END-2P'		# (nothing else to check)
	}
	if ($killed) {
		foreach ($peer in $peers) {
			if ($peer -eq $killed) { continue }
			$line = [regex]::Match((Log-Text $peer), 'MP match: [^\r\n]*goes on')
			Check $line.Success ("{0} log: {1}" -f $peer.Name, $(if ($line.Success) { $line.Value } else { 'no "the match goes on" line' }))
		}
		$peers = @($peers | Where-Object { $_ -ne $killed })
		Say "the $($peers.Count) other copies are checked and compared"
	}

	# the automatic setup: every copy on the team and vehicle page with the host's settings
	foreach ($peer in $peers) {
		$log = Log-Text $peer
		$done = [regex]::Match($log, 'MP SETUP done[^\r\n]*')
		Check $done.Success ("{0}: automatic setup: {1}" -f $peer.Name, $(if ($done.Success) { $done.Value } else { ([regex]::Match($log, 'MP SETUP failed[^\r\n]*')).Value + ' (no "MP SETUP done")' }))
		# the menu word, read up to 10 times: a guest waiting for inputs is stopped in the middle of a
		# frame, where the game has the word in a passing state (0x2 seen); any read with the page is enough
		$reads = @(); $page = $null
		for ($k = 0; $k -lt 10; $k++) {
			$pg = Peek $peer 0x8C0A5A6C 4
			if ($pg) {
				$v = [BitConverter]::ToUInt32($pg, 0); $reads += '{0:X8}' -f $v; $page = $pg
				if (($v -band 0x4000) -ne 0 -and ($v -band 0x10000) -eq 0) { break }
			} else { $reads += 'none' }
			Start-Sleep -Milliseconds 100
		}
		$slots = Peek $peer 0x8C0A6048 0xA0
		$opt = Peek $peer 0x8C0A5AC8 12		# active environment, mode, room
		if ($null -eq $page -or $null -eq $slots -or $null -eq $opt) { Check $false "$($peer.Name): PEEK answered"; continue }
		$bits = [BitConverter]::ToUInt32($page, 0)
		Check (($bits -band 0x4000) -ne 0 -and ($bits -band 0x10000) -eq 0) ("{0}: on the team and vehicle page (menu bits {1})" -f $peer.Name, ($reads -join ' '))
		$states = @(for ($i = 0; $i -lt 4; $i++) { [BitConverter]::ToUInt32($slots, $i * 0x28 + 8) })
		$joined = @($states | Select-Object -First $Players | Where-Object { $_ -ge 2 }).Count
		Check ($joined -eq $Players) ("{0}: {1} of {2} players on the page (slot states {3})" -f $peer.Name, $joined, $Players, ($states -join ' '))
		$m = [BitConverter]::ToUInt32($opt, 4); $rm = [BitConverter]::ToUInt32($opt, 8)
		Check ($m -eq $Mode -and $rm -eq $Room) ("{0}: options mode {1} room {2} (wanted {3} {4})" -f $peer.Name, $m, $rm, $Mode, $Room)
		# the limit line's index (0x8C0A60E8): 0 none, then 1..10, 15, 20, 25, 30, 40 .. 100 points (seconds in mode 1: 30 .. 1140)
		$steps = if ($Mode -eq 1) { @(0, 30, 60, 90, 120, 180, 240, 300, 360, 420, 480, 540, 600, 660, 720, 780, 840, 900, 960, 1020, 1080, 1140) } else { @(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 15, 20, 25, 30, 40, 50, 60, 70, 80, 90, 100) }
		$wantIdx = 0; for ($i = 1; $i -lt $steps.Count; $i++) { if ([math]::Abs($steps[$i] - $Points) -lt [math]::Abs($steps[$wantIdx] - $Points)) { $wantIdx = $i } }
		$lim = Peek $peer 0x8C0A60E8 4
		$li = if ($lim) { [BitConverter]::ToUInt32($lim, 0) } else { -1 }
		Check ($li -eq $wantIdx) ("{0}: limit index {1} (wanted {2} = {3})" -f $peer.Name, $li, $wantIdx, $steps[$wantIdx])
	}

	# the screens: the last picture of each copy
	# (its own folder: -WorkDir may be shared with netplay-test-tc.ps1, whose pictures are in shots\)
	$shotDir = Join-Path $WorkDir 'lobby-shots'
	if (Test-Path $shotDir) { Remove-Item $shotDir -Recurse -Force }
	New-Item -ItemType Directory -Force $shotDir | Out-Null
	foreach ($peer in $peers) {
		$m = [Text.Encoding]::ASCII.GetBytes(("SHOT {0}" -f (Join-Path $shotDir ("match_{0}.png" -f $peer.Name))))
		[void]$cmdUdp.Send($m, $m.Length, '127.0.0.1', $peer.Port)
	}
	Start-Sleep -Seconds 1

	# ---- 5. compare the memory of the copies
	$logs = @{}
	foreach ($peer in $peers) {
		$hash = @{}
		$file = Join-Path $peer.Dir 'sync.txt'
		if (Test-Path $file) {
			foreach ($line in ((Read-Shared $file) -split "`r?`n")) {
				$f = $line.Split(' ')
				if ($f.Count -lt 5) { continue }
				$hash[[int]$f[0]] = @($f[2], $f[4])		# main RAM, video+sound RAM
			}
		}
		$logs[$peer.Name] = $hash
		Say ("{0}: {1} frames logged" -f $peer.Name, $hash.Count)
	}
	$ref = $peers[0].Name
	$others = @($peers | Select-Object -Skip 1 | ForEach-Object { $_.Name })
	$upTo = ($peers | ForEach-Object { ($logs[$_.Name].Keys | Measure-Object -Maximum).Maximum } | Measure-Object -Minimum).Minimum - 10
	$common = @($logs[$ref].Keys | Where-Object { $fr = $_; $_ -le $upTo -and -not ($others | Where-Object { -not $logs[$_].ContainsKey($fr) }) } | Sort-Object)
	$firstDiff = $null
	foreach ($fr in $common) {
		foreach ($o in $others) {
			if ($logs[$ref][$fr][0] -ne $logs[$o][$fr][0] -or $logs[$ref][$fr][1] -ne $logs[$o][$fr][1]) { if ($null -eq $firstDiff) { $firstDiff = "frame $fr against $ref, copy $o" } }
		}
	}
	Check ($common.Count -gt 600) "the match ran: $($common.Count) confirmed frames compared"
	Check ($null -eq $firstDiff) $(if ($null -eq $firstDiff) { "IDENTICAL on $($peers.Count) copies for $($common.Count) confirmed frames (main RAM, video/sound RAM)" } else { "FIRST DIFFERENCE at $firstDiff" })

	# ---- 6. leave the match
	foreach ($peer in $peers) {
		$r = Send-Mp $peer 'LEAVEMATCH' 5000
		Check ($r -like 'MPR left*') "$($peer.Name) leaves the match"
	}
	Start-Sleep -Seconds 1
	foreach ($peer in $peers) {
		$cfg = Cfg-Text $peer
		Check ($cfg -notmatch 'GGPOPeers[ \t]*=[ \t]*\S') "$($peer.Name): netplay options cleared in emu.cfg"
		Check ((Log-Text $peer) -match 'MP RESTART') "$($peer.Name): asked for a restart"
	}
	foreach ($peer in $peers) { Stop-Copy $peer }
	Start-Sleep -Seconds 2
	foreach ($peer in $peers) { Start-Copy $peer $false; Start-Sleep -Seconds 1 }
	foreach ($peer in $peers) {
		$r = Wait-Mp $peer 'STATE' { param($x) $x -like 'MPR idle*' } 120
		Check ($r -like 'MPR idle*match=0*') "$($peer.Name): the game is single player again: $r"
		$r = Send-Mp $peer 'RETURN'
		Check ($r -like '*return=1*') "$($peer.Name): the menu would open on the Multiplayer page (match over mark): $r"
		$r = Send-Mp $peer 'RETURN'
		Check ($r -like '*return=0*') "$($peer.Name): the mark is given once"
	}
} catch {
	if ($_.Exception.Message -ne 'END-2P') { Check $false ("the test stopped: " + $_.Exception.Message) }
} finally {
	foreach ($peer in $peers) { try { Stop-Copy $peer } catch { } }
	$cmdUdp.Close()
	Say ("{0} failed check(s)" -f $script:failures)
	$report | Set-Content -Path (Join-Path $OutDir 'summary.txt') -Encoding utf8
	foreach ($peer in $peers) {
		$f = Get-ChildItem $peer.Dir -Filter *.log -Recurse -ErrorAction SilentlyContinue | Sort-Object LastWriteTime | Select-Object -Last 1
		if ($f) { Copy-Item $f.FullName (Join-Path $OutDir "$($peer.Name).log") -ErrorAction SilentlyContinue }
	}
	if (Test-Path (Join-Path $WorkDir 'lobby-shots')) { Copy-Item (Join-Path $WorkDir 'lobby-shots\*') $OutDir -ErrorAction SilentlyContinue }
}
exit $(if ($script:failures -eq 0) { 0 } else { 1 })
