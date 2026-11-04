<#
.SYNOPSIS
    Packs up Winamp, Exodium and their settings for import-setup.ps1 to put on another PC.

.DESCRIPTION
    Close Winamp first. Into -Destination, which must be new or empty, this copies:

        Winamp\      The Winamp folder: the program, its plugins, and Exodium's history,
                     zoom levels and browser profile (Plugins\exodium_data).
        Settings\    The folder paths.ini sends Winamp's settings to (%APPDATA%\Winamp):
                     winamp.ini, playlists, the media library, bookmarks.
        Claude\      Claude Code's CLAUDE.md and settings.json, and its memory for this repo.
        setup.json   Where all that was on this PC, so paths can be changed to match the next.
        import-setup.ps1, register-browser.ps1 and README.txt, to set it up from.

    Left out:
      - The browser's caches, which pages fill again.
      - Its cookies, saved passwords and Local State. Windows locks them to this PC's
        account, so no other PC could read them, and sites have to be signed in to again.
        The rest of a site's data (localStorage, IndexedDB) comes along.
      - The *.bak files in Plugins, and wv2_userdata, an older build's browser profile.
      - Your music. The playlists and the media library point at it where it is now.

.EXAMPLE
    tools\export-setup.ps1 -Destination E:\winamp-move

    Onto a USB drive; use the letter it has.

.EXAMPLE
    tools\export-setup.ps1 -Destination $HOME\winamp-move

    Onto this PC, to be moved on some other way.
#>
param(
    # A new or empty folder, on a USB drive for instance.
    [Parameter(Mandatory)][string]$Destination,
    [string]$WinampDir = (Join-Path ([Environment]::GetFolderPath("MyDocuments")) "Winamp"),
    # The Exodium repo, whose memory in Claude Code goes along. By default the one this script is in.
    [string]$RepoDir
)

$ErrorActionPreference = "Stop"
# Not as the parameter's default: Windows PowerShell hasn't set $PSScriptRoot by then.
if (-not $RepoDir) { $RepoDir = Split-Path $PSScriptRoot -Parent }

# ---- Shared with import-setup.ps1, which has to work on its own ----

function FullPath([string]$path) {
    # A relative path is taken from PowerShell's current folder, not the process's. That way
    # fails for a drive that isn't there, which an old PC's path may be on, so only then.
    if (-not [IO.Path]::IsPathRooted($path)) { $path = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($path) }
    $full = [IO.Path]::GetFullPath($path)
    if ($full.Length -gt 3) { $full = $full.TrimEnd('\') } # but C:\ stays C:\
    $full
}

function IsUnder([string]$path, [string]$dir) {
    ((FullPath $path).TrimEnd('\') + '\').StartsWith((FullPath $dir).TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase)
}

function NotEmpty([string]$dir) {
    (Test-Path -LiteralPath $dir) -and $null -ne (Get-ChildItem -LiteralPath $dir -Force | Select-Object -First 1)
}

# Where Winamp keeps winamp.ini: the folder paths.ini's inidir names, in which {26} and the
# like are shell folders by their CSIDL number, or else Winamp's own folder.
function WinampSettingsDir([string]$pathsIni, [string]$winampDir) {
    $spec = $null
    if (Test-Path -LiteralPath $pathsIni) {
        $section = ""
        foreach ($line in Get-Content -LiteralPath $pathsIni) {
            if ($line -match '^\s*\[(.+)\]\s*$') { $section = $Matches[1] }
            elseif ($section -eq "Winamp" -and $line -match '^\s*inidir\s*=\s*(.*?)\s*$') { $spec = $Matches[1] }
        }
    }
    if (-not $spec) { return FullPath $winampDir }
    $spec = [regex]::Replace($spec, '\{(\d+)\}', { param($m) [Environment]::GetFolderPath([int]$m.Groups[1].Value) })
    $spec = [Environment]::ExpandEnvironmentVariables($spec)
    if (-not [IO.Path]::IsPathRooted($spec)) { $spec = Join-Path $winampDir $spec }
    FullPath $spec
}

function WinampRunningFrom([string]$dir) {
    @(Get-Process winamp -ErrorAction SilentlyContinue | Where-Object { $_.Path -and (IsUnder $_.Path $dir) })
}

# robocopy, which keeps up with the browser profile's long paths. It reads "C:\" as an
# escaped quote, hence "C:\.".
function CopyFolder([string]$from, [string]$to, [string[]]$skipDirs = @(), [string[]]$skipFiles = @()) {
    $arguments = @(
        $(if ($from.EndsWith('\')) { "$from." } else { $from }),
        $(if ($to.EndsWith('\')) { "$to." } else { $to }),
        "/E", "/DCOPY:DAT", "/XJ", "/MT:8", "/R:1", "/W:1", "/NP", "/NFL", "/NDL", "/NJH", "/NJS")
    if ($skipDirs) { $arguments += @("/XD") + $skipDirs }
    if ($skipFiles) { $arguments += @("/XF") + $skipFiles }
    & robocopy.exe @arguments | Where-Object { $_.Trim() } # only errors, with those switches
    if ($LASTEXITCODE -ge 8) { throw "Copying $from to $to failed (robocopy exit code $LASTEXITCODE), see above." }
}

# The folder Claude Code keeps a project's memory in is named after the project's path.
function ClaudeProjectKey([string]$dir) { [regex]::Replace((FullPath $dir), '[^A-Za-z0-9]', '-') }

function ClaudeHome { if ($env:CLAUDE_CONFIG_DIR) { $env:CLAUDE_CONFIG_DIR } else { Join-Path $env:USERPROFILE ".claude" } }

# Bytes in a folder, without the paths in $skip (a set) or anything behind a junction.
function SizeOf([string]$dir, $skip = $null) {
    $total = 0L
    foreach ($item in Get-ChildItem -LiteralPath $dir -Force -ErrorAction SilentlyContinue) {
        if (($skip -and $skip.Contains($item.FullName)) -or ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)) { continue }
        if ($item.PSIsContainer) { $total += SizeOf $item.FullName $skip } else { $total += $item.Length }
    }
    $total
}

function CheckDrive([string]$path) {
    $root = [IO.Path]::GetPathRoot($path)
    if (-not (Test-Path -LiteralPath $root)) {
        throw "There's no drive $root on this PC, which has $((Get-PSDrive -PSProvider FileSystem | ForEach-Object Root) -join ' '). Plug in the drive to use, or give a folder on one of those."
    }
}

# Whether $bytes fit on $path's drive, with some to spare. A network share can't say, so is let be.
function CheckRoom([string]$path, [long]$bytes) {
    $root = [IO.Path]::GetPathRoot($path)
    if ($root -notmatch '^[A-Za-z]:\\$') { return }
    $free = ([IO.DriveInfo]::new($root)).AvailableFreeSpace
    if ($free -lt $bytes + 200MB) { throw ("{0} has {1:N2} GB free, and this needs {2:N2} GB." -f $root, ($free / 1GB), ($bytes / 1GB)) }
}

# ---- Checks, before anything is written ----

$WinampDir = FullPath $WinampDir
$RepoDir = FullPath $RepoDir
$Destination = FullPath $Destination

CheckDrive $Destination
if (-not (Test-Path -LiteralPath (Join-Path $WinampDir "winamp.exe"))) { throw "There's no winamp.exe in $WinampDir. Give its folder with -WinampDir." }
foreach ($script in "import-setup.ps1", "register-browser.ps1") {
    if (-not (Test-Path -LiteralPath (Join-Path $PSScriptRoot $script))) { throw "$script should be beside this script, in $PSScriptRoot." }
}
if (WinampRunningFrom $WinampDir) { throw "Winamp is running. Close it first: its browser profile can't be copied while it's in use." }

$browserData = Join-Path $WinampDir "Plugins\exodium_data\webview_profile"
$webviews = @(Get-CimInstance Win32_Process -Filter "Name = 'msedgewebview2.exe'" |
    Where-Object { $_.CommandLine -and $_.CommandLine.IndexOf($browserData, [StringComparison]::OrdinalIgnoreCase) -ge 0 })
if ($webviews) { throw "Exodium's browser is still running ($($webviews.Count) msedgewebview2.exe), though Winamp isn't. Wait a few seconds and try again." }

$settingsDir = WinampSettingsDir (Join-Path $WinampDir "paths.ini") $WinampDir
$settingsInWinampDir = IsUnder $settingsDir $WinampDir
if (-not (Test-Path -LiteralPath $settingsDir)) { throw "Winamp's settings should be in $settingsDir, as paths.ini says, but that folder isn't there." }

if ((IsUnder $Destination $WinampDir) -or (IsUnder $Destination $settingsDir)) { throw "-Destination can't be inside what it's a copy of." }
if (NotEmpty $Destination) { throw "$Destination isn't empty. Give a new or empty folder." }

# ---- What to leave out ----

$plugins = Join-Path $WinampDir "Plugins"
$browser = Join-Path $browserData "EBWebView"
$skipDirs = @(Join-Path $WinampDir "wv2_userdata")
$skipFiles = @(Get-ChildItem -LiteralPath $plugins -File | Where-Object { $_.Name -like "*.bak" } | ForEach-Object FullName)
if (Test-Path -LiteralPath $browser) {
    # Caches.
    foreach ($name in "BrowserMetrics", "component_crx_cache", "Crashpad", "extensions_crx_cache", "GPUPersistentCache",
                      "GraphiteDawnCache", "GrShaderCache", "ShaderCache") {
        $skipDirs += Join-Path $browser $name
    }
    # It holds the key that cookies and passwords are encrypted with, which Windows has locked to
    # this PC's account. Without it the browser makes a new key.
    $skipFiles += Join-Path $browser "Local State"
    foreach ($browserProfile in Get-ChildItem -LiteralPath $browser -Directory | Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName "Preferences") }) {
        foreach ($name in "Cache", "Code Cache", "DawnGraphiteCache", "DawnWebGPUCache", "GPUCache", "Shared Dictionary", "shared_proto_db") {
            $skipDirs += Join-Path $browserProfile.FullName $name
        }
        foreach ($name in "Cookies", "Cookies-journal", "Network\Cookies", "Network\Cookies-journal", "Login Data", "Login Data-journal",
                          "Login Data For Account", "Login Data For Account-journal") {
            $skipFiles += Join-Path $browserProfile.FullName $name
        }
    }
}

$skip = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
foreach ($path in $skipDirs + $skipFiles) { [void]$skip.Add($path) }
$needed = SizeOf $WinampDir $skip
if (-not $settingsInWinampDir) { $needed += SizeOf $settingsDir }
CheckRoom $Destination $needed

# ---- Copying ----

New-Item -ItemType Directory -Path $Destination -Force | Out-Null

"Copying Winamp from $WinampDir ..."
CopyFolder $WinampDir (Join-Path $Destination "Winamp") $skipDirs $skipFiles

if (-not $settingsInWinampDir) {
    "Copying Winamp's settings from $settingsDir ..."
    CopyFolder $settingsDir (Join-Path $Destination "Settings")
}

$claudeHome = ClaudeHome
$claudeOut = Join-Path $Destination "Claude"
$memory = Join-Path $claudeHome ("projects\" + (ClaudeProjectKey $RepoDir) + "\memory")
foreach ($name in "CLAUDE.md", "settings.json") {
    $from = Join-Path $claudeHome $name
    if (Test-Path -LiteralPath $from) {
        New-Item -ItemType Directory -Path $claudeOut -Force | Out-Null
        Copy-Item -LiteralPath $from -Destination $claudeOut
    }
}
if (NotEmpty $memory) {
    "Copying Claude Code's memory for $RepoDir ..."
    CopyFolder $memory (Join-Path $claudeOut "memory")
}

foreach ($script in "import-setup.ps1", "register-browser.ps1") {
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot $script) -Destination $Destination
}

[ordered]@{
    exportedAt          = (Get-Date).ToString("s")
    winampDir           = $WinampDir
    settingsDir         = $settingsDir
    settingsInWinampDir = $settingsInWinampDir
    userProfile         = FullPath $env:USERPROFILE
    repoDir             = $RepoDir
} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $Destination "setup.json") -Encoding UTF8

@"
Winamp with Exodium, packed up by export-setup.ps1 on $((Get-Date).ToString("yyyy-MM-dd")).

On the new PC, close Winamp if one is running, then in PowerShell run import-setup.ps1
from this folder:

    powershell -ExecutionPolicy Bypass -File "$(Join-Path $Destination "import-setup.ps1")"

(with this folder's path, if it has moved). It puts Winamp in Documents\Winamp, or in
%USERPROFILE%\Winamp when Documents is in OneDrive; -WinampDir puts it somewhere else.
"Get-Help .\import-setup.ps1 -Full" tells the rest.
"@ | Set-Content -LiteralPath (Join-Path $Destination "README.txt") -Encoding UTF8

# ---- Report ----

$size = SizeOf $Destination
""
"Packed up in $Destination ({0:N2} GB)." -f ($size / 1GB)

$genMl = Join-Path $settingsDir "Plugins\gen_ml.ini"
if (Test-Path -LiteralPath $genMl) {
    $music = foreach ($line in Get-Content -LiteralPath $genMl -Encoding UTF8) {
        if ($line -match '^scandirlist_utf8=(.+)$') { $Matches[1] -split '\|' | Where-Object { $_ } }
    }
    if ($music) { "Your music isn't in it. Copy it to the new PC yourself: $($music -join ', ')" }
}
"On the new PC, run import-setup.ps1 from that folder; README.txt there says how."
