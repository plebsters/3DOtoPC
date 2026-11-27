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
; Nothing else needs to be installed: the package includes the compiler that
; builds converted games (toolchain\: zig, CMake, Ninja) and the windowed apps
; are self-contained (the .NET runtime is included). Visual Studio is no longer
; needed; if it is installed, the bundled compiler is still used first.

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
; Excludes starting with "\" match only at the top of dist\: zig has a folder named
; Cache of its own (toolchain\zig\lib\std\Build\Cache), which "cache\*" also left out.
Source: "{#DistDir}\*"; DestDir: "{app}"; Excludes: "*.pdb,\README.md,\games\*,\cache\*"; Flags: ignoreversion recursesubdirs createallsubdirs
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
Name: "{group}\Uninstall {#MyAppName}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExe}"; WorkingDir: "{app}"; Tasks: desktopicon
Name: "{autodesktop}\3DO Launcher"; Filename: "{app}\{#MyLauncherExe}"; WorkingDir: "{app}"; Tasks: desktopicon

[InstallDelete]
; The bundled compiler is installed fresh each time (an older version's files,
; or the Nov 22 test build's LLVM folder, are not left mixed in).
Type: filesandordirs; Name: "{app}\toolchain"
Type: files; Name: "{app}\Install-Prerequisites.ps1"
Type: files; Name: "{app}\Install-Prerequisites.bat"

[Run]
Filename: "{app}\{#MyAppExe}"; Description: "Start {#MyAppName}"; Flags: postinstall skipifsilent nowait

[UninstallDelete]
; The runtime cache is rebuilt on demand; converted games in games\ are kept.
Type: filesandordirs; Name: "{app}\cache"
