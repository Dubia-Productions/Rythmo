#ifndef AppVersion
  #define AppVersion "0.4.1"
#endif
#ifndef PackageDir
  #define PackageDir "package/Rythmo-windows-x64"
#endif
#ifndef OutputFile
  #define OutputFile "Rythmo-windows-x64-setup"
#endif

[Setup]
AppId={{10D36E3D-5D25-4939-92C0-5A9B0DAF7F82}
AppName=Rythmo
AppVersion={#AppVersion}
AppPublisher=Dubia Productions
DefaultDirName={localappdata}\Programs\Rythmo
PrivilegesRequired=lowest
DisableProgramGroupPage=yes
OutputDir=.
OutputBaseFilename={#OutputFile}
Compression=lzma2/fast
SolidCompression=yes
WizardStyle=modern
CloseApplications=yes
RestartApplications=no
ArchitecturesInstallIn64BitMode=x64 arm64
UninstallDisplayIcon={app}\Rythmo.exe

[Files]
Source: "{#PackageDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{autoprograms}\Rythmo"; Filename: "{app}\Rythmo.exe"
Name: "{autodesktop}\Rythmo"; Filename: "{app}\Rythmo.exe"; Tasks: desktopicon

[Tasks]
Name: "desktopicon"; Description: "Créer un raccourci sur le Bureau"; Flags: unchecked
