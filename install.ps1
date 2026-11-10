# install.ps1
# Builds ancora (build_driver.bat all, which fetches the OpenVR SDK if missing)
# and installs it to %LOCALAPPDATA%\ancora:
#   ancora.exe, ancora.vrmanifest                    control panel
#   ancora.ini                                       config (an existing one is kept)
#   driver\ancora\                                   SteamVR driver
#   diag\                                            ancora_probe.exe, ancora_reader.exe with the SDK's
#                                                    openvr_api.dll, manifest and bindings
# Then registers it with SteamVR through ancora.exe --register (Steam file backups
# go to the repo's backup folder). SteamVR must be closed.
# For an installer exe instead, see package.bat.
# Usage: powershell -ExecutionPolicy Bypass -File install.ps1 [-PanelOnly]
#   -PanelOnly   build, then swap only the panel (ancora.exe, ancora.vrmanifest) into an
#                existing install; SteamVR may keep running, since it never loads the
#                panel. A running panel is closed and started again with its arguments.

param([switch]$PanelOnly)
$ErrorActionPreference = 'Stop'

$repo = $PSScriptRoot
$src  = Join-Path $repo 'driver\ancora'
$gui  = Join-Path $repo 'gui\bin'
$root = Join-Path $env:LOCALAPPDATA 'ancora'
$dest = Join-Path $root 'driver\ancora'

# a running panel locks ancora.exe and only hides to the tray on a normal close;
# WM_CLOSE with wParam 1 makes it exit cleanly (tray icon removed). Ended after 3 s.
# Returns the command line of the panel it closed, or $null.
function Close-Panel {
    $proc = Get-CimInstance Win32_Process -Filter "Name = 'ancora.exe'" | Select-Object -First 1
    if (-not $proc) { return $null }
    Add-Type -Namespace Ancora -Name Win -MemberDefinition @'
[DllImport("user32.dll")] public static extern IntPtr FindWindow(string c, string t);
[DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
'@ -ErrorAction SilentlyContinue
    $h = [Ancora.Win]::FindWindow('ancora_panel', [NullString]::Value)
    if ($h -ne [IntPtr]::Zero) { [void][Ancora.Win]::PostMessage($h, 0x0010, [IntPtr]1, [IntPtr]::Zero) }
    for ($i = 0; $i -lt 30 -and (Get-Process ancora -ErrorAction SilentlyContinue); $i++) { Start-Sleep -Milliseconds 100 }
    Get-Process ancora -ErrorAction SilentlyContinue | Stop-Process -Force
    return $proc.CommandLine
}

if ($PanelOnly) {
    $installed = Join-Path $root 'ancora.exe'
    if (-not (Test-Path $installed)) { throw "no install at $root, run install.ps1 without -PanelOnly first" }
    Write-Host '== build'
    & cmd /c "`"$repo\build_driver.bat`" all"
    if ($LASTEXITCODE -ne 0) { throw 'build failed' }
    Write-Host '== swap panel'
    $cmd = Close-Panel
    Copy-Item (Join-Path $gui 'ancora.exe'), (Join-Path $gui 'ancora.vrmanifest') $root -Force
    if ($cmd) {
        # the same arguments it ran with, such as --steamvr when SteamVR started it
        $args0 = ''
        if ($cmd -match '^\s*(?:"[^"]*"|\S+)\s*(.*)$') { $args0 = $Matches[1].Trim() }
        if ($args0) { Start-Process $installed -ArgumentList $args0 } else { Start-Process $installed }
        Write-Host "panel restarted $(if ($args0) { "with $args0" })"
    }
    Write-Host "panel swapped in $root"
    exit 0
}

if (Get-Process vrserver, vrmonitor -ErrorAction SilentlyContinue) {
    throw 'SteamVR is running. Close it, then run install again.'
}

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
[void](Close-Panel)
New-Item -ItemType Directory -Force (Join-Path $dest 'bin\win64'), (Join-Path $dest 'logic') | Out-Null
Copy-Item $files.drvman $dest -Force
Copy-Item $files.loader (Join-Path $dest 'bin\win64') -Force
Copy-Item $files.logic  (Join-Path $dest 'logic') -Force
Remove-Item (Join-Path $dest 'logic\ancora_logic_live*.dll') -ErrorAction SilentlyContinue   # stale hot reload copies
Copy-Item $files.panel, $files.appman $root -Force
Remove-Item (Join-Path $root 'steamvr.ps1') -ErrorAction SilentlyContinue   # registration script of 0.1.2 and older
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
# ancora.exe is a GUI program: the pipe makes PowerShell wait for it and get its output
& (Join-Path $root 'ancora.exe') --register --root $root --backup-dir (Join-Path $repo 'backup') | Write-Host
if ($LASTEXITCODE -ne 0) { throw 'SteamVR registration failed' }

Write-Host "ancora installed to $root. Start SteamVR; the panel starts with it (tray icon)."
Write-Host "Open the panel any time: $(Join-Path $root 'ancora.exe')"
