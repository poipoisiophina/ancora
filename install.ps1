# install.ps1
# Builds ancora (build_driver.bat all, which fetches the OpenVR SDK if missing)
# and installs it to %LOCALAPPDATA%\ancora:
#   ancora.exe, ancora.vrmanifest, steamvr.ps1      control panel
#   ancora.ini                                       config (an existing one is kept)
#   driver\ancora\                                   SteamVR driver
#   diag\                                            probe.exe, reader.exe with the SDK's
#                                                    openvr_api.dll, manifest and bindings
# Then registers it with SteamVR through steamvr.ps1. SteamVR must be closed.
# For an installer exe instead, see package.bat.
# Usage: powershell -ExecutionPolicy Bypass -File install.ps1

$ErrorActionPreference = 'Stop'

$repo = $PSScriptRoot
$src  = Join-Path $repo 'driver\ancora'
$gui  = Join-Path $repo 'gui\bin'
$root = Join-Path $env:LOCALAPPDATA 'ancora'
$dest = Join-Path $root 'driver\ancora'
$steamvr = Join-Path $repo 'steamvr.ps1'

& powershell -NoProfile -ExecutionPolicy Bypass -File $steamvr check
if ($LASTEXITCODE -ne 0) { throw 'SteamVR is running. Close it, then run install again.' }

Write-Host '== build'
& cmd /c "`"$repo\build_driver.bat`" all"
if ($LASTEXITCODE -ne 0) { throw 'build failed' }

$files = @{
    loader = Join-Path $src 'bin\win64\driver_ancora.dll'
    logic  = Join-Path $src 'logic\ancora_logic.dll'
    drvman = Join-Path $src 'driver.vrdrivermanifest'
    ini    = Join-Path $src 'ancora.ini'
    panel  = Join-Path $gui 'ancora.exe'
    appman = Join-Path $gui 'ancora.vrmanifest'
    diag   = Join-Path $repo 'src\bin'
    openvr = Join-Path $repo 'src\bin\openvr_api.dll'
}
foreach ($f in $files.Values) { if (-not (Test-Path $f)) { throw "missing $f" } }

Write-Host '== copy'
Get-Process ancora -ErrorAction SilentlyContinue | Stop-Process -Force   # running panel locks ancora.exe
New-Item -ItemType Directory -Force (Join-Path $dest 'bin\win64'), (Join-Path $dest 'logic') | Out-Null
Copy-Item $files.drvman $dest -Force
Copy-Item $files.loader (Join-Path $dest 'bin\win64') -Force
Copy-Item $files.logic  (Join-Path $dest 'logic') -Force
Remove-Item (Join-Path $dest 'logic\ancora_logic_live*.dll') -ErrorAction SilentlyContinue   # stale hot reload copies
Copy-Item $files.panel, $files.appman, $steamvr $root -Force
$diag = Join-Path $root 'diag'
if (Test-Path $diag) { Remove-Item $diag -Recurse -Force }
Copy-Item $files.diag $diag -Recurse

$ini = Join-Path $root 'ancora.ini'
if (Test-Path $ini) {
    Write-Host "keeping existing config $ini"
} else {
    Copy-Item $files.ini $ini
    Write-Host "installed default config $ini"
}

Write-Host '== register with SteamVR'
& powershell -NoProfile -ExecutionPolicy Bypass -File $steamvr register -Root $root
if ($LASTEXITCODE -ne 0) { throw 'SteamVR registration failed' }

Write-Host "ancora installed to $root. Start SteamVR; the panel starts with it (tray icon)."
Write-Host "Open the panel any time: $(Join-Path $root 'ancora.exe')"
