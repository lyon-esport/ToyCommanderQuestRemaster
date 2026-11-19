<#
  Toy Commander netplay sync test on one PC (tcvr, multiplayer phase 0, task 0.1).

  Two to four copies of the Windows build play a battle together over the fork's GGPO
  netplay on localhost (copy p1 = player 1, p2 = player 2, ...). Two copies use the classic
  two-player setup (ActAsServer + server) unless -List; three or four use the player list
  (network:GGPOPeers + network:GGPOPlayer, every copy linked to every other). Each copy is driven by a scripted
  Dreamcast pad over UDP ("PAD" datagrams, core/input/udp_lightgun.cpp): the host goes through
  the menus into a multiplayer arena, the others join on the setup page, then all drive,
  shoot, die and respawn on a fixed pattern. At the end each side's per-frame memory hash
  (main, video and sound RAM, HOTD2VR_SYNCLOG in core/network/ggpo.cpp) is compared: either
  "identical for N frames" or the first frame that differs, with -Pages the 64 KB pages of
  main RAM that differ first.

  Adapted from hotd2-vr/netplay-test.ps1 (HOTD2, light guns). The head camera and the 60
  pictures/s option are off on both copies (vr.HeadCamera=0, vr.Game60=no), as in netplay.

  .\tcvr\tools\netplay-test-tc.ps1                         clean network, 240 s
  .\tcvr\tools\netplay-test-tc.ps1 -NetDelay 80 -Loss 2    80 ms each way (jitter), 2% lost
  .\tcvr\tools\netplay-test-tc.ps1 -Pages                  per-page hashes (sync.txt.pages)
  .\tcvr\tools\netplay-test-tc.ps1 -Players 4 -Seconds 670 four copies, 10 min of battle
  .\tcvr\tools\netplay-test-tc.ps1 -Local 4                no network: one copy, 4 local pads
                                                           (the 4-player check), shots only
  .\tcvr\tools\netplay-test-tc.ps1 -Players 4 -KillPlayer 3 -KillAt 120 -Seconds 440
                                                           player 3's copy is killed (its process ended) 120 s
                                                           into the game: the other copies must go on together
                                                           (task 1.4); they are compared with each other

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
	[int]$StallGuest = 0,	# the last copy's render loop stops this many ms every 20 s
	[switch]$Pages,
	[string]$Dump = '',		# frames to write state + RAM of, e.g. '1200,5000'
	[ValidateRange(0, 4)][int]$Local = 0,	# >0: no netplay, one copy with this many pads
	[ValidateRange(2, 4)][int]$Players = 2,	# netplay: this many copies, one player each
	[switch]$List,			# 2 players: the player list (GGPOPeers) rather than ActAsServer + server
	[ValidateSet('wave', 'hunt')][string]$Pattern = 'wave',	# hunt: P1 chases and shoots P2 (kills, respawns)
	[ValidateRange(0, 2)][int]$Mode = 0,	# 0 Deathmatch, 1 Cat and Mice, 2 Capture the Flag
	[switch]$Ram,			# with -Pattern hunt: no passes, the hunter flies straight at the target (collisions)
	[switch]$TargetStill,	# with -Pattern hunt: the target never takes off (pad at rest, A now and then to respawn): an easy target (task 1.5)
	[string]$BuildDir = '',	# folder with flycast.exe (default $env:TCVR_BUILD_WIN, else build-win here)
	[string]$WorkDir = '',
	[string]$OutDir = '',
	[string]$Name = '',
	[int]$KillPlayer = 0,	# >0: this player's copy is killed at -KillAt (a player who leaves or drops, task 1.4)
	[double]$KillAt = 0,	# seconds of game (as -Seconds) when it is killed
	[int]$PortOffset = 0	# added to the pad listener ports (27041+) and, with the player list, the GGPO ports (19713+): two tests on one PC at once (use -List for 2 copies)
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
	$Name = if ($Local) { "local$($Local)p" } else { "net$($Players)p" + $(if ($List -and $Players -eq 2) { 'list' } else { '' }) + "_d$($NetDelay)_l$($Loss)" + $(if ($StallGuest) { "_stall$StallGuest" } else { '' }) + $(if ($KillPlayer) { "_kill$KillPlayer" } else { '' }) }
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
	if ($Mode) {
		@{ T = 51.5; P = 0; B = 'DOWN'; Hold = 0.15 }	# options: the Mode line
		for ($i = 0; $i -lt $Mode; $i++) { @{ T = 52.3 + 0.7 * $i; P = 0; B = 'RIGHT'; Hold = 0.15 } }
	}
	@{ T = 54; P = 0; B = 'A'; Hold = 0.15 }			# start the battle
)
$playFrom = 64
# extra Starts for players 3 and 4 (4 local pads, or 3-4 copies)
$pads = if ($Local) { $Local } else { $Players }
if ($pads -ge 3) { $menu += @{ T = 45; P = 2; B = 'START'; Hold = 0.15 }; $menu += @{ T = 48; P = 2; B = 'A'; Hold = 0.15 } }
if ($pads -ge 4) { $menu += @{ T = 46; P = 3; B = 'START'; Hold = 0.15 }; $menu += @{ T = 48.5; P = 3; B = 'A'; Hold = 0.15 } }
$shots = @(23, 36, 43, 46, 52, 62, 75, 100, 130, 160, 200, 230, 280, 340, 380, 420, 445) + @(for ($s = 500; $s -lt $Seconds; $s += 60) { $s }) |
	Where-Object { $_ -lt $Seconds }

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

# -Pattern hunt: P1 hunts P2. Both vehicles' positions come from the host copy's RAM ("PEEK", the
# player record +0x84 -> vehicle, position at vehicle +0x24, 3 floats, z up). The hunter's heading is
# taken from its movement over 0.3 s; stick x + turns right (heading down), stick y + climbs
# (measured 2026-11-18). It flies at P2, fires A when lined up and close, a missile (B) now and
# then. P2 flies full throttle nose up into the ceiling, so it dies again and again, and presses A
# every 2 s (fires, and "Ready" to respawn after a death); a player
# that has not moved for 2 s presses A too (the respawn screen's "Ready").
$peekUdp = New-Object System.Net.Sockets.UdpClient 0
$peekUdp.Client.ReceiveTimeout = 300
function Peek([int64]$addr, [int]$len) {
	$m = [Text.Encoding]::ASCII.GetBytes(('PEEK {0:X8} {1}' -f $addr, $len))
	[void]$peekUdp.Send($m, $m.Length, '127.0.0.1', $peers[0].Port)
	$ep = New-Object System.Net.IPEndPoint([System.Net.IPAddress]::Any, 0)
	try { $f = [Text.Encoding]::ASCII.GetString($peekUdp.Receive([ref]$ep)).Split(' ') } catch { return $null }
	if ($f.Count -lt 3 -or $f[2] -eq 'ERR') { return $null }
	$b = [byte[]]::new($f[2].Length / 2)
	for ($i = 0; $i -lt $b.Length; $i++) { $b[$i] = [Convert]::ToByte($f[2].Substring($i * 2, 2), 16) }
	return ,$b
}
function Vehicle-Pos([int]$p) {
	$b = Peek (0x8C0D5904L + $p * 0x26C + 0x84) 4
	if ($null -eq $b) { return $null }
	$ptr = [BitConverter]::ToUInt32($b, 0)
	if ($ptr -lt 0x8C000000L -or $ptr -ge 0x8D000000L) { return $null }
	# a new vehicle after a death: count the respawns
	if ($hunt.Vehicle.ContainsKey($p) -and $hunt.Vehicle[$p] -ne $ptr) { $hunt.Respawns[$p]++; $hunt.Log.Add(("{0:0.0} P{1} respawned (vehicle {2:X8})" -f ((Get-Date) - $t0).TotalSeconds, ($p + 1), $ptr)) }
	$hunt.Vehicle[$p] = $ptr
	$b = Peek ([int64]$ptr + 0x24) 12
	if ($null -eq $b) { return $null }
	return ,@([BitConverter]::ToSingle($b, 0), [BitConverter]::ToSingle($b, 4), [BitConverter]::ToSingle($b, 8))
}
$hunt = @{ Hist = @{ 0 = [System.Collections.Generic.List[object]]::new(); 1 = [System.Collections.Generic.List[object]]::new() }
	Vehicle = @{}; Respawns = @{ 0 = 0; 1 = 0 }; Still = @{}; Extend = $false; ExtendAt = 0.0; NextLog = 0.0; Log = [System.Collections.Generic.List[string]]::new() }
function Hunt-State([int]$p, [double]$t) {
	$u = $t - $playFrom
	$me = Vehicle-Pos $p
	$hist = $hunt.Hist[$p]
	$heading = $null; $climb = 0.0; $still = $false
	if ($null -ne $me) {
		$hist.Add(@($t, $me))
		while ($hist.Count -gt 2 -and $t - $hist[1][0] -ge 0.3) { $hist.RemoveAt(0) }
		$old = $hist[0]
		$dx = $me[0] - $old[1][0]; $dy = $me[1] - $old[1][1]; $dz = $me[2] - $old[1][2]
		$run = [math]::Sqrt($dx * $dx + $dy * $dy)
		if ($run -gt 10) {
			$heading = [math]::Atan2($dy, $dx)
			$climb = [math]::Atan2($dz, $run) * 180 / [math]::PI
			$hunt.Still[$p] = $t
		} elseif ($null -eq $hunt.Still[$p]) { $hunt.Still[$p] = $t }
		$still = ($t - $hunt.Still[$p]) -gt 2
	}
	$pulseA = (($u % 2) -lt 0.2)
	# the target: full throttle, stick centred: it takes off, climbs into the ceiling and dies, then
	# respawns with A ("Ready")
	if ($p -ne 0) { return @($(if ($pulseA) { $BTN.A } else { 0 }), 0, 0, 0, $(if ($TargetStill) { 0 } else { 255 })) }
	$it = Vehicle-Pos 1
	if ($null -eq $me -or $null -eq $it -or $null -eq $heading) {
		# (re)start: throttle and Ready; stuck against something for 2 s: reverse, turn and climb in turns
		$state = @($(if ($pulseA) { $BTN.A } else { 0 }), 0, 0, 0, 255)
		if ($still) { $state = if (($u % 4) -lt 1.5) { @($state[0], 127, 100, 255, 0) } else { @($state[0], -60, 60, 0, 255) } }
		if ($u -ge $hunt.NextLog) {
			$hunt.NextLog = $u + 0.5
			$hunt.Log.Add(('{0:0.0} waiting: me {1} target {2} still {3} pad {4}' -f $u, ($me -join ','), ($it -join ','), $still, ($state -join ' ')))
		}
		return $state
	}
	$dx = $it[0] - $me[0]; $dy = $it[1] - $me[1]; $dz = $it[2] - $me[2]
	$dist = [math]::Sqrt($dx * $dx + $dy * $dy)
	$err = [math]::Atan2($dy, $dx) - $heading
	while ($err -gt [math]::PI) { $err -= 2 * [math]::PI }
	while ($err -lt -[math]::PI) { $err += 2 * [math]::PI }
	$deg = $err * 180 / [math]::PI
	# passes: turn in and attack; after passing within 900, fly on straight until 2200 away
	if ($Ram) { $hunt.Extend = $false }
	elseif ($dist -lt 900 -and [math]::Abs($deg) -gt 30 -and -not $hunt.Extend) { $hunt.Extend = $true; $hunt.ExtendAt = $t }
	if ($dist -gt 1800 -or ($hunt.Extend -and $t - $hunt.ExtendAt -gt 4)) { $hunt.Extend = $false }
	$x = if ($hunt.Extend) { 0 } else { [int][math]::Max(-127, [math]::Min(127, -2.5 * $deg)) }
	# pitch toward the slope that points at the target (y + = climb), limited to +-35 degrees and
	# not below the ground: far away keep some height
	$above = if ($dist -lt 2600) { 0 } else { 300 }	# close: straight at it
	$want = [math]::Atan2($dz + $above, [math]::Max(1, $dist)) * 180 / [math]::PI
	$want = [math]::Max(-45, [math]::Min(35, $want))
	if ([math]::Abs($deg) -gt 60 -or $hunt.Extend) { $want = 3 }	# turning back or extending: level
	if (-not $Ram -and $me[2] -lt $it[2] + 120 -and $climb -lt 0) { $want = 15 }	# near the ground: pull up
	$y = [int][math]::Max(-100, [math]::Min(100, 4 * ($want - $climb)))
	$b = 0
	if ([math]::Abs($deg) -lt 15 -and -not $hunt.Extend -and $dist -lt 2600 -and (($u * 1000) % 300) -lt 150) { $b = $BTN.A }
	if ([math]::Abs($deg) -lt 15 -and -not $hunt.Extend -and $dist -lt 3000 -and ($u % 2) -gt 1.7) { $b = $b -bor $BTN.B }
	if ($still -and $pulseA) { $b = $b -bor $BTN.A }
	$r = if ($Ram -or $dist -gt 1000) { 255 } else { 160 }
	if ($u -ge $hunt.NextLog) {
		$hunt.NextLog = $u + 0.5
		$hunt.Log.Add(('{0:0.0} me {1:0} {2:0} {3:0} target {4:0} {5:0} {6:0} dist {7:0} err {8:0} climb {9:0} want {10:0} x {11} y {12} b {13}' -f `
			$u, $me[0], $me[1], $me[2], $it[0], $it[1], $it[2], $dist, $deg, $climb, $want, $x, $y, $b))
	}
	return @($b, $x, $y, 0, $r)
}

function Pad-Message([int]$p, [double]$t) {
	$state = @(0, 0, 0, 0, 0)
	foreach ($m in $menu) {
		if ($m.P -eq $p -and $t -ge $m.T -and $t -lt $m.T + $m.Hold) {
			$state = @(($BTN[$m.B] -as [int]), [int]$m.X, [int]$m.Y, 0, 0)
		}
	}
	if ($t -ge $playFrom) { $state = if ($Pattern -eq 'hunt') { Hunt-State $p $t } else { Play-State $p $t } }
	return "PAD 0 {0} {1} {2} {3} {4}" -f $state
}

# ---- The copies ------------------------------------------------------------------------------
# Both copies: the same build and saves (none: a blank VMU on each, as two fresh headsets).
# Copy p<n> is player n; its pad listener (vr.UdpPort) is 27040 + n. Netplay ports (GGPO):
# classic two players 19713 (host) / 19712 (guest); with the player list 19713 + n - 1.
$peers = if ($Local) {
	@([pscustomobject]@{ Name = 'local'; Port = 27041 + $PortOffset; Net = 'network:GGPO=no' })
} elseif ($Players -eq 2 -and -not $List) {
	@(
		[pscustomobject]@{ Name = 'p1'; Port = 27041 + $PortOffset; Net = 'network:GGPO=yes,network:ActAsServer=yes' },
		[pscustomobject]@{ Name = 'p2'; Port = 27042 + $PortOffset; Net = 'network:GGPO=yes,network:ActAsServer=no,network:server=127.0.0.1' }
	)
} else {
	# (in quotes: -config splits on commas outside them)
	$peerList = (0..($Players - 1) | ForEach-Object { "127.0.0.1:$(19713 + $PortOffset + $_)" }) -join ','
	@(for ($i = 0; $i -lt $Players; $i++) {
		[pscustomobject]@{ Name = "p$($i + 1)"; Port = 27041 + $PortOffset + $i; Net = "network:GGPO=yes,network:GGPOPeers='$peerList',network:GGPOPlayer=$i" }
	})
}
foreach ($peer in $peers) {
	$dir = Join-Path $WorkDir $peer.Name
	if (Test-Path $dir) { Remove-Item $dir -Recurse -Force }
	New-Item -ItemType Directory -Force (Join-Path $dir 'data') | Out-Null
	Copy-Item (Join-Path $build 'flycast.exe') $dir
	Get-ChildItem $build -Filter *.dll -ErrorAction SilentlyContinue | Copy-Item -Destination $dir
	$peer | Add-Member Dir $dir
}

# OpenGL with the VR reprojection's path (as on the Quest), one Dreamcast pad per player in
# A..D (the same on every copy), no head camera, 30 pictures/s. (GGPO sends the whole pad:
# buttons, stick, both triggers.)
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
	if ($StallGuest -and $peer -eq $peers[-1]) { $env:HOTD2VR_STALLRENDER = $StallGuest } else { Remove-Item Env:HOTD2VR_STALLRENDER -ErrorAction SilentlyContinue }
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
$killed = $null		# the killed copy, and the last frame in each other copy's sync log at that moment
$killFrame = @{}
$shotQueue = [System.Collections.Generic.Queue[double]]::new()
foreach ($s in ($shots | Sort-Object)) { $shotQueue.Enqueue($s) }
try {
	while ($true) {
		foreach ($peer in $peers) { if ($peer -ne $killed -and $peer.Proc.HasExited) { throw "$($peer.Name) closed early (code $($peer.Proc.ExitCode))" } }
		if ($null -eq $t0) {
			$ready = if ($Local) { ((Get-Date) - $launch).TotalSeconds -ge 0.5 } else {
				$f = Join-Path $peers[0].Dir 'sync.txt'; (Test-Path $f) -and (Get-Item $f).Length -gt 0 }
			if ($ready) { $t0 = Get-Date } elseif (((Get-Date) - $launch).TotalSeconds -gt 90) { throw 'the copies did not start a session within 90 s' }
			Start-Sleep -Milliseconds 100
			continue
		}
		$t = ((Get-Date) - $t0).TotalSeconds
		if ($t -ge $Seconds) { break }
		if ($KillPlayer -and $null -eq $killed -and $t -ge $KillAt) {
			$killed = $peers[$KillPlayer - 1]
			Stop-Process -Id $killed.Proc.Id -Force
			foreach ($peer in $peers) {
				if ($peer -eq $killed) { continue }
				$last = (Get-Content (Join-Path $peer.Dir 'sync.txt') -Tail 1 -ErrorAction SilentlyContinue)
				$killFrame[$peer.Name] = if ($last) { [int]($last.Split(' ')[0]) } else { 0 }
			}
			Write-Output ("{0:0.0} s: {1} killed (the others were at frame {2})" -f $t, $killed.Name, (($killFrame.Values | Measure-Object -Maximum).Maximum))
		}
		for ($p = 0; $p -lt $pads; $p++) {
			if ($peers[$p] -eq $killed) { continue }
			if ($Local) { $text = (Pad-Message $p $t) -replace '^PAD 0', "PAD $p"; $port = $peers[0].Port }
			else { $text = Pad-Message $p $t; $port = $peers[$p].Port }
			$msg = [Text.Encoding]::ASCII.GetBytes($text)
			[void]$udp.Send($msg, $msg.Length, '127.0.0.1', $port)
		}
		if ($shotQueue.Count -gt 0 -and $t -ge $shotQueue.Peek()) {
			$at = $shotQueue.Dequeue()
			# the emulator saves its last frame itself ("SHOT"): works with the desktop locked
			foreach ($peer in $peers) {
				if ($peer -eq $killed) { continue }
				$msg = [Text.Encoding]::ASCII.GetBytes(("SHOT {0}" -f (Join-Path $shotDir ("{0}_{1:000}.png" -f $peer.Name, $at))))
				[void]$udp.Send($msg, $msg.Length, '127.0.0.1', $peer.Port)
			}
		}
		Start-Sleep -Milliseconds 30
	}
} finally {
	# the multiplayer score counters (0x8C0CDC68, 4 x s32) of each copy before closing
	$scores = @{}
	foreach ($peer in $peers) {
		if ($peer.Proc.HasExited) { continue }
		$m = [Text.Encoding]::ASCII.GetBytes('PEEK 8C0CDC68 16')
		[void]$peekUdp.Send($m, $m.Length, '127.0.0.1', $peer.Port)
		$ep = New-Object System.Net.IPEndPoint([System.Net.IPAddress]::Any, 0)
		try {
			$h = [Text.Encoding]::ASCII.GetString($peekUdp.Receive([ref]$ep)).Split(' ')[2]
			$b = [byte[]]::new(16)
			for ($i = 0; $i -lt 16; $i++) { $b[$i] = [Convert]::ToByte($h.Substring($i * 2, 2), 16) }
			$scores[$peer.Name] = (0..3 | ForEach-Object { [BitConverter]::ToInt32($b, $_ * 4) }) -join ' '
		} catch { }
	}
	$peekUdp.Close()
	$udp.Close()
	foreach ($peer in $peers) { if (-not $peer.Proc.HasExited) { Stop-Process -Id $peer.Proc.Id -Force } }
}
Start-Sleep -Milliseconds 800

# ---- Compare ---------------------------------------------------------------------------------
$report = [System.Collections.Generic.List[string]]::new()
function Say([string]$s) { $report.Add($s); Write-Output $s }
Say ("run {0}: {1} s, {7} copies, GGPO delay {2}, network delay {3} ms, loss {4}%, out of order {5}%, last copy stall {6} ms" -f `
	$Name, $Seconds, $Delay, $NetDelay, $Loss, $Oop, $StallGuest, $peers.Count)
Say ("pattern {0}, mode {1}" -f $Pattern, @("Deathmatch", "Cat and Mice", "Capture the Flag")[$Mode])
foreach ($k in $scores.Keys) { Say ("{0}: score counters 0x8C0CDC68 at the end: {1}" -f $k, $scores[$k]) }
if ($KillPlayer) {
	Say ("{0} killed {1:0.0} s into the game, at about frame {2}" -f $peers[$KillPlayer - 1].Name, $KillAt, (($killFrame.Values | Measure-Object -Maximum).Maximum))
	foreach ($peer in $peers) {
		if ($peer -eq $killed) { continue }
		$log = Join-Path $peer.Dir 'flycast.log'
		$left = if (Test-Path $log) { Select-String -Path $log -Pattern 'GGPO: player \d+ left.*' | Select-Object -First 1 } else { $null }
		Say ("{0} log: {1}" -f $peer.Name, $(if ($left) { $left.Matches[0].Value } else { 'no "player left" line' }))
	}
}
if ($Pattern -eq "hunt") { Say ("respawns seen (new vehicle in the player record): P1 {0}, P2 {1}" -f $hunt.Respawns[0], $hunt.Respawns[1]) }
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
	# Every copy is compared with p1's.
	# (with -KillPlayer: the copies that were still there at the end)
	$cmp = @($peers | Where-Object { $_ -ne $killed })
	$ref = $cmp[0].Name
	$others = @($cmp | Select-Object -Skip 1 | ForEach-Object { $_.Name })
	$upTo = ($cmp | ForEach-Object { ($logs[$_.Name].Keys | Measure-Object -Maximum).Maximum } | Measure-Object -Minimum).Minimum - 10
	$common = @($logs[$ref].Keys | Where-Object { $fr = $_; $_ -le $upTo -and -not ($others | Where-Object { -not $logs[$_].ContainsKey($fr) }) } | Sort-Object)
	# The verdict is on main RAM and video + sound RAM. The saved-state hash is only reported: the
	# rollback state holds a few host pointers (SH-4 part; seen at frames 600 and 4000 on
	# 2026-11-18: 8-byte values like 0x00007FF6xxxxxxxx), which differ between two processes.
	$names = @('main RAM', 'video/sound RAM')
	$firstDiff = $null
	$diffCount = 0
	$stateDiff = 0
	foreach ($fr in $common) {
		$a = $logs[$ref][$fr]
		$which = @(); $stateDiffers = $false
		foreach ($o in $others) {
			$b = $logs[$o][$fr]
			if ($a[2] -ne $b[2]) { $stateDiffers = $true }
			$which += @(for ($i = 0; $i -lt 2; $i++) { if ($a[$i] -ne $b[$i]) { "$o $($names[$i])" } })
		}
		if ($stateDiffers) { $stateDiff++ }
		if ($which.Count) {
			$diffCount++
			if ($null -eq $firstDiff) { $firstDiff = $fr; Say ("FIRST DIFFERENCE at frame {0} ({1:0.0} s of game), against {2}: {3}" -f $fr, ($fr / 60.0), $ref, ($which -join ', ')) }
		}
	}
	Say ("saved-state hash (host pointers included, informative only) differs on {0} of {1} frames" -f $stateDiff, $common.Count)
	if ($null -eq $firstDiff) {
		Say ("IDENTICAL on {4} copies for {0} confirmed frames (frames {1}..{2}, {3:0.0} s of game): main RAM and video/sound RAM" -f `
			$common.Count, $common[0], $common[-1], ($common.Count / 60.0), $cmp.Count)
		if ($KillPlayer) {
			$after = @($common | Where-Object { $_ -gt (($killFrame.Values | Measure-Object -Maximum).Maximum) }).Count
			Say ("  of which {0} frames ({1:0.0} s of game) after {2} was killed" -f $after, ($after / 60.0), $peers[$KillPlayer - 1].Name)
		}
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
				$a = $pg[$ref][$fr]
				if ($null -eq $a) { continue }
				$bad = @(foreach ($o in $others) {
					$b = $pg[$o][$fr]
					if ($null -eq $b) { continue }
					for ($i = 1; $i -lt [math]::Min($a.Count, $b.Count); $i++) { if ($a[$i] -ne $b[$i]) { '{0} 0x{1:X8}' -f $o, (0x8C000000 + ($i - 1) * 0x10000) } }
				})
				if ($bad.Count) { Say ("frame {0}: 64 KB pages that differ from {1}: {2}" -f $fr, $ref, ($bad -join ', ')); break }
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
if ($hunt.Log.Count) { $hunt.Log | Set-Content -Encoding utf8 (Join-Path $OutDir "hunt.log") }
$report | Set-Content -Encoding utf8 (Join-Path $OutDir 'summary.txt')
Write-Output "results: $OutDir"
