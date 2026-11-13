; reGH2 - Setup.exe (Inno Setup 6). Made by packaging\windows\make-installer.ps1, which stages the program folder and runs:
;   ISCC /DStageDir=<staged reGH2 folder> /DAppVersion=<version> /DOutputDir=<dist> packaging\windows\reGH2.iss
; A per-user install (no administrator rights): %LOCALAPPDATA%\Programs\reGH2. It holds the launcher, the prebuilt game host (host\), the tools that compile the game
; module (llvm-mingw, Ninja, the SDK's code generator) and the shader tools; the game itself is built from the player's own copy by the launcher and lives in
; %LOCALAPPDATA%\reGH2\game, the saves in %LOCALAPPDATA%\gh2. Nothing is downloaded and nothing from Microsoft is installed.

#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
#ifndef StageDir
  #error StageDir is required (the staged program folder)
#endif
#ifndef OutputDir
  #define OutputDir "."
#endif

[Setup]
AppId={{6F1E3C2A-8B47-4D95-A3E0-52C7B9D14F68}
AppName=reGH2
AppVersion={#AppVersion}
AppVerName=reGH2 {#AppVersion}
AppPublisher=reGH2 contributors
AppPublisherURL=https://github.com/jillsandwich1998/reGH2
AppSupportURL=https://github.com/jillsandwich1998/reGH2/issues
AppUpdatesURL=https://github.com/jillsandwich1998/reGH2/releases
VersionInfoVersion={#AppVersion}
DefaultDirName={autopf}\reGH2
DefaultGroupName=reGH2
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0.17763
OutputDir={#OutputDir}
OutputBaseFilename=reGH2-Setup-{#AppVersion}
SetupIconFile=reGH2.ico
UninstallDisplayIcon={app}\reGH2.exe
UninstallDisplayName=reGH2
WizardStyle=modern
LicenseFile=..\..\LICENSE
InfoBeforeFile=welcome.txt
Compression=lzma2/max
SolidCompression=yes
LZMAUseSeparateProcess=yes
CloseApplications=yes
CloseApplicationsFilter=reGH2.exe,gh2.exe
RestartApplications=no
UsePreviousAppDir=yes
DisableDirPage=auto

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "Create a &desktop icon"; GroupDescription: "Shortcuts:"

; An upgrade replaces the program folders whole (nothing of an older version may stay: a stale .py would still be imported). The built game and the saves live elsewhere and stay.
[InstallDelete]
Type: filesandordirs; Name: "{app}\scripts"
Type: filesandordirs; Name: "{app}\launcher"
Type: filesandordirs; Name: "{app}\app"
Type: filesandordirs; Name: "{app}\native"
Type: filesandordirs; Name: "{app}\sdk-patches"
Type: filesandordirs; Name: "{app}\sdk"
Type: filesandordirs; Name: "{app}\host"
Type: filesandordirs; Name: "{app}\llvm-mingw"
Type: filesandordirs; Name: "{app}\ninja"
Type: filesandordirs; Name: "{app}\shader-tools"
; (folders of the versions that compiled everything with Visual Studio's tools; gone since the game module)
Type: filesandordirs; Name: "{app}\llvm"
Type: filesandordirs; Name: "{app}\cmake"
Type: filesandordirs; Name: "{app}\vulkan"
Type: filesandordirs; Name: "{app}\deps"

[Files]
Source: "{#StageDir}\*"; DestDir: "{app}"; Flags: recursesubdirs createallsubdirs ignoreversion

[Icons]
Name: "{autoprograms}\reGH2"; Filename: "{app}\reGH2.exe"; Comment: "Install, update or start reGH2"
Name: "{autoprograms}\reGH2 - Play"; Filename: "{app}\reGH2.exe"; Parameters: "--play"; Comment: "Start Guitar Hero II (reGH2)"
Name: "{autodesktop}\reGH2"; Filename: "{app}\reGH2.exe"; Parameters: "--play"; Tasks: desktopicon; Comment: "Start Guitar Hero II (reGH2)"

[Run]
Filename: "{app}\reGH2.exe"; Description: "Open reGH2 (install the game from your own copy)"; Flags: nowait postinstall skipifsilent

; What the program made after it was installed: the pycache of its Python files, the clang++ link, the built game (several GB). Saves are asked about below.
[UninstallDelete]
Type: filesandordirs; Name: "{app}"
Type: filesandordirs; Name: "{localappdata}\reGH2"

[Code]
procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  Data: String;
begin
  if CurUninstallStep = usPostUninstall then
  begin
    Data := ExpandConstant('{localappdata}\gh2');
    { Only an interactive uninstall asks. A silent one (an update, a script) never deletes saves: with /SUPPRESSMSGBOXES Inno answers a Yes/No box with Yes, which would erase them. }
    if (not UninstallSilent) and DirExists(Data) then
      if MsgBox('Also delete your saves, installed DLC, achievements and the shader cache?' + #13#10 + #13#10 + Data + #13#10 + #13#10 +
                'This cannot be undone. Choose No to keep them (a later install finds them again).', mbConfirmation, MB_YESNO or MB_DEFBUTTON2) = IDYES then
        DelTree(Data, True, True, True);
  end;
end;
