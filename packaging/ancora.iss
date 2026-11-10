; ancora.iss
; Inno Setup 7 script (open source, https://jrsoftware.org/isinfo.php) for the
; ancora installer. Built by package.bat after build_driver.bat all.
; Per user install to %LOCALAPPDATA%\ancora, no admin rights, no Start Menu
; entries. SteamVR registration runs ancora.exe --register / --unregister, the
; same switches install.ps1 and uninstall.ps1 use, so no PowerShell script runs.
; SteamVR must be closed to install or uninstall.

#define AppVersion "0.1.3"

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
Source: "..\driver\ancora\bin\win64\driver_ancora.dll"; DestDir: "{app}\driver\ancora\bin\win64"; Flags: ignoreversion
Source: "..\driver\ancora\logic\ancora_logic.dll"; DestDir: "{app}\driver\ancora\logic"; Flags: ignoreversion
; diagnostic tools with the SDK's openvr_api.dll, manifest and bindings
Source: "..\src\bin\*"; DestDir: "{app}\diag"; Flags: ignoreversion recursesubdirs
; an existing config is kept on upgrade
Source: "..\driver\ancora\ancora.ini"; DestDir: "{app}"; Flags: onlyifdoesntexist

[InstallDelete]
; stale hot reload copies of the logic dll
Type: files; Name: "{app}\driver\ancora\logic\ancora_logic_live*.dll"
; registration script of 0.1.2 and older, replaced by ancora.exe --register
Type: files; Name: "{app}\steamvr.ps1"

[Run]
Filename: "{app}\ancora.exe"; Parameters: "--register --root ""{app}"""; \
  StatusMsg: "{cm:Registering}"; Flags: runhidden waituntilterminated

[UninstallRun]
Filename: "{app}\ancora.exe"; Parameters: "--unregister --root ""{app}"""; \
  RunOnceId: "ancoraUnregister"; Flags: runhidden waituntilterminated

[UninstallDelete]
; everything ancora created after install: config, hot reload copies
Type: filesandordirs; Name: "{app}"

[Code]
const
  WM_CLOSE = $0010;

{ A running panel holds ancora.exe open and only hides to the tray on a normal close.
  WM_CLOSE with wParam 1 makes it exit (every version of the panel does this); a panel
  that has not gone after 3 s is ended, so setup never stops on "files in use". }
procedure ClosePanel();
var
  I, Code: Integer;
begin
  if FindWindowByClassName('ancora_panel') = 0 then Exit;
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

function InitializeSetup(): Boolean;
begin
  Result := True;
  while SteamVRRunning() do
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
