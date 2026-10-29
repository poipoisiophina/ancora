; ancora.iss
; Inno Setup 7 script (open source, https://jrsoftware.org/isinfo.php) for the
; ancora installer. Built by package.bat after build_driver.bat all.
; Per user install to %LOCALAPPDATA%\ancora, no admin rights, no Start Menu
; entries. SteamVR registration runs steamvr.ps1, the same script install.ps1
; uses. SteamVR must be closed to install or uninstall.

#define AppVersion "0.1.0"

[Setup]
AppId={{6AB13E94-DDD3-4AAB-B3FE-6965F0462422}
AppName=ancora
AppVersion={#AppVersion}
AppVerName=ancora {#AppVersion}
AppPublisher=ancora
DefaultDirName={localappdata}\ancora
DisableDirPage=yes
DisableProgramGroupPage=yes
DisableReadyPage=yes
PrivilegesRequired=lowest
; the driver is x64 only, so the installer is a 64-bit program too (new in Inno Setup 7)
SetupArchitecture=x64
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputDir=..\dist
OutputBaseFilename=ancora-setup-{#AppVersion}
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
; icon of the setup exe; Settings > Apps shows the one embedded in ancora.exe
SetupIconFile=..\driver\obj\ancora.ico
UninstallDisplayIcon={app}\ancora.exe
UninstallDisplayName=ancora
CloseApplications=yes
RestartApplications=no

[Files]
Source: "..\gui\bin\ancora.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\gui\ancora.vrmanifest"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\steamvr.ps1"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\driver\ancora\driver.vrdrivermanifest"; DestDir: "{app}\driver\ancora"; Flags: ignoreversion
Source: "..\driver\ancora\bin\win64\driver_ancora.dll"; DestDir: "{app}\driver\ancora\bin\win64"; Flags: ignoreversion
Source: "..\driver\ancora\logic\ancora_logic.dll"; DestDir: "{app}\driver\ancora\logic"; Flags: ignoreversion
; diagnostic tools with the SDK's openvr_api.dll, manifest and bindings
Source: "..\src\bin\*"; DestDir: "{app}\diag"; Flags: ignoreversion recursesubdirs
; an existing config is kept on upgrade
Source: "..\driver\ancora\ancora.ini"; DestDir: "{app}"; Flags: onlyifdoesntexist

[InstallDelete]
; stale hot reload copies of the logic dll
Type: files; Name: "{app}\driver\ancora\logic\ancora_logic_live*.dll"

[Run]
Filename: "powershell.exe"; \
  Parameters: "-NoProfile -ExecutionPolicy Bypass -File ""{app}\steamvr.ps1"" register -Root ""{app}"""; \
  StatusMsg: "Registering ancora with SteamVR..."; Flags: runhidden waituntilterminated

[UninstallRun]
Filename: "powershell.exe"; \
  Parameters: "-NoProfile -ExecutionPolicy Bypass -File ""{app}\steamvr.ps1"" unregister -Root ""{app}"""; \
  RunOnceId: "ancoraUnregister"; Flags: runhidden waituntilterminated

[UninstallDelete]
; everything ancora created after install: config, hot reload copies
Type: filesandordirs; Name: "{app}"

[Code]
function SteamVRRunning(): Boolean;
var
  Locator, Service, Procs: Variant;
begin
  Locator := CreateOleObject('WbemScripting.SWbemLocator');
  Service := Locator.ConnectServer('.', 'root\CIMV2');
  Procs := Service.ExecQuery('SELECT ProcessId FROM Win32_Process WHERE Name = ''vrserver.exe'' OR Name = ''vrmonitor.exe''');
  Result := Procs.Count > 0;
end;

function InitializeSetup(): Boolean;
begin
  Result := True;
  while SteamVRRunning() do
    if MsgBox('SteamVR is running. Close SteamVR, then press OK to continue.', mbInformation, MB_OKCANCEL) = IDCANCEL then
    begin
      Result := False;
      Exit;
    end;
end;

function InitializeUninstall(): Boolean;
begin
  Result := True;
  while SteamVRRunning() do
    if MsgBox('SteamVR is running. Close SteamVR, then press OK to uninstall ancora.', mbInformation, MB_OKCANCEL) = IDCANCEL then
    begin
      Result := False;
      Exit;
    end;
end;
