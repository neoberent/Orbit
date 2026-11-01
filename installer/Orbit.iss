#define AppVersion "3.1.4"

[Setup]
AppId={{287650A1-6323-4DC5-8A2E-F1327D88CA43}
AppName=Orbit Download Manager
AppVersion={#AppVersion}
AppVerName=Orbit Download Manager {#AppVersion}
AppPublisher=Neo Berent
AppPublisherURL=https://orbitdm.netlify.app
AppSupportURL=https://orbitdm.netlify.app
AppUpdatesURL=https://orbitdm.netlify.app
AppCopyright=Copyright (C) 2026 Neo Berent
VersionInfoVersion={#AppVersion}
VersionInfoProductVersion={#AppVersion}
VersionInfoCompany=Neo Berent
VersionInfoDescription=Orbit Download Manager Setup
VersionInfoProductName=Orbit Download Manager
VersionInfoCopyright=Copyright (C) 2026 Neo Berent
DefaultDirName={localappdata}\Orbit
DefaultGroupName=Orbit
DisableProgramGroupPage=no
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
OutputDir=Output
OutputBaseFilename=OrbitSetup-{#AppVersion}
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
SetupIconFile=..\src\ui\assets\orbit.ico
UninstallDisplayIcon={app}\orbit.exe
LicenseFile=License.txt
; An Orbit that is still running (or stuck closing) is shut down instead of failing the
; update with "file in use". Orbit is relaunched by [Run], not by the Restart Manager.
CloseApplications=force
RestartApplications=no
; installer\build-installer.ps1 defines SignSetup (and the "orbitsign" tool) when a
; code-signing certificate is configured, so the setup and uninstaller get signed too.
#ifdef SignSetup
SignTool=orbitsign
SignedUninstaller=yes
SignToolRetryCount=3
#endif

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "Create a &desktop shortcut"; GroupDescription: "Additional shortcuts:"; Flags: unchecked

[Files]
Source: "..\build-vcpkg\Release\*"; DestDir: "{app}"; Excludes: "*.lib,*.pdb,*.ilk,*.exp,portable.flag,Data"; Flags: recursesubdirs ignoreversion
Source: "License.txt"; DestDir: "{app}"; Flags: ignoreversion
Source: "PrivacyPolicy.txt"; DestDir: "{app}"; Flags: ignoreversion
Source: "PrivacyPolicy.txt"; DestDir: "{tmp}"; Flags: dontcopy

[UninstallDelete]
Type: dirifempty; Name: "{app}\Orbit Download Manager"

[Registry]
; Registers the orbit:// protocol for the current user so the browser extension can
; hand links to Orbit without needing admin rights. Orbit also self-repairs this on
; every launch, but registering it here means it works immediately after install.
Root: HKCU; Subkey: "Software\Classes\orbit"; ValueType: string; ValueName: ""; ValueData: "URL:Orbit Protocol"; Flags: uninsdeletekey
Root: HKCU; Subkey: "Software\Classes\orbit"; ValueType: string; ValueName: "URL Protocol"; ValueData: ""
Root: HKCU; Subkey: "Software\Classes\orbit\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\orbit.exe,0"
Root: HKCU; Subkey: "Software\Classes\orbit\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\orbit.exe"" ""%1"""

; Registers Orbit as a candidate handler for .torrent files under its own ProgID so
; it's selectable via "Open with" / Default apps. The .torrent key itself isn't flagged
; for deletion on uninstall since it may be shared with another torrent client; only
; our own ProgID (fully owned by Orbit) is cleaned up. Orbit also self-repairs this on
; every launch.
Root: HKCU; Subkey: "Software\Classes\.torrent"; ValueType: string; ValueName: ""; ValueData: "Orbit.Torrent"
Root: HKCU; Subkey: "Software\Classes\Orbit.Torrent"; ValueType: string; ValueName: ""; ValueData: "BitTorrent File"; Flags: uninsdeletekey
Root: HKCU; Subkey: "Software\Classes\Orbit.Torrent\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\orbit.exe,0"
Root: HKCU; Subkey: "Software\Classes\Orbit.Torrent\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\orbit.exe"" ""%1"""

; Native messaging host for the Orbit browser extension (Chrome, Edge, Brave, Chromium,
; Firefox). Orbit writes the manifest files and refreshes these keys on every launch;
; they're listed here so uninstalling removes them.
Root: HKCU; Subkey: "Software\Google\Chrome\NativeMessagingHosts\com.orbit.downloadmanager"; ValueType: string; ValueName: ""; ValueData: "{userappdata}\Orbit\native-messaging\com.orbit.downloadmanager.json"; Flags: uninsdeletekey
Root: HKCU; Subkey: "Software\Microsoft\Edge\NativeMessagingHosts\com.orbit.downloadmanager"; ValueType: string; ValueName: ""; ValueData: "{userappdata}\Orbit\native-messaging\com.orbit.downloadmanager.json"; Flags: uninsdeletekey
Root: HKCU; Subkey: "Software\BraveSoftware\Brave-Browser\NativeMessagingHosts\com.orbit.downloadmanager"; ValueType: string; ValueName: ""; ValueData: "{userappdata}\Orbit\native-messaging\com.orbit.downloadmanager.json"; Flags: uninsdeletekey
Root: HKCU; Subkey: "Software\Chromium\NativeMessagingHosts\com.orbit.downloadmanager"; ValueType: string; ValueName: ""; ValueData: "{userappdata}\Orbit\native-messaging\com.orbit.downloadmanager.json"; Flags: uninsdeletekey
Root: HKCU; Subkey: "Software\Mozilla\NativeMessagingHosts\com.orbit.downloadmanager"; ValueType: string; ValueName: ""; ValueData: "{userappdata}\Orbit\native-messaging\com.orbit.downloadmanager.firefox.json"; Flags: uninsdeletekey

[Icons]
Name: "{group}\Orbit"; Filename: "{app}\orbit.exe"
Name: "{group}\Privacy Policy"; Filename: "{app}\PrivacyPolicy.txt"
Name: "{group}\Uninstall Orbit"; Filename: "{uninstallexe}"
Name: "{autodesktop}\Orbit"; Filename: "{app}\orbit.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\orbit.exe"; Description: "Launch Orbit Download Manager"; Flags: nowait postinstall skipifsilent
Filename: "{app}\orbit.exe"; Flags: nowait; Check: IsOrbitUpdate

[Code]
var
  PrivacyPage: TWizardPage;
  PrivacyMemo: TNewMemo;
  PrivacyCheck: TNewCheckBox;

function IsOrbitUpdate(): Boolean;
begin
  Result := ExpandConstant('{param:ORBITUPDATE|0}') = '1';
end;

function WaitForOrbitToClose(Timeout: Integer): Boolean;
var
  Waited: Integer;
begin
  Waited := 0;
  while CheckForMutexes('OrbitDownloadManager.Running') and (Waited < Timeout) do
  begin
    Sleep(250);
    Waited := Waited + 250;
  end;
  Result := not CheckForMutexes('OrbitDownloadManager.Running');
end;

function InitializeSetup(): Boolean;
var
  ResultCode: Integer;
begin
  Result := True;
  if IsOrbitUpdate() then
  begin
    if not WaitForOrbitToClose(30000) then
    begin
      // Orbit 3.1.1 and older hand this installer copies of their network sockets, and
      // then can't finish closing while it runs. That's the last step of their shutdown,
      // after downloads and torrents are saved, so end it.
      Exec(ExpandConstant('{sys}\taskkill.exe'), '/F /IM orbit.exe', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
      WaitForOrbitToClose(15000);
    end;
  end;
end;

procedure InitializeWizard();
begin
  PrivacyPage := CreateCustomPage(wpLicense, 'Privacy Policy',
    'Please read the following privacy policy before continuing.');

  PrivacyMemo := TNewMemo.Create(PrivacyPage);
  PrivacyMemo.Parent := PrivacyPage.Surface;
  PrivacyMemo.Left := 0;
  PrivacyMemo.Top := 0;
  PrivacyMemo.Width := PrivacyPage.SurfaceWidth;
  PrivacyMemo.Height := PrivacyPage.SurfaceHeight - 32;
  PrivacyMemo.ScrollBars := ssVertical;
  PrivacyMemo.ReadOnly := True;
  PrivacyMemo.Font.Name := 'Segoe UI';
  PrivacyMemo.Font.Size := 9;

  ExtractTemporaryFile('PrivacyPolicy.txt');
  PrivacyMemo.Lines.LoadFromFile(ExpandConstant('{tmp}\PrivacyPolicy.txt'));

  PrivacyCheck := TNewCheckBox.Create(PrivacyPage);
  PrivacyCheck.Parent := PrivacyPage.Surface;
  PrivacyCheck.Left := 0;
  PrivacyCheck.Top := PrivacyMemo.Top + PrivacyMemo.Height + 8;
  PrivacyCheck.Width := PrivacyPage.SurfaceWidth;
  PrivacyCheck.Caption := 'I have read and accept the Privacy Policy';
end;

function NextButtonClick(CurPageID: Integer): Boolean;
begin
  Result := True;
  if WizardSilent() then
    Exit;
  if CurPageID = PrivacyPage.ID then
  begin
    if not PrivacyCheck.Checked then
    begin
      MsgBox('You must accept the Privacy Policy to continue installing Orbit.', mbError, MB_OK);
      Result := False;
    end;
  end;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  DataDir: String;
begin
  if CurUninstallStep = usPostUninstall then
  begin
    DataDir := ExpandConstant('{userappdata}\Orbit');
    if DirExists(DataDir) then
    begin
      // SuppressibleMsgBox, so a silent uninstall (/SUPPRESSMSGBOXES) doesn't wait on an
      // invisible question; it then keeps the user's data.
      if SuppressibleMsgBox('Also delete your Orbit settings, logs, and download history stored in ' + DataDir + '?',
                            mbConfirmation, MB_YESNO, IDNO) = IDYES then
        DelTree(DataDir, True, True, True);
    end;
  end;
end;
