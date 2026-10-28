; Installer for the native DRA2 build: install -> play.
; Per-user install (no administrator rights), Start menu shortcut, optional
; desktop icon, launch at the end. The payload includes game data from your own
; disc, so the setup program is for your own machines only.
; Built by `reproduce.py --installer`, which passes StageDir (the executable,
; vcruntime140.dll and README.txt), GameDir (the extracted disc) and Version.
; /DTest builds an extract-only variant for verification.

#ifndef StageDir
  #error StageDir must be defined (build.py passes it)
#endif
#ifndef GameDir
  #error GameDir must be defined (the verified extraction)
#endif
#ifdef Test
  #define Suffix "-test"
#else
  #define Suffix ""
#endif
#ifndef Version
  #define Version "0.1.0"
#endif

[Setup]
AppId={{6E1B9F3A-2C4D-4E8B-9A71-0D2A2F0A7100}
AppName=Digimon Rumble Arena 2 (native port)
AppVersion={#Version}
AppPublisher=DRA2 native port (private build)
DefaultDirName={localappdata}\Programs\DigimonRumbleArena2
DefaultGroupName=Digimon Rumble Arena 2
PrivilegesRequired=lowest
DisableProgramGroupPage=yes
OutputDir=output
OutputBaseFilename=DigimonRumbleArena2-Setup-{#Version}{#Suffix}
Compression=lzma2/max
SolidCompression=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
UninstallDisplayIcon={app}\DigimonRumbleArena2.exe
WizardStyle=modern
#ifdef IconFile
; The game icon for the setup program (shortcuts use the exe's own icon).
SetupIconFile={#IconFile}
#endif
#ifdef Test
; Verification build: extracts only -- no uninstaller, registry entry, icons or launch.
Uninstallable=no
CreateUninstallRegKey=no
#endif

#ifndef Test
[Tasks]
Name: "desktopicon"; Description: "Create a desktop icon"; GroupDescription: "Additional icons:"
#endif

[Files]
Source: "{#StageDir}\DigimonRumbleArena2.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#StageDir}\vcruntime140.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#StageDir}\SDL3.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#StageDir}\libusb-1.0.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#StageDir}\licenses\*"; DestDir: "{app}\licenses"; Flags: ignoreversion
Source: "{#StageDir}\README.txt"; DestDir: "{app}"; Flags: ignoreversion isreadme
Source: "{#GameDir}\*"; DestDir: "{app}\game"; Excludes: "UDATA\*"; Flags: ignoreversion recursesubdirs createallsubdirs

#ifndef Test
[Icons]
Name: "{autoprograms}\Digimon Rumble Arena 2"; Filename: "{app}\DigimonRumbleArena2.exe"; WorkingDir: "{app}"
Name: "{autodesktop}\Digimon Rumble Arena 2"; Filename: "{app}\DigimonRumbleArena2.exe"; WorkingDir: "{app}"; Tasks: desktopicon

[Run]
Filename: "{app}\DigimonRumbleArena2.exe"; Description: "Play now"; WorkingDir: "{app}"; Flags: nowait postinstall skipifsilent
#endif
