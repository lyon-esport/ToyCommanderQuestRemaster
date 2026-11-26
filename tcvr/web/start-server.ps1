# Toy Commander in the browser: starts the local server for the whole network.
# Put this folder (the kit) anywhere, with your own disc image ToyCommander.chd in it, then double-click
# "Start Toy Commander server.cmd". Players open the address shown in Chrome or Edge on the same network.
# The window also shows the admin page's address and password.
$ErrorActionPreference = "Stop"
Set-Location -LiteralPath $PSScriptRoot
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
# One app starts everything (tcvr_host.py): the internet multiplayer server (tcvr-server, from a "tcvr-server"
# folder next to this script or the folder set on the admin page), the browser version and the admin page.
# Extra words after the script name go to it, for example:  --services mp,admin  (default: all three).
$Host.UI.RawUI.WindowTitle = "Toy Commander server (close this window to stop it)"
& python tcvr_host.py @args
Read-Host "The server stopped. Press Enter to close"
