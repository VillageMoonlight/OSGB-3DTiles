; ─────────────────────────────────────────────────────────────
; OSGB to 3D Tiles Tool  -  Inno Setup Script
; ─────────────────────────────────────────────────────────────
#define AppName    "OSGB 2 3DTiles"
#define AppVersion "2.2"
#define AppPublisher "AntiGravity GIS"
#define AppExeGUI  "osgb2tiles_gui.exe"
#define AppExeCLI  "osgb2tiles.exe"
#define SrcDir     "build\bin"

[Setup]
AppId={{B3A7C2F1-8E4D-4A6B-9C3E-2F1D8A5B7E9C}
AppName={#AppName}
AppVersion={#AppVersion}
AppPublisher={#AppPublisher}
DefaultDirName={autopf}\OSGB2Tiles
DefaultGroupName={#AppName}
AllowNoIcons=yes
OutputDir=dist
OutputBaseFilename=OSGB2Tiles_v{#AppVersion}_Setup
SetupIconFile={#SrcDir}\osgb2tiles_gui.ico
Compression=lzma2/ultra64
SolidCompression=yes
LZMAUseSeparateProcess=yes
PrivilegesRequired=admin
WizardStyle=modern
WizardSizePercent=120
MinVersion=10.0
ArchitecturesInstallIn64BitMode=x64compatible

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "Create &desktop shortcut (GUI)"; GroupDescription: "Additional:"; Flags: unchecked

[Files]
Source: "{#SrcDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#SrcDir}\vcredist_x64.exe"; DestDir: "{tmp}"; Flags: deleteafterinstall

[Icons]
Name: "{group}\OSGB2Tiles GUI"; Filename: "{app}\{#AppExeGUI}"; WorkingDir: "{app}"; IconFilename: "{app}\osgb2tiles_gui.ico"
Name: "{group}\OSGB2Tiles CLI (readme)"; Filename: "{app}\{#AppExeCLI}"; Parameters: "--help"; WorkingDir: "{app}"
Name: "{group}\Uninstall {#AppName}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\OSGB2Tiles"; Filename: "{app}\{#AppExeGUI}"; WorkingDir: "{app}"; IconFilename: "{app}\osgb2tiles_gui.ico"; Tasks: desktopicon

[Run]
Filename: "{tmp}\vcredist_x64.exe"; Parameters: "/install /quiet /norestart"; StatusMsg: "Installing Visual C++ Redistributable..."; Flags: waituntilterminated runhidden; Check: VCRedistNeedsInstall
Filename: "{app}\{#AppExeGUI}"; Description: "Launch OSGB2Tiles GUI now"; Flags: nowait postinstall skipifsilent

[Code]
function VCRedistNeedsInstall: Boolean;
var
  sVersion: String;
  iMajor: Integer;
begin
  Result := True;
  if RegQueryStringValue(HKEY_LOCAL_MACHINE,
    'SOFTWARE\Microsoft\VisualStudio\14.0\VC\Runtimes\x64',
    'Version', sVersion) then
  begin
    iMajor := StrToIntDef(Copy(sVersion, 2, 2), 0);
    if iMajor >= 14 then
    begin
      Result := False;
      Log('VC++ already installed: ' + sVersion);
    end;
  end;
end;
