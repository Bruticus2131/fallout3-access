<#
.SYNOPSIS
  Does the actual work for the Fallout 3 Access installer: fetches the pieces,
  unpacks them into the game folder and reports progress.

.DESCRIPTION
  The Inno Setup front end collects the game path and the Nexus API key, then
  calls this script. Keeping the work here rather than in Pascal Script means it
  can be run and debugged on its own, which matters because most of what can go
  wrong is network- or account-shaped rather than UI-shaped.

  Three pieces are needed:

    * the mod itself          - GitHub Releases, no account required
    * FOSE                    - silverlock.org, no account required
    * Anniversary Patcher     - Nexus, to downgrade Steam builds to 1.7.0.3

  Only the last one needs the Nexus API, and Nexus only issues download links
  through the API to PREMIUM accounts. A non-premium key is not an error in this
  script: it reports what happened and points at the page, so the user can fetch
  that one file by hand and re-run with -PatcherArchive.

.NOTES
  Exit codes: 0 = done, 1 = bad arguments, 2 = download failed, 3 = install failed.
#>
[CmdletBinding()]
param(
    # Required for an install; not for -NexusSsoLogin, which only signs in.
    [string] $GamePath = "",
    [string] $NexusApiKey = "",
    # Sign in through Nexus Mods SSO and write the API key to -KeyOutFile, then
    # stop. The wizard runs this before installing, so the key can be used.
    [switch] $NexusSsoLogin,
    [string] $NexusAppSlug = "",
    [string] $KeyOutFile = "",
    # Our version, sent to Nexus in the Application-Version header.
    [string] $AppVersion = "0.0.0",
    # A patcher archive the user downloaded themselves; skips the Nexus call.
    [string] $PatcherArchive = "",
    # The mod's own archive, when it came with the installer (a test build
    # handed round before it is released); skips the download from GitHub.
    [string] $ModArchive = "",
    [switch] $SkipPatcher,
    [switch] $SkipFose,
    [string] $LogPath = "",
    # "pl" or "en": the language the user picked in the wizard. Run on its own,
    # the script speaks English, as the wizard does by default.
    [string] $Language = ""
)

$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"   # keeps Invoke-WebRequest quiet and fast

# TLS 1.2 — Windows PowerShell 5.1 still defaults to something GitHub and Nexus
# will refuse.
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

$script:Work = Join-Path $env:TEMP "f3a-install"
New-Item -ItemType Directory -Force $script:Work | Out-Null

# How the installer introduces itself to the Nexus Mods API, as Nexus asks
# registered applications to: Application-Name / Application-Version and a
# User-Agent that names the program and where it comes from.
$script:NexusAppName = "Fallout 3 Access"
function Get-NexusHeaders {
    $h = @{
        "Application-Name"    = $script:NexusAppName
        "Application-Version" = $AppVersion
        "User-Agent"          = "Fallout3Access-Installer/$AppVersion (+https://github.com/Bruticus2131/fallout3-access)"
        "Accept"              = "application/json"
    }
    if ($NexusApiKey) { $h["apikey"] = $NexusApiKey }
    return $h
}

if (-not $Language) { $Language = "en" }
$script:Pl = ($Language -eq "pl")

# Every message is written in both languages at the place it is used; this
# picks the one the user asked for.
function T([string] $PlText, [string] $EnText) {
    if ($script:Pl) { return $PlText } else { return $EnText }
}

function Write-Step([string] $Text) {
    Write-Host "== $Text"
    if ($LogPath) { Add-Content -Path $LogPath -Value ("[{0}] {1}" -f (Get-Date -Format "HH:mm:ss"), $Text) -Encoding utf8 }
}

# ---------------------------------------------------------------------------
# Nexus Mods SSO (the protocol of github.com/Nexus-Mods/sso-integration-demo):
# open a websocket to wss://sso.nexusmods.com, send our request id, receive a
# connection token, open https://www.nexusmods.com/sso?id=<id>&application=<slug>
# in the browser, and wait for the API key to arrive on the socket once the
# user presses "Authorise". On a dropped connection, reconnect with the same id
# and the connection token. The application slug is issued by Nexus staff.
function Send-WsText($Ws, [string] $Text) {
    $bytes = [Text.Encoding]::UTF8.GetBytes($Text)
    $seg = [ArraySegment[byte]]::new($bytes)
    $Ws.SendAsync($seg, [Net.WebSockets.WebSocketMessageType]::Text, $true,
                  [Threading.CancellationToken]::None).Wait()
}

# One whole text message, or $null when the socket closed or the time ran out.
function Receive-WsText($Ws, [datetime] $Deadline) {
    $buffer = New-Object byte[] 8192
    $ms = New-Object IO.MemoryStream
    do {
        $left = [int][Math]::Max(1000, ($Deadline - (Get-Date)).TotalMilliseconds)
        $cts = New-Object Threading.CancellationTokenSource
        $cts.CancelAfter($left)
        $seg = [ArraySegment[byte]]::new($buffer)
        $task = $Ws.ReceiveAsync($seg, $cts.Token)
        try { $task.Wait() } catch { return $null }
        $r = $task.Result
        if ($r.MessageType -eq [Net.WebSockets.WebSocketMessageType]::Close) { return $null }
        $ms.Write($buffer, 0, $r.Count)
    } while (-not $r.EndOfMessage)
    return [Text.Encoding]::UTF8.GetString($ms.ToArray())
}

function Get-NexusKeyViaSso([string] $Slug, [int] $TimeoutSec = 300) {
    if (-not $Slug) { Write-Step (T "Nexus SSO: brak identyfikatora aplikacji" "Nexus SSO: no application id"); return $null }
    $id = [guid]::NewGuid().ToString()
    $token = $null
    $opened = $false
    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    while ((Get-Date) -lt $deadline) {
        $ws = New-Object Net.WebSockets.ClientWebSocket
        try {
            $ws.ConnectAsync([Uri]"wss://sso.nexusmods.com", [Threading.CancellationToken]::None).Wait()
            Send-WsText $ws (@{ id = $id; token = $token; protocol = 2 } | ConvertTo-Json -Compress)
            while ($ws.State -eq [Net.WebSockets.WebSocketState]::Open -and (Get-Date) -lt $deadline) {
                $msg = Receive-WsText $ws $deadline
                if ($null -eq $msg) { break }
                $obj = $msg | ConvertFrom-Json
                if (-not $obj.success) {
                    Write-Step (T "Nexus SSO odmówił: $($obj.error)" "Nexus SSO refused: $($obj.error)")
                    return $null
                }
                if ($obj.data.connection_token) {
                    $token = $obj.data.connection_token
                    if (-not $opened) {
                        $opened = $true
                        Write-Step (T "Nexus SSO: otwieram przeglądarkę — zaloguj się i kliknij Authorise (Autoryzuj)" `
                                      "Nexus SSO: opening the browser — sign in and click Authorise")
                        Start-Process "https://www.nexusmods.com/sso?id=$id&application=$Slug"
                    }
                }
                if ($obj.data.api_key) {
                    Write-Step (T "Nexus SSO: klucz otrzymany" "Nexus SSO: key received")
                    return [string]$obj.data.api_key
                }
            }
        } catch {
            Write-Step (T "Nexus SSO: połączenie przerwane ($($_.Exception.Message)), łączę ponownie" `
                          "Nexus SSO: connection lost ($($_.Exception.Message)), reconnecting")
            Start-Sleep -Seconds 2
        } finally {
            $ws.Dispose()
        }
    }
    Write-Step (T "Nexus SSO: nie dostałem klucza w ciągu $TimeoutSec s" "Nexus SSO: no key received within $TimeoutSec s")
    return $null
}

if ($NexusSsoLogin) {
    $key = Get-NexusKeyViaSso -Slug $NexusAppSlug
    if (-not $key) { exit 2 }
    if ($KeyOutFile) {
        Set-Content -Path $KeyOutFile -Value $key -Encoding ASCII -NoNewline
    } else {
        Write-Output $key
    }
    exit 0
}
if (-not $GamePath) {
    Write-Step (T "BŁĄD: nie podano folderu gry (-GamePath)" "ERROR: no game folder given (-GamePath)")
    exit 1
}

# A failed step THROWS instead of ending the script: one piece that cannot be
# fetched (FOSE's site down, a patcher that will not unpack) must not stop the
# rest — above all, it must not stop the mod itself from being installed, which
# is what happened when the mod was the last step of the run.
$script:ExitCode = 0
function Fail([string] $Text, [int] $Code) {
    Write-Step ((T "BŁĄD: " "ERROR: ") + $Text)
    throw [System.Exception]::new("F3A:$Code")
}

function Invoke-Step([string] $Name, [scriptblock] $Body) {
    try {
        & $Body
    } catch {
        $code = 3
        if ($_.Exception.Message -match '^F3A:(\d+)$') { $code = [int]$Matches[1] }
        else { Write-Step (T "BŁĄD w kroku '$Name': $($_.Exception.Message)" "ERROR in step '$Name': $($_.Exception.Message)") }
        if ($code -gt $script:ExitCode) { $script:ExitCode = $code }
        Write-Step (T "Krok '$Name' nie powiódł się — instaluję dalej pozostałe." "Step '$Name' failed — installing the rest anyway.")
    }
}

# Copy a folder's contents into another, file by file. Copy-Item -Recurse with
# a wildcard has corner cases in Windows PowerShell 5.1 (nesting a folder that
# already exists inside itself), and with the game under Program Files a copy
# that quietly lands in the wrong place looks exactly like "the mod did not
# install". This one creates each folder it needs and copies each file to the
# exact relative path, so the result is the archive's layout, every time.
function Copy-Tree([string] $From, [string] $To) {
    $base = (Resolve-Path -LiteralPath $From).Path.TrimEnd('\')
    foreach ($f in Get-ChildItem -LiteralPath $From -Recurse -File) {
        $rel = $f.FullName.Substring($base.Length).TrimStart('\')
        $dst = Join-Path $To $rel
        $dstDir = Split-Path $dst -Parent
        if (-not (Test-Path -LiteralPath $dstDir)) {
            New-Item -ItemType Directory -Force -Path $dstDir | Out-Null
        }
        Copy-Item -LiteralPath $f.FullName -Destination $dst -Force
    }
}

# --- checks ------------------------------------------------------------------

function Test-GameFolder {
    if (-not (Test-Path (Join-Path $GamePath "Fallout3.exe"))) {
        Fail (T "W podanym folderze nie ma Fallout3.exe: $GamePath" "There is no Fallout3.exe in the given folder: $GamePath") 1
    }
    Write-Step (T "Gra znaleziona: $GamePath" "Game found: $GamePath")
}

# The Anniversary update (1.7.0.4) breaks FOSE, and that is the single most
# common reason the mod "does nothing" for a new user. Version info is missing
# from the executable, so go by file size, which differs between the builds.
function Get-GameBuild {
    $exe = Get-Item (Join-Path $GamePath "Fallout3.exe")
    # Sizes measured on real installs rather than guessed. Compared with -eq
    # against [int64] literals on purpose: a hashtable lookup silently misses
    # here, because FileInfo.Length is Int64 while a bare number key is Int32.
    switch ([int64]$exe.Length) {
        15038976L { return (T "1.7.0.3 (obsługiwana)" "1.7.0.3 (supported)") }
    }
    return (T "nieznana (rozmiar $($exe.Length) B)" "unknown (size $($exe.Length) bytes)")
}

# --- downloads ---------------------------------------------------------------

function Get-File([string] $Url, [string] $OutFile, [hashtable] $Headers = $null) {
    Write-Step (T "Pobieram: $Url" "Downloading: $Url")
    try {
        if ($Headers) { Invoke-WebRequest -Uri $Url -OutFile $OutFile -Headers $Headers -UseBasicParsing }
        else          { Invoke-WebRequest -Uri $Url -OutFile $OutFile -UseBasicParsing }
    } catch {
        Fail (T "Nie udało się pobrać $Url — $($_.Exception.Message)" "Could not download $Url — $($_.Exception.Message)") 2
    }
    if (-not (Test-Path $OutFile)) { Fail (T "Plik nie powstał: $OutFile" "The file was not created: $OutFile") 2 }
}

function Get-LatestModRelease {
    Write-Step (T "Szukam najnowszej wersji moda na GitHubie" "Looking for the latest version of the mod on GitHub")
    $api = "https://api.github.com/repos/Bruticus2131/fallout3-access/releases/latest"
    try {
        $rel = Invoke-RestMethod -Uri $api -Headers @{ "User-Agent" = "f3a-installer" } -UseBasicParsing
    } catch {
        Fail (T "Nie mogę sprawdzić wydań moda — $($_.Exception.Message)" "Cannot check the mod's releases — $($_.Exception.Message)") 2
    }
    $asset = $rel.assets | Where-Object { $_.name -like "*.zip" } | Select-Object -First 1
    if (-not $asset) { Fail (T "Wydanie $($rel.tag_name) nie ma pliku ZIP." "Release $($rel.tag_name) has no ZIP file.") 2 }
    Write-Step (T "Najnowsza wersja: $($rel.tag_name)" "Latest version: $($rel.tag_name)")
    $out = Join-Path $script:Work $asset.name
    Get-File $asset.browser_download_url $out
    return $out
}

# Nexus: ask for the newest MAIN file of a mod, then for its download link.
# The link call is the one that requires Premium; a 403 there is an account
# limitation, not a failure of this installer, and is reported as such.
function Get-NexusFile([int] $ModId, [string] $Game = "fallout3") {
    if (-not $NexusApiKey) { return $null }
    $h = Get-NexusHeaders

    Write-Step (T "Nexus: pobieram listę plików moda $ModId" "Nexus: getting the file list of mod $ModId")
    try {
        $files = Invoke-RestMethod -Uri "https://api.nexusmods.com/v1/games/$Game/mods/$ModId/files.json" -Headers $h -UseBasicParsing
    } catch {
        Write-Step (T "Nexus odmówił dostępu do listy plików: $($_.Exception.Message)" "Nexus refused access to the file list: $($_.Exception.Message)")
        return $null
    }
    $main = $files.files | Where-Object { $_.category_name -eq "MAIN" } | Sort-Object uploaded_timestamp -Descending | Select-Object -First 1
    if (-not $main) { $main = $files.files | Sort-Object uploaded_timestamp -Descending | Select-Object -First 1 }
    if (-not $main) { Write-Step (T "Nexus: brak plików w modzie $ModId" "Nexus: mod $ModId has no files"); return $null }

    Write-Step (T "Nexus: proszę o link do pliku '$($main.file_name)'" "Nexus: asking for a download link to '$($main.file_name)'")
    try {
        $link = Invoke-RestMethod -Uri "https://api.nexusmods.com/v1/games/$Game/mods/$ModId/files/$($main.file_id)/download_link.json" -Headers $h -UseBasicParsing
    } catch {
        Write-Step (T "Nexus nie dał linku do pobrania. Tak odpowiada kontom bez Premium — ten jeden plik trzeba pobrać ręcznie." `
                      "Nexus gave no download link. That is its answer to accounts without Premium — this one file has to be downloaded by hand.")
        return $null
    }
    $url = $link[0].URI
    $out = Join-Path $script:Work $main.file_name
    Get-File $url $out
    return $out
}

function Test-NexusKey {
    if (-not $NexusApiKey) { return $false }
    try {
        $u = Invoke-RestMethod -Uri "https://api.nexusmods.com/v1/users/validate.json" `
                -Headers (Get-NexusHeaders) -UseBasicParsing
        $prem = if ($u.is_premium) { "Premium" } else { T "zwykłe (bez automatycznego pobierania)" "standard (no automatic downloads)" }
        Write-Step (T "Klucz Nexusa działa. Konto: $($u.name), $prem" "The Nexus key works. Account: $($u.name), $prem")
        return [bool]$u.is_premium
    } catch {
        Write-Step (T "Klucz Nexusa odrzucony: $($_.Exception.Message)" "The Nexus key was rejected: $($_.Exception.Message)")
        return $false
    }
}

# --- unpacking ---------------------------------------------------------------

# Both Program Files folders. A 32-bit PowerShell - which is what a 32-bit
# installer starts - reads $env:ProgramFiles as "Program Files (x86)", so the
# real one comes from ProgramW6432 (absent on 32-bit Windows).
function Get-ProgramFilesRoots {
    return @($env:ProgramW6432, $env:ProgramFiles, ${env:ProgramFiles(x86)}) |
           Where-Object { $_ } | Select-Object -Unique
}

# Registry values from BOTH views of a key. A 32-bit process is shown only the
# 32-bit half of HKLM\SOFTWARE, and 64-bit WinRAR and 7-Zip record themselves
# in the other half - so a 32-bit run never found a 64-bit WinRAR at all.
function Get-RegValues([string] $Hive, [string] $SubKey, [string[]] $Names) {
    $out = @()
    $base = if ($Hive -eq "HKCU") { [Microsoft.Win32.RegistryHive]::CurrentUser }
            else { [Microsoft.Win32.RegistryHive]::LocalMachine }
    foreach ($view in @([Microsoft.Win32.RegistryView]::Registry64,
                        [Microsoft.Win32.RegistryView]::Registry32)) {
        try {
            $root = [Microsoft.Win32.RegistryKey]::OpenBaseKey($base, $view)
            $key = $root.OpenSubKey($SubKey)
            if ($key) {
                foreach ($n in $Names) {
                    $v = $key.GetValue($n)
                    if ($v) { $out += [string]$v }
                }
                $key.Close()
            }
            $root.Close()
        } catch { }
    }
    return $out
}

# Find 7-Zip wherever it is: the fixed Program Files paths miss a portable
# copy, a per-user install, or anything on PATH, and the registry is where the
# installer records itself.
function Find-SevenZip {
    $candidates = @()
    foreach ($root in Get-ProgramFilesRoots) { $candidates += (Join-Path $root "7-Zip\7z.exe") }
    $candidates += "$env:LOCALAPPDATA\Programs\7-Zip\7z.exe"
    foreach ($hive in @("HKLM", "HKCU")) {
        foreach ($p in (Get-RegValues $hive "SOFTWARE\7-Zip" @("Path", "Path64", "Path32"))) {
            $candidates += (Join-Path $p "7z.exe")
        }
    }
    # The file manager's App Paths entry: 7z.exe sits next to it.
    foreach ($p in (Get-RegValues "HKLM" "SOFTWARE\Microsoft\Windows\CurrentVersion\App Paths\7zFM.exe" @(""))) {
        $candidates += (Join-Path (Split-Path $p.Trim('"') -Parent) "7z.exe")
    }
    $onPath = Get-Command 7z.exe -ErrorAction SilentlyContinue
    if ($onPath) { $candidates += $onPath.Source }
    return ($candidates | Where-Object { $_ -and (Test-Path -LiteralPath $_) } | Select-Object -First 1)
}

# WinRAR opens .7z too, and plenty of people have it instead of 7-Zip. Without
# this the installer simply stopped dead on anyone who had one but not the other
# - which is exactly what happened to a tester.
function Find-WinRar {
    $candidates = @()
    foreach ($root in Get-ProgramFilesRoots) { $candidates += (Join-Path $root "WinRAR\WinRAR.exe") }
    foreach ($hive in @("HKLM", "HKCU")) {
        $candidates += (Get-RegValues $hive "SOFTWARE\WinRAR" @("exe64", "exe32"))
    }
    # WinRAR installed somewhere else: its App Paths entry and its uninstall
    # entry still say where.
    foreach ($p in (Get-RegValues "HKLM" "SOFTWARE\Microsoft\Windows\CurrentVersion\App Paths\WinRAR.exe" @(""))) {
        $candidates += $p.Trim('"')
    }
    foreach ($p in (Get-RegValues "HKLM" "SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\WinRAR archiver" @("InstallLocation"))) {
        $candidates += (Join-Path $p "WinRAR.exe")
    }
    $onPath = Get-Command WinRAR.exe -ErrorAction SilentlyContinue
    if ($onPath) { $candidates += $onPath.Source }
    return ($candidates | Where-Object { $_ -and (Test-Path -LiteralPath $_) } | Select-Object -First 1)
}

function Expand-Any([string] $Archive, [string] $Destination) {
    # Start from an empty folder: leftovers of an earlier run would be copied
    # into the game along with the new files.
    if (Test-Path -LiteralPath $Destination) {
        Remove-Item -LiteralPath $Destination -Recurse -Force -ErrorAction SilentlyContinue
    }
    New-Item -ItemType Directory -Force $Destination | Out-Null

    if ($Archive -like "*.zip") {
        Expand-Archive -Path $Archive -DestinationPath $Destination -Force
        return
    }

    $sevenZip = Find-SevenZip
    if ($sevenZip) {
        Write-Step (T "Rozpakowuję 7-Zipem" "Unpacking with 7-Zip")
        & $sevenZip x $Archive "-o$Destination" -y | Out-Null
        if ($LASTEXITCODE -eq 0) { return }
        Write-Step (T "7-Zip zwrócił błąd ($LASTEXITCODE) — próbuję WinRAR-em" "7-Zip returned an error ($LASTEXITCODE) — trying WinRAR")
    }

    $winRar = Find-WinRar
    if ($winRar) {
        Write-Step (T "Rozpakowuję WinRAR-em" "Unpacking with WinRAR")
        # x keeps the folder structure, -y answers every prompt, -ibck keeps the
        # window out of the way. The destination needs its trailing backslash or
        # WinRAR reads it as a file name.
        $proc = Start-Process -FilePath $winRar `
                    -ArgumentList @("x", "-y", "-ibck", "`"$Archive`"", "`"$Destination\`"") `
                    -Wait -PassThru
        $got = Get-ChildItem $Destination -Recurse -File -ErrorAction SilentlyContinue |
               Select-Object -First 1
        if ($proc.ExitCode -eq 0 -and $got) { return }
        Write-Step (T "WinRAR zwrócił błąd ($($proc.ExitCode))" "WinRAR returned an error ($($proc.ExitCode))")
    }

    Fail (T ("Nie umiem rozpakować $Archive — to nie jest ZIP, a nie znalazłem ani " +
             "7-Zipa, ani WinRAR-a. Zainstaluj jeden z nich (7-Zip jest darmowy: " +
             "https://www.7-zip.org ) i uruchom instalator ponownie.") `
            ("Cannot unpack $Archive — it is not a ZIP, and neither 7-Zip nor WinRAR " +
             "was found. Install one of them (7-Zip is free: https://www.7-zip.org ) " +
             "and run the installer again.")) 3
}

# --- install steps -----------------------------------------------------------

function Install-Mod([string] $Zip) {
    $dir = Join-Path $script:Work "mod"
    Expand-Any $Zip $dir

    # Updating over an existing install must not throw away the player's own
    # settings. Fallout3Access.ini holds every key binding, and someone who has
    # rebound half the mod would lose the lot to a plain overwrite — the one
    # thing an update is never allowed to do. Keep theirs, and leave the fresh
    # one beside it so new settings are still visible and comparable.
    $iniPath = Join-Path $GamePath "Data\FOSE\Plugins\Fallout3Access.ini"
    $keepIni = $null
    if (Test-Path $iniPath) {
        Write-Step (T "Aktualizacja: zachowuję Twoje ustawienia i przypisania klawiszy" "Update: keeping your settings and key bindings")
        $keepIni = Join-Path $script:Work "Fallout3Access.ini.keep"
        Copy-Item $iniPath $keepIni -Force
    } else {
        Write-Step (T "Instaluję mod" "Installing the mod")
    }

    # The archive is laid out as the game folder (Data\FOSE\Plugins\... plus the
    # screen-reader DLLs beside Fallout3.exe). Find that root by the plugin
    # itself rather than trusting the layout: an archive that gained a top
    # folder would otherwise install everything one level too deep.
    $dll = Get-ChildItem -LiteralPath $dir -Recurse -File -Filter "fallout3_access.dll" | Select-Object -First 1
    if (-not $dll) { Fail (T "W archiwum moda nie ma fallout3_access.dll." "The mod archive has no fallout3_access.dll.") 3 }
    if ($dll.Directory.Parent.Parent.Name -ne "Data") {
        Fail (T "Nieoczekiwany układ archiwum moda: $($dll.FullName)" "Unexpected layout of the mod archive: $($dll.FullName)") 3
    }
    $root = $dll.Directory.Parent.Parent.Parent      # Plugins -> FOSE -> Data -> root
    Write-Step (T "Kopiuję pliki moda do: $GamePath" "Copying the mod files to: $GamePath")
    Copy-Tree $root.FullName $GamePath

    $plugins = Join-Path $GamePath "Data\FOSE\Plugins"
    # The archive's Fallout3Access.ini has English comments (and Language =
    # auto: the game's own language); its Polish twin, with Polish comments,
    # is Fallout3Access\Fallout3Access.pl.ini. Polish is put in its place.
    $plIni = Join-Path $plugins "Fallout3Access\Fallout3Access.pl.ini"
    if ($script:Pl -and (Test-Path -LiteralPath $plIni)) {
        Copy-Item -LiteralPath $plIni -Destination $iniPath -Force
    }
    if ($keepIni) {
        $newIni = T "Fallout3Access.ini.nowy" "Fallout3Access.ini.new"
        Copy-Item -LiteralPath $iniPath -Destination (Join-Path $plugins $newIni) -Force
        Copy-Item -LiteralPath $keepIni -Destination $iniPath -Force
        Write-Step (T "Wzorcowy plik nowej wersji leży obok jako $newIni" "The new version's default file is next to it as $newIni")
        # The first versions shipped CrosshairDistance = 1, and an update keeps
        # the old file: "Rozmawiaj Moira Brown, tuż obok" on every glance. The
        # distance is off by default now; bring kept files in line.
        $text = [IO.File]::ReadAllText($iniPath)
        $fixed = $text -replace '(?m)^(\s*CrosshairDistance\s*=\s*)1\b', '${1}0'
        if ($fixed -ne $text) {
            [IO.File]::WriteAllText($iniPath, $fixed, (New-Object Text.UTF8Encoding($false)))
            Write-Step (T "Wyłączam odległość po podpowiedziach typu Rozmawiaj (CrosshairDistance = 0)" "Turning off the distance after prompts like Talk (CrosshairDistance = 0)")
        }
        # Exploration announcements were on by default (0.4.5-0.5.2) and talked
        # through every walk; auto-walk is quiet now, Shift+O turns them on.
        $text = [IO.File]::ReadAllText($iniPath)
        $fixed = $text -replace '(?m)^(\s*ExploreMode\s*=\s*)1\b', '${1}0'
        if ($fixed -ne $text) {
            [IO.File]::WriteAllText($iniPath, $fixed, (New-Object Text.UTF8Encoding($false)))
            Write-Step (T "Autowalk idzie teraz w ciszy (ExploreMode = 0); Shift+O włącza znaleziska po drodze" "Auto-walk is quiet now (ExploreMode = 0); Shift+O turns on finds along the way")
        }
        # PressRow was Insert, which NVDA takes for itself (its modifier key),
        # so the game never saw it. It is F2 now; move a kept file along.
        $text = [IO.File]::ReadAllText($iniPath)
        $fixed = $text -replace '(?m)^(\s*PressRow\s*=\s*)0xD2\b', '${1}0x3C'
        $fixed = $fixed -replace '(?m)^(\s*PressRow\s*=\s*0x3C\s*;\s*)Insert   ', '${1}F2       '
        if ($fixed -ne $text) {
            [IO.File]::WriteAllText($iniPath, $fixed, (New-Object Text.UTF8Encoding($false)))
            Write-Step (T "Klawisz klikania w menu (PressRow) zmieniony z Insert na F2 - Insert zabiera NVDA" "Menu click key (PressRow) moved from Insert to F2 - NVDA takes Insert")
        }
    } else {
        # A fresh install speaks the language the installer was run in. An
        # update keeps the player's own choice, with the rest of their settings.
        $lang = if ($script:Pl) { "pl" } else { "en" }
        $text = [IO.File]::ReadAllText($iniPath)
        $text = $text -replace '(?m)^(Language\s*=\s*)[A-Za-z]+', ('${1}' + $lang)
        [IO.File]::WriteAllText($iniPath, $text, (New-Object Text.UTF8Encoding($false)))
        Write-Step (T "Język moda: polski (Language = pl w Fallout3Access.ini)" "Mod language set to English (Language = en in Fallout3Access.ini)")
    }

    # Both files, where the game looks for them — checked, not assumed.
    foreach ($name in @("fallout3_access.dll", "Fallout3Access.ini")) {
        $f = Join-Path $plugins $name
        if (-not (Test-Path -LiteralPath $f)) { Fail (T "Po kopiowaniu brak $name w $plugins" "After copying, $name is missing from $plugins") 3 }
        Write-Step (T "Jest: $f" "Present: $f")
    }
    Write-Step (T "Mod zainstalowany" "Mod installed")
}

function Install-Fose([string] $Url) {
    if ($SkipFose) { Write-Step (T "Pomijam FOSE (na życzenie)" "Skipping FOSE (as asked)"); return }
    if (Test-Path (Join-Path $GamePath "fose_loader.exe")) {
        Write-Step (T "FOSE już jest — pomijam" "FOSE is already there — skipping")
        return
    }
    $archive = Join-Path $script:Work "fose.7z"
    Get-File $Url $archive
    $dir = Join-Path $script:Work "fose"
    Expand-Any $archive $dir
    # FOSE archives keep the loader inside a versioned folder; find it wherever
    # it landed and copy that folder's contents next to Fallout3.exe.
    $loader = Get-ChildItem $dir -Recurse -Filter "fose_loader.exe" | Select-Object -First 1
    if (-not $loader) { Fail (T "W archiwum FOSE nie ma fose_loader.exe" "The FOSE archive has no fose_loader.exe") 3 }
    # FILES only, no recursion: the archive also carries FOSE's complete source
    # tree beside the loader, and a recursive copy dumped all of it into the
    # game folder. Only the loader, its DLLs and the readmes belong there.
    Get-ChildItem $loader.Directory.FullName -File |
        Copy-Item -Destination $GamePath -Force
    Write-Step (T "FOSE zainstalowany" "FOSE installed")
}

function Invoke-Patcher([string] $Archive) {
    if ($SkipPatcher) { Write-Step (T "Pomijam patcher (na życzenie)" "Skipping the patcher (as asked)"); return }
    $dir = Join-Path $script:Work "patcher"
    Expand-Any $Archive $dir
    # Patcher.exe by NAME: the archive also carries xdelta3.exe, and running
    # "the first .exe found" could start that instead and patch nothing.
    $exe = Get-ChildItem -LiteralPath $dir -Recurse -File -Filter "Patcher.exe" | Select-Object -First 1
    if (-not $exe) { Fail (T "W archiwum patchera nie ma Patcher.exe" "The patcher archive has no Patcher.exe") 3 }
    # The patcher only works from the game folder (its readme: unpack ALL files
    # next to Fallout3.exe, no subfolder), so it is copied there and run there.
    Write-Step (T "Kopiuję patcher do folderu gry" "Copying the patcher to the game folder")
    Copy-Tree $exe.Directory.FullName $GamePath
    Write-Step (T "Uruchamiam patcher — potwierdź w jego oknie (Enter), potem wróć tutaj" `
                  "Starting the patcher — confirm in its window (Enter), then come back here")
    $proc = Start-Process -FilePath (Join-Path $GamePath $exe.Name) -WorkingDirectory $GamePath -Wait -PassThru
    Write-Step (T "Patcher zakończony (kod $($proc.ExitCode)). Wersja gry teraz: $(Get-GameBuild)" `
                  "Patcher finished (code $($proc.ExitCode)). Game version now: $(Get-GameBuild)")
    if ((Get-GameBuild) -notlike "1.7.0.3*") {
        Write-Step (T "UWAGA: gra nadal nie jest w wersji 1.7.0.3 — sprawdź okno patchera." `
                      "WARNING: the game is still not version 1.7.0.3 — check the patcher window.")
    }
}

# --- main --------------------------------------------------------------------

Test-GameFolder
Write-Step (T "Wersja gry: $(Get-GameBuild)" "Game version: $(Get-GameBuild)")

# The mod FIRST. It is the one piece this installer exists for, and the other
# steps can fail for reasons outside anyone's control (a site down, a Nexus
# account without Premium, an archive tool missing).
Invoke-Step "mod" {
    if ($ModArchive -and (Test-Path -LiteralPath $ModArchive)) {
        Write-Step (T "Instaluję moda z paczki obok instalatora: $ModArchive" "Installing the mod from the archive next to the installer: $ModArchive")
        Install-Mod $ModArchive
    } else {
        Install-Mod (Get-LatestModRelease)
    }
}

$premium = Test-NexusKey

# Loot Menu Updated is a REQUIREMENT, not a nicety: it replaces the container
# screen with a list drawn on the HUD, and the mod reads that list out - the
# container, the highlighted entry, its place in the list, whether it is worn.
function Install-LootMenu {
    $dll = Join-Path $GamePath "Data\FOSE\Plugins\F3LootMenu.dll"
    if (Test-Path $dll) { Write-Step (T "Loot Menu Updated już jest — pomijam" "Loot Menu Updated is already there — skipping"); return }

    $file = Get-NexusFile -ModId 27191
    if ($file) {
        $dir = Join-Path $script:Work "lootmenu"
        Expand-Any $file $dir
        Copy-Tree $dir $GamePath
        if (Test-Path $dll) { Write-Step (T "Loot Menu Updated zainstalowany" "Loot Menu Updated installed"); return }
        Write-Step (T "Rozpakowałem Loot Menu, ale nie widzę F3LootMenu.dll" "Loot Menu was unpacked, but F3LootMenu.dll is not there")
    }

    Write-Step (T "UWAGA: brakuje wymaganego moda Loot Menu Updated." "WARNING: the required mod Loot Menu Updated is missing.")
    Write-Step (T "Pobierz go ręcznie: https://www.nexusmods.com/fallout3/mods/27191" "Download it by hand: https://www.nexusmods.com/fallout3/mods/27191")
    Write-Step (T "i wypakuj do folderu gry (plik trafia do Data\FOSE\Plugins)." "and unpack it into the game folder (the file goes to Data\FOSE\Plugins).")
}

# The path differs from the stable download: 1.3 beta 2 lives under /beta/.
# Verified live rather than guessed — the obvious /download/ URL 404s.
Invoke-Step "FOSE" { Install-Fose "https://fose.silverlock.org/beta/fose_v1_3_beta2.7z" }
Invoke-Step "Loot Menu" { Install-LootMenu }

if (-not $SkipPatcher) {
    Invoke-Step "patcher" {
        $patcher = $PatcherArchive
        if (-not $patcher) { $patcher = Get-NexusFile -ModId 24913 }
        if ($patcher) {
            Invoke-Patcher $patcher
        } else {
            Write-Step (T "Patcher nie został pobrany automatycznie." "The patcher was not downloaded automatically.")
            Write-Step (T "Pobierz go ręcznie: https://www.nexusmods.com/fallout3/mods/24913" "Download it by hand: https://www.nexusmods.com/fallout3/mods/24913")
            Write-Step (T "potem uruchom ten instalator ponownie i wskaż pobrany plik." "then run this installer again and choose the downloaded file.")
        }
    }
}

if ($script:ExitCode -eq 0) {
    Write-Step (T "Gotowe. Uruchamiaj grę przez fose_loader.exe, nie Fallout3.exe." "Done. Start the game with fose_loader.exe, not Fallout3.exe.")
} else {
    Write-Step (T "Zakończono z problemami (kod $($script:ExitCode)) — szczegóły powyżej." "Finished with problems (code $($script:ExitCode)) — details above.")
}
exit $script:ExitCode
