; 3DO2PC.iss - Inno Setup 6 script for the "3DO to PC" Windows installer.
;
; Packs what Build-App.bat puts in dist\ (the app, the launcher, the CLI,
; tdocore.dll, runtime\) into one Setup .exe with Start-menu shortcuts and an
; uninstaller. Build it with Build-Installer.bat, or by hand:
;   "C:\Program Files (x86)\Inno Setup 6\ISCC.exe" /DMyAppVersion=1.0 installer\3DO2PC.iss
; The Setup file is written to installer\Output\.
;
; Install location: by default for the current user only, in
; %LOCALAPPDATA%\Programs\3DO to PC (no administrator rights needed, and the
; folder stays writable for the runtime cache and the games\ folder). Setup
; also offers "all users" (Program Files); the cache\ and games\ folders are
; then made writable for users.
;
; Converting games needs Visual Studio C++ Build Tools, and the two windowed
; apps need the .NET 8 (or newer) desktop runtime. Setup checks for both and, if
; either is missing, offers to run Install-Prerequisites.ps1 at the end (it
; asks for administrator rights and downloads only what is missing).

#ifndef MyAppVersion
  #define MyAppVersion "1.0"
#endif
#define MyAppName "3DO to PC"
#define MyAppPublisher "3DO to PC"
#define MyAppExe "3DO to PC.exe"
#define MyLauncherExe "3DO Launcher.exe"
#define DistDir "..\dist"

[Setup]
; Keep this AppId for every release: it is how Setup finds an older install to upgrade.
AppId={{6E0F3B2A-9C41-4D7B-8A35-3D0C2F1B7E64}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppVerName={#MyAppName} {#MyAppVersion}
AppPublisher={#MyAppPublisher}
DefaultDirName={autopf}\{#MyAppName}
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
OutputDir=Output
OutputBaseFilename=3DO2PC-{#MyAppVersion}-Setup
SetupIconFile=..\assets\3do2pc.ico
UninstallDisplayIcon={app}\{#MyAppExe}
UninstallDisplayName={#MyAppName}
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
CloseApplications=yes
RestartApplications=no
ChangesEnvironment=no

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "Create desktop shortcuts"; GroupDescription: "Additional shortcuts:"; Flags: unchecked

[Files]
; The apps, the converter core and the runtime sources games are built from.
Source: "{#DistDir}\*"; DestDir: "{app}"; Excludes: "*.pdb,README.md,games\*,cache\*"; Flags: ignoreversion recursesubdirs createallsubdirs
; One-time setup of the compiler tools (run from the last page, or later from the Start menu).
Source: "..\Install-Prerequisites.ps1"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\Install-Prerequisites.bat"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\README.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\assets\3do2pc.ico"; DestDir: "{app}"; Flags: ignoreversion

[Dirs]
; Converted games (the launcher looks here first) and the compiled-runtime cache.
Name: "{app}\games"; Permissions: users-modify
Name: "{app}\cache"; Permissions: users-modify

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExe}"; WorkingDir: "{app}"
Name: "{group}\3DO Launcher"; Filename: "{app}\{#MyLauncherExe}"; WorkingDir: "{app}"
Name: "{group}\Converted games folder"; Filename: "{app}\games"
Name: "{group}\Install the tools for converting games"; Filename: "{sys}\WindowsPowerShell\v1.0\powershell.exe"; Parameters: "-NoProfile -ExecutionPolicy Bypass -File ""{app}\Install-Prerequisites.ps1"" -Installed"; WorkingDir: "{app}"; IconFilename: "{app}\3do2pc.ico"
Name: "{group}\Uninstall {#MyAppName}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExe}"; WorkingDir: "{app}"; Tasks: desktopicon
Name: "{autodesktop}\3DO Launcher"; Filename: "{app}\{#MyLauncherExe}"; WorkingDir: "{app}"; Tasks: desktopicon

[InstallDelete]
; Left by the Nov 22 test build that bundled its own compiler: remove it so Visual Studio is used.
Type: filesandordirs; Name: "{app}\toolchain"

[Run]
; Offered (and ticked) only when the compiler tools or the .NET desktop runtime are missing.
; The script asks for administrator rights itself and installs only what is missing.
Filename: "{sys}\WindowsPowerShell\v1.0\powershell.exe"; Parameters: "-NoProfile -ExecutionPolicy Bypass -File ""{app}\Install-Prerequisites.ps1"" -Installed"; WorkingDir: "{app}"; Description: "Install the tools needed to convert games (Visual Studio C++ Build Tools, .NET 8) - internet, a few GB"; Flags: postinstall skipifsilent runasoriginaluser; Check: PrerequisitesMissing
Filename: "{app}\{#MyAppExe}"; Description: "Start {#MyAppName}"; Flags: postinstall skipifsilent nowait unchecked; Check: DotNetDesktopInstalled

[UninstallDelete]
; The runtime cache is rebuilt on demand; converted games in games\ are kept.
Type: filesandordirs; Name: "{app}\cache"

[Code]
// .NET desktop runtime 8 or newer (the apps roll forward to newer majors).
function DotNetDesktopInstalled: Boolean;
var
  FindRec: TFindRec;
  Dir, Name: String;
  Major, Dot: Integer;
begin
  Result := False;
  Dir := ExpandConstant('{commonpf64}\dotnet\shared\Microsoft.WindowsDesktop.App\');
  if FindFirst(Dir + '*', FindRec) then
  begin
    try
      repeat
        if (FindRec.Attributes and FILE_ATTRIBUTE_DIRECTORY) <> 0 then
        begin
          Name := FindRec.Name;
          Dot := Pos('.', Name);
          if Dot > 1 then
          begin
            Major := StrToIntDef(Copy(Name, 1, Dot - 1), 0);
            if Major >= 8 then Result := True;
          end;
        end;
      until Result or not FindNext(FindRec);
    finally
      FindClose(FindRec);
    end;
  end;
end;

// Visual Studio (or Build Tools) with the x64 C++ compiler, found through vswhere.
function BuildToolsInstalled: Boolean;
var
  VsWhere, OutFile: String;
  Lines: TArrayOfString;
  Code: Integer;
begin
  Result := False;
  VsWhere := ExpandConstant('{commonpf32}\Microsoft Visual Studio\Installer\vswhere.exe');
  if not FileExists(VsWhere) then Exit;
  OutFile := ExpandConstant('{tmp}\vswhere.txt');
  if Exec(ExpandConstant('{cmd}'),
          '/C ""' + VsWhere + '" -all -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath > "' + OutFile + '""',
          '', SW_HIDE, ewWaitUntilTerminated, Code) then
  begin
    if LoadStringsFromFile(OutFile, Lines) then
      Result := (GetArrayLength(Lines) > 0) and (Trim(Lines[0]) <> '');
  end;
end;

function PrerequisitesMissing: Boolean;
begin
  Result := (not DotNetDesktopInstalled) or (not BuildToolsInstalled);
end;
