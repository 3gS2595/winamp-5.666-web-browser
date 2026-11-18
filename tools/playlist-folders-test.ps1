<#
.SYNOPSIS
Runs ml_playlist_folders.dll in a scratch copy of Winamp, out of sight, and checks it.

.DESCRIPTION
Copies Winamp and its settings to -ScratchDir (the first time), puts the plugin in,
replaces the media library's saved playlists in the copy with five of silent songs it
makes, and starts that Winamp with a window class of its own, beside the real one, on a
desktop of its own. It works the folders through the plugin's test commands (WM_COPYDATA
to its message-only window), ml_playlists' own dialogs and keys, and the folder view's
buttons, reads the tree back from the plugin, restarts Winamp to see it all kept, and
saves screenshots in -ScratchDir\shots.
#>
param(
    [string]$PluginPath = "$PSScriptRoot\..\bin\Win32\Release\ml_playlist_folders.dll",
    [string]$WinampDir = "$env:USERPROFILE\Documents\Winamp",
    [string]$SettingsDir = "$env:APPDATA\Winamp",
    [string]$ScratchDir = "$env:TEMP\playlist-folders-test",
    [string]$Instance = "PlaylistFoldersTest",
    # A copy of the user's own saved playlists instead of the test's five: folders made,
    # half of them moved in, Winamp started again, and it all checked.
    [switch]$RealPlaylists,
    [switch]$OnScreen,
    [switch]$OnOwnDesktop
)

$ErrorActionPreference = "Stop"
$wa = Join-Path $ScratchDir "Winamp"
$ini = Join-Path $ScratchDir "settings"
$shots = Join-Path $ScratchDir "shots"
$media = Join-Path $ScratchDir "media"

if (-not $OnScreen -and -not $OnOwnDesktop) {
    Add-Type @"
using System;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text;
public static class FoldersDesktop {
    [DllImport("user32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    public static extern IntPtr CreateDesktopW(string name, IntPtr device, IntPtr mode, uint flags, uint access, IntPtr security);
    [DllImport("user32.dll")] public static extern bool CloseDesktop(IntPtr desktop);
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    struct STARTUPINFO {
        public int cb; public string lpReserved; public string lpDesktop; public string lpTitle;
        public int dwX, dwY, dwXSize, dwYSize, dwXCountChars, dwYCountChars, dwFillAttribute, dwFlags;
        public short wShowWindow, cbReserved2; public IntPtr lpReserved2, hStdInput, hStdOutput, hStdError;
    }
    [StructLayout(LayoutKind.Sequential)]
    struct PROCESS_INFORMATION { public IntPtr hProcess, hThread; public int dwProcessId, dwThreadId; }
    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern bool CreateProcessW(string application, StringBuilder commandLine, IntPtr processAttributes, IntPtr threadAttributes,
                                      bool inheritHandles, uint flags, IntPtr environment, string directory, ref STARTUPINFO startup,
                                      out PROCESS_INFORMATION info);
    [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr handle);
    public static Process Start(string desktop, string commandLine, string directory) {
        var startup = new STARTUPINFO();
        startup.cb = Marshal.SizeOf(startup);
        startup.lpDesktop = desktop;
        PROCESS_INFORMATION info;
        if (!CreateProcessW(null, new StringBuilder(commandLine), IntPtr.Zero, IntPtr.Zero, false, 0x08000000, IntPtr.Zero,
                            directory, ref startup, out info)) {
            throw new System.ComponentModel.Win32Exception();
        }
        try {
            Process process = Process.GetProcessById(info.dwProcessId);
            IntPtr kept = process.Handle;
            return process;
        } finally {
            CloseHandle(info.hThread);
            CloseHandle(info.hProcess);
        }
    }
}
"@
    $desktop = [FoldersDesktop]::CreateDesktopW($Instance, [IntPtr]::Zero, [IntPtr]::Zero, 0, 0x10000000, [IntPtr]::Zero)
    if ($desktop -eq [IntPtr]::Zero) { throw "Couldn't make a desktop to run on. -OnScreen runs without one." }
    $log = Join-Path ([IO.Path]::GetTempPath()) "playlist-folders-test-$Instance.log"
    Remove-Item $log -ErrorAction SilentlyContinue
    $quote = { param($text) "'" + ($text -replace "'", "''") + "'" }
    $arguments = "-OnOwnDesktop -PluginPath $(& $quote (Resolve-Path $PluginPath).Path) -WinampDir $(& $quote $WinampDir) " +
                 "-SettingsDir $(& $quote $SettingsDir) -ScratchDir $(& $quote $ScratchDir) -Instance $(& $quote $Instance)"
    if ($RealPlaylists) { $arguments += " -RealPlaylists" }
    $command = "& $(& $quote $PSCommandPath) $arguments *> $(& $quote $log); exit `$LASTEXITCODE"
    $commandLine = "`"$PSHOME\powershell.exe`" -NoProfile -ExecutionPolicy Bypass -Command `"$command`""
    "(running on a desktop of its own, out of sight; -OnScreen to watch it)"
    $child = $null; $reader = $null; $exitCode = 1
    try {
        $child = [FoldersDesktop]::Start($Instance, $commandLine, (Get-Location).Path)
        $pending = ""
        while ($true) {
            $finished = $child.HasExited
            if (-not $reader -and (Test-Path $log)) {
                $reader = New-Object IO.StreamReader((New-Object IO.FileStream($log, "Open", "Read", "ReadWrite")), $true)
            }
            if ($reader) {
                $lines = ($pending + $reader.ReadToEnd()) -split "`r?`n"
                $pending = $lines[-1]
                $lines | Select-Object -SkipLast 1
            }
            if ($finished) { break }
            Start-Sleep -Milliseconds 300
        }
        if ($pending) { $pending }
        $exitCode = $child.ExitCode
    } finally {
        if ($reader) { $reader.Dispose() }
        if ($child -and -not $child.HasExited) { & taskkill /pid $child.Id /t /f | Out-Null }
        foreach ($left in @(Get-Process winamp -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq "$wa\winamp.exe" })) {
            "  (stopping the test Winamp, left running on the test desktop)"
            $left.Kill()
        }
        [void][FoldersDesktop]::CloseDesktop($desktop)
    }
    exit $exitCode
}

Add-Type -ReferencedAssemblies System.Drawing @"
using System;
using System.Collections.Generic;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;
using System.Text;
public static class W {
    public delegate bool EnumProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc p, IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr parent, EnumProc p, IntPtr l);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern bool IsWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern IntPtr SendMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("user32.dll", CharSet = CharSet.Unicode, EntryPoint = "SendMessageW")]
    public static extern IntPtr SendText(IntPtr h, uint m, IntPtr w, string l);
    [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
    // Handled as sent (not through the thread's queue and its accelerators), without waiting.
    [DllImport("user32.dll")] public static extern bool SendNotifyMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags);
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
    [DllImport("user32.dll")] public static extern int GetDlgCtrlID(IntPtr h);
    [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr h, int id);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern IntPtr FindWindowExW(IntPtr parent, IntPtr after, string className, string title);
    public struct RECT { public int Left, Top, Right, Bottom; }
    [DllImport("user32.dll")] public static extern int GetMenuItemCount(IntPtr menu);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetMenuStringW(IntPtr menu, uint item, StringBuilder s, int n, uint flags);
    [DllImport("user32.dll")] public static extern uint GetMenuState(IntPtr menu, uint item, uint flags);
    public static string MenuText(IntPtr menu, int position) { var s = new StringBuilder(256); GetMenuStringW(menu, (uint)position, s, 256, 0x400); return s.ToString(); }
    [StructLayout(LayoutKind.Sequential)]
    struct COPYDATASTRUCT { public IntPtr dwData; public int cbData; public IntPtr lpData; }
    public static string ClassOf(IntPtr h) { var s = new StringBuilder(256); GetClassName(h, s, 256); return s.ToString(); }
    public static string TextOf(IntPtr h) { var s = new StringBuilder(512); GetWindowText(h, s, 512); return s.ToString(); }
    public static List<IntPtr> TopWindows(uint pid) {
        var found = new List<IntPtr>();
        EnumWindows((h, l) => { uint p; GetWindowThreadProcessId(h, out p); if (p == pid) found.Add(h); return true; }, IntPtr.Zero);
        return found;
    }
    public static List<IntPtr> Descendants(IntPtr parent) {
        var found = new List<IntPtr>();
        EnumChildWindows(parent, (h, l) => { found.Add(h); return true; }, IntPtr.Zero);
        return found;
    }
    public static IntPtr ChildOfClass(IntPtr parent, string name) {
        IntPtr found = IntPtr.Zero;
        EnumChildWindows(parent, (h, l) => { if (ClassOf(h) == name) { found = h; return false; } return true; }, IntPtr.Zero);
        return found;
    }
    // The plugin's message-only window in that process.
    public static IntPtr MessageWindow(string className, uint pid) {
        IntPtr after = IntPtr.Zero;
        while (true) {
            after = FindWindowExW((IntPtr)(-3), after, className, null);
            if (after == IntPtr.Zero) return IntPtr.Zero;
            uint p; GetWindowThreadProcessId(after, out p);
            if (p == pid) return after;
        }
    }
    public static long Command(IntPtr window, string text) {
        byte[] bytes = Encoding.Unicode.GetBytes(text);
        IntPtr data = Marshal.AllocHGlobal(Math.Max(2, bytes.Length));
        Marshal.Copy(bytes, 0, data, bytes.Length);
        var copy = new COPYDATASTRUCT { dwData = (IntPtr)0x50465431, cbData = bytes.Length, lpData = data };
        IntPtr block = Marshal.AllocHGlobal(Marshal.SizeOf(copy));
        Marshal.StructureToPtr(copy, block, false);
        try { return SendMessageW(window, 0x4A, IntPtr.Zero, block).ToInt64(); }
        finally { Marshal.FreeHGlobal(block); Marshal.FreeHGlobal(data); }
    }
    public static void Capture(IntPtr h, string path) {
        RECT r; GetWindowRect(h, out r);
        int width = Math.Max(1, r.Right - r.Left), height = Math.Max(1, r.Bottom - r.Top);
        using (var bitmap = new Bitmap(width, height, PixelFormat.Format24bppRgb))
        using (var g = Graphics.FromImage(bitmap)) {
            IntPtr dc = g.GetHdc();
            bool ok = PrintWindow(h, dc, 2);
            if (!ok) PrintWindow(h, dc, 0);
            g.ReleaseHdc(dc);
            bitmap.Save(path, ImageFormat.Png);
        }
    }
}
"@
[void][W]::SetProcessDPIAware()

$failures = 0
function Check([bool]$ok, [string]$what) {
    if ($ok) { "  PASS  $what" } else { "  FAIL  $what"; $script:failures++ }
}
function WaitFor([scriptblock]$cond, [int]$seconds = 20) {
    $until = (Get-Date).AddSeconds($seconds)
    while ((Get-Date) -lt $until) { if (& $cond) { return $true }; Start-Sleep -Milliseconds 100 }
    return [bool](& $cond)
}
function Ipc($main, [int]$w, [int]$command) { [int64][W]::SendMessageW($main, 0x400, [IntPtr]$w, [IntPtr]$command) }
function Shot($window, [string]$name) {
    $path = Join-Path $shots $name
    [W]::Capture($window, $path)
    "        screenshot: $path"
}

# ---- The scratch Winamp

if (-not (Test-Path "$wa\winamp.exe")) {
    "Copying $WinampDir to $wa ..."
    # Left out: Exodium (its browser and its data), and the plug-ins that play a sound at
    # start-up, take global keys, or report crashes by mail.
    $skipDirs = @("$WinampDir\Plugins\exodium_data", "$WinampDir\wv2_userdata")
    robocopy $WinampDir $wa /E /XD $skipDirs /XF gen_llama.dll gen_tray.dll gen_hotkeys.dll gen_crasher.dll "gen_browser.dll*" "in_exodium.dll*" "exodium_*" "*.bak" /NFL /NDL /NJH /NJS /NP /R:1 /W:1 | Out-Null
    robocopy $SettingsDir $ini /E /NFL /NDL /NJH /NJS /NP /R:1 /W:1 | Out-Null
}
Set-Content -Path "$wa\paths.ini" -Value "[Winamp]`r`ninidir=$ini" -Encoding ascii
Copy-Item $PluginPath "$wa\Plugins\ml_playlist_folders.dll" -Force
New-Item -ItemType Directory -Force $shots | Out-Null
$foldersFile = Join-Path $ini "Plugins\ml\playlist_folders.txt"
Remove-Item $foldersFile -ErrorAction SilentlyContinue

# ---- Songs, silent ones (a hidden desktop doesn't hide sound)

$ffmpeg = "C:\ffmpeg\bin\ffmpeg.exe"
if (-not (Test-Path "$media\done.txt")) {
    "Making silent test songs in $media ..."
    Remove-Item $media -Recurse -Force -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Force $media | Out-Null
    foreach ($n in 1..8) {
        & $ffmpeg -hide_banner -loglevel error -y -f lavfi -t 3 -i "anullsrc=r=22050:cl=mono" -c:a libmp3lame -b:a 32k `
            -metadata "title=Quiet $n" -metadata "artist=Nobody" "$media\quiet$n.mp3"
    }
    Set-Content "$media\done.txt" "made"
}
$song = 1..8 | ForEach-Object { "$media\quiet$_.mp3" }

# ---- The library's playlists: the test's own, in place of the user's (in the copy)

$playlistsDir = Join-Path $ini "Plugins\ml\playlists"
function WriteLibrary([hashtable[]]$lists) {
    New-Item -ItemType Directory -Force $playlistsDir | Out-Null
    Get-ChildItem $playlistsDir -File | Remove-Item
    $xml = "<?xml version=`"1.0`" encoding=`"UTF-16`"?><playlists playlists=`"$($lists.Count)`">"
    foreach ($list in $lists) {
        $lines = @("#EXTM3U")
        foreach ($entry in $list.songs) { $lines += "#EXTINF:3,$([IO.Path]::GetFileNameWithoutExtension($entry))"; $lines += $entry }
        [IO.File]::WriteAllLines((Join-Path $playlistsDir $list.file), $lines, (New-Object Text.UTF8Encoding($true)))
        $xml += "<playlist filename=`"$($list.file)`" title=`"$($list.title)`" id=`"{$([guid]::NewGuid().ToString().ToUpper())}`" songs=`"$($list.songs.Count)`" seconds=`"$(3 * $list.songs.Count)`"/>"
    }
    $xml += "</playlists>"
    [IO.File]::WriteAllText((Join-Path $playlistsDir "playlists.xml"), $xml, (New-Object Text.UnicodeEncoding($false, $true)))
}
function SetLibraryIni([string]$lastView) {
    $file = Join-Path $ini "Plugins\gen_ml.ini"
    $lines = [IO.File]::ReadAllLines($file, [Text.Encoding]::Default) | Where-Object { $_ -notmatch '^(last_view|bgrescan_do|visible)=' }
    $lines = @($lines[0], "last_view=$lastView", "bgrescan_do=0", "visible=1") + ($lines | Select-Object -Skip 1)
    [IO.File]::WriteAllLines($file, $lines, [Text.Encoding]::Default)
}
function LastView {
    $line = [IO.File]::ReadAllLines((Join-Path $ini "Plugins\gen_ml.ini"), [Text.Encoding]::UTF8) | Where-Object { $_ -match '^last_view=' } | Select-Object -First 1
    if ($line) { $line.Substring(10) } else { "" }
}
function WriteWinampPlaylist {
    [IO.File]::WriteAllLines((Join-Path $ini "winamp.m3u8"), @("#EXTM3U", $song[7]), (New-Object Text.UTF8Encoding($true)))
    Remove-Item (Join-Path $ini "Winamp.m3u") -ErrorAction SilentlyContinue
    [IO.File]::WriteAllText((Join-Path $ini "gen_jumpex.m3u8"), "#EXTM3U`r`n")
}

WriteLibrary @(
    @{file = "plfA001.m3u8"; title = "Alpha"; songs = @($song[0])},
    @{file = "plfB002.m3u8"; title = "Bravo"; songs = @($song[1], $song[2])},
    @{file = "plfC003.m3u8"; title = "Charlie"; songs = @($song[3])},
    @{file = "plfD004.m3u8"; title = "Delta"; songs = @($song[4], $song[5])},
    @{file = "plfE005.m3u8"; title = "Echo"; songs = @($song[6])})
SetLibraryIni "Playlists/Alpha"
WriteWinampPlaylist

# ---- Running it

$script:proc = $null
function StartWinamp {
    $script:proc = Start-Process "$wa\winamp.exe" -ArgumentList "/CLASS=$Instance" -PassThru
    if (-not (WaitFor { [W]::TopWindows([uint32]$script:proc.Id) | Where-Object { [W]::ClassOf($_) -eq $Instance } } 30)) { throw "Winamp didn't start" }
    $script:main = [W]::TopWindows([uint32]$script:proc.Id) | Where-Object { [W]::ClassOf($_) -eq $Instance } | Select-Object -First 1
    [void](WaitFor { $script:library = [W]::TopWindows([uint32]$script:proc.Id) | Where-Object { [W]::ClassOf($_) -eq "Winamp Gen" -and [W]::TextOf($_) -eq "Winamp Library" } | Select-Object -First 1; $script:library } 20)
    [void](WaitFor { $script:plugin = [W]::MessageWindow("PlaylistFoldersHost", [uint32]$script:proc.Id); $script:plugin -ne [IntPtr]::Zero } 20)
}
function StopWinamp {
    [void][W]::PostMessageW($script:main, 0x10, [IntPtr]::Zero, [IntPtr]::Zero)
    $exited = $script:proc.WaitForExit(30000)
    if (-not $exited) { $script:proc.Kill(); return $null }
    return $script:proc.ExitCode
}
function KeepQuiet {
    if ((Ipc $script:main 0 104) -ne 0) { [void][W]::SendMessageW($script:main, 0x111, [IntPtr]40047, [IntPtr]::Zero) }
}
function Cmd([string[]]$parts) { [W]::Command($script:plugin, ($parts -join "`t")) }
function Tree {
    $file = Join-Path $ScratchDir "tree.txt"
    Remove-Item $file -ErrorAction SilentlyContinue
    [void](Cmd "dump", $file)
    ([IO.File]::ReadAllText($file, [Text.Encoding]::UTF8) -replace "`r", "").TrimEnd("`n")
}
function ShowTree([string]$tree) { ($tree -split "`n") | ForEach-Object { "          | $_" } }
function TreeIs([string[]]$expected, [string]$what) {
    # Whether a folder shows open is left out here (an empty one may or may not).
    $tree = (Tree) -replace "\[folder, open\]", "[folder]"
    $want = ($expected -join "`n") -replace "\[folder, open\]", "[folder]"
    Check ($tree -eq $want) $what
    if ($tree -ne $want) { "        got:"; ShowTree $tree; "        wanted:"; ShowTree $want }
}
# The plugin's view of a folder, in the library: a dialog with its New Folder button (1004).
function FolderView {
    foreach ($w in [W]::Descendants($script:library)) {
        if ([W]::ClassOf($w) -eq "#32770") {
            $button = [W]::GetDlgItem($w, 1004)
            if ($button -ne [IntPtr]::Zero -and [W]::TextOf($button) -eq "New Folder" -and [W]::IsWindowVisible($w)) { return $w }
        }
    }
    return [IntPtr]::Zero
}
# ml_playlists' view of a playlist (its list is 1012), as shown.
function PlaylistView {
    [W]::Descendants($script:library) | Where-Object { [W]::ClassOf($_) -eq "#32770" -and [W]::GetDlgItem($_, 1012) -ne [IntPtr]::Zero -and [W]::IsWindowVisible($_) } | Select-Object -First 1
}
# A dialog ml_playlists shows (it's owned by the library, so a window of the process of its own).
function Dialog([string]$title) {
    [W]::TopWindows([uint32]$script:proc.Id) | Where-Object { [W]::ClassOf($_) -eq "#32770" -and [W]::IsWindowVisible($_) -and (-not $title -or [W]::TextOf($_) -like $title) -and
        @([W]::Descendants($_) | Where-Object { [W]::ClassOf($_) -eq "Button" }).Count -gt 0 } | Select-Object -First 1
}

if ($RealPlaylists) {
    # The user's playlists, copied as they are (the files and playlists.xml).
    Get-ChildItem $playlistsDir -File | Remove-Item
    Copy-Item "$SettingsDir\Plugins\ml\playlists\*" $playlistsDir -Force
    $xml = [IO.File]::ReadAllText((Join-Path $playlistsDir "playlists.xml"), [Text.Encoding]::Unicode)
    $titles = @([regex]::Matches($xml, 'title="([^"]*)"') | ForEach-Object { [Net.WebUtility]::HtmlDecode($_.Groups[1].Value) })
    SetLibraryIni "Playlists"
    "Starting Winamp with the user's $($titles.Count) playlists ..."
    $started = Get-Date
    StartWinamp
    KeepQuiet
    [void](WaitFor { (Cmd "selected") -ne 0 } 15)
    "        the library was there $([int]((Get-Date) - $started).TotalMilliseconds) ms after starting Winamp"
    Check ((Cmd "playlistcount") -eq $titles.Count) "the library has them all ($(Cmd "playlistcount"))"
    TreeIs (@("Playlists") + ($titles | ForEach-Object { "  $_" })) "with no folders, the tree lists them as without the plugin"
    $a = Cmd "newfolder", "0", "Folder A"
    $b = Cmd "newfolder", "$a", "Folder B"
    $c = Cmd "newfolder", "0", "Folder C"
    $moved = 0
    for ($i = 0; $i -lt $titles.Count; $i += 2) {
        $into = @($a, $b, $c)[($i / 2) % 3]
        if ((Cmd "moveplaylist", $titles[$i], "$into") -eq 1) { $moved++ }
    }
    Check ($moved -eq [math]::Ceiling($titles.Count / 2)) "every other one moved into a folder ($moved)"
    Check ((Cmd "rows", "$a") -eq 1 + [math]::Ceiling($titles.Count / 6)) "Folder A has Folder B and its share ($(Cmd "rows", "$a") rows)"
    [void](Cmd "select", $titles[0])
    $before = Tree
    Shot $script:library "real-1-folders.png"
    $code = StopWinamp
    Check ($code -eq 0) "Winamp closes cleanly (exit code $code)"
    $started = Get-Date
    StartWinamp
    KeepQuiet
    [void](WaitFor { (Cmd "selected") -ne 0 } 15)
    "        the library was there $([int]((Get-Date) - $started).TotalMilliseconds) ms after starting Winamp"
    $after = Tree
    Check ($after -eq $before) "after a restart, the tree is as it was"
    if ($after -ne $before) { "        got:"; ShowTree $after; "        wanted:"; ShowTree $before }
    Check (WaitFor { (Cmd "selected") -eq (Cmd "idof", $titles[0]) } 10) "and the library opens on the playlist picked, in its folder"
    $reread = [IO.File]::ReadAllText((Join-Path $playlistsDir "playlists.xml"), [Text.Encoding]::Unicode)
    Check ($reread -eq $xml) "ml_playlists' own list of playlists is as it was (the plugin never changes it)"
    Shot $script:library "real-2-after-restart.png"
    $code = StopWinamp
    Check ($code -eq 0) "Winamp closes cleanly (exit code $code)"
    ""
    if ($failures) { "$failures FAILED" } else { "all passed" }
    exit $failures
}

"Starting Winamp ..."
StartWinamp
KeepQuiet
Check ($null -ne $script:library) "the library is open"
Check ($script:plugin -ne [IntPtr]::Zero) "the plugin is loaded (its window is there)"
if ($script:plugin -eq [IntPtr]::Zero) { [void](StopWinamp); "stopping"; exit 1 }
Check ((Cmd "playlistcount") -eq 5) "the library has the test's 5 playlists"
TreeIs @("Playlists", "  Alpha", "  Bravo", "  Charlie", "  Delta", "  Echo") "with no folders yet, the tree is as without the plugin"
Check (WaitFor { (Cmd "selected") -eq (Cmd "idof", "Alpha") -and $null -ne (PlaylistView) } 10) "the library opens on Alpha, with ml_playlists' view of it (picked $(Cmd "selected"), Alpha is $(Cmd "idof", "Alpha"))"

"Folders"
$rock = Cmd "newfolder", "0", "Rock"
$chill = Cmd "newfolder", "0", "Chill"
$classic = Cmd "newfolder", "$rock", "Classic"
Check ($rock -gt 0 -and $chill -gt 0 -and $classic -gt 0) "three folders made (keys $rock, $chill, $classic)"
[void](Cmd "moveplaylist", "Bravo", "$rock")
[void](Cmd "moveplaylist", "Delta", "$classic")
TreeIs @("Playlists", "  Chill [folder, open]", "  Rock [folder, open]", "    Classic [folder, open]", "      Delta", "    Bravo", "  Alpha", "  Charlie", "  Echo") `
    "folders come first, in name order, playlists move into them"
$duplicate = Cmd "newfolder", "0", "rock"
Check ((Tree) -match "rock \(2\) \[folder") "a second folder of the same name gets a number"
[void](Cmd "deletefolder", "$duplicate")
Check ((Cmd "selected") -eq (Cmd "idof", "Alpha") -and $null -ne (PlaylistView)) "making, moving and deleting folders leaves the playlist that's open alone"
Shot $script:library "1-folders.png"

"Picking a playlist in a folder"
Check ((Cmd "select", "Delta") -eq 1) "a playlist inside two folders can be picked"
Check ((Cmd "selected") -eq (Cmd "idof", "Delta")) "and is the one picked"
Start-Sleep -Milliseconds 600
Check ($null -ne (PlaylistView)) "ml_playlists shows its own view of it"
Shot $script:library "2-playlist-in-folder.png"

"Dragging"
Check ((Cmd "drop", "Echo", "Chill") -eq 1) "a playlist dropped on a folder is taken"
Check ((Tree) -match "Chill \[folder[^\]]*\]\n    Echo") "and goes into it"
Check ((Cmd "drop", "Echo", "Alpha") -eq 1) "dropped on a playlist directly under Playlists"
Check ((Tree) -match "  Charlie\n  Echo$") "it goes back out of the folder"
Check ((Cmd "drop", "Classic", "Playlists") -eq 1) "a folder dropped on Playlists"
Check ((Tree) -match "^Playlists\n  Chill \[folder[^\]]*\]\n  Classic \[folder[^\]]*\]\n    Delta\n  Rock") "goes to the top, its playlist with it"
Check ((Cmd "drop", "Classic", "Rock") -eq 1) "and dropped on Rock"
Check ((Tree) -match "  Rock \[folder[^\]]*\]\n    Classic \[folder[^\]]*\]\n      Delta\n    Bravo") "goes back inside it"
[void](Cmd "drop", "Rock", "Classic")
Check ((Tree) -match "  Rock \[folder[^\]]*\]\n    Classic \[folder[^\]]*\]") "a folder dropped inside itself stays where it was"

"ml_playlists' own moves"
# Echo dropped after Alpha, among its neighbours: ml_playlists moves it in its list (now
# Alpha, Echo, Bravo, Charlie, Delta), taking its item out and putting it back.
[void](Cmd "drop", "Echo", "Alpha", "1")
Check ((Tree) -match "  Alpha\n  Echo\n  Charlie$") "a move among the playlists directly under Playlists still works"
[void](Cmd "moveplaylist", "Alpha", "$rock")
TreeIs @("Playlists", "  Chill [folder, open]", "  Rock [folder, open]", "    Classic [folder, open]", "      Delta", "    Alpha", "    Bravo", "  Echo", "  Charlie") "Alpha into Rock, before Bravo as in the library's list"
[void](Cmd "drop", "Alpha", "Bravo", "1")
Check ((Tree) -match "    Bravo\n    Alpha\n  Echo\n  Charlie$") "ml_playlists moves Alpha after Bravo, and it stays in Rock"

"Sorting, renaming"
[void](Cmd "renameplaylist", "Echo", "Zulu")
[void](Cmd "renameplaylist", "Charlie", "Aardvark")
Check ((Tree) -match "  Zulu\n  Aardvark$") "a playlist renamed keeps its place"
[void](Cmd "sort")
Start-Sleep -Milliseconds 300
TreeIs @("Playlists", "  Chill [folder, open]", "  Rock [folder, open]", "    Classic [folder, open]", "      Delta", "    Alpha", "    Bravo", "  Aardvark", "  Zulu") `
    "after ml_playlists sorts its list, each playlist is still in its folder, in the new order"
[void](Cmd "renameplaylist", "Aardvark", "Charlie")
[void](Cmd "renameplaylist", "Zulu", "Echo")

"The folder's view"
Check ((Cmd "select", "Rock") -eq 1) "a folder can be picked"
[void](WaitFor { (FolderView) -ne [IntPtr]::Zero } 5)
$view = FolderView
Check ($view -ne [IntPtr]::Zero) "it has a view of its own"
if ($view -ne [IntPtr]::Zero) {
    $list = [W]::GetDlgItem($view, 1001)
    Check ([int64][W]::SendMessageW($list, 0x1004, [IntPtr]::Zero, [IntPtr]::Zero) -eq 3) "listing what's in it: Classic, Bravo, Alpha"
    Check ([W]::TextOf([W]::GetDlgItem($view, 1005)) -eq "1 folder, 3 playlists, 5 songs [0:15]") "with its totals ($([W]::TextOf([W]::GetDlgItem($view, 1005))))"
    Shot $script:library "3-folder-view.png"
    [void][W]::SendMessageW($script:main, 0x400, [IntPtr]::Zero, [IntPtr]101) # IPC_DELETE: Winamp's playlist emptied
    [void][W]::SendMessageW([W]::GetDlgItem($view, 1002), 0xF5, [IntPtr]::Zero, [IntPtr]::Zero) # BM_CLICK on Play
    Check (WaitFor { (Ipc $script:main 0 124) -eq 5 } 5) "Play puts the folder's 5 songs in Winamp's playlist (it has $(Ipc $script:main 0 124))"
    Check (WaitFor { (Ipc $script:main 0 104) -eq 1 } 5) "and plays them"
    KeepQuiet
}

"New playlist in a folder"
[void](Cmd "newplaylisthere", "$chill")
$asked = $null
[void](WaitFor { $script:asked = Dialog; $null -ne $script:asked } 5)
Check ($null -ne $asked) "ml_playlists asks for the new playlist's name ($(if ($asked) { [W]::TextOf($asked) }))"
if ($asked) {
    $edit = [W]::ChildOfClass($asked, "Edit")
    [void][W]::SendText($edit, 0x0C, [IntPtr]::Zero, "Foxtrot") # WM_SETTEXT
    [void][W]::PostMessageW($asked, 0x111, [IntPtr]1, [IntPtr]::Zero) # IDOK
    Check (WaitFor { (Tree) -match "Chill \[folder[^\]]*\]\n    Foxtrot" } 5) "and it is made in the folder"
}

"Deleting a folder"
[void](Cmd "deletefolder", "$classic")
Check ((Tree) -match "  Rock \[folder[^\]]*\]\n    Alpha\n    Bravo\n    Delta") "what was in it goes to the folder above"
[void](Cmd "select", "Bravo")
[void](Cmd "expand", "$chill", "0")
$before = Tree
Shot $script:library "4-before-restart.png"

"Closing and starting again"
$code = StopWinamp
Check ($code -eq 0) "Winamp closes cleanly (exit code $code)"
$saved = if (Test-Path $foldersFile) { [IO.File]::ReadAllText($foldersFile) } else { "" }
Check ($saved -match "folder\t$rock\t0\t1\tRock" -and $saved -match "folder\t$chill\t0\t0\tChill") "the folders are kept, Rock open, Chill shut"
Check ($saved -match "playlist\t$rock\tplfb002\.m3u8") "with the playlists in them"
Check ((LastView) -eq "Playlists/PlaylistFolder$rock/Bravo") "the library remembers the playlist picked, by its folder's lasting name ($(LastView))"
StartWinamp
KeepQuiet
$after = Tree
Check ($after -eq $before) "after a restart, the tree is as it was"
if ($after -ne $before) { "        got:"; ShowTree $after; "        wanted:"; ShowTree $before }
Check (WaitFor { (Cmd "selected") -eq (Cmd "idof", "Bravo") -and $null -ne (PlaylistView) } 10) "and the library opens on that playlist, in its folder (picked $(Cmd "selected"), Bravo is $(Cmd "idof", "Bravo"))"
Shot $script:library "5-after-restart.png"

"Deleting a playlist in a folder"
[void](Cmd "select", "Delta")
$tree = [W]::ChildOfClass($script:library, "SysTreeView32")
[void][W]::SendNotifyMessageW($tree, 0x100, [IntPtr]0x2E, [IntPtr]1) # Delete
$confirm = $null
[void](WaitFor { $script:confirm = Dialog; $null -ne $script:confirm } 5)
Check ($null -ne $confirm) "ml_playlists asks before deleting it"
if ($confirm) {
    "        (asked: '$([W]::TextOf($confirm))', buttons: $(([W]::Descendants($confirm) | Where-Object { [W]::ClassOf($_) -eq "Button" } | ForEach-Object { "$([W]::GetDlgCtrlID($_))=$([W]::TextOf($_))" }) -join ", "))"
    $yes = [W]::GetDlgItem($confirm, 6)
    if ($yes -ne [IntPtr]::Zero) { [void][W]::PostMessageW($yes, 0xF5, [IntPtr]::Zero, [IntPtr]::Zero) } # BM_CLICK on Yes
    else { [void][W]::PostMessageW($confirm, 0x111, [IntPtr]6, [IntPtr]::Zero) }
    Check (WaitFor { (Cmd "playlistcount") -eq 5 -and (Tree) -notmatch "Delta" } 5) "it is gone from the library and the tree ($(Cmd "playlistcount") playlists)"
    if ((Tree) -match "Delta") { ShowTree (Tree) }
    [void](Cmd "save")
    Check ([IO.File]::ReadAllText($foldersFile) -notmatch "plfd004") "and forgotten by the plugin"
}

"Menus"
$tree = [W]::ChildOfClass($script:library, "SysTreeView32")
function MenuWindows { @([W]::TopWindows([uint32]$script:proc.Id) | Where-Object { [W]::ClassOf($_) -eq "#32768" -and [W]::IsWindowVisible($_) }) }
function MenuOf($window) { [IntPtr][W]::SendMessageW($window, 0x01E1, [IntPtr]::Zero, [IntPtr]::Zero) } # MN_GETHMENU
function MenuTexts($menu) { @(0..([W]::GetMenuItemCount($menu) - 1) | ForEach-Object { [W]::MenuText($menu, $_) }) }
function Highlighted($menu) { 0..([W]::GetMenuItemCount($menu) - 1) | Where-Object { ([W]::GetMenuState($menu, $_, 0x400) -band 0x80) -ne 0 } | Select-Object -First 1 }
# Down the menu with the keys, as far as that item, then Enter (or Right, for a submenu).
function Choose($window, $menu, [int]$position, [int]$key = 0x0D) {
    for ($i = 0; $i -lt 30 -and (Highlighted $menu) -ne $position; $i++) {
        [void][W]::PostMessageW($window, 0x100, [IntPtr]0x28, [IntPtr]::Zero); Start-Sleep -Milliseconds 80
    }
    if ((Highlighted $menu) -ne $position) { return $false }
    [void][W]::PostMessageW($window, 0x100, [IntPtr]$key, [IntPtr]::Zero)
    return $true
}
function OpenMenu([string]$on) {
    [void](Cmd "menu", $on)
    $script:menuWindow = $null
    [void](WaitFor { $script:menuWindow = MenuWindows | Select-Object -First 1; $null -ne $script:menuWindow } 5)
    $script:menuWindow
}
function TreeEdit { [W]::Descendants($tree) | Where-Object { [W]::ClassOf($_) -eq "Edit" -and [W]::IsWindowVisible($_) } | Select-Object -First 1 }
function TypeName([string]$name) {
    $edit = $null
    [void](WaitFor { $script:edit = TreeEdit; $null -ne $script:edit } 5)
    if (-not $script:edit) { return $false }
    [void][W]::SendText($script:edit, 0x0C, [IntPtr]::Zero, $name)
    [void][W]::SendMessageW($tree, 0x1116, [IntPtr]::Zero, [IntPtr]::Zero) # TVM_ENDEDITLABELNOW, keeping it
    return $true
}

$window = OpenMenu "Playlists"
Check ($null -ne $window) "the menu on Playlists opens"
if ($window) {
    $menu = MenuOf $window
    $texts = MenuTexts $menu
    Check ($texts[-1] -eq "New Folder`tCtrl+Shift+N" -and $texts.Count -gt 4) "with New Folder below ml_playlists' own ($($texts -join ' | '))"
    Check (Choose $window $menu ($texts.Count - 1)) "which can be chosen"
    Check (TypeName "Jazz") "New Folder makes one and starts its name being typed, in the tree"
    Check (WaitFor { (Tree) -match "\n  Jazz \[folder" } 5) "the name typed is the folder's"
}
$window = OpenMenu "Charlie"
Check ($null -ne $window) "the menu on a playlist opens"
if ($window) {
    $menu = MenuOf $window
    $texts = MenuTexts $menu
    Check ($texts[-1] -eq "Move To Folder") "with Move To Folder below ml_playlists' own ($($texts.Count) items)"
    [void](Choose $window $menu ($texts.Count - 1) 0x27) # Right: its submenu
    $sub = $null
    [void](WaitFor { $script:sub = MenuWindows | Where-Object { $_ -ne $window } | Select-Object -First 1; $null -ne $script:sub } 3)
    if ($sub) {
        $subMenu = MenuOf $sub
        $subTexts = MenuTexts $subMenu
        Check (($subTexts -join "|") -match "^Playlists \(no folder\)\|\s+Chill\|\s+Jazz\|\s+Rock\|\|New Folder\.\.\.$") "listing Playlists and every folder ($($subTexts -join ' | '))"
        [void](Choose $sub $subMenu ([array]::IndexOf($subTexts, ($subTexts | Where-Object { $_.Trim() -eq "Jazz" } | Select-Object -First 1))))
        Check (WaitFor { (Tree) -match "  Jazz \[folder[^\]]*\]\n    Charlie" } 5) "choosing a folder there moves the playlist into it"
    } else { Check $false "its Move To Folder opens" }
}
$window = OpenMenu "Jazz"
Check ($null -ne $window) "the menu on a folder opens"
if ($window) {
    $menu = MenuOf $window
    $texts = MenuTexts $menu
    Check (($texts -join "|") -eq "Play`tEnter|Enqueue`tShift+Enter||New Playlist Here...`tShift+Insert|New Folder`tCtrl+Shift+N||Move To|Rename Folder`tF2||Delete Folder`tDel") "with the folder's commands ($($texts -join ' | '))"
    [void](Choose $window $menu ($texts.Count - 1))
    $confirm = $null
    [void](WaitFor { $script:confirm = Dialog "Delete Folder"; $null -ne $script:confirm } 5)
    Check ($null -ne $confirm) "Delete Folder asks first, as it has a playlist in it"
    if ($confirm) {
        [void][W]::PostMessageW([W]::GetDlgItem($confirm, 6), 0xF5, [IntPtr]::Zero, [IntPtr]::Zero)
        Check (WaitFor { (Tree) -notmatch "Jazz" -and (Tree) -match "\n  Charlie" } 5) "and then goes, its playlist back under Playlists"
    }
}
"Keys"
[void](Cmd "select", "Chill")
[void][W]::SendNotifyMessageW($tree, 0x100, [IntPtr]0x71, [IntPtr]1) # F2
Check (TypeName "Chill Out") "F2 on a folder starts its name being typed"
Check (WaitFor { (Tree) -match "\n  Chill Out \[folder" } 5) "and the folder is renamed"
Check ((MenuWindows).Count -eq 0 -and $null -eq (Dialog)) "(no menu or dialog left open)"
Shot $script:library "6-end.png"

"Closing"
$code = StopWinamp
Check ($code -eq 0) "Winamp closes cleanly (exit code $code)"

""
if ($failures) { "$failures FAILED" } else { "all passed" }
exit $failures
