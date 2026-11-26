# Toy Commander in the browser: starts the local server for the whole network.
# Put this folder (the kit) anywhere, with your own disc image ToyCommander.chd in it, then double-click
# "Start Toy Commander server.cmd". Players open the address shown in Chrome or Edge on the same network.
$ErrorActionPreference = "Stop"
Set-Location -LiteralPath $PSScriptRoot
$disc = Join-Path $PSScriptRoot "ToyCommander.chd"
if (-not (Test-Path -LiteralPath $disc)) {
  Write-Host "Put your own Toy Commander (Europe) disc image in this folder, named ToyCommander.chd:" -ForegroundColor Yellow
  Write-Host "  $disc"
  Read-Host "Press Enter to close"
  exit 1
}
$py = Get-Command python -ErrorAction SilentlyContinue
if (-not $py) {
  Write-Host "Python 3 is needed: install it from python.org (tick 'Add python.exe to PATH'), then start again." -ForegroundColor Yellow
  Read-Host "Press Enter to close"
  exit 1
}
# HTTPS (gamepads on tablets and phones) needs the cryptography package, installed once
# (Windows PowerShell 5.1 stops on a native command's error text under "Stop": these two run under "Continue")
$ErrorActionPreference = "Continue"
& python -c "import cryptography" 2>$null
if ($LASTEXITCODE -ne 0) {
  Write-Host "Installing the cryptography package once (for HTTPS)..."
  & python -m pip install --user --quiet cryptography
}
$ErrorActionPreference = "Stop"
# The internet multiplayer server (tcvr-server, for headsets and PCs playing over the internet) starts too,
# in its own window, when this computer has it: in a "tcvr-server" folder next to this script, or in the
# folder named on the first line of internet-server.txt here. It keeps running from its own folder, where
# its key file lives (the headsets remember that key).
$inetDir = Join-Path $PSScriptRoot "tcvr-server"
$inetCfg = Join-Path $PSScriptRoot "internet-server.txt"
if (Test-Path -LiteralPath $inetCfg) {
  $line = (Get-Content -LiteralPath $inetCfg -TotalCount 1)
  if ($line) { $inetDir = $line.Trim().Trim('"') }
}
if ($inetDir -like "*.exe") { $inetDir = Split-Path -LiteralPath $inetDir }
$inetExe = Join-Path $inetDir "tcvr-server.exe"
if (Test-Path -LiteralPath $inetExe) {
  if (Get-Process -Name "tcvr-server" -ErrorAction SilentlyContinue) {
    Write-Host "The internet multiplayer server is already running."
  } else {
    $runner = Join-Path $inetDir "run-server.ps1"
    if (Test-Path -LiteralPath $runner) {
      Start-Process -FilePath "powershell.exe" -WorkingDirectory $inetDir -ArgumentList @(
        "-NoProfile", "-ExecutionPolicy", "Bypass", "-NoExit", "-File", "`"$runner`"")
    } else {
      Start-Process -FilePath $inetExe -WorkingDirectory $inetDir -ArgumentList @(
        "--port", "19800", "--key", "`"$(Join-Path $inetDir 'tcvr-server.key')`"")
    }
    Write-Host "Internet multiplayer server started in its own window (port 19800)."
  }
}
$Host.UI.RawUI.WindowTitle = "Toy Commander server (close this window to stop it)"
& python serve.py --disc $disc --host 0.0.0.0
Read-Host "The server stopped. Press Enter to close"
