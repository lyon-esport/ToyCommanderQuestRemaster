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
    3. the restarted copies are one netplay session (the safety gate of a fresh start marker): p1 goes through
       the game's menus to its main menu, and the per-frame memory hash of every copy is compared with p1's
       (main RAM and video/sound RAM, HOTD2VR_SYNCLOG), as in netplay-test-tc.ps1;
    4. every copy leaves the match (MP LEAVEMATCH): netplay is off in emu.cfg again, the copy is restarted
       and a game start finds no match (MP STATE match=0) and a "match over" mark for the menu (MP RETURN).

  .\tcvr\tools\lobby-test-tc.ps1                       2 copies
  .\tcvr\tools\lobby-test-tc.ps1 -Players 4            4 copies
  .\tcvr\tools\lobby-test-tc.ps1 -NetDelay 80 -Loss 2  the match with simulated delay and loss

  Paths, build and output as netplay-test-tc.ps1 (needs build-win\flycast.exe, or -BuildDir /
  $env:TCVR_BUILD_WIN; the game image from -GamePath or $env:TCVR_PRIVATE_ROOT\Source). Pad ports
  27241+n, netplay ports 19913+n (apart from the other tests'), discovery port 19790.
  Nothing personal goes in the report: no addresses, paths or names.
#>
param(
	[string]$GamePath = '',
	[ValidateRange(2, 4)][int]$Players = 2,
	[int]$Seconds = 70,			# the match, from its first frame
	[int]$NetDelay = 0,			# GGPO's simulated network delay (ms each way)
	[int]$Loss = 0,				# % packets lost
	[string]$BuildDir = '',
	[string]$WorkDir = '',
	[string]$OutDir = '',
	[string]$Name = ''
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
if (-not $Name) { $Name = "lobby$($Players)p_d$($NetDelay)_l$($Loss)" }
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
# the route to the game's main menu on player 1's pad (as in netplay-test-tc.ps1), seconds after the first frame
$route = @(
	foreach ($at in 3, 7, 11, 15, 19) { @{ T = $at; B = 'START'; Hold = 0.15 } }
	@{ T = 24; B = 'START'; Hold = 0.15 }
	@{ T = 25.5; B = 'UP'; Hold = 0.15 }
	foreach ($at in 26.5, 27.5, 28.5, 29.5) { @{ T = $at; B = 'RIGHT'; Hold = 0.15 } }
	@{ T = 31; B = 'A'; Hold = 0.15 }
)

# ---- The copies ------------------------------------------------------------------------------
$peers = @(for ($i = 0; $i -lt $Players; $i++) {
	[pscustomobject]@{ Name = "p$($i + 1)"; Index = $i; Port = 27241 + $i; GgpoPort = 19913 + $i }
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
	if ($sync) { $env:HOTD2VR_SYNCLOG = Join-Path $peer.Dir 'sync.txt' } else { Remove-Item Env:HOTD2VR_SYNCLOG -ErrorAction SilentlyContinue }
	if ($NetDelay) { $env:GGPO_NETWORK_DELAY = $NetDelay }
	if ($Loss) { $env:GGPO_PACKET_LOSS = $Loss }
	$cfg = "$base,config:vr.UdpPort=$($peer.Port)"
	$peer.Proc = Start-Process -FilePath (Join-Path $peer.Dir 'flycast.exe') `
		-ArgumentList @('-config', $cfg, "`"$GamePath`"") -WorkingDirectory $peer.Dir -PassThru
	foreach ($k in 'TCVR_MP_PORT', 'HOTD2VR_SYNCLOG', 'GGPO_NETWORK_DELAY', 'GGPO_PACKET_LOSS') { Remove-Item "Env:$k" -ErrorAction SilentlyContinue }
}
function Stop-Copy($peer) {
	if ($peer.Proc -and -not $peer.Proc.HasExited) { Stop-Process -Id $peer.Proc.Id -Force }
	if ($peer.Proc) { $peer.Proc.WaitForExit(5000) | Out-Null }
}

# "MP <command>" to a copy's pad port, the answer text ("" when none within the time)
$cmdUdp = New-Object System.Net.Sockets.UdpClient 0
function Mp($peer, [string]$cmd, [int]$timeoutMs = 8000) {
	$cmdUdp.Client.ReceiveTimeout = $timeoutMs
	$m = [Text.Encoding]::ASCII.GetBytes("MP $cmd")
	[void]$cmdUdp.Send($m, $m.Length, '127.0.0.1', $peer.Port)
	$ep = New-Object System.Net.IPEndPoint([System.Net.IPAddress]::Any, 0)
	try { return [Text.Encoding]::ASCII.GetString($cmdUdp.Receive([ref]$ep)) } catch { return '' }
}
function Wait-Mp($peer, [string]$cmd, [scriptblock]$ok, [int]$seconds) {
	$end = (Get-Date).AddSeconds($seconds)
	$r = ''
	while ((Get-Date) -lt $end) {
		$r = Mp $peer $cmd 1500
		if ($r -and (& $ok $r)) { return $r }
		Start-Sleep -Milliseconds 300
	}
	return $r
}
function Cfg-Text($peer) {
	$f = Get-ChildItem (Join-Path $peer.Dir 'data') -Filter emu.cfg -Recurse -ErrorAction SilentlyContinue | Select-Object -First 1
	if ($f) { return [IO.File]::ReadAllText($f.FullName) } else { return '' }
}
function Log-Text($peer) {
	$f = Get-ChildItem $peer.Dir -Filter *.log -Recurse -ErrorAction SilentlyContinue | Sort-Object LastWriteTime | Select-Object -Last 1
	if ($f) { try { return [IO.File]::ReadAllText($f.FullName) } catch { return '' } } else { return '' }
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
	$r = Mp $peers[0] 'HOST P1 1 2 20'
	Check ($r -like 'MPR lobby players=1 slot=0*') "p1 creates a game: $r"
	$findTimes = @()
	for ($i = 1; $i -lt $Players; $i++) {
		$sw = [Diagnostics.Stopwatch]::StartNew()
		$r = Mp $peers[$i] ("JOIN P{0}" -f ($i + 1)) 12000
		$sw.Stop()
		Check ($r -like "MPR lobby*slot=$i *") ("{0} finds the game and joins it as slot {1}, in {2:0.0} s: {3}" -f $peers[$i].Name, $i, $sw.Elapsed.TotalSeconds, $r)
		$findTimes += $sw.Elapsed.TotalSeconds
	}
	Check (($findTimes | Measure-Object -Maximum).Maximum -lt 3.5) ("every joiner found and joined within 3.5 s (slowest {0:0.0} s)" -f ($findTimes | Measure-Object -Maximum).Maximum)
	$r = Wait-Mp $peers[0] 'STATE' { param($x) $x -like "MPR lobby players=$Players *" } 10
	Check ($r -like "MPR lobby players=$Players *") "the host sees all $Players players: $r"
	Check ($r -like '*mode=1 room=2 points=20*') 'the host settings (mode 1, room 2, 20 points) are in the lobby'
	$r = Mp $peers[1] 'STATE'
	Check ($r -like '*mode=1 room=2 points=20*') "a joiner sees the host's settings: $r"
	$r = Mp $peers[0] 'START 2'
	Check ($r -like 'MPR error*') "Start is refused while the joiners are not ready: $r"
	for ($i = 1; $i -lt $Players; $i++) { [void](Mp $peers[$i] 'READY 1') }
	$r = Wait-Mp $peers[0] 'STATE' { param($x) $x -like 'MPR lobby*ready=1111*' -or ($Players -eq 3 -and $x -like 'MPR lobby*ready=111-*') -or ($Players -eq 2 -and $x -like 'MPR lobby*ready=11--*') } 10
	Check ($r -like '*ready=11*') "the ready marks reach the host: $r"

	# ---- 3. start: options written, restart asked
	$r = Mp $peers[0] 'START 2'
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
	while ($true) {
		foreach ($peer in $peers) { if ($peer.Proc.HasExited) { throw "$($peer.Name) closed during the match (code $($peer.Proc.ExitCode))" } }
		if ($null -eq $t0) {
			$f = Join-Path $peers[0].Dir 'sync.txt'
			if ((Test-Path $f) -and (Get-Item $f).Length -gt 0) { $t0 = Get-Date }
			elseif (((Get-Date) - $launch).TotalSeconds -gt 120) { throw 'the copies did not start a netplay session within 120 s' }
			Start-Sleep -Milliseconds 100
			continue
		}
		$t = ((Get-Date) - $t0).TotalSeconds
		if ($t -ge $Seconds) { break }
		foreach ($peer in $peers) {
			$state = @(0, 0, 0, 0, 0)
			if ($peer.Index -eq 0) {
				foreach ($m in $route) { if ($t -ge $m.T -and $t -lt $m.T + $m.Hold) { $state = @($BTN[$m.B], 0, 0, 0, 0) } }
			}
			$msg = [Text.Encoding]::ASCII.GetBytes(("PAD 0 {0} {1} {2} {3} {4}" -f $state))
			[void]$udp.Send($msg, $msg.Length, '127.0.0.1', $peer.Port)
		}
		Start-Sleep -Milliseconds 30
	}
	$udp.Close()

	# the screens: the last picture of each copy
	$shotDir = Join-Path $WorkDir 'shots'
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
			foreach ($line in [IO.File]::ReadLines($file)) {
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
	Check ($null -eq $firstDiff) $(if ($null -eq $firstDiff) { "IDENTICAL on $Players copies for $($common.Count) confirmed frames (main RAM, video/sound RAM)" } else { "FIRST DIFFERENCE at $firstDiff" })

	# ---- 6. leave the match
	foreach ($peer in $peers) {
		$r = Mp $peer 'LEAVEMATCH' 5000
		Check ($r -like 'MPR left*') "$($peer.Name) leaves the match"
	}
	Start-Sleep -Seconds 1
	foreach ($peer in $peers) {
		$cfg = Cfg-Text $peer
		Check ($cfg -notmatch 'GGPOPeers\s*=\s*\S') "$($peer.Name): netplay options cleared in emu.cfg"
		Check ((Log-Text $peer) -match 'MP RESTART') "$($peer.Name): asked for a restart"
	}
	foreach ($peer in $peers) { Stop-Copy $peer }
	Start-Sleep -Seconds 2
	foreach ($peer in $peers) { Start-Copy $peer $false; Start-Sleep -Seconds 1 }
	foreach ($peer in $peers) {
		$r = Wait-Mp $peer 'STATE' { param($x) $x -like 'MPR idle*' } 120
		Check ($r -like 'MPR idle*match=0*') "$($peer.Name): the game is single player again: $r"
		$r = Mp $peer 'RETURN'
		Check ($r -like '*return=1*') "$($peer.Name): the menu would open on the Multiplayer page (match over mark): $r"
		$r = Mp $peer 'RETURN'
		Check ($r -like '*return=0*') "$($peer.Name): the mark is given once"
	}
} catch {
	Check $false ("the test stopped: " + $_.Exception.Message)
} finally {
	foreach ($peer in $peers) { try { Stop-Copy $peer } catch { } }
	$cmdUdp.Close()
	Say ("{0} failed check(s)" -f $script:failures)
	$report | Set-Content -Path (Join-Path $OutDir 'summary.txt') -Encoding utf8
	foreach ($peer in $peers) {
		$f = Get-ChildItem $peer.Dir -Filter *.log -Recurse -ErrorAction SilentlyContinue | Sort-Object LastWriteTime | Select-Object -Last 1
		if ($f) { Copy-Item $f.FullName (Join-Path $OutDir "$($peer.Name).log") -ErrorAction SilentlyContinue }
	}
	if (Test-Path (Join-Path $WorkDir 'shots')) { Copy-Item (Join-Path $WorkDir 'shots\*') $OutDir -ErrorAction SilentlyContinue }
}
exit $(if ($script:failures -eq 0) { 0 } else { 1 })
