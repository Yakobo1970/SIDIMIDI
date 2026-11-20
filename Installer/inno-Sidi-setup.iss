; SIDiMIDI 2.0 – Setup (Inno Setup 7). Die AppId bleibt dieselbe wie bei Version 1.0, damit eine Installation darüber aktualisiert.
; Aufruf:  "C:\Program Files\Inno Setup 7\ISCC.exe" Installer\inno-Sidi-setup.iss   (zuvor: cmake --build build --config Release --target SIDIMIDI)

#define MyAppName "SIDiMIDI"
#define MyAppVersion "2.0.0"
#define MyAppPublisher "Un(d)AbtanzBAR"
#define MyAppURL "https://www.undabtanzbar.de/software/sidimidi/"
#define MyAppExeName "SIDiMIDI.exe"

[Setup]
AppId={{1FDF4C68-EC4D-4E7D-A4CB-56142905E3C7}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppVerName={#MyAppName} {#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}
AppUpdatesURL=https://github.com/Yakobo1970/SIDIMIDI/releases
VersionInfoVersion={#MyAppVersion}
DefaultDirName={autopf}\{#MyAppName}
UninstallDisplayIcon={app}\{#MyAppExeName}
SetupIconFile=SIDIMIDI.ico
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
DisableProgramGroupPage=yes
PrivilegesRequiredOverridesAllowed=dialog
OutputBaseFilename=SIDiMIDI_Setup_v{#MyAppVersion}
OutputDir=Output
SolidCompression=yes
WizardStyle=modern dynamic

[Languages]
Name: "german"; MessagesFile: "compiler:Languages\German.isl"
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "french"; MessagesFile: "compiler:Languages\French.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "..\build\SIDIMIDI_artefacts\Release\{#MyAppExeName}"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "{cm:LaunchProgram,{#StringChange(MyAppName, '&', '&&')}}"; Flags: nowait postinstall skipifsilent
