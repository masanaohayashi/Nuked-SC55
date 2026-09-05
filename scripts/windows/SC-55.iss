#ifndef AppVersion
  #error AppVersion is required. Run package-release.ps1.
#endif
#ifndef Architecture
  #error Architecture is required. Run package-release.ps1.
#endif
#ifndef AllowedArchitectures
  #error AllowedArchitectures is required. Run package-release.ps1.
#endif
#ifndef Install64Architectures
  #error Install64Architectures is required. Run package-release.ps1.
#endif
#ifndef OutputBaseFilename
  #error OutputBaseFilename is required. Run package-release.ps1.
#endif
#ifndef OutputDir
  #error OutputDir is required. Run package-release.ps1.
#endif
#ifndef BuildRoot
  #error BuildRoot is required. Run package-release.ps1.
#endif

[Setup]
AppId=tokyo.studio-r.sc55
AppName=SC-55
AppVersion={#AppVersion}
AppVerName=SC-55 {#AppVersion} ({#Architecture})
AppPublisher=STUDIO-R
AppPublisherURL=https://studio-r.tokyo
DefaultDirName={autopf}\STUDIO-R\SC-55
DefaultGroupName=SC-55
OutputDir={#OutputDir}
OutputBaseFilename={#OutputBaseFilename}
ArchitecturesAllowed={#AllowedArchitectures}
ArchitecturesInstallIn64BitMode={#Install64Architectures}
PrivilegesRequired=admin
WizardStyle=modern
Compression=lzma2/ultra64
SolidCompression=yes
CloseApplications=yes
RestartApplications=no
VersionInfoVersion={#AppVersion}
VersionInfoCompany=STUDIO-R
VersionInfoDescription=SC-55 Windows {#Architecture} Installer
VersionInfoProductName=SC-55
VersionInfoProductVersion={#AppVersion}

[Types]
Name: "full"; Description: "Full installation"
Name: "custom"; Description: "Custom installation"; Flags: iscustom

[Components]
Name: "standalone"; Description: "SC-55 standalone application"; Types: full
Name: "vst3"; Description: "SC-55 VST3 plug-in"; Types: full

[Tasks]
Name: "desktopicon"; Description: "Create a desktop shortcut"; GroupDescription: "Additional shortcuts:"; Components: standalone; Flags: unchecked

[Files]
Source: "{#BuildRoot}\Standalone Plugin\SC-55.exe"; DestDir: "{app}"; Components: standalone; Flags: ignoreversion
Source: "{#BuildRoot}\VST3\SC-55.vst3\*"; DestDir: "{commoncf64}\VST3\SC-55.vst3"; Components: vst3; Flags: ignoreversion recursesubdirs createallsubdirs

[InstallDelete]
Type: filesandordirs; Name: "{commoncf64}\VST3\SC-55.vst3"; Components: vst3

[Icons]
Name: "{group}\SC-55"; Filename: "{app}\SC-55.exe"; Components: standalone
Name: "{autodesktop}\SC-55"; Filename: "{app}\SC-55.exe"; Components: standalone; Tasks: desktopicon

[Run]
Filename: "{app}\SC-55.exe"; Description: "Launch SC-55"; Components: standalone; Flags: nowait postinstall skipifsilent
