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
& python -c "import cryptography" 2>$null
if ($LASTEXITCODE -ne 0) {
  Write-Host "Installing the cryptography package once (for HTTPS)..."
  & python -m pip install --user --quiet cryptography
}
$Host.UI.RawUI.WindowTitle = "Toy Commander server (close this window to stop it)"
& python serve.py --disc $disc --host 0.0.0.0
Read-Host "The server stopped. Press Enter to close"
