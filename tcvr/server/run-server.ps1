<#
.SYNOPSIS
  Starts tcvr-server, the internet meeting point for Toy Commander VR multiplayer, on Windows.

.DESCRIPTION
  Runs tcvr-server.exe from this script's folder (or -Exe). The server lists games, introduces the
  players and relays their traffic when no direct path works; it never runs the game itself, and what
  the players send each other is encrypted end to end.

  The first run makes the server's key file (tcvr-server.key, next to the exe). Keep it: the headsets
  remember the key for this server's address and refuse a different one.

  Players reach it through this computer's public address: forward TCP and UDP port 19800 (or -Port)
  on the internet router to this computer. -OpenFirewall adds the Windows Firewall rules (run
  PowerShell as administrator once for that).

.EXAMPLE
  .\run-server.ps1
.EXAMPLE
  .\run-server.ps1 -Port 20000 -OpenFirewall
#>
param(
	[int]$Port = 19800,
	[string]$Exe = (Join-Path $PSScriptRoot 'tcvr-server.exe'),
	[string]$KeyFile = '',
	[switch]$OpenFirewall,
	[switch]$VerboseLog
)
$ErrorActionPreference = 'Stop'
if (-not (Test-Path $Exe)) {
	Write-Error "tcvr-server.exe not found at $Exe (build it: cmake -S tcvr/server -B build-server; cmake --build build-server --config Release)"
}
$Exe = (Resolve-Path $Exe).Path
if ($KeyFile -eq '') { $KeyFile = Join-Path (Split-Path $Exe) 'tcvr-server.key' }

if ($OpenFirewall) {
	$admin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
		[Security.Principal.WindowsBuiltInRole]::Administrator)
	if (-not $admin) { Write-Error 'Run PowerShell as administrator for -OpenFirewall' }
	foreach ($proto in 'TCP', 'UDP') {
		$name = "tcvr-server $proto $Port"
		if (-not (Get-NetFirewallRule -DisplayName $name -ErrorAction SilentlyContinue)) {
			New-NetFirewallRule -DisplayName $name -Direction Inbound -Action Allow -Protocol $proto `
				-LocalPort $Port -Program $Exe -Profile Any | Out-Null
			Write-Host "Firewall: allowed $proto $Port for $Exe"
		}
	}
}

$argList = @('--port', "$Port", '--key', $KeyFile)
if ($VerboseLog) { $argList += '--verbose' }
Write-Host "Starting tcvr-server on port $Port (TCP and UDP). Ctrl+C stops it."
& $Exe @argList
exit $LASTEXITCODE
