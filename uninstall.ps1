# uninstall.ps1
# Removes everything ancora put into SteamVR and onto this PC outside the repo:
#   - SteamVR side through steamvr.ps1: driver registration, Steam\config\ancora,
#     ancora sections in steamvr.vrsettings, app manifest path and vrappconfig files
#   - a development registration of the repo copy of the driver
#   - %LOCALAPPDATA%\ancora (driver, control panel, config)
# SteamVR must be closed. The repo and its build outputs are left alone.
# For an installer built with package.bat, use Windows Settings > Apps instead.
# Usage: powershell -ExecutionPolicy Bypass -File uninstall.ps1 [-KeepConfig]
#   -KeepConfig   keep %LOCALAPPDATA%\ancora\ancora.ini for a later reinstall

param([switch]$KeepConfig)
$ErrorActionPreference = 'Stop'

$root = Join-Path $env:LOCALAPPDATA 'ancora'
$steamvr = Join-Path $PSScriptRoot 'steamvr.ps1'

Write-Host '== SteamVR'
& powershell -NoProfile -ExecutionPolicy Bypass -File $steamvr unregister -Root $root
if ($LASTEXITCODE -ne 0) { throw 'SteamVR is running. Close it, then run uninstall again.' }
# a development build registered straight from the repo
& powershell -NoProfile -ExecutionPolicy Bypass -File $steamvr unregister -Root $PSScriptRoot | Out-Null

Write-Host '== installed files'
Get-Process ancora -ErrorAction SilentlyContinue | Stop-Process -Force   # a running panel locks its files
if (Test-Path $root) {
    if ($KeepConfig) {
        Get-ChildItem $root -Exclude 'ancora.ini' | Remove-Item -Recurse -Force
        Write-Host "removed ancora files, kept $(Join-Path $root 'ancora.ini')"
    } else {
        Remove-Item $root -Recurse -Force
        Write-Host "removed $root"
    }
} else {
    Write-Host 'nothing installed'
}
Write-Host 'ancora uninstalled.'
