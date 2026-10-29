# steamvr.ps1
# SteamVR side of an ancora install, shared by install.ps1, uninstall.ps1 and
# the Inno Setup installer. Locations come from SteamVR's own
# %LOCALAPPDATA%\openvr\openvrpaths.vrpath ("runtime" has vrpathreg.exe,
# "config" is Steam\config), never from fixed paths.
# Usage: powershell -ExecutionPolicy Bypass -File steamvr.ps1 <check|register|unregister> [-Root <dir>]
#   check       exit 1 while SteamVR runs (its loaded driver files are locked)
#   register    register <Root>\driver\ancora with SteamVR and the panel as an auto launch app
#   unregister  undo register and remove everything ancora left in Steam\config
#   -Root       install folder, default %LOCALAPPDATA%\ancora

param(
    [Parameter(Mandatory = $true)][ValidateSet('check', 'register', 'unregister')][string]$Action,
    [string]$Root = (Join-Path $env:LOCALAPPDATA 'ancora')
)
$ErrorActionPreference = 'Stop'

$driver = Join-Path $Root 'driver\ancora'
$manifest = Join-Path $Root 'ancora.vrmanifest'

$reg = $null
$steamCfg = $null
$vrpaths = Join-Path $env:LOCALAPPDATA 'openvr\openvrpaths.vrpath'
if (Test-Path $vrpaths) {
    $vr = Get-Content $vrpaths -Raw | ConvertFrom-Json
    $reg = @($vr.runtime | ForEach-Object { Join-Path $_ 'bin\win64\vrpathreg.exe' } | Where-Object { Test-Path $_ })[0]
    $steamCfg = @($vr.config | Where-Object { Test-Path $_ })[0]
}

# backups of Steam files go next to this script, never into Steam\config:
# backup\<file>.<time>.bak (the repo ignores *.bak; the installer's copy is
# removed with the install folder)
$backupDir = Join-Path $PSScriptRoot 'backup'
function Backup-File($path) {
    New-Item -ItemType Directory -Force $backupDir | Out-Null
    $dest = Join-Path $backupDir ("{0}.{1}.bak" -f (Split-Path $path -Leaf), (Get-Date -Format 'yyyyMMdd-HHmmss'))
    Copy-Item $path $dest -Force
    return $dest
}

# rewrite a JSON file with a backup, only when $edit reports a change
function Edit-Json($path, [scriptblock]$edit) {
    if (-not (Test-Path $path)) { return }
    $json = Get-Content $path -Raw | ConvertFrom-Json
    if (& $edit $json) {
        $bak = Backup-File $path
        $json | ConvertTo-Json -Depth 32 | Set-Content $path -Encoding UTF8
        Write-Host "updated $path (backup $bak)"
    }
}

if (Get-Process vrserver, vrmonitor -ErrorAction SilentlyContinue) {
    Write-Host 'SteamVR is running. Close it first.'
    exit 1
}
if ($Action -eq 'check') { exit 0 }

if ($Action -eq 'register') {
    if (-not $reg) { throw "SteamVR not found ($vrpaths). Run SteamVR once first." }
    if (-not $steamCfg) { throw 'Steam config folder from openvrpaths.vrpath not found' }
    & $reg adddriver $driver
    Write-Host "registered driver $driver"

    # panel: manifest path in appconfig.json, auto launch in vrappconfig
    $appconfig = Join-Path $steamCfg 'appconfig.json'
    $j = if (Test-Path $appconfig) { Get-Content $appconfig -Raw | ConvertFrom-Json } else { [pscustomobject]@{} }
    $paths = @()
    if ($j.PSObject.Properties['manifest_paths']) { $paths = @($j.manifest_paths | Where-Object { $_ -notmatch 'ancora\.vrmanifest$' }) }
    $paths += $manifest
    if ($j.PSObject.Properties['manifest_paths']) { $j.manifest_paths = $paths } else { $j | Add-Member manifest_paths $paths }
    if (Test-Path $appconfig) { $bak = Backup-File $appconfig; Write-Host "backup $bak" }
    $j | ConvertTo-Json -Depth 32 | Set-Content $appconfig -Encoding UTF8
    $appCfgDir = Join-Path $steamCfg 'vrappconfig'
    New-Item -ItemType Directory -Force $appCfgDir | Out-Null
    '{ "autolaunch" : true }' | Set-Content (Join-Path $appCfgDir 'ancora.panel.vrappconfig') -Encoding ASCII
    Write-Host "registered panel $manifest (auto launch on)"
    exit 0
}

# unregister: missing pieces are skipped so a half removed setup still cleans up
if ($reg) {
    & $reg removedriver $driver | Out-Null
    $left = & $reg show | Select-String -Pattern 'ancora'
    if ($left) { Write-Host "still registered: $left" } else { Write-Host 'no ancora driver registered' }
} else {
    Write-Host 'SteamVR not found, skipping driver unregister'
}

if (-not $steamCfg) { Write-Host 'Steam config folder not found, skipping'; exit 0 }
$drvCfg = Join-Path $steamCfg 'ancora'
if (Test-Path $drvCfg) { Remove-Item $drvCfg -Recurse -Force; Write-Host "removed $drvCfg" }

Edit-Json (Join-Path $steamCfg 'steamvr.vrsettings') {
    param($j)
    $hit = $false
    foreach ($name in 'driver_ancora', 'ancora.panel', 'ancora.reader',
                      'system.generated.ancora.exe', 'system.generated.reader.exe', 'system.generated.probe.exe') {
        if ($j.PSObject.Properties[$name]) { $j.PSObject.Properties.Remove($name); $hit = $true }
    }
    $hit
}

Edit-Json (Join-Path $steamCfg 'appconfig.json') {
    param($j)
    if (-not $j.PSObject.Properties['manifest_paths']) { return $false }
    $keep = @($j.manifest_paths | Where-Object { $_ -notmatch 'ancora\.vrmanifest$' })
    if ($keep.Count -eq @($j.manifest_paths).Count) { return $false }
    $j.manifest_paths = $keep
    $true
}

$appCfg = Join-Path $steamCfg 'vrappconfig'
if (Test-Path $appCfg) {
    Get-ChildItem $appCfg -File |
        Where-Object { $_.Name -match '^(ancora\.(panel|reader)|system\.generated\.(ancora|reader|probe)\.exe)\.vrappconfig$' } |
        ForEach-Object { Remove-Item $_.FullName -Force; Write-Host "removed $($_.FullName)" }
}
exit 0
