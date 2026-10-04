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
    [Parameter(Mandatory = $true)] [string] $GamePath,
    [string] $NexusApiKey = "",
    # A patcher archive the user downloaded themselves; skips the Nexus call.
    [string] $PatcherArchive = "",
    [switch] $SkipPatcher,
    [switch] $SkipFose,
    [string] $LogPath = ""
)

$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"   # keeps Invoke-WebRequest quiet and fast

# TLS 1.2 — Windows PowerShell 5.1 still defaults to something GitHub and Nexus
# will refuse.
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

$script:Work = Join-Path $env:TEMP "f3a-install"
New-Item -ItemType Directory -Force $script:Work | Out-Null

function Write-Step([string] $Text) {
    Write-Host "== $Text"
    if ($LogPath) { Add-Content -Path $LogPath -Value ("[{0}] {1}" -f (Get-Date -Format "HH:mm:ss"), $Text) -Encoding utf8 }
}

function Fail([string] $Text, [int] $Code) {
    Write-Step "BŁĄD: $Text"
    exit $Code
}

# --- checks ------------------------------------------------------------------

function Test-GameFolder {
    if (-not (Test-Path (Join-Path $GamePath "Fallout3.exe"))) {
        Fail "W podanym folderze nie ma Fallout3.exe: $GamePath" 1
    }
    Write-Step "Gra znaleziona: $GamePath"
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
        15038976L { return "1.7.0.3 (obsługiwana)" }
    }
    return "nieznana (rozmiar $($exe.Length) B)"
}

# --- downloads ---------------------------------------------------------------

function Get-File([string] $Url, [string] $OutFile, [hashtable] $Headers = $null) {
    Write-Step "Pobieram: $Url"
    try {
        if ($Headers) { Invoke-WebRequest -Uri $Url -OutFile $OutFile -Headers $Headers -UseBasicParsing }
        else          { Invoke-WebRequest -Uri $Url -OutFile $OutFile -UseBasicParsing }
    } catch {
        Fail "Nie udało się pobrać $Url — $($_.Exception.Message)" 2
    }
    if (-not (Test-Path $OutFile)) { Fail "Plik nie powstał: $OutFile" 2 }
}

function Get-LatestModRelease {
    Write-Step "Szukam najnowszej wersji moda na GitHubie"
    $api = "https://api.github.com/repos/Bruticus2131/fallout3-access/releases/latest"
    try {
        $rel = Invoke-RestMethod -Uri $api -Headers @{ "User-Agent" = "f3a-installer" } -UseBasicParsing
    } catch {
        Fail "Nie mogę sprawdzić wydań moda — $($_.Exception.Message)" 2
    }
    $asset = $rel.assets | Where-Object { $_.name -like "*.zip" } | Select-Object -First 1
    if (-not $asset) { Fail "Wydanie $($rel.tag_name) nie ma pliku ZIP." 2 }
    Write-Step "Najnowsza wersja: $($rel.tag_name)"
    $out = Join-Path $script:Work $asset.name
    Get-File $asset.browser_download_url $out
    return $out
}

# Nexus: ask for the newest MAIN file of a mod, then for its download link.
# The link call is the one that requires Premium; a 403 there is an account
# limitation, not a failure of this installer, and is reported as such.
function Get-NexusFile([int] $ModId, [string] $Game = "fallout3") {
    if (-not $NexusApiKey) { return $null }
    $h = @{ "apikey" = $NexusApiKey; "User-Agent" = "f3a-installer"; "Accept" = "application/json" }

    Write-Step "Nexus: pobieram listę plików moda $ModId"
    try {
        $files = Invoke-RestMethod -Uri "https://api.nexusmods.com/v1/games/$Game/mods/$ModId/files.json" -Headers $h -UseBasicParsing
    } catch {
        Write-Step "Nexus odmówił dostępu do listy plików: $($_.Exception.Message)"
        return $null
    }
    $main = $files.files | Where-Object { $_.category_name -eq "MAIN" } | Sort-Object uploaded_timestamp -Descending | Select-Object -First 1
    if (-not $main) { $main = $files.files | Sort-Object uploaded_timestamp -Descending | Select-Object -First 1 }
    if (-not $main) { Write-Step "Nexus: brak plików w modzie $ModId"; return $null }

    Write-Step "Nexus: proszę o link do pliku '$($main.file_name)'"
    try {
        $link = Invoke-RestMethod -Uri "https://api.nexusmods.com/v1/games/$Game/mods/$ModId/files/$($main.file_id)/download_link.json" -Headers $h -UseBasicParsing
    } catch {
        Write-Step "Nexus nie dał linku do pobrania. Tak odpowiada kontom bez Premium — ten jeden plik trzeba pobrać ręcznie."
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
                -Headers @{ "apikey" = $NexusApiKey; "User-Agent" = "f3a-installer" } -UseBasicParsing
        $prem = if ($u.is_premium) { "Premium" } else { "zwykłe (bez automatycznego pobierania)" }
        Write-Step "Klucz Nexusa działa. Konto: $($u.name), $prem"
        return [bool]$u.is_premium
    } catch {
        Write-Step "Klucz Nexusa odrzucony: $($_.Exception.Message)"
        return $false
    }
}

# --- unpacking ---------------------------------------------------------------

# Find 7-Zip wherever it is: the two fixed Program Files paths miss a portable
# copy, a per-user install, or anything on PATH, and the registry is where the
# installer records itself.
function Find-SevenZip {
    $candidates = @(
        "$env:ProgramFiles\7-Zip\7z.exe",
        "${env:ProgramFiles(x86)}\7-Zip\7z.exe",
        "$env:LOCALAPPDATA\Programs\7-Zip\7z.exe"
    )
    foreach ($key in @("HKLM:\SOFTWARE\7-Zip", "HKLM:\SOFTWARE\WOW6432Node\7-Zip",
                       "HKCU:\SOFTWARE\7-Zip")) {
        try {
            $p = (Get-ItemProperty -Path $key -ErrorAction Stop).Path
            if ($p) { $candidates += (Join-Path $p "7z.exe") }
        } catch { }
    }
    $onPath = Get-Command 7z.exe -ErrorAction SilentlyContinue
    if ($onPath) { $candidates += $onPath.Source }
    return ($candidates | Where-Object { $_ -and (Test-Path $_) } | Select-Object -First 1)
}

# WinRAR opens .7z too, and plenty of people have it instead of 7-Zip. Without
# this the installer simply stopped dead on anyone who had one but not the other
# - which is exactly what happened to a tester.
function Find-WinRar {
    $candidates = @(
        "$env:ProgramFiles\WinRAR\WinRAR.exe",
        "${env:ProgramFiles(x86)}\WinRAR\WinRAR.exe"
    )
    foreach ($key in @("HKLM:\SOFTWARE\WinRAR", "HKLM:\SOFTWARE\WOW6432Node\WinRAR",
                       "HKCU:\SOFTWARE\WinRAR")) {
        foreach ($name in @("exe64", "exe32")) {
            try {
                $p = (Get-ItemProperty -Path $key -ErrorAction Stop).$name
                if ($p) { $candidates += $p }
            } catch { }
        }
    }
    $onPath = Get-Command WinRAR.exe -ErrorAction SilentlyContinue
    if ($onPath) { $candidates += $onPath.Source }
    return ($candidates | Where-Object { $_ -and (Test-Path $_) } | Select-Object -First 1)
}

function Expand-Any([string] $Archive, [string] $Destination) {
    New-Item -ItemType Directory -Force $Destination | Out-Null

    if ($Archive -like "*.zip") {
        Expand-Archive -Path $Archive -DestinationPath $Destination -Force
        return
    }

    $sevenZip = Find-SevenZip
    if ($sevenZip) {
        Write-Step "Rozpakowuje 7-Zipem"
        & $sevenZip x $Archive "-o$Destination" -y | Out-Null
        if ($LASTEXITCODE -eq 0) { return }
        Write-Step "7-Zip zwrocil blad ($LASTEXITCODE) - probuje WinRAR-em"
    }

    $winRar = Find-WinRar
    if ($winRar) {
        Write-Step "Rozpakowuje WinRAR-em"
        # x keeps the folder structure, -y answers every prompt, -ibck keeps the
        # window out of the way. The destination needs its trailing backslash or
        # WinRAR reads it as a file name.
        $proc = Start-Process -FilePath $winRar `
                    -ArgumentList @("x", "-y", "-ibck", "`"$Archive`"", "`"$Destination\`"") `
                    -Wait -PassThru
        $got = Get-ChildItem $Destination -Recurse -File -ErrorAction SilentlyContinue |
               Select-Object -First 1
        if ($proc.ExitCode -eq 0 -and $got) { return }
        Write-Step "WinRAR zwrocil blad ($($proc.ExitCode))"
    }

    Fail ("Nie umiem rozpakowac $Archive - to nie jest ZIP, a nie znalazlem ani " +
          "7-Zipa, ani WinRAR-a. Zainstaluj jeden z nich (7-Zip jest darmowy: " +
          "https://www.7-zip.org ) i uruchom instalator ponownie.") 3
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
        Write-Step "Aktualizacja: zachowuję Twoje ustawienia i przypisania klawiszy"
        $keepIni = Join-Path $script:Work "Fallout3Access.ini.keep"
        Copy-Item $iniPath $keepIni -Force
    } else {
        Write-Step "Instaluję mod"
    }

    # The archive is laid out exactly as the game folder, so a straight copy is
    # the whole install.
    Copy-Item -Path (Join-Path $dir "*") -Destination $GamePath -Recurse -Force

    if ($keepIni) {
        Copy-Item $iniPath (Join-Path $GamePath "Data\FOSE\Plugins\Fallout3Access.ini.nowy") -Force
        Copy-Item $keepIni $iniPath -Force
        Write-Step "Wzorcowy plik nowej wersji leży obok jako Fallout3Access.ini.nowy"
    }

    if (-not (Test-Path (Join-Path $GamePath "Data\FOSE\Plugins\fallout3_access.dll"))) {
        Fail "Po kopiowaniu brak fallout3_access.dll w Data\FOSE\Plugins." 3
    }
    Write-Step "Mod zainstalowany"
}

function Install-Fose([string] $Url) {
    if ($SkipFose) { Write-Step "Pomijam FOSE (na życzenie)"; return }
    if (Test-Path (Join-Path $GamePath "fose_loader.exe")) {
        Write-Step "FOSE już jest — pomijam"
        return
    }
    $archive = Join-Path $script:Work "fose.7z"
    Get-File $Url $archive
    $dir = Join-Path $script:Work "fose"
    Expand-Any $archive $dir
    # FOSE archives keep the loader inside a versioned folder; find it wherever
    # it landed and copy that folder's contents next to Fallout3.exe.
    $loader = Get-ChildItem $dir -Recurse -Filter "fose_loader.exe" | Select-Object -First 1
    if (-not $loader) { Fail "W archiwum FOSE nie ma fose_loader.exe" 3 }
    # FILES only, no recursion: the archive also carries FOSE's complete source
    # tree beside the loader, and a recursive copy dumped all of it into the
    # game folder. Only the loader, its DLLs and the readmes belong there.
    Get-ChildItem $loader.Directory.FullName -File |
        Copy-Item -Destination $GamePath -Force
    Write-Step "FOSE zainstalowany"
}

function Invoke-Patcher([string] $Archive) {
    if ($SkipPatcher) { Write-Step "Pomijam patcher (na życzenie)"; return }
    $dir = Join-Path $script:Work "patcher"
    Expand-Any $Archive $dir
    $exe = Get-ChildItem $dir -Recurse -Filter "*.exe" | Select-Object -First 1
    if (-not $exe) { Fail "W archiwum patchera nie ma pliku .exe" 3 }
    # The patcher works in place, on the folder it is run from.
    Copy-Item -Path (Join-Path $exe.Directory.FullName "*") -Destination $GamePath -Recurse -Force
    Write-Step "Uruchamiam patcher — potwierdź w jego oknie, potem wróć tutaj"
    Start-Process -FilePath (Join-Path $GamePath $exe.Name) -WorkingDirectory $GamePath -Wait
    Write-Step "Patcher zakończony"
}

# --- main --------------------------------------------------------------------

Test-GameFolder
Write-Step "Wersja gry: $(Get-GameBuild)"

$premium = Test-NexusKey

if (-not $SkipPatcher) {
    $patcher = $PatcherArchive
    if (-not $patcher) { $patcher = Get-NexusFile -ModId 24913 }
    if ($patcher) {
        Invoke-Patcher $patcher
    } else {
        Write-Step "Patcher nie został pobrany automatycznie."
        Write-Step "Pobierz go ręcznie: https://www.nexusmods.com/fallout3/mods/24913"
        Write-Step "potem uruchom ten instalator ponownie i wskaż pobrany plik."
    }
}

# Loot Menu Updated is a REQUIREMENT, not a nicety: it replaces the container
# screen with a list drawn on the HUD, and the mod reads that list out - the
# container, the highlighted entry, its place in the list, whether it is worn.
function Install-LootMenu {
    $dll = Join-Path $GamePath "Data\FOSE\Plugins\F3LootMenu.dll"
    if (Test-Path $dll) { Write-Step "Loot Menu Updated juz jest - pomijam"; return }

    $file = Get-NexusFile -ModId 27191
    if ($file) {
        $dir = Join-Path $script:Work "lootmenu"
        Expand-Any $file $dir
        Copy-Item -Path (Join-Path $dir "*") -Destination $GamePath -Recurse -Force
        if (Test-Path $dll) { Write-Step "Loot Menu Updated zainstalowany"; return }
        Write-Step "Rozpakowalem Loot Menu, ale nie widze F3LootMenu.dll"
    }

    Write-Step "UWAGA: brakuje wymaganego moda Loot Menu Updated."
    Write-Step "Pobierz go recznie: https://www.nexusmods.com/fallout3/mods/27191"
    Write-Step "i wypakuj do folderu gry (plik trafia do Data\FOSE\Plugins)."
}

# The path differs from the stable download: 1.3 beta 2 lives under /beta/.
# Verified live rather than guessed — the obvious /download/ URL 404s.
Install-Fose "https://fose.silverlock.org/beta/fose_v1_3_beta2.7z"
Install-LootMenu
Install-Mod (Get-LatestModRelease)

Write-Step "Gotowe. Uruchamiaj grę przez fose_loader.exe, nie Fallout3.exe."
exit 0
