#ifndef AppVersion
  #error AppVersion must be supplied by build-windows-installer.ps1
#endif
#ifndef NumericVersion
  #error NumericVersion must be supplied by build-windows-installer.ps1
#endif
#ifndef AllowedArchitectures
  #error AllowedArchitectures must be supplied by build-windows-installer.ps1
#endif
#ifndef OutputBaseFilename
  #error OutputBaseFilename must be supplied by build-windows-installer.ps1
#endif
#ifndef OutputDir
  #error OutputDir must be supplied by build-windows-installer.ps1
#endif
#ifndef PayloadDir
  #error PayloadDir must be supplied by build-windows-installer.ps1
#endif

[Setup]
AppId={{9C95442B-6351-4E5D-86C8-D3A0A0A15175}
AppName=Codebase Memory
AppVersion={#AppVersion}
AppVerName=Codebase Memory {#AppVersion}
AppPublisher=ycsx/codebase-memory-mcp
AppPublisherURL=https://github.com/ycsx/codebase-memory-mcp
AppSupportURL=https://github.com/ycsx/codebase-memory-mcp/issues
AppUpdatesURL=https://github.com/ycsx/codebase-memory-mcp/releases
VersionInfoVersion={#NumericVersion}
DefaultDirName={localappdata}\Programs\codebase-memory-mcp
DefaultGroupName=Codebase Memory
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
ArchitecturesAllowed={#AllowedArchitectures}
ArchitecturesInstallIn64BitMode={#AllowedArchitectures}
OutputDir={#OutputDir}
OutputBaseFilename={#OutputBaseFilename}
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
LicenseFile={#PayloadDir}\LICENSE
UninstallDisplayName=Codebase Memory
UninstallDisplayIcon={app}\desktop\Codebase Memory.exe
ChangesEnvironment=yes
CloseApplications=yes
RestartApplications=no
SetupLogging=yes

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "Create a &desktop shortcut"; GroupDescription: "Additional shortcuts:"; Flags: unchecked

[Files]
Source: "{#PayloadDir}\codebase-memory-mcp.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#PayloadDir}\LICENSE"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#PayloadDir}\THIRD_PARTY_NOTICES.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#PayloadDir}\desktop\*"; DestDir: "{app}\desktop"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\Codebase Memory"; Filename: "{app}\desktop\Codebase Memory.exe"; WorkingDir: "{app}\desktop"
Name: "{group}\Uninstall Codebase Memory"; Filename: "{uninstallexe}"
Name: "{autodesktop}\Codebase Memory"; Filename: "{app}\desktop\Codebase Memory.exe"; WorkingDir: "{app}\desktop"; Tasks: desktopicon

[Run]
Filename: "{app}\codebase-memory-mcp.exe"; Parameters: "install -y"; StatusMsg: "Configuring coding agents..."; Flags: runhidden waituntilterminated
Filename: "{app}\desktop\Codebase Memory.exe"; Description: "Launch Codebase Memory"; Flags: nowait postinstall skipifsilent; Check: not LaunchAfterUpdate
Filename: "{app}\desktop\Codebase Memory.exe"; Flags: nowait; Check: LaunchAfterUpdate

[InstallDelete]
Type: files; Name: "{app}\codebase-memory-console.vbs"
Type: files; Name: "{group}\Codebase Memory Console.lnk"
Type: files; Name: "{autodesktop}\Codebase Memory Console.lnk"

[UninstallDelete]
Type: files; Name: "{app}\codebase-memory-mcp.exe.old"

[Code]
const
  InstallerKey = 'Software\CodebaseMemoryMCP\Installer';
  PathOwnerValue = 'PathOwner';

function LaunchAfterUpdate(): Boolean;
begin
  Result := CompareText(ExpandConstant('{param:LaunchAfterUpdate|0}'), '1') = 0;
end;

function NormalizePathEntry(Value: String): String;
begin
  Result := Trim(Value);
  if (Length(Result) >= 2) and (Result[1] = '"') and
     (Result[Length(Result)] = '"') then
    Result := Copy(Result, 2, Length(Result) - 2);
  while (Length(Result) > 3) and
        ((Result[Length(Result)] = '\') or (Result[Length(Result)] = '/')) do
    Delete(Result, Length(Result), 1);
end;

function SamePath(Left, Right: String): Boolean;
begin
  Result := CompareText(NormalizePathEntry(Left), NormalizePathEntry(Right)) = 0;
end;

function TakePathEntry(var Remaining: String): String;
var
  Separator: Integer;
begin
  Separator := Pos(';', Remaining);
  if Separator = 0 then
  begin
    Result := Remaining;
    Remaining := '';
  end
  else
  begin
    Result := Copy(Remaining, 1, Separator - 1);
    Delete(Remaining, 1, Separator);
  end;
end;

function PathContains(CurrentPath, Entry: String): Boolean;
var
  Part: String;
begin
  Result := False;
  while CurrentPath <> '' do
  begin
    Part := TakePathEntry(CurrentPath);
    if SamePath(Part, Entry) then
    begin
      Result := True;
      Exit;
    end;
  end;
end;

procedure AddToUserPath;
var
  AppDir: String;
  CurrentPath: String;
  Owner: String;
begin
  if CompareText(ExpandConstant('{param:NoUserPath|0}'), '1') = 0 then
    Exit;

  AppDir := ExpandConstant('{app}');
  if not RegQueryStringValue(HKCU, 'Environment', 'Path', CurrentPath) then
    CurrentPath := '';
  if PathContains(CurrentPath, AppDir) then
    Exit;

  if CurrentPath = '' then
    CurrentPath := AppDir
  else
    CurrentPath := CurrentPath + ';' + AppDir;
  if RegWriteExpandStringValue(HKCU, 'Environment', 'Path', CurrentPath) then
  begin
    Owner := AppDir;
    RegWriteStringValue(HKCU, InstallerKey, PathOwnerValue, Owner);
  end;
end;

procedure RemoveFromUserPath;
var
  AppDir: String;
  CurrentPath: String;
  Owner: String;
  Part: String;
  UpdatedPath: String;
begin
  AppDir := ExpandConstant('{app}');
  if not RegQueryStringValue(HKCU, InstallerKey, PathOwnerValue, Owner) or
     not SamePath(Owner, AppDir) then
    Exit;
  if not RegQueryStringValue(HKCU, 'Environment', 'Path', CurrentPath) then
    Exit;

  UpdatedPath := '';
  while CurrentPath <> '' do
  begin
    Part := TakePathEntry(CurrentPath);
    if (Trim(Part) <> '') and not SamePath(Part, AppDir) then
    begin
      if UpdatedPath <> '' then
        UpdatedPath := UpdatedPath + ';';
      UpdatedPath := UpdatedPath + Part;
    end;
  end;
  RegWriteExpandStringValue(HKCU, 'Environment', 'Path', UpdatedPath);
  RegDeleteValue(HKCU, InstallerKey, PathOwnerValue);
  RegDeleteKeyIfEmpty(HKCU, InstallerKey);
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssPostInstall then
    AddToUserPath;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usUninstall then
    RemoveFromUserPath;
end;
