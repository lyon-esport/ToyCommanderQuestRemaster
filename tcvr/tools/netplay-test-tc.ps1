<#
  Toy Commander netplay sync test on one PC (tcvr, multiplayer phase 0, task 0.1).

  Two copies of the Windows build play a 2-player battle together over the fork's GGPO
  netplay on localhost (host = player 1, guest = player 2). Each copy is driven by a scripted
  Dreamcast pad over UDP ("PAD" datagrams, core/input/udp_lightgun.cpp): the host goes through
  the menus into a multiplayer arena, the guest joins on the setup page, then both drive,
  shoot, die and respawn on a fixed pattern. At the end each side's per-frame memory hash
  (main, video and sound RAM, HOTD2VR_SYNCLOG in core/network/ggpo.cpp) is compared: either
  "identical for N frames" or the first frame that differs, with -Pages the 64 KB pages of
  main RAM that differ first.

  Adapted from hotd2-vr/netplay-test.ps1 (HOTD2, light guns). The head camera and the 60
  pictures/s option are off on both copies (vr.HeadCamera=0, vr.Game60=no), as in netplay.

  .\tcvr\tools\netplay-test-tc.ps1                         clean network, 240 s
  .\tcvr\tools\netplay-test-tc.ps1 -NetDelay 80 -Loss 2    80 ms each way (jitter), 2% lost
  .\tcvr\tools\netplay-test-tc.ps1 -Pages                  per-page hashes (sync.txt.pages)
  .\tcvr\tools\netplay-test-tc.ps1 -Local 4                no network: one copy, 4 local pads
                                                           (the 4-player check), shots only

  Paths: the game image is taken from -GamePath or the first .chd/.gdi/.cue in
  $env:TCVR_PRIVATE_ROOT\Source (default: the folder above this repository). Each copy runs
  from its own folder under -WorkDir (default $env:TEMP\tcvr-netplay; an ASCII path, as the
  sync log name goes through getenv/fopen). Logs, shots and a summary go to -OutDir (default
  <private root>\multiplayer\phase0\<timestamp>_<run name>), outside git.
  Needs build-win\flycast.exe (hotd2-vr\build-win.cmd).
#>
param(
	[string]$GamePath = '',
	[int]$Seconds = 240,
	[int]$Delay = 0,		# GGPO input delay (frames)
	[int]$NetDelay = 0,		# GGPO's simulated network delay (ms each way)
	[int]$Loss = 0,			# % packets lost
	[int]$Oop = 0,			# % packets out of order
	[int]$StallGuest = 0,	# the guest's render loop stops this many ms every 20 s
	[switch]$Pages,
	[string]$Dump = '',		# frames to write state + RAM of, e.g. '1200,5000'
	[ValidateRange(0, 4)][int]$Local = 0,	# >0: no netplay, one copy with this many pads
	[string]$BuildDir = '',	# folder with flycast.exe (default $env:TCVR_BUILD_WIN, else build-win here)
	[string]$WorkDir = '',
	[string]$OutDir = '',
	[string]$Name = ''
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$private = if ($env:TCVR_PRIVATE_ROOT) { $env:TCVR_PRIVATE_ROOT } else { Split-Path -Parent $repo }
$build = if ($BuildDir) { $BuildDir } elseif ($env:TCVR_BUILD_WIN) { $env:TCVR_BUILD_WIN } else { Join-Path $repo 'build-win' }
if (-not (Test-Path (Join-Path $build 'flycast.exe'))) { throw "No flycast.exe in $build (hotd2-vr\build-win.cmd, or give -BuildDir)" }
if (-not $GamePath) {
	$GamePath = (Get-ChildItem (Join-Path $private 'Source') -Include *.chd, *.gdi, *.cue -Recurse -ErrorAction SilentlyContinue |
		Select-Object -First 1).FullName
}
if (-not $GamePath -or -not (Test-Path $GamePath)) { throw 'No Toy Commander image (give -GamePath)' }
if (-not $WorkDir) { $WorkDir = Join-Path $env:TEMP 'tcvr-netplay' }
if (-not $Name) {
	$Name = if ($Local) { "local$($Local)p" } else { "net_d$($NetDelay)_l$($Loss)" + $(if ($StallGuest) { "_stall$StallGuest" } else { '' }) }
}
if (-not $OutDir) { $OutDir = Join-Path $private ("multiplayer\phase0\{0}_{1}" -f (Get-Date -Format 'yyyyMMdd_HHmmss'), $Name) }
$shotDir = Join-Path $WorkDir "shots"
if (Test-Path $shotDir) { Remove-Item $shotDir -Recurse -Force }
New-Item -ItemType Directory -Force $WorkDir, $OutDir, $shotDir | Out-Null

# Dreamcast pad bits (core/input/gamepad.h)
$BTN = @{ B = 2; A = 4; START = 8; UP = 0x10; DOWN = 0x20; LEFT = 0x40; RIGHT = 0x80; Y = 0x200; X = 0x400 }

# ---- The route: menu presses, in seconds after the first emulated frame ----------------------
# Measured on PC from a blank memory card (both copies start with none): Start skips the logos,
# the intro film and the title, picks the memory card and "New Game"; on the name entry Start
# types an A, Up then Right x4 reach END, A confirms; on the main menu Down = Multiplayer. On
# the setup page player 2 joins with Start, both are ready with A; the options page (room:
# Kitchen and dining room, mode: Deathmatch, 10 points: the defaults) starts with A.
$menu = @(
	foreach ($at in 3, 7, 11, 15, 19) { @{ T = $at; P = 0; B = 'START'; Hold = 0.15 } }	# logos .. "New Game"
	@{ T = 24; P = 0; B = 'START'; Hold = 0.15 }		# name entry: "A"
	@{ T = 25.5; P = 0; B = 'UP'; Hold = 0.15 }		# to the bottom row
	foreach ($at in 26.5, 27.5, 28.5, 29.5) { @{ T = $at; P = 0; B = 'RIGHT'; Hold = 0.15 } }	# to END
	@{ T = 31; P = 0; B = 'A'; Hold = 0.15 }			# END: saves, main menu
	@{ T = 38; P = 0; B = 'DOWN'; Hold = 0.15 }		# Multiplayer
	@{ T = 40; P = 0; B = 'A'; Hold = 0.15 }			# setup page
	@{ T = 44; P = 1; B = 'START'; Hold = 0.15 }		# player 2 joins
	@{ T = 47; P = 0; B = 'A'; Hold = 0.15 }			# player 1 ready
	@{ T = 49; P = 1; B = 'A'; Hold = 0.15 }			# player 2 ready: options page
	@{ T = 54; P = 0; B = 'A'; Hold = 0.15 }			# start the battle
)
$playFrom = 64
# extra Starts for players 3 and 4 in the local 4-player check
if ($Local -ge 3) { $menu += @{ T = 45; P = 2; B = 'START'; Hold = 0.15 }; $menu += @{ T = 48; P = 2; B = 'A'; Hold = 0.15 } }
if ($Local -ge 4) { $menu += @{ T = 46; P = 3; B = 'START'; Hold = 0.15 }; $menu += @{ T = 48.5; P = 3; B = 'A'; Hold = 0.15 } }
$shots = @(23, 36, 43, 46, 52, 62, 75, 100, 130, 160, 200, 230, 280, 340, 380, 420, 445) | Where-Object { $_ -lt $Seconds }

# What a pad does at time t (s) during the battle: a fixed pattern per player (no randomness,
# the same on every run): accelerate, steer on a slow wave, bursts of fire, a special weapon now
# and then. Battles end by deaths on their own; the player respawns by holding A.
function Play-State([int]$p, [double]$t) {
	$u = $t - $playFrom + 3.1 * $p
	$x = [int](110 * [math]::Sin($u * 0.7 + $p))
	$y = [int](50 * [math]::Sin($u * 0.37 + 2 * $p))
	$b = 0
	if ((($u * 1000) % 400) -lt 180) { $b = $b -bor $BTN.A }
	if ((($u) % 7) -gt 6.6) { $b = $b -bor $BTN.B }
	$r = if (($u % 11) -lt 9) { 255 } else { 0 }
	$l = if (($u % 11) -ge 9.5) { 255 } else { 0 }
	return @($b, $x, $y, $l, $r)
}

function Pad-Message([int]$p, [double]$t) {
	$state = @(0, 0, 0, 0, 0)
	foreach ($m in $menu) {
		if ($m.P -eq $p -and $t -ge $m.T -and $t -lt $m.T + $m.Hold) {
			$state = @(($BTN[$m.B] -as [int]), [int]$m.X, [int]$m.Y, 0, 0)
		}
	}
	if ($t -ge $playFrom) { $state = Play-State $p $t }
	return "PAD 0 {0} {1} {2} {3} {4}" -f $state
}

# ---- The copies ------------------------------------------------------------------------------
# Both copies: the same build and saves (none: a blank VMU on each, as two fresh headsets).
$peers = if ($Local) {
	@([pscustomobject]@{ Name = 'local'; Port = 27041; Net = 'network:GGPO=no' })
} else {
	@(
		[pscustomobject]@{ Name = 'host';  Port = 27041; Net = 'network:GGPO=yes,network:ActAsServer=yes' },
		[pscustomobject]@{ Name = 'guest'; Port = 27042; Net = 'network:GGPO=yes,network:ActAsServer=no,network:server=127.0.0.1' }
	)
}
foreach ($peer in $peers) {
	$dir = Join-Path $WorkDir $peer.Name
	if (Test-Path $dir) { Remove-Item $dir -Recurse -Force }
	New-Item -ItemType Directory -Force (Join-Path $dir 'data') | Out-Null
	Copy-Item (Join-Path $build 'flycast.exe') $dir
	Get-ChildItem $build -Filter *.dll -ErrorAction SilentlyContinue | Copy-Item -Destination $dir
	$peer | Add-Member Dir $dir
}

# OpenGL with the VR reprojection's path (as on the Quest), Dreamcast pads in A..D (4 only for
# -Local 4), the analog stick sent over GGPO (2 axes), no head camera, 30 pictures/s.
$pads = if ($Local) { $Local } else { 2 }
$base = 'config:pvr.rend=0,config:rend.NativeDepthInterpolation=yes,config:aica.Volume=0,log:LogToFile=yes' +
	',config:vr.Reproject=yes,config:vr.HeadCamera=0,config:vr.Game60=no,network:EnableUPnP=no' +
	",network:GGPODelay=$Delay,network:GGPOAnalogAxes=2"
for ($i = 1; $i -le 4; $i++) { $base += ",input:device$i=" + $(if ($i -le $pads) { 0 } else { 10 }) }
$netEnv = @{ GGPO_NETWORK_DELAY = $NetDelay; GGPO_PACKET_LOSS = $Loss; GGPO_OOP_PERCENT = $Oop }
foreach ($k in $netEnv.Keys) { if ($netEnv[$k]) { Set-Item "Env:$k" $netEnv[$k] } }
if ($Dump) { $env:HOTD2VR_SYNCDUMP = $Dump }
if ($Pages) { $env:HOTD2VR_SYNCPAGES = '1' }
foreach ($peer in $peers) {
	if (-not $Local) { $env:HOTD2VR_SYNCLOG = Join-Path $peer.Dir 'sync.txt' }
	if ($StallGuest -and $peer.Name -eq 'guest') { $env:HOTD2VR_STALLRENDER = $StallGuest } else { Remove-Item Env:HOTD2VR_STALLRENDER -ErrorAction SilentlyContinue }
	$cfg = "$base,config:vr.UdpPort=$($peer.Port),$($peer.Net)"
	$peer | Add-Member Proc (Start-Process -FilePath (Join-Path $peer.Dir 'flycast.exe') `
		-ArgumentList @('-config', $cfg, "`"$GamePath`"") -WorkingDirectory $peer.Dir -PassThru)
	Start-Sleep -Seconds 1
}
foreach ($k in @('HOTD2VR_SYNCLOG', 'HOTD2VR_SYNCDUMP', 'HOTD2VR_SYNCPAGES', 'HOTD2VR_STALLRENDER') + @($netEnv.Keys)) { Remove-Item "Env:$k" -ErrorAction SilentlyContinue }

# ---- Drive the pads --------------------------------------------------------------------------
# t = 0 when the game starts running: in netplay the host's first logged frame (after the two
# copies found each other), alone at the launch (the times above were measured so).
$udp = New-Object System.Net.Sockets.UdpClient
$launch = Get-Date
$t0 = $null
$shotQueue = [System.Collections.Generic.Queue[double]]::new()
foreach ($s in ($shots | Sort-Object)) { $shotQueue.Enqueue($s) }
try {
	while ($true) {
		foreach ($peer in $peers) { if ($peer.Proc.HasExited) { throw "$($peer.Name) closed early (code $($peer.Proc.ExitCode))" } }
		if ($null -eq $t0) {
			$ready = if ($Local) { ((Get-Date) - $launch).TotalSeconds -ge 0.5 } else {
				$f = Join-Path $peers[0].Dir 'sync.txt'; (Test-Path $f) -and (Get-Item $f).Length -gt 0 }
			if ($ready) { $t0 = Get-Date } elseif (((Get-Date) - $launch).TotalSeconds -gt 90) { throw 'the copies did not start a session within 90 s' }
			Start-Sleep -Milliseconds 100
			continue
		}
		$t = ((Get-Date) - $t0).TotalSeconds
		if ($t -ge $Seconds) { break }
		for ($p = 0; $p -lt $pads; $p++) {
			if ($Local) { $text = (Pad-Message $p $t) -replace '^PAD 0', "PAD $p"; $port = $peers[0].Port }
			else { $text = Pad-Message $p $t; $port = $peers[$p].Port }
			$msg = [Text.Encoding]::ASCII.GetBytes($text)
			[void]$udp.Send($msg, $msg.Length, '127.0.0.1', $port)
		}
		if ($shotQueue.Count -gt 0 -and $t -ge $shotQueue.Peek()) {
			$at = $shotQueue.Dequeue()
			# the emulator saves its last frame itself ("SHOT"): works with the desktop locked
			foreach ($peer in $peers) {
				$msg = [Text.Encoding]::ASCII.GetBytes(("SHOT {0}" -f (Join-Path $shotDir ("{0}_{1:000}.png" -f $peer.Name, $at))))
				[void]$udp.Send($msg, $msg.Length, '127.0.0.1', $peer.Port)
			}
		}
		Start-Sleep -Milliseconds 30
	}
} finally {
	$udp.Close()
	foreach ($peer in $peers) { if (-not $peer.Proc.HasExited) { Stop-Process -Id $peer.Proc.Id -Force } }
}
Start-Sleep -Milliseconds 800

# ---- Compare ---------------------------------------------------------------------------------
$report = [System.Collections.Generic.List[string]]::new()
function Say([string]$s) { $report.Add($s); Write-Output $s }
Say ("run {0}: {1} s, GGPO delay {2}, network delay {3} ms, loss {4}%, out of order {5}%, guest stall {6} ms" -f `
	$Name, $Seconds, $Delay, $NetDelay, $Loss, $Oop, $StallGuest)
if (-not $Local) {
	$logs = @{}
	foreach ($peer in $peers) {
		$hash = @{}
		$rollbacks = 0; $resim = 0; $deepest = 0; $prev = -1; $first = $null; $last = $null
		$file = Join-Path $peer.Dir 'sync.txt'
		if (Test-Path $file) {
			foreach ($line in [IO.File]::ReadLines($file)) {
				$f = $line.Split(' ')
				if ($f.Count -lt 5) { continue }
				$frame = [int]$f[0]
				if ($frame -le $prev) { $rollbacks++; $d = $prev - $frame + 1; $resim += $d; if ($d -gt $deepest) { $deepest = $d } }
				$prev = $frame
				$hash[$frame] = @($f[2], $f[4], $f[1])	# main RAM, video+sound RAM, saved state
				if ($null -eq $first) { $first = @($frame, [long]$f[3]) }
				$last = @($frame, [long]$f[3])
			}
		}
		$logs[$peer.Name] = $hash
		$fps = if ($first -and $last -and $last[1] -gt $first[1]) { ($last[0] - $first[0]) * 1000.0 / ($last[1] - $first[1]) } else { 0 }
		Say ("{0}: {1} frames, {2:0.0} fps; {3} rollbacks, at most {4} frames deep; {5:0.0}% of frames simulated again" -f `
			$peer.Name, $hash.Count, $fps, $rollbacks, $deepest, (100.0 * $resim / [math]::Max(1, $hash.Count)))
	}
	# the last frames may still be predictions: only frames GGPO can no longer roll back count
	$upTo = [math]::Min(($logs['host'].Keys | Measure-Object -Maximum).Maximum, ($logs['guest'].Keys | Measure-Object -Maximum).Maximum) - 10
	$common = @($logs['host'].Keys | Where-Object { $_ -le $upTo -and $logs['guest'].ContainsKey($_) } | Sort-Object)
	# The verdict is on main RAM and video + sound RAM. The saved-state hash is only reported: the
	# rollback state holds a few host pointers (SH-4 part; seen at frames 600 and 4000 on
	# 2026-11-18: 8-byte values like 0x00007FF6xxxxxxxx), which differ between two processes.
	$names = @('main RAM', 'video/sound RAM')
	$firstDiff = $null
	$diffCount = 0
	$stateDiff = 0
	foreach ($fr in $common) {
		$a = $logs['host'][$fr]; $b = $logs['guest'][$fr]
		if ($a[2] -ne $b[2]) { $stateDiff++ }
		$which = @(for ($i = 0; $i -lt 2; $i++) { if ($a[$i] -ne $b[$i]) { $names[$i] } })
		if ($which.Count) {
			$diffCount++
			if ($null -eq $firstDiff) { $firstDiff = $fr; Say ("FIRST DIFFERENCE at frame {0} ({1:0.0} s of game): {2}" -f $fr, ($fr / 60.0), ($which -join ' + ')) }
		}
	}
	Say ("saved-state hash (host pointers included, informative only) differs on {0} of {1} frames" -f $stateDiff, $common.Count)
	if ($null -eq $firstDiff) {
		Say ("IDENTICAL for {0} confirmed frames (frames {1}..{2}, {3:0.0} s of game): main RAM and video/sound RAM" -f `
			$common.Count, $common[0], $common[-1], ($common.Count / 60.0))
	} else {
		Say ("frames compared: {0}, frames that differ: {1}" -f $common.Count, $diffCount)
		if ($Pages) {
			# which 64 KB pages of main RAM (0x8C000000 + page * 0x10000) differ first
			$pg = @{}
			foreach ($peer in $peers) {
				$m = @{}
				$file = Join-Path $peer.Dir 'sync.txt.pages'
				if (Test-Path $file) { foreach ($line in [IO.File]::ReadLines($file)) { $f = $line.Split(' '); $m[[int]$f[0]] = $f } }
				$pg[$peer.Name] = $m
			}
			foreach ($fr in ($common | Where-Object { $_ -ge $firstDiff } | Select-Object -First 600)) {
				$a = $pg['host'][$fr]; $b = $pg['guest'][$fr]
				if ($null -eq $a -or $null -eq $b) { continue }
				$bad = @(for ($i = 1; $i -lt [math]::Min($a.Count, $b.Count); $i++) { if ($a[$i] -ne $b[$i]) { '0x{0:X8}' -f (0x8C000000 + ($i - 1) * 0x10000) } })
				if ($bad.Count) { Say ("frame {0}: 64 KB pages that differ: {1}" -f $fr, ($bad -join ' ')); break }
			}
		}
	}
}
foreach ($peer in $peers) {
	$log = Join-Path $peer.Dir 'flycast.log'
	if (Test-Path $log) {
		Copy-Item $log (Join-Path $OutDir "$($peer.Name)_flycast.log")
		Select-String -Path $log -Pattern 'GGPO stats|GGPO error|stopped|Exception|disconnect' | Select-Object -Last 3 |
			ForEach-Object { Say ("$($peer.Name) log: " + ($_.Line -replace '^\S+ \S+ ', '')) }
	}
	foreach ($f in 'sync.txt', 'sync.txt.pages') {
		$src = Join-Path $peer.Dir $f
		if (Test-Path $src) { Copy-Item $src (Join-Path $OutDir "$($peer.Name)_$f") }
	}
}
Get-ChildItem $shotDir -Filter *.png -ErrorAction SilentlyContinue | Copy-Item -Destination $OutDir
$report | Set-Content -Encoding utf8 (Join-Path $OutDir 'summary.txt')
Write-Output "results: $OutDir"
