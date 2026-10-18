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
#define AppVersion   "0.5.4"
#define AppPublisher "Maciej Krynicki"
#define AppURL       "https://github.com/Bruticus2131/fallout3-access"
; Nexus Mods SSO application id ("slug"), issued by Nexus staff. While it is
; empty the "sign in with Nexus Mods" choice is not offered. Can also be given
; on the command line: ISCC /DNexusAppSlug=<slug> installer\fallout3-access.iss
#ifndef NexusAppSlug
  #define NexusAppSlug ""
#endif

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

; Every text of our own pages, in both languages. Code reads them through
; CustomMessage('...') and the sections through {cm:...}; a text typed straight
; into the code would be shown in that one language whatever the user picked.
[CustomMessages]
pl.ShowLog=Pokaż dziennik instalacji
en.ShowLog=Show the installation log
pl.GamePageCaption=Folder gry
en.GamePageCaption=Game folder
pl.GamePageDescription=Gdzie jest zainstalowany Fallout 3?
en.GamePageDescription=Where is Fallout 3 installed?
pl.GamePageText=Wskaż folder zawierający Fallout3.exe. Jeśli gra została wykryta, ścieżka jest już wpisana.
en.GamePageText=Choose the folder that contains Fallout3.exe. If the game was found, the path is already filled in.
pl.MethodPageCaption=Łatka obniżająca wersję
en.MethodPageCaption=Downgrade patch
pl.MethodPageDescription=Twoja wersja gry wymaga łatki, żeby FOSE działał
en.MethodPageDescription=Your version of the game needs a patch for FOSE to work
pl.MethodPageText=Łatkę udostępnia Nexus Mods. Nexus pozwala pobierać automatycznie tylko kontom Premium, więc domyślnie po prostu otworzę stronę, a Ty wskażesz pobrany plik.
en.MethodPageText=The patch is hosted on Nexus Mods. Nexus allows automatic downloads only for Premium accounts, so by default the installer simply opens the page and you choose the downloaded file.
pl.MethodManual=Pobiorę sam — otwórz stronę i pozwól wskazać plik (nie wymaga konta Premium)
en.MethodManual=I will download it myself — open the page and let me choose the file (no Premium account needed)
pl.MethodApiKey=Mam klucz API Nexusa i konto Premium — pobierz automatycznie
en.MethodApiKey=I have a Nexus API key and a Premium account — download it automatically
pl.MethodSso=Zaloguj się przez Nexus Mods (konto Premium) — pobierz automatycznie
en.MethodSso=Sign in with Nexus Mods (Premium account) — download it automatically
pl.KeyPageCaption=Klucz Nexus Mods
en.KeyPageCaption=Nexus Mods key
pl.KeyPageDescription=Klucz z ustawień konta
en.KeyPageDescription=The key from your account settings
pl.KeyPageText=Znajdziesz go na nexusmods.com w Ustawieniach konta, w sekcji API Access ("Personal API Key").
en.KeyPageText=You will find it on nexusmods.com in your account settings, under API Access ("Personal API Key").
pl.KeyPrompt=Klucz API:
en.KeyPrompt=API key:
pl.FilePageCaption=Pobrana łatka
en.FilePageCaption=Downloaded patch
pl.FilePageDescription=Wskaż archiwum pobrane z Nexusa
en.FilePageDescription=Choose the archive downloaded from Nexus
pl.FilePageText=Strona łatki otworzyła się w przeglądarce. Pobierz łatkę, a potem wskaż pobrany plik (zwykle trafia do folderu Pobrane).
en.FilePageText=The patch page has opened in your browser. Download the patch, then choose the downloaded file (it usually goes to your Downloads folder).
pl.FilePrompt=Plik łatki:
en.FilePrompt=Patch file:
pl.FileFilter=Archiwa|*.zip;*.7z;*.rar|Wszystkie pliki|*.*
en.FileFilter=Archives|*.zip;*.7z;*.rar|All files|*.*
pl.OptPageCaption=Co zainstalować
en.OptPageCaption=What to install
pl.OptPageDescription=Możesz pominąć elementy, które już masz
en.OptPageDescription=You can skip the parts you already have
pl.OptFose=Zainstaluj FOSE (wymagany do działania moda)
en.OptFose=Install FOSE (the mod needs it to work)
pl.OptMod=Zainstaluj / zaktualizuj Fallout 3 Access
en.OptMod=Install / update Fallout 3 Access
pl.SsoInfo=Za chwilę otworzę stronę Nexus Mods w przeglądarce. Zaloguj się, kliknij Authorise (Autoryzuj), a potem wróć do instalatora — przejdzie dalej sam.
en.SsoInfo=The Nexus Mods page will now open in your browser. Sign in, click Authorise, then come back to the installer — it will continue on its own.
pl.SsoFailed=Nie udało się zalogować przez Nexus Mods. Spróbuj jeszcze raz albo wybierz inną metodę.
en.SsoFailed=Signing in with Nexus Mods did not work. Try again or choose another method.
pl.NoGameExe=W tym folderze nie ma Fallout3.exe. Wskaż folder, w którym jest gra.
en.NoGameExe=There is no Fallout3.exe in this folder. Choose the folder the game is in.
pl.NoPatchNeeded=Twoja gra jest już w wersji 1.7.0.3 — łatka nie jest potrzebna.
en.NoPatchNeeded=Your game is already version 1.7.0.3 — no patch is needed.
pl.FileNotFound=Nie znajduję wskazanego pliku.
en.FileNotFound=The chosen file cannot be found.
pl.Working=Pobieram i instaluję składniki…
en.Working=Downloading and installing the components…
pl.NoPowerShell=Nie udało się uruchomić PowerShella.
en.NoPowerShell=PowerShell could not be started.
pl.DownloadFailed=Któregoś pliku nie udało się pobrać. Szczegóły w dzienniku: %1
en.DownloadFailed=One of the files could not be downloaded. Details are in the log: %1
pl.InstallFailed=Pobieranie się udało, ale instalacja plików nie. Szczegóły w dzienniku: %1
en.InstallFailed=The download worked, but installing the files did not. Details are in the log: %1
pl.FailedCode=Instalacja zakończyła się kodem %1. Szczegóły w dzienniku: %2
en.FailedCode=The installation ended with code %1. Details are in the log: %2

[Files]
Source: "f3a-install.ps1"; DestDir: "{app}"; Flags: ignoreversion

[Registry]
; Declared here only so the uninstaller takes it away again. The value itself is
; written after a successful run — see CurStepChanged.
Root: HKLM; Subkey: "Software\Fallout3Access"; Flags: uninsdeletekey

[Run]
Filename: "{app}\install-log.txt"; Description: "{cm:ShowLog}"; \
  Flags: postinstall shellexec skipifsilent unchecked

[Code]
var
  GamePage:    TInputDirWizardPage;
  MethodPage:  TInputOptionWizardPage;   // how to get the patcher
  KeyPage:     TInputQueryWizardPage;    // only when the API route is chosen
  FilePage:    TInputFileWizardPage;     // only when the manual route is chosen
  OptPage:     TInputOptionWizardPage;
  GameNeedsPatch: Boolean;
  SsoIndex:    Integer;                  // MethodPage choice for SSO, -1 = not offered
  SsoKey:      String;                   // API key obtained through SSO

// Steam and GOG both record the install path here, so most users never have to
// find it themselves. A previous run of this installer is asked FIRST: that is
// the folder the user actually chose, which matters on a machine with more than
// one copy of the game, and it turns an update into clicking Next.
function LooksLikeGame(const Dir: String): Boolean;
begin
  Result := (Dir <> '') and FileExists(AddBackslash(Dir) + 'Fallout3.exe');
end;

// Steam keeps its library roots in steamapps\libraryfolders.vdf. Reading that
// matters because the game is very often NOT on the system drive, and neither
// the Bethesda registry key nor a guess at Program Files finds it there.
function DetectSteamPath(): String;
var
  SteamDir, Vdf: String;
  Lines: TArrayOfString;
  i, q1, q2: Integer;
  Root, Candidate, Line: String;
  Roots: TArrayOfString;
  RootCount, r: Integer;
  Names: TArrayOfString;
begin
  Result := '';
  if not RegQueryStringValue(HKCU, 'Software\Valve\Steam', 'SteamPath', SteamDir) then
    if not RegQueryStringValue(HKLM, 'SOFTWARE\WOW6432Node\Valve\Steam', 'InstallPath', SteamDir) then
      if not RegQueryStringValue(HKLM, 'SOFTWARE\Valve\Steam', 'InstallPath', SteamDir) then
        Exit;
  // The registry spells it c:/program files (x86)/steam.
  StringChangeEx(SteamDir, '/', '\', True);

  SetArrayLength(Roots, 1);
  Roots[0] := SteamDir;
  RootCount := 1;

  Vdf := AddBackslash(SteamDir) + 'steamapps\libraryfolders.vdf';
  if FileExists(Vdf) and LoadStringsFromFile(Vdf, Lines) then
  begin
    for i := 0 to GetArrayLength(Lines) - 1 do
    begin
      Line := Lines[i];
      // Every library root appears as:   "path"   "D:\SteamLibrary"
      if Pos('"path"', LowerCase(Line)) > 0 then
      begin
        q1 := Pos('"path"', LowerCase(Line)) + 6;
        Line := Copy(Line, q1, Length(Line));
        q1 := Pos('"', Line);
        if q1 > 0 then
        begin
          Line := Copy(Line, q1 + 1, Length(Line));
          q2 := Pos('"', Line);
          if q2 > 1 then
          begin
            Root := Copy(Line, 1, q2 - 1);
            // The file escapes every backslash: "D:\\SteamLibrary".
            StringChangeEx(Root, '\\', '\', True);
            SetArrayLength(Roots, RootCount + 1);
            Roots[RootCount] := Root;
            RootCount := RootCount + 1;
          end;
        end;
      end;
    end;
  end;

  // The GOTY edition and the base game use different folder names, and a
  // localised install can use yet another, so try the known ones and then fall
  // back to looking for anything with Fallout in the name.
  SetArrayLength(Names, 3);
  Names[0] := 'Fallout 3 goty';
  Names[1] := 'Fallout 3';
  Names[2] := 'Fallout3';

  for r := 0 to RootCount - 1 do
    for i := 0 to GetArrayLength(Names) - 1 do
    begin
      Candidate := AddBackslash(Roots[r]) + 'steamapps\common\' + Names[i];
      if LooksLikeGame(Candidate) then
      begin
        Result := Candidate;
        Exit;
      end;
    end;
end;

// GOG records each game under its own numeric key, and the number differs
// between the base game and the GOTY bundle. Rather than hard-code ids that
// might be wrong, walk the keys and accept the first whose folder really holds
// Fallout3.exe - the file is the evidence, not the id.
function DetectGogPath(): String;
var
  Keys: TArrayOfString;
  Base, Path: String;
  i, h: Integer;
  Hives: array[0..2] of Integer;
begin
  Result := '';
  Hives[0] := HKLM; Hives[1] := HKLM32; Hives[2] := HKCU;
  for h := 0 to 2 do
  begin
    Base := 'SOFTWARE\GOG.com\Games';
    if not RegGetSubkeyNames(Hives[h], Base, Keys) then Continue;
    for i := 0 to GetArrayLength(Keys) - 1 do
      if RegQueryStringValue(Hives[h], Base + '\' + Keys[i], 'path', Path) then
        if LooksLikeGame(Path) then
        begin
          Result := Path;
          Exit;
        end;
  end;
end;

function DetectGamePath(): String;
var
  Path: String;
begin
  Result := '';
  // A previous run of this installer is asked FIRST: that is the folder the
  // user actually chose, which matters on a machine with more than one copy,
  // and it turns an update into clicking Next.
  if RegQueryStringValue(HKLM, 'Software\Fallout3Access', 'GamePath', Path) and LooksLikeGame(Path) then
  begin
    Result := Path;
    Exit;
  end;
  if RegQueryStringValue(HKLM, 'SOFTWARE\WOW6432Node\Bethesda Softworks\Fallout3', 'Installed Path', Path) and LooksLikeGame(Path) then
  begin
    Result := Path;
    Exit;
  end;
  if RegQueryStringValue(HKLM, 'SOFTWARE\Bethesda Softworks\Fallout3', 'Installed Path', Path) and LooksLikeGame(Path) then
  begin
    Result := Path;
    Exit;
  end;
  Result := DetectSteamPath();
  if Result <> '' then Exit;
  Result := DetectGogPath();
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
    CustomMessage('GamePageCaption'), CustomMessage('GamePageDescription'),
    CustomMessage('GamePageText'), False, '');
  GamePage.Add('');
  Detected := DetectGamePath();
  if Detected <> '' then
    GamePage.Values[0] := Detected;

  // Shown only when the game actually needs downgrading.
  MethodPage := CreateInputOptionPage(GamePage.ID,
    CustomMessage('MethodPageCaption'), CustomMessage('MethodPageDescription'),
    CustomMessage('MethodPageText'), True, False);
  MethodPage.Add(CustomMessage('MethodManual'));
  MethodPage.Add(CustomMessage('MethodApiKey'));
  SsoIndex := -1;
  SsoKey := '';
  if '{#NexusAppSlug}' <> '' then
  begin
    MethodPage.Add(CustomMessage('MethodSso'));
    SsoIndex := 2;
  end;
  MethodPage.Values[0] := True;

  KeyPage := CreateInputQueryPage(MethodPage.ID,
    CustomMessage('KeyPageCaption'), CustomMessage('KeyPageDescription'),
    CustomMessage('KeyPageText'));
  KeyPage.Add(CustomMessage('KeyPrompt'), False);

  FilePage := CreateInputFilePage(MethodPage.ID,
    CustomMessage('FilePageCaption'), CustomMessage('FilePageDescription'),
    CustomMessage('FilePageText'));
  FilePage.Add(CustomMessage('FilePrompt'), CustomMessage('FileFilter'), '');

  OptPage := CreateInputOptionPage(wpSelectDir,
    CustomMessage('OptPageCaption'), CustomMessage('OptPageDescription'),
    '', False, False);
  OptPage.Add(CustomMessage('OptFose'));
  OptPage.Add(CustomMessage('OptMod'));
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

// Was "sign in with Nexus Mods" chosen? (Only offered once the app id is set.)
function SsoChosen(): Boolean;
begin
  Result := False;
  if SsoIndex >= 0 then
    Result := MethodPage.Values[SsoIndex];
end;

// Sign in through Nexus Mods SSO: the helper script talks to the SSO server,
// opens the authorise page in the browser and writes the key it receives to a
// temporary file. Returns the key, or '' when the sign-in did not complete.
function NexusSsoLogin(): String;
var
  Params, KeyFile, LogFile: String;
  Key: AnsiString;
  ResultCode: Integer;
begin
  Result := '';
  MsgBox(CustomMessage('SsoInfo'), mbInformation, MB_OK);
  ExtractTemporaryFile('f3a-install.ps1');
  KeyFile := ExpandConstant('{tmp}\nexus-key.txt');
  LogFile := ExpandConstant('{tmp}\nexus-sso-log.txt');
  DeleteFile(KeyFile);
  Params := '-NoProfile -ExecutionPolicy Bypass -File "' + ExpandConstant('{tmp}\f3a-install.ps1') + '"' +
            ' -NexusSsoLogin -NexusAppSlug "{#NexusAppSlug}" -AppVersion "{#AppVersion}"' +
            ' -KeyOutFile "' + KeyFile + '" -LogPath "' + LogFile + '"' +
            ' -Language "' + ActiveLanguage() + '"';
  if not Exec('powershell.exe', Params, '', SW_SHOWMINNOACTIVE, ewWaitUntilTerminated, ResultCode) then
    Exit;
  if (ResultCode = 0) and LoadStringFromFile(KeyFile, Key) then
    Result := Trim(String(Key));
  DeleteFile(KeyFile);
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
      MsgBox(CustomMessage('NoGameExe'), mbError, MB_OK);
      Result := False;
      Exit;
    end;
    GameNeedsPatch := not GameIsSupportedBuild(GamePage.Values[0]);
    if not GameNeedsPatch then
      MsgBox(CustomMessage('NoPatchNeeded'), mbInformation, MB_OK);
  end

  else if CurPageID = MethodPage.ID then
  begin
    if MethodPage.Values[0] then
    begin
      // Open the mod page for them; the next page takes the downloaded file.
      ShellExecAsOriginalUser('open', 'https://www.nexusmods.com/fallout3/mods/24913',
                              '', '', SW_SHOW, ewNoWait, ErrorCode);
    end
    else if SsoChosen() then
    begin
      if SsoKey = '' then
        SsoKey := NexusSsoLogin();
      if SsoKey = '' then
      begin
        MsgBox(CustomMessage('SsoFailed'), mbError, MB_OK);
        Result := False;
      end;
    end;
  end

  else if CurPageID = FilePage.ID then
  begin
    if (Trim(FilePage.Values[0]) <> '') and (not FileExists(FilePage.Values[0])) then
    begin
      MsgBox(CustomMessage('FileNotFound'), mbError, MB_OK);
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
  ModFind: TFindRec;
begin
  if CurStep <> ssPostInstall then
    Exit;

  LogFile := AddBackslash(ExpandConstant('{app}')) + 'install-log.txt';

  Params := '-NoProfile -ExecutionPolicy Bypass -File "' +
            ExpandConstant('{app}\f3a-install.ps1') + '"' +
            // No trailing backslash: "...\" would escape the closing quote.
            ' -GamePath "' + RemoveBackslashUnlessRoot(GamePage.Values[0]) + '"' +
            ' -LogPath "' + LogFile + '"' +
            ' -AppVersion "{#AppVersion}"' +
            ' -Language "' + ActiveLanguage() + '"';

  if not GameNeedsPatch then
    Params := Params + ' -SkipPatcher'
  else if SsoChosen() and (SsoKey <> '') then
    Params := Params + ' -NexusApiKey "' + SsoKey + '"'
  else if MethodPage.Values[1] and (Trim(KeyPage.Values[0]) <> '') then
    Params := Params + ' -NexusApiKey "' + Trim(KeyPage.Values[0]) + '"'
  else if Trim(FilePage.Values[0]) <> '' then
    Params := Params + ' -PatcherArchive "' + FilePage.Values[0] + '"'
  else
    Params := Params + ' -SkipPatcher';

  if not OptPage.Values[0] then Params := Params + ' -SkipFose';

  // A mod archive next to setup.exe (a test build handed round with the
  // installer) is installed instead of the latest release on GitHub.
  if FindFirst(ExpandConstant('{src}\fallout3-access-*.zip'), ModFind) then
  begin
    Params := Params + ' -ModArchive "' + ExpandConstant('{src}\') + ModFind.Name + '"';
    FindClose(ModFind);
  end;

  WizardForm.StatusLabel.Caption := CustomMessage('Working');

  if not Exec('powershell.exe', Params, '', SW_SHOW, ewWaitUntilTerminated, ResultCode) then
  begin
    MsgBox(CustomMessage('NoPowerShell'), mbError, MB_OK);
    Exit;
  end;

  case ResultCode of
    0: // Remember the folder, so the next update opens already pointing at it.
       RegWriteStringValue(HKLM, 'Software\Fallout3Access', 'GamePath', GamePage.Values[0]);
    2: MsgBox(FmtMessage(CustomMessage('DownloadFailed'), [LogFile]), mbInformation, MB_OK);
    3: MsgBox(FmtMessage(CustomMessage('InstallFailed'), [LogFile]), mbError, MB_OK);
  else
    MsgBox(FmtMessage(CustomMessage('FailedCode'), [IntToStr(ResultCode), LogFile]), mbError, MB_OK);
  end;
end;
