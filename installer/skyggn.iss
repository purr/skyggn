; skyggn's installer (inno setup 6.7). build it with cmake, which passes the version and folders:
;   cmake --build --preset release --target installer

#ifndef AppVersion
  #error "build the installer with: cmake --build --preset release --target installer"
#endif

[Setup]
; identifies skyggn to windows across versions: an install over an older one upgrades it
AppId={{535D4990-130A-45BC-BC49-CE85116105F5}
AppName=skyggn
AppVersion={#AppVersion}
AppVerName=skyggn {#AppVersion}
AppPublisher=skyggn
VersionInfoVersion={#AppVersion}
DefaultDirName={autopf}\skyggn
DisableDirPage=yes
DisableProgramGroupPage=yes
; thumbnails are registered for every user of the pc, which needs admin rights: one uac prompt
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0.19041
WizardStyle=modern dynamic windows11
WizardImageFile={#ImageDir}\wizard-image.png
WizardSmallImageFile={#ImageDir}\wizard-small.png
SetupIconFile={#RepoDir}\app\Assets\skyggn.ico
UninstallDisplayIcon={app}\skyggn.exe
UninstallDisplayName=skyggn
OutputDir={#OutputDir}
OutputBaseFilename=skyggn-{#AppVersion}-x64-setup
Compression=lzma2/max
SolidCompression=yes
; file explorer keeps the engine loaded (for files' details), and windows' thumbnail helper
; (dllhost.exe) may hold it open: the restart manager closes them, and anything else using the
; files, then starts them again
CloseApplications=yes
RestartApplications=yes
; the wizard speaks windows' language when it has it (see [Languages]), without asking first
ShowLanguageDialog=auto

; the wizard follows windows' language: german on a german windows, english otherwise
[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "german"; MessagesFile: "compiler:Languages\German.isl"

[CustomMessages]
english.OpenApp=Open skyggn
german.OpenApp=skyggn öffnen
english.ControlFailed=skyggnctl %1 failed (exit code %2).
german.ControlFailed=skyggnctl %1 ist fehlgeschlagen (Exitcode %2).

[Files]
; a file still in use (explorer not closed) is replaced, or on uninstall removed, at the next restart
Source: "{#SourceDir}\*"; DestDir: "{app}"; Excludes: "*.pdb"; Flags: ignoreversion recursesubdirs createallsubdirs restartreplace uninsrestartdelete

[Icons]
Name: "{autoprograms}\skyggn"; Filename: "{app}\skyggn.exe"

[Registry]
; the uninstalling user's own settings go with the program
Root: HKCU; Subkey: "Software\skyggn"; Flags: uninsdeletekey dontcreatekey

[Run]
Filename: "{app}\skyggn.exe"; Description: "{cm:OpenApp}"; Flags: postinstall nowait skipifsilent runasoriginaluser

[Code]
var
  { the dlls of the install being updated, moved aside by PrepareToInstall: where each was, and
    where it went }
  MovedFrom, MovedTo: TArrayOfString;
  Installed: Boolean;

procedure RunControl(const Arguments: String);
var
  ResultCode: Integer;
begin
  if not Exec(ExpandConstant('{app}\skyggnctl.exe'), Arguments, '', SW_HIDE, ewWaitUntilTerminated, ResultCode)
    or (ResultCode <> 0) then
    RaiseException(FmtMessage(CustomMessage('ControlFailed'), [Arguments, IntToStr(ResultCode)]));
end;

{ windows' thumbnail helper (dllhost.exe) keeps the engine and ffmpeg loaded for a while after the
  last thumbnail, and so does any program that showed a file's details. the restart manager cannot
  close the helper, so setup stopped with "unable to close all applications", and a silent update
  gave up. a loaded dll can still be renamed: each one moves aside before setup checks for files in
  use, the new one takes its name, and the old copy goes once nothing has it loaded. }
function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  Found: TFindRec;
  Folder, Stamp: String;
  Count: Integer;
begin
  Result := '';
  Folder := ExpandConstant('{app}\');
  Stamp := GetDateTimeString('yyyymmddhhnnss', #0, #0);
  { going back from the preparing page and on again calls this twice: what moved first stays listed }
  Count := GetArrayLength(MovedFrom);
  if FindFirst(Folder + '*.dll', Found) then
  try
    repeat
      SetArrayLength(MovedFrom, Count + 1);
      SetArrayLength(MovedTo, Count + 1);
      MovedFrom[Count] := Folder + Found.Name;
      MovedTo[Count] := Folder + Found.Name + '.' + Stamp + '.old';
      if RenameFile(MovedFrom[Count], MovedTo[Count]) then
        Count := Count + 1;
    until not FindNext(Found);
  finally
    FindClose(Found);
  end;
  SetArrayLength(MovedFrom, Count);
  SetArrayLength(MovedTo, Count);
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  I: Integer;
begin
  if CurStep = ssPostInstall then
  begin
    Installed := True;
    { an old copy still loaded somewhere is deleted when windows next starts }
    for I := 0 to GetArrayLength(MovedTo) - 1 do
      if not DeleteFile(MovedTo[I]) then
        RestartReplace(MovedTo[I], '');
    { a test install for this account (skyggnctl install --user) would take priority over the one
      for every user, so it is given back first; with none, this changes nothing }
    RunControl('uninstall --user');
    RunControl('install');
  end;
end;

procedure DeinitializeSetup;
var
  I: Integer;
begin
  { setup stopped after the old dlls moved aside (cancelled, or a file could not be written): they
    go back where no new one took their place, so the old version keeps working }
  if not Installed then
    for I := 0 to GetArrayLength(MovedFrom) - 1 do
      if not FileExists(MovedFrom[I]) then
        RenameFile(MovedTo[I], MovedFrom[I]);
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  { while the files still exist: every file type gets its previous thumbnail handler back, so no
    entry points to a removed program afterwards }
  if CurUninstallStep = usUninstall then
    RunControl('uninstall');
end;
