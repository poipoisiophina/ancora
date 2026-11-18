; ancora.iss
; Inno Setup 7 script (open source, https://jrsoftware.org/isinfo.php) for the
; ancora installer. Built by package.bat after build_driver.bat all.
; Per user install to %LOCALAPPDATA%\ancora, no admin rights, no Start Menu
; entries. SteamVR registration runs ancora.exe --register / --unregister, the
; same switches install.ps1 and uninstall.ps1 use, so no PowerShell script runs.
; SteamVR must be closed to install or uninstall, except for a hot update: when the
; installed loader.id equals the new one, the running loader driver_ancora.dll is
; left alone and only ancora_logic.dll (hot reloaded by the driver), the panel and
; the other files are replaced while SteamVR runs.

#define AppVersion "0.2.1"

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
; the BSD 3-Clause text, shown on a page the user accepts before the install
LicenseFile=..\LICENSE
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
; setup speaks the Windows display language when it has it (the panel's languages,
; less toki pona and Persian, which Inno Setup has no official file for), and only
; asks when it has not
ShowLanguageDialog=auto
LanguageDetectionMethod=uilanguage

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "german"; MessagesFile: "compiler:Languages\German.isl"
Name: "spanish"; MessagesFile: "compiler:Languages\Spanish.isl"
Name: "french"; MessagesFile: "compiler:Languages\French.isl"
Name: "italian"; MessagesFile: "compiler:Languages\Italian.isl"
Name: "japanese"; MessagesFile: "compiler:Languages\Japanese.isl"
Name: "korean"; MessagesFile: "compiler:Languages\Korean.isl"
Name: "hebrew"; MessagesFile: "compiler:Languages\Hebrew.isl"
Name: "brazilianportuguese"; MessagesFile: "compiler:Languages\BrazilianPortuguese.isl"
Name: "polish"; MessagesFile: "compiler:Languages\Polish.isl"
Name: "chinesesimplified"; MessagesFile: "compiler:Languages\ChineseSimplified.isl"
Name: "chinesetraditional"; MessagesFile: "compiler:Languages\ChineseTraditional.isl"
Name: "russian"; MessagesFile: "compiler:Languages\Russian.isl"
Name: "arabic"; MessagesFile: "compiler:Languages\Arabic.isl"
Name: "ukrainian"; MessagesFile: "compiler:Languages\Ukrainian.isl"
Name: "bulgarian"; MessagesFile: "compiler:Languages\Bulgarian.isl"
Name: "czech"; MessagesFile: "compiler:Languages\Czech.isl"
Name: "danish"; MessagesFile: "compiler:Languages\Danish.isl"
Name: "dutch"; MessagesFile: "compiler:Languages\Dutch.isl"
Name: "finnish"; MessagesFile: "compiler:Languages\Finnish.isl"
Name: "hungarian"; MessagesFile: "compiler:Languages\Hungarian.isl"
Name: "lithuanian"; MessagesFile: "compiler:Languages\Lithuanian.isl"
Name: "portuguese"; MessagesFile: "compiler:Languages\Portuguese.isl"
Name: "slovak"; MessagesFile: "compiler:Languages\Slovak.isl"
Name: "slovenian"; MessagesFile: "compiler:Languages\Slovenian.isl"
Name: "swedish"; MessagesFile: "compiler:Languages\Swedish.isl"
; no Inno Setup message file for Croatian, Estonian, Greek, Irish, Latvian, Maltese
; or Romanian: setup shows English there, the panel itself is translated

; ancora's own setup messages in those languages (UTF-8 file with a BOM)
#include "messages.iss"

[Files]
Source: "..\gui\bin\ancora.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\gui\ancora.vrmanifest"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\driver\ancora\driver.vrdrivermanifest"; DestDir: "{app}\driver\ancora"; Flags: ignoreversion
; the loader is locked by a running SteamVR: not touched in a hot update
Source: "..\driver\ancora\bin\win64\driver_ancora.dll"; DestDir: "{app}\driver\ancora\bin\win64"; Flags: ignoreversion; Check: not IsHotUpdate
; loader.id twice: setup reads the first copy before the install, the second is installed
Source: "..\driver\obj\loader.id"; DestDir: "{tmp}"; Flags: dontcopy
Source: "..\driver\obj\loader.id"; DestDir: "{app}\driver\ancora"; Flags: ignoreversion
Source: "..\driver\ancora\logic\ancora_logic.dll"; DestDir: "{app}\driver\ancora\logic"; Flags: ignoreversion
; diagnostic tools with the SDK's openvr_api.dll, manifest and bindings
Source: "..\src\bin\*"; DestDir: "{app}\diag"; Flags: ignoreversion recursesubdirs
; license and the OpenVR notice its license asks to ship with openvr_api.dll
Source: "..\LICENSE"; DestDir: "{app}"; DestName: "LICENSE.txt"; Flags: ignoreversion
Source: "..\THIRD-PARTY-NOTICES.md"; DestDir: "{app}"; DestName: "THIRD-PARTY-NOTICES.txt"; Flags: ignoreversion
; an existing config is kept on upgrade
Source: "..\driver\ancora\ancora.ini"; DestDir: "{app}"; Flags: onlyifdoesntexist

[InstallDelete]
; stale hot reload copies of the logic dll
Type: files; Name: "{app}\driver\ancora\logic\ancora_logic_live*.dll"; Check: not IsHotUpdate
; registration script of 0.1.2 and older, replaced by ancora.exe --register
Type: files; Name: "{app}\steamvr.ps1"
; diagnostic tools of 0.1.3 and older, now ancora_probe.exe and ancora_reader.exe
Type: files; Name: "{app}\diag\probe.exe"
Type: files; Name: "{app}\diag\reader.exe"

[Run]
; already registered in a hot update, and --register needs SteamVR closed
Filename: "{app}\ancora.exe"; Parameters: "--register --root ""{app}"""; \
  StatusMsg: "{cm:Registering}"; Flags: runhidden waituntilterminated; Check: not IsHotUpdate
; the panel that setup closed comes back in the tray
Filename: "{app}\ancora.exe"; Parameters: "--tray"; \
  Flags: nowait runasoriginaluser; Check: PanelWasRunning

[UninstallRun]
Filename: "{app}\ancora.exe"; Parameters: "--unregister --root ""{app}"""; \
  RunOnceId: "ancoraUnregister"; Flags: runhidden waituntilterminated

[UninstallDelete]
; everything ancora created after install: config, hot reload copies
Type: filesandordirs; Name: "{app}"

[Code]
const
  WM_CLOSE = $0010;
  UninstallKey = 'Software\Microsoft\Windows\CurrentVersion\Uninstall\{6AB13E94-DDD3-4AAB-B3FE-6965F0462422}_is1';

var
  HotUpdate: Boolean;      { SteamVR runs and the installed loader is the new one }
  WasRunning: Boolean;     { the panel was open when setup closed it }

function IsHotUpdate(): Boolean;
begin
  Result := HotUpdate;
end;

function PanelWasRunning(): Boolean;
begin
  Result := WasRunning;
end;

{ A running panel holds ancora.exe open and only hides to the tray on a normal close.
  WM_CLOSE with wParam 1 makes it exit (every version of the panel does this); a panel
  that has not gone after 3 s is ended, so setup never stops on "files in use". }
procedure ClosePanel();
var
  I, Code: Integer;
begin
  if FindWindowByClassName('ancora_panel') = 0 then Exit;
  WasRunning := True;
  PostMessage(FindWindowByClassName('ancora_panel'), WM_CLOSE, 1, 0);
  for I := 1 to 30 do
  begin
    if FindWindowByClassName('ancora_panel') = 0 then Exit;
    Sleep(100);
  end;
  Exec(ExpandConstant('{sys}\taskkill.exe'), '/F /IM ancora.exe', '', SW_HIDE, ewWaitUntilTerminated, Code);
end;

{ runs before setup checks for files in use }
function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  ClosePanel();
  Result := '';
end;

function SteamVRRunning(): Boolean;
var
  Locator, Service, Procs: Variant;
begin
  Locator := CreateOleObject('WbemScripting.SWbemLocator');
  Service := Locator.ConnectServer('.', 'root\CIMV2');
  Procs := Service.ExecQuery('SELECT ProcessId FROM Win32_Process WHERE Name = ''vrserver.exe'' OR Name = ''vrmonitor.exe''');
  Result := Procs.Count > 0;
end;

{ the install folder of the installed ancora, '' when there is none }
function InstalledDir(): String;
begin
  Result := '';
  if not RegQueryStringValue(HKCU, UninstallKey, 'Inno Setup: App Path', Result) then Result := '';
end;

{ Releases 0.1.7 to 0.2.0 ship no loader.id, but their loader sources, manifests, registration
  code and OpenVR SDK are exactly what produces this id (git diff v0.1.7..v0.2.0 over
  those files is empty). So such an install counts as having it. A release that changes
  any of them gets another id and is not a match. }
const
  LegacyLoaderId =
    'f19696c1b1079299a3a3581f3e8fb18f56eac0a2e376f44dc479183f1d0f077f' + #13#10 +
    'd75df60730a47638201fcb3045ec22ac3092df7904fc6507171870931dc3fd5a' + #13#10 +
    '69bfe147b7c7f74aee5a99762ad2998120c15c73ea1c50051c1ab87e33117341' + #13#10 +
    '4da1f7a3f6ee0819013f8cd368b56b32e1bec3b9ab44ac239b6952391d280f18' + #13#10 +
    'a1372755224cba68a31eb93c690ea932828694f7cafbfbc824b98e14f008b6aa' + #13#10 +
    'openvr 0924064316de3effbcd1acf1e309182a2deb1c05';

function IsLegacyRelease(): Boolean;
var
  V: String;
begin
  Result := False;
  if not RegQueryStringValue(HKCU, UninstallKey, 'DisplayVersion', V) then Exit;
  Result := (V = '0.1.7') or (V = '0.1.8') or (V = '0.1.9') or (V = '0.2.0');
end;

{ True when the installed loader is the one in this setup and the installed loader and
  logic dll exist: the installed loader.id equals the new one, or the install is one of
  the releases without a loader.id (see LegacyLoaderId). Anything else (an older release,
  a changed loader, a damaged folder) is False: then SteamVR has to be closed. }
function LoaderUnchanged(): Boolean;
var
  Dir, Mine, Theirs: AnsiString;
begin
  Result := False;
  Dir := InstalledDir();
  if Dir = '' then Exit;
  if not FileExists(Dir + '\driver\ancora\bin\win64\driver_ancora.dll') then Exit;
  if not FileExists(Dir + '\driver\ancora\logic\ancora_logic.dll') then Exit;
  ExtractTemporaryFile('loader.id');
  if not LoadStringFromFile(ExpandConstant('{tmp}\loader.id'), Mine) then Exit;
  Mine := Trim(Mine);
  if Mine = '' then Exit;
  if LoadStringFromFile(Dir + '\driver\ancora\loader.id', Theirs) then
    Result := Mine = Trim(Theirs)
  else
    Result := IsLegacyRelease() and (Mine = LegacyLoaderId);
end;

function InitializeSetup(): Boolean;
begin
  Result := True;
  HotUpdate := SteamVRRunning() and LoaderUnchanged();
  while (not HotUpdate) and SteamVRRunning() do
    if MsgBox(CustomMessage('SteamVRRunning'), mbInformation, MB_OKCANCEL) = IDCANCEL then
    begin
      Result := False;
      Exit;
    end;
end;

function InitializeUninstall(): Boolean;
begin
  Result := True;
  while SteamVRRunning() do
    if MsgBox(CustomMessage('SteamVRRunningUninstall'), mbInformation, MB_OKCANCEL) = IDCANCEL then
    begin
      Result := False;
      Exit;
    end;
  ClosePanel();   { its files go next }
end;
