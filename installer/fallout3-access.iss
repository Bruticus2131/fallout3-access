; Fallout 3 Access — installer
;
; The wizard collects two things (where the game lives, and a Nexus API key) and
; then hands off to installer\f3a-install.ps1, which does the downloading and
; unpacking. Keeping the work in PowerShell rather than Pascal Script means it
; can be run and debugged on its own — most of what goes wrong here is network-
; or account-shaped, not UI-shaped.
;
; Build:  "C:\Users\<you>\AppData\Local\Programs\Inno Setup 6\ISCC.exe" installer\fallout3-access.iss

#define AppName      "Fallout 3 Access"
#define AppVersion   "0.3.0"
#define AppPublisher "Maciej Krynicki"
#define AppURL       "https://github.com/Bruticus2131/fallout3-access"

[Setup]
AppId={{9C2E5C1E-2C5B-4E7B-9E3E-F3A0AC0E5510}
AppName={#AppName}
AppVersion={#AppVersion}
AppPublisher={#AppPublisher}
AppSupportURL={#AppURL}
DefaultDirName={autopf}\Fallout3Access
DisableDirPage=yes
DisableProgramGroupPage=yes
UninstallDisplayName={#AppName}
OutputDir=..\dist
OutputBaseFilename=fallout3-access-setup-{#AppVersion}
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
PrivilegesRequired=admin
; The game folder is usually under Program Files, and the patcher rewrites
; Fallout3.exe in place, so admin rights are needed for a normal Steam install.

[Languages]
Name: "pl"; MessagesFile: "compiler:Languages\Polish.isl"
Name: "en"; MessagesFile: "compiler:Default.isl"

[Files]
Source: "f3a-install.ps1"; DestDir: "{app}"; Flags: ignoreversion

[Registry]
; Declared here only so the uninstaller takes it away again. The value itself is
; written after a successful run — see CurStepChanged.
Root: HKLM; Subkey: "Software\Fallout3Access"; Flags: uninsdeletekey

[Run]
Filename: "{app}\install-log.txt"; Description: "Pokaż dziennik instalacji"; \
  Flags: postinstall shellexec skipifsilent unchecked

[Code]
var
  GamePage:    TInputDirWizardPage;
  MethodPage:  TInputOptionWizardPage;   // how to get the patcher
  KeyPage:     TInputQueryWizardPage;    // only when the API route is chosen
  FilePage:    TInputFileWizardPage;     // only when the manual route is chosen
  OptPage:     TInputOptionWizardPage;
  GameNeedsPatch: Boolean;

// Steam and GOG both record the install path here, so most users never have to
// find it themselves. A previous run of this installer is asked FIRST: that is
// the folder the user actually chose, which matters on a machine with more than
// one copy of the game, and it turns an update into clicking Next.
function DetectGamePath(): String;
var
  Path: String;
begin
  Result := '';
  if RegQueryStringValue(HKLM, 'Software\Fallout3Access', 'GamePath', Path) and (Path <> '') then
    Result := Path
  else if RegQueryStringValue(HKLM, 'SOFTWARE\WOW6432Node\Bethesda Softworks\Fallout3', 'Installed Path', Path) then
    Result := Path
  else if RegQueryStringValue(HKLM, 'SOFTWARE\Bethesda Softworks\Fallout3', 'Installed Path', Path) then
    Result := Path;
end;

// Is this build already the one FOSE supports? The executable carries no
// version resource, so go by size. Sizes are the ones measured on real
// installs; an unrecognised size is treated as "probably needs the patch",
// which is the safe way round — the patcher refuses a game it cannot patch,
// whereas skipping it on 1.7.0.4 leaves the user with a mod that does nothing.
function GameIsSupportedBuild(const Dir: String): Boolean;
var
  Size: Int64;
begin
  Result := False;
  if FileSize64(AddBackslash(Dir) + 'Fallout3.exe', Size) then
    Result := (Size = 15038976);
end;

procedure InitializeWizard();
var
  Detected: String;
begin
  GameNeedsPatch := True;

  GamePage := CreateInputDirPage(wpWelcome,
    'Folder gry', 'Gdzie jest zainstalowany Fallout 3?',
    'Wskaż folder zawierający Fallout3.exe. Jeśli gra została wykryta, ścieżka jest już wpisana.',
    False, '');
  GamePage.Add('');
  Detected := DetectGamePath();
  if Detected <> '' then
    GamePage.Values[0] := Detected;

  // Shown only when the game actually needs downgrading.
  MethodPage := CreateInputOptionPage(GamePage.ID,
    'Łatka obniżająca wersję', 'Twoja wersja gry wymaga łatki, żeby FOSE działał',
    'Łatkę udostępnia Nexus Mods. Nexus pozwala pobierać automatycznie tylko ' +
    'kontom Premium, więc domyślnie po prostu otworzę stronę, a Ty wskażesz pobrany plik.',
    True, False);
  MethodPage.Add('Pobiorę sam — otwórz stronę i pozwól wskazać plik (nie wymaga konta Premium)');
  MethodPage.Add('Mam klucz API Nexusa i konto Premium — pobierz automatycznie');
  MethodPage.Values[0] := True;

  KeyPage := CreateInputQueryPage(MethodPage.ID,
    'Klucz Nexus Mods', 'Klucz z ustawień konta',
    'Znajdziesz go na nexusmods.com w Ustawieniach konta, w sekcji API Access ' +
    '("Personal API Key").');
  KeyPage.Add('Klucz API:', False);

  FilePage := CreateInputFilePage(MethodPage.ID,
    'Pobrana łatka', 'Wskaż archiwum pobrane z Nexusa',
    'Kliknij "Otwórz stronę", pobierz łatkę, a potem wskaż pobrany plik ' +
    '(zwykle trafia do folderu Pobrane).');
  FilePage.Add('Plik łatki:', 'Archiwa|*.zip;*.7z;*.rar|Wszystkie pliki|*.*', '');

  OptPage := CreateInputOptionPage(wpSelectDir,
    'Co zainstalować', 'Możesz pominąć elementy, które już masz',
    '', False, False);
  OptPage.Add('Zainstaluj FOSE (wymagany do działania moda)');
  OptPage.Add('Zainstaluj / zaktualizuj Fallout 3 Access');
  OptPage.Values[0] := True;
  OptPage.Values[1] := True;
end;

// The patcher pages exist only for a game that needs patching, and only the one
// matching the chosen method is shown. A user on a supported build never sees
// any of them — and is never asked for an API key.
function ShouldSkipPage(PageID: Integer): Boolean;
begin
  Result := False;
  if PageID = MethodPage.ID then
    Result := not GameNeedsPatch
  else if PageID = KeyPage.ID then
    Result := (not GameNeedsPatch) or (not MethodPage.Values[1])
  else if PageID = FilePage.ID then
    Result := (not GameNeedsPatch) or (not MethodPage.Values[0]);
end;

function NextButtonClick(CurPageID: Integer): Boolean;
var
  ErrorCode: Integer;
begin
  Result := True;

  if CurPageID = GamePage.ID then
  begin
    if not FileExists(AddBackslash(GamePage.Values[0]) + 'Fallout3.exe') then
    begin
      MsgBox('W tym folderze nie ma Fallout3.exe. Wskaż folder, w którym jest gra.',
             mbError, MB_OK);
      Result := False;
      Exit;
    end;
    GameNeedsPatch := not GameIsSupportedBuild(GamePage.Values[0]);
    if not GameNeedsPatch then
      MsgBox('Twoja gra jest już w wersji 1.7.0.3 — łatka nie jest potrzebna.',
             mbInformation, MB_OK);
  end

  else if CurPageID = MethodPage.ID then
  begin
    if MethodPage.Values[0] then
    begin
      // Open the mod page for them; the next page takes the downloaded file.
      ShellExecAsOriginalUser('open', 'https://www.nexusmods.com/fallout3/mods/24913',
                              '', '', SW_SHOW, ewNoWait, ErrorCode);
    end;
  end

  else if CurPageID = FilePage.ID then
  begin
    if (Trim(FilePage.Values[0]) <> '') and (not FileExists(FilePage.Values[0])) then
    begin
      MsgBox('Nie znajduję wskazanego pliku.', mbError, MB_OK);
      Result := False;
    end;
  end;
end;

// Everything happens after the files are copied: the helper script is on disk by
// then, and its output goes to a log the user can open from the finish page.
procedure CurStepChanged(CurStep: TSetupStep);
var
  Params, LogFile: String;
  ResultCode: Integer;
begin
  if CurStep <> ssPostInstall then
    Exit;

  LogFile := AddBackslash(ExpandConstant('{app}')) + 'install-log.txt';

  Params := '-NoProfile -ExecutionPolicy Bypass -File "' +
            ExpandConstant('{app}\f3a-install.ps1') + '"' +
            ' -GamePath "' + GamePage.Values[0] + '"' +
            ' -LogPath "' + LogFile + '"';

  if not GameNeedsPatch then
    Params := Params + ' -SkipPatcher'
  else if MethodPage.Values[1] and (Trim(KeyPage.Values[0]) <> '') then
    Params := Params + ' -NexusApiKey "' + Trim(KeyPage.Values[0]) + '"'
  else if Trim(FilePage.Values[0]) <> '' then
    Params := Params + ' -PatcherArchive "' + FilePage.Values[0] + '"'
  else
    Params := Params + ' -SkipPatcher';

  if not OptPage.Values[0] then Params := Params + ' -SkipFose';

  WizardForm.StatusLabel.Caption := 'Pobieram i instaluję składniki…';

  if not Exec('powershell.exe', Params, '', SW_SHOW, ewWaitUntilTerminated, ResultCode) then
  begin
    MsgBox('Nie udało się uruchomić PowerShella.', mbError, MB_OK);
    Exit;
  end;

  case ResultCode of
    0: // Remember the folder, so the next update opens already pointing at it.
       RegWriteStringValue(HKLM, 'Software\Fallout3Access', 'GamePath', GamePage.Values[0]);
    2: MsgBox('Któregoś pliku nie udało się pobrać. Szczegóły w dzienniku: ' + LogFile,
              mbInformation, MB_OK);
    3: MsgBox('Pobieranie się udało, ale instalacja plików nie. Szczegóły w dzienniku: ' +
              LogFile, mbError, MB_OK);
  else
    MsgBox('Instalacja zakończyła się kodem ' + IntToStr(ResultCode) +
           '. Szczegóły w dzienniku: ' + LogFile, mbError, MB_OK);
  end;
end;
