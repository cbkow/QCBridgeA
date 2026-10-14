; QCBridgeA — Windows installer (Inno Setup 6).
;
;   packaging\windows\build-setup.cmd   (stages the exe, then this)
;
; Installs the sink under Program Files\QCBridgeA and registers the output
; with QCView by writing %ProgramData%\QCView\Outputs\aja.json. The exe is
; self-contained (static runtime, the AJA SDK linked in); the AJA device
; driver comes from AJA's own installer and is not part of this package.

#define MyAppName "QCBridgeA"
#define MyAppVersion "0.0.1"
#define MyAppPublisher "cbkow"
#define MyAppExeName "QCBridgeA.exe"
#define StageDir "..\..\build\stage"

[Setup]
AppId={{5B1D0C6E-2F3A-4F0E-9C61-7A1E4D2B8A3C}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppVerName={#MyAppName} {#MyAppVersion}
AppPublisher={#MyAppPublisher}
DefaultDirName={autopf}\{#MyAppName}
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
OutputDir=..\..\dist
OutputBaseFilename={#MyAppName}-{#MyAppVersion}-Setup-x64
Compression=lzma2
SolidCompression=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=admin
WizardStyle=modern
LicenseFile=..\..\LICENSE
UninstallDisplayIcon={app}\{#MyAppExeName}
CloseApplications=yes

[Messages]
WelcomeLabel2=This will install [name/ver] on your computer.%n%nQCBridgeA sends what QCView shows and plays out of an AJA device's SDI and HDMI outputs. It needs AJA's own software for the device driver (AJA Desktop Software 17.x) and QCView 2.5.7 or later. Afterwards, in QCView, open the Output section and press Start on the AJA row.

[Files]
Source: "{#StageDir}\{#MyAppExeName}"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#StageDir}\LICENSE.txt"; DestDir: "{app}"; Flags: ignoreversion
; QCView's registration of this output (O12).
Source: "aja.json"; DestDir: "{commonappdata}\QCView\Outputs"; Flags: ignoreversion

[Icons]
Name: "{group}\Uninstall {#MyAppName}"; Filename: "{uninstallexe}"

[UninstallDelete]
Type: files; Name: "{commonappdata}\QCView\Outputs\aja.json"
