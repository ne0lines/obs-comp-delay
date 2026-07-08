#ifndef MyAppVersion
#define MyAppVersion "0.1.2"
#endif

#ifndef BuildDir
#define BuildDir "..\build_x64\RelWithDebInfo"
#endif

#ifndef Rundir
#define Rundir "..\build_x64\rundir\RelWithDebInfo"
#endif

#ifndef OutputDir
#define OutputDir "..\release"
#endif

[Setup]
AppId={{E1C4051E-9739-4E54-99C2-A81D54C60E77}
AppName=OBS Comp Delay
AppVersion={#MyAppVersion}
AppPublisher=ne0lines
AppPublisherURL=https://github.com/ne0lines/obs-comp-delay
AppSupportURL=https://github.com/ne0lines/obs-comp-delay/issues
AppUpdatesURL=https://github.com/ne0lines/obs-comp-delay/releases
DefaultDirName={code:GetDefaultObsDir}
DisableProgramGroupPage=yes
OutputDir={#OutputDir}
OutputBaseFilename=obs-comp-delay-{#MyAppVersion}-windows-x64-setup
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=admin
UninstallDisplayName=OBS Comp Delay

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Files]
Source: "{#BuildDir}\obs-comp-delay.dll"; DestDir: "{app}\obs-plugins\64bit"; Flags: ignoreversion
Source: "{#Rundir}\obs-comp-delay\*"; DestDir: "{app}\data\obs-plugins\obs-comp-delay"; Flags: ignoreversion recursesubdirs createallsubdirs

[UninstallDelete]
Type: files; Name: "{app}\obs-plugins\64bit\obs-comp-delay.dll"
Type: filesandordirs; Name: "{app}\data\obs-plugins\obs-comp-delay"

[Code]
function GetDefaultObsDir(Param: String): String;
begin
  if DirExists(ExpandConstant('{autopf}\obs-studio')) then
    Result := ExpandConstant('{autopf}\obs-studio')
  else
    Result := ExpandConstant('{autopf}\obs-studio');
end;

function InitializeSetup(): Boolean;
begin
  if MsgBox('Close OBS Studio before installing OBS Comp Delay. Continue?', mbConfirmation, MB_YESNO) = IDYES then
    Result := True
  else
    Result := False;
end;
