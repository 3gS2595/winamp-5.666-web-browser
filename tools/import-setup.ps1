<#
.SYNOPSIS
    Sets up Winamp, Exodium and their settings on this PC, from what export-setup.ps1
    packed up on another.

.DESCRIPTION
    Close Winamp first, if this PC has one running, and run this from the folder
    export-setup.ps1 made:

        powershell -ExecutionPolicy Bypass -File E:\winamp-move\import-setup.ps1

    It copies Winamp to -WinampDir and its settings to the folder paths.ini names
    (%APPDATA%\Winamp), and won't copy over either if something is there already. Where
    this PC's user folder or Winamp folder differ from the old PC's, the paths in
    winamp.ini, playlists and bookmarks are changed to match. Then it:

      - Installs the WebView2 runtime with winget, if this PC doesn't have it.
      - Puts Winamp and Exodium in the Start menu, registers Exodium as a browser, and
        opens Default apps, where choosing it is up to you: Windows doesn't let programs
        do that.
      - Tells Claude Code about Exodium's browser tools, and puts back CLAUDE.md, the
        exodium permissions in settings.json, and Claude's memory of the Exodium repo.
        What this PC already has is kept.

    Nothing else needs installing for Exodium: it has the C++ runtime built in.

.EXAMPLE
    .\import-setup.ps1

.EXAMPLE
    .\import-setup.ps1 -WinampDir D:\Winamp -RepoDir D:\src\exodium
#>
param(
    # Where Winamp goes. By default Documents\Winamp, as on the old PC, or %USERPROFILE%\Winamp
    # when Documents is kept in OneDrive, which would be syncing the browser's profile as it changes.
    [string]$WinampDir,
    # Where the Exodium repo is on this PC, for Claude's memory of it. By default where it was,
    # under this PC's user folder if it was under the old one's.
    [string]$RepoDir,
    # Leave the registry, the Start menu and Default apps alone.
    [switch]$SkipWindowsSetup,
    # Leave Claude Code alone.
    [switch]$SkipClaude
)

$ErrorActionPreference = "Stop"

# ---- Shared with export-setup.ps1; this one has to work on its own ----

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

# ---- Changing the old PC's paths to this one's ----

# The pairs of old and new paths that differ. Playlists name a file on their own drive
# without the drive's letter, so those go in too.
function PathMoves($moves) {
    foreach ($move in $moves) {
        $old = $move[0].TrimEnd('\')
        $new = $move[1].TrimEnd('\')
        if ($old -eq $new) { continue } # -eq ignores case, as Windows does
        [pscustomobject]@{ Old = $old; New = $new }
        if ($old -match '^[A-Za-z]:\\' -and $new -match '^[A-Za-z]:\\' -and $old.Substring(0, 2) -eq $new.Substring(0, 2)) {
            [pscustomobject]@{ Old = $old.Substring(2); New = $new.Substring(2) }
        }
    }
}

$latin1 = [Text.Encoding]::GetEncoding(28591)

# Changes the paths in one text file, all in one pass so that nothing is changed twice, and
# only where a whole folder name matches. UTF-16 files are read as such. Anything else is
# taken byte for byte, so whatever else is in it comes back unchanged whatever its encoding,
# and a path is looked for both as UTF-8 and in the ANSI code page. Returns whether it changed.
function UpdatePaths([string]$file, $pairs) {
    $bytes = [IO.File]::ReadAllBytes($file)
    $utf16 = $bytes.Length -ge 2 -and $bytes[0] -eq 0xFF -and $bytes[1] -eq 0xFE
    $map = @{} # ignores case
    if ($utf16) {
        $text = [Text.Encoding]::Unicode.GetString($bytes, 2, $bytes.Length - 2)
        foreach ($pair in $pairs) { if (-not $map.ContainsKey($pair.Old)) { $map[$pair.Old] = $pair.New } }
    }
    else {
        $text = $latin1.GetString($bytes)
        foreach ($pair in $pairs) {
            foreach ($encoding in [Text.Encoding]::UTF8, [Text.Encoding]::Default) {
                $old = $latin1.GetString($encoding.GetBytes($pair.Old))
                if (-not $map.ContainsKey($old)) { $map[$old] = $latin1.GetString($encoding.GetBytes($pair.New)) }
            }
        }
    }
    $alternatives = ($map.Keys | Sort-Object Length -Descending | ForEach-Object { [regex]::Escape($_) }) -join '|'
    $pattern = '(?:' + $alternatives + ')(?=[\\/"''<|;,\r\n]|$)'
    $result = [regex]::Replace($text, $pattern, { param($m) $map[$m.Value] }, "IgnoreCase, CultureInvariant")
    if ($result -ceq $text) { return $false }
    if ($utf16) { [IO.File]::WriteAllText($file, $result, [Text.Encoding]::Unicode) } # with its byte order mark
    else { [IO.File]::WriteAllBytes($file, $latin1.GetBytes($result)) }
    $true
}

# The files in a Winamp folder, without going through the browser's profile.
function WinampFiles([string]$dir) {
    foreach ($item in Get-ChildItem -LiteralPath $dir -Force) {
        if (-not $item.PSIsContainer) { $item }
        elseif ($item.Name -ne "exodium_data") { WinampFiles $item.FullName }
    }
}

function WebView2Version {
    $id = "{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}" # the Evergreen runtime's, as Microsoft documents
    foreach ($key in "HKLM:\SOFTWARE\WOW6432Node\Microsoft\EdgeUpdate\Clients\$id", "HKLM:\SOFTWARE\Microsoft\EdgeUpdate\Clients\$id",
                     "HKCU:\Software\Microsoft\EdgeUpdate\Clients\$id") {
        $version = (Get-ItemProperty -LiteralPath $key -Name pv -ErrorAction SilentlyContinue).pv
        if ($version -and $version -ne "0.0.0.0") { return $version }
    }
}

# Runs a program whose complaints on stderr shouldn't stop this script, as Windows
# PowerShell would make them do. Gives back what it printed and whether it succeeded.
function RunTolerant([scriptblock]$block) {
    $saved = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
        $output = & $block 2>&1 | ForEach-Object { "$_" }
        [pscustomobject]@{ Ok = $LASTEXITCODE -eq 0; Output = ($output -join "`n").Trim() }
    }
    finally { $ErrorActionPreference = $saved }
}

# ---- Checks, before anything is written ----

$bundle = $PSScriptRoot
$setupFile = Join-Path $bundle "setup.json"
if (-not (Test-Path -LiteralPath $setupFile)) { throw "There's no setup.json beside this script. Run the copy of it in the folder export-setup.ps1 made." }
$setup = Get-Content -LiteralPath $setupFile -Raw | ConvertFrom-Json
$fromWinamp = Join-Path $bundle "Winamp"
$fromSettings = Join-Path $bundle "Settings"
$fromClaude = Join-Path $bundle "Claude"
if (-not (Test-Path -LiteralPath (Join-Path $fromWinamp "winamp.exe"))) { throw "$fromWinamp should have winamp.exe in it." }

if (-not $WinampDir) {
    $documents = [Environment]::GetFolderPath("MyDocuments")
    $inOneDrive = @($env:OneDrive, $env:OneDriveConsumer, $env:OneDriveCommercial | Where-Object { $_ -and (IsUnder $documents $_) }).Count -gt 0
    $WinampDir = if ($inOneDrive) { Join-Path $env:USERPROFILE "Winamp" } else { Join-Path $documents "Winamp" }
}
$WinampDir = FullPath $WinampDir
$settingsDir = WinampSettingsDir (Join-Path $fromWinamp "paths.ini") $WinampDir

CheckDrive $WinampDir
CheckDrive $settingsDir
if (WinampRunningFrom $WinampDir) { throw "Winamp is running from $WinampDir. Close it first." }
if (NotEmpty $WinampDir) { throw "$WinampDir has things in it already. Move them away, or give another folder with -WinampDir." }
if (-not $setup.settingsInWinampDir) {
    if (-not (Test-Path -LiteralPath $fromSettings)) { throw "$fromSettings, Winamp's settings, is missing." }
    if (NotEmpty $settingsDir) {
        throw "$settingsDir, where Winamp keeps its settings, has things in it already (from a Winamp installed or run on this PC?). Move that folder away first, so the two aren't mixed."
    }
}
$winampBytes = SizeOf $fromWinamp
$settingsBytes = if ($setup.settingsInWinampDir) { 0L } else { SizeOf $fromSettings }
if ([IO.Path]::GetPathRoot($WinampDir) -eq [IO.Path]::GetPathRoot($settingsDir)) { CheckRoom $WinampDir ($winampBytes + $settingsBytes) }
else { CheckRoom $WinampDir $winampBytes; CheckRoom $settingsDir $settingsBytes }

# ---- Copying ----

$todo = [Collections.Generic.List[string]]::new()

"Copying Winamp to $WinampDir ..."
CopyFolder $fromWinamp $WinampDir
if (-not $setup.settingsInWinampDir) {
    "Copying its settings to $settingsDir ..."
    CopyFolder $fromSettings $settingsDir
}

# In case they came by way of a download, which would have Windows ask before running them.
WinampFiles $WinampDir | Where-Object { $_.Extension -in ".exe", ".dll" } | Unblock-File

$pairs = @(PathMoves @(
    , @($setup.winampDir, $WinampDir)
    , @($setup.settingsDir, $settingsDir)
    , @($setup.userProfile, (FullPath $env:USERPROFILE))
))
if ($pairs) {
    "The old PC's paths are different here:"
    foreach ($pair in $pairs) { "  $($pair.Old)  ->  $($pair.New)" }
    $textFiles = @(WinampFiles $WinampDir) + @(Get-ChildItem -LiteralPath $settingsDir -Recurse -File -Force) |
        Where-Object { $_.Extension -in ".ini", ".bm", ".bm8", ".m3u", ".m3u8", ".pls", ".xml", ".xnf" } |
        Sort-Object FullName -Unique
    $changed = @($textFiles | Where-Object { UpdatePaths $_.FullName $pairs })
    "Changed them in $($changed.Count) file(s): $(($changed | ForEach-Object Name) -join ', ')"
    $todo.Add("The media library lists your music where it was. Once the music is in place, rescan it (Media Library > Local Media) and remove the missing files.")
}

# ---- Windows ----

$webView2 = WebView2Version
if (-not $webView2 -and (Get-Command winget -ErrorAction SilentlyContinue)) {
    "Installing the WebView2 runtime, which Exodium shows pages with ..."
    $result = RunTolerant { winget install --id Microsoft.EdgeWebView2Runtime --exact --silent --accept-package-agreements --accept-source-agreements }
    if ($result.Output) { $result.Output }
    $webView2 = WebView2Version
}
if ($webView2) { "WebView2 runtime: $webView2" }
else { $todo.Add("Install the WebView2 runtime (the Evergreen Bootstrapper): https://developer.microsoft.com/microsoft-edge/webview2/") }

if (-not $SkipWindowsSetup) {
    # Where plugin installers look for Winamp. An install of Winamp's own is left alone.
    $key = [Microsoft.Win32.Registry]::CurrentUser.CreateSubKey("Software\Winamp")
    try {
        $current = $key.GetValue("")
        if (-not $current -or -not (Test-Path -LiteralPath (Join-Path $current "winamp.exe"))) { $key.SetValue("", $WinampDir) }
    }
    finally { $key.Close() }

    $shortcut = Join-Path ([Environment]::GetFolderPath("Programs")) "Winamp.lnk"
    if (-not (Test-Path -LiteralPath $shortcut)) {
        $link = (New-Object -ComObject WScript.Shell).CreateShortcut($shortcut)
        $link.TargetPath = Join-Path $WinampDir "winamp.exe"
        $link.WorkingDirectory = $WinampDir
        $link.Save()
        "Put Winamp in the Start menu."
    }

    & (Join-Path $bundle "register-browser.ps1") -WinampDir $WinampDir
    Start-Process "ms-settings:defaultapps"
    $todo.Add("In Default apps, which has just opened, set the web browser to Exodium.")
}

# ---- Claude Code ----

if (-not $SkipClaude -and (Test-Path -LiteralPath $fromClaude)) {
    $claudeHome = ClaudeHome
    New-Item -ItemType Directory -Path $claudeHome -Force | Out-Null

    $from = Join-Path $fromClaude "CLAUDE.md"
    $to = Join-Path $claudeHome "CLAUDE.md"
    if (Test-Path -LiteralPath $from) {
        if (-not (Test-Path -LiteralPath $to)) { Copy-Item -LiteralPath $from -Destination $to; "Put back Claude Code's CLAUDE.md." }
        elseif ((Get-FileHash -LiteralPath $from).Hash -ne (Get-FileHash -LiteralPath $to).Hash) {
            $aside = Join-Path $claudeHome "CLAUDE.from-old-pc.md"
            Copy-Item -LiteralPath $from -Destination $aside
            $todo.Add("Claude Code already had a CLAUDE.md here, so the old PC's is beside it as $aside. Merge the two.")
        }
    }

    # settings.json: all of it if there's none, or else only the old PC's permissions for
    # Exodium's tools, added to what's there.
    $from = Join-Path $fromClaude "settings.json"
    $to = Join-Path $claudeHome "settings.json"
    if (Test-Path -LiteralPath $from) {
        if (-not (Test-Path -LiteralPath $to)) { Copy-Item -LiteralPath $from -Destination $to; "Put back Claude Code's settings.json." }
        else {
            try {
                $old = Get-Content -LiteralPath $from -Raw | ConvertFrom-Json
                $current = Get-Content -LiteralPath $to -Raw | ConvertFrom-Json
                $added = @()
                foreach ($list in "allow", "ask", "deny") {
                    $wanted = @($old.permissions.$list | Where-Object { $_ -like "mcp__exodium*" })
                    if (-not $wanted) { continue }
                    if (-not $current.permissions) { $current | Add-Member -NotePropertyName permissions -NotePropertyValue ([pscustomobject]@{}) }
                    $have = @($current.permissions.$list | Where-Object { $_ })
                    $missing = @($wanted | Where-Object { $have -notcontains $_ })
                    if (-not $missing) { continue }
                    $current.permissions | Add-Member -NotePropertyName $list -NotePropertyValue ([object[]]($have + $missing)) -Force
                    $added += $missing | ForEach-Object { "$list $_" }
                }
                if ($added) {
                    Copy-Item -LiteralPath $to -Destination "$to.before-import.bak" -Force
                    [IO.File]::WriteAllText($to, ($current | ConvertTo-Json -Depth 64), (New-Object Text.UTF8Encoding $false))
                    "Added to Claude Code's settings.json: $($added -join ', ') (the file as it was is settings.json.before-import.bak)"
                }
            }
            catch {
                $todo.Add("Couldn't add Exodium's permissions to $to ($($_.Exception.Message)). The old PC's settings.json is in $fromClaude.")
            }
        }
    }

    $fromMemory = Join-Path $fromClaude "memory"
    if (Test-Path -LiteralPath $fromMemory) {
        # By default where it was, moved to this PC's user folder if it was in the old one's.
        $repo = if ($RepoDir) { FullPath $RepoDir }
                elseif (IsUnder $setup.repoDir $setup.userProfile) { $env:USERPROFILE.TrimEnd('\') + $setup.repoDir.Substring($setup.userProfile.TrimEnd('\').Length) }
                else { $setup.repoDir }
        $toMemory = Join-Path $claudeHome ("projects\" + (ClaudeProjectKey $repo) + "\memory")
        if (NotEmpty $toMemory) { $todo.Add("Claude Code already has a memory of $repo here, so the old PC's wasn't put over it. It's in $fromMemory.") }
        else {
            CopyFolder $fromMemory $toMemory
            "Put back Claude Code's memory of $repo (it's used when Claude runs in that folder)."
        }
    }

    $mcp = Join-Path $WinampDir "Plugins\exodium_mcp.mjs"
    if (Get-Command claude -ErrorAction SilentlyContinue) {
        RunTolerant { claude mcp remove exodium --scope user } | Out-Null
        $result = RunTolerant { claude mcp add --scope user exodium node $mcp }
        if ($result.Ok) { "Told Claude Code about Exodium's browser tools." }
        else { $todo.Add("Telling Claude Code about Exodium's tools failed: $($result.Output)") }
    }
    else { $todo.Add("Install Claude Code, then run: claude mcp add --scope user exodium node `"$mcp`"") }
    if (-not (Get-Command node -ErrorAction SilentlyContinue)) { $todo.Add("Install Node.js. Exodium's browser tools for Claude Code run on it.") }
}

# ---- What's left ----

$genMl = Join-Path $settingsDir "Plugins\gen_ml.ini"
if (Test-Path -LiteralPath $genMl) {
    foreach ($line in Get-Content -LiteralPath $genMl -Encoding UTF8) {
        if ($line -match '^scandirlist_utf8=(.+)$') {
            foreach ($dir in $Matches[1] -split '\|' | Where-Object { $_ -and -not (Test-Path -LiteralPath $_) }) {
                $todo.Insert(0, "Copy your music to $dir, where Winamp's playlists and media library look for it.")
            }
        }
    }
}
$todo.Add("Sign in to your sites again in Exodium. Sign-ins can't move between PCs.")
$todo.Add("To have music files open in Winamp: Options > Preferences > File Types.")

""
"Done. Winamp is in $WinampDir. Still to do:"
foreach ($item in $todo) { "  - $item" }
