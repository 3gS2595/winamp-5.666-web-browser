<#
.SYNOPSIS
Runs gen_playlist_covers.dll in a scratch copy of Winamp, out of sight, and checks it.

.DESCRIPTION
Copies Winamp and its settings to -ScratchDir (the first time), puts the plug-in in,
replaces the media library's saved playlists in the copy with two of silent songs it
makes (album tags, covers in the files and a folder.jpg), and starts that Winamp with a
window class of its own, beside the real one, on a desktop of its own, the library open
on the first playlist. It checks the covers in the library's playlist view through sent
mouse messages, the library's tree, and the pane's probe message, and saves screenshots
in -ScratchDir\shots.

With -MusicFolder the library's playlist is every song in that folder instead. Nothing is
played then; it says how long the tags and the covers take, the first time and from the
cache.
#>
param(
    [string]$PluginPath = "$PSScriptRoot\..\bin\Win32\Release\gen_playlist_covers.dll",
    [string]$WinampDir = "$env:USERPROFILE\Documents\Winamp",
    [string]$SettingsDir = "$env:APPDATA\Winamp",
    [string]$ScratchDir = "$env:TEMP\playlist-covers-test",
    [string]$MusicFolder = "",
    [string]$Instance = "PlaylistCoversTest",
    # Only screenshots: the covers in a playlist, then the library's own Audio view (from
    # the copy of the user's library), to compare them.
    [switch]$Look,
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
public static class CoversDesktop {
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
    $desktop = [CoversDesktop]::CreateDesktopW($Instance, [IntPtr]::Zero, [IntPtr]::Zero, 0, 0x10000000, [IntPtr]::Zero)
    if ($desktop -eq [IntPtr]::Zero) { throw "Couldn't make a desktop to run on. -OnScreen runs without one." }
    $log = Join-Path ([IO.Path]::GetTempPath()) "playlist-covers-test-$Instance.log"
    Remove-Item $log -ErrorAction SilentlyContinue
    $quote = { param($text) "'" + ($text -replace "'", "''") + "'" }
    $arguments = "-OnOwnDesktop -PluginPath $(& $quote (Resolve-Path $PluginPath).Path) -WinampDir $(& $quote $WinampDir) " +
                 "-SettingsDir $(& $quote $SettingsDir) -ScratchDir $(& $quote $ScratchDir) -Instance $(& $quote $Instance)"
    if ($MusicFolder) { $arguments += " -MusicFolder $(& $quote $MusicFolder)" }
    if ($Look) { $arguments += " -Look" }
    $command = "& $(& $quote $PSCommandPath) $arguments *> $(& $quote $log); exit `$LASTEXITCODE"
    $commandLine = "`"$PSHOME\powershell.exe`" -NoProfile -ExecutionPolicy Bypass -Command `"$command`""
    "(running on a desktop of its own, out of sight; -OnScreen to watch it)"
    $child = $null; $reader = $null; $exitCode = 1
    try {
        $child = [CoversDesktop]::Start($Instance, $commandLine, (Get-Location).Path)
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
        [void][CoversDesktop]::CloseDesktop($desktop)
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
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern IntPtr SendMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("user32.dll", CharSet = CharSet.Unicode, EntryPoint = "SendMessageW")]
    public static extern IntPtr SendText(IntPtr h, uint m, IntPtr w, string l);
    [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags);
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
    [DllImport("user32.dll")] public static extern int GetMenuItemCount(IntPtr menu);
    [DllImport("user32.dll")] public static extern uint GetMenuItemID(IntPtr menu, int pos);
    [DllImport("user32.dll")] public static extern uint GetMenuState(IntPtr menu, uint id, uint flags);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetMenuStringW(IntPtr menu, uint item, StringBuilder s, int n, uint flags);
    public struct RECT { public int Left, Top, Right, Bottom; }
    public static string ClassOf(IntPtr h) { var s = new StringBuilder(256); GetClassName(h, s, 256); return s.ToString(); }
    public static string TextOf(IntPtr h) { var s = new StringBuilder(512); GetWindowText(h, s, 512); return s.ToString(); }
    public static List<IntPtr> TopWindows(uint pid) {
        var found = new List<IntPtr>();
        EnumWindows((h, l) => { uint p; GetWindowThreadProcessId(h, out p); if (p == pid) found.Add(h); return true; }, IntPtr.Zero);
        return found;
    }
    public static IntPtr ChildOfClass(IntPtr parent, string name) {
        IntPtr found = IntPtr.Zero;
        EnumChildWindows(parent, (h, l) => { if (ClassOf(h) == name) { found = h; return false; } return true; }, IntPtr.Zero);
        return found;
    }
    // The window as it is drawn, even where nothing shows it.
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
    [DllImport("user32.dll")] public static extern IntPtr GetParent(IntPtr h);
    [DllImport("kernel32.dll")] static extern IntPtr OpenProcess(uint access, bool inherit, uint pid);
    [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);
    [DllImport("kernel32.dll")] static extern IntPtr VirtualAllocEx(IntPtr p, IntPtr at, IntPtr size, uint type, uint protect);
    [DllImport("kernel32.dll")] static extern bool VirtualFreeEx(IntPtr p, IntPtr at, IntPtr size, uint type);
    [DllImport("kernel32.dll")] static extern bool WriteProcessMemory(IntPtr p, IntPtr at, byte[] data, IntPtr size, out IntPtr done);
    [DllImport("kernel32.dll")] static extern bool ReadProcessMemory(IntPtr p, IntPtr at, byte[] data, IntPtr size, out IntPtr done);
    // A tree view item's text in another process (Winamp's, 32-bit): the TVITEMW and the
    // text have to be in its memory.
    static string TreeText(IntPtr tree, IntPtr process, IntPtr memory, IntPtr item) {
        uint at = (uint)memory.ToInt64();
        byte[] tv = new byte[40];
        BitConverter.GetBytes(1).CopyTo(tv, 0);                         // mask: TVIF_TEXT
        BitConverter.GetBytes((uint)item.ToInt64()).CopyTo(tv, 4);      // hItem
        BitConverter.GetBytes(at + 64).CopyTo(tv, 16);                  // pszText
        BitConverter.GetBytes(256).CopyTo(tv, 20);                      // cchTextMax
        IntPtr done;
        WriteProcessMemory(process, memory, tv, (IntPtr)40, out done);
        SendMessageW(tree, 0x113E, IntPtr.Zero, memory);                // TVM_GETITEMW
        byte[] text = new byte[512];
        ReadProcessMemory(process, (IntPtr)(long)(at + 64), text, (IntPtr)512, out done);
        string s = Encoding.Unicode.GetString(text);
        int end = s.IndexOf('\0');
        return end >= 0 ? s.Substring(0, end) : s;
    }
    public static IntPtr FindTreeItem(IntPtr tree, uint pid, string text) {
        IntPtr process = OpenProcess(0x0438, false, pid);
        if (process == IntPtr.Zero) return IntPtr.Zero;
        IntPtr memory = VirtualAllocEx(process, IntPtr.Zero, (IntPtr)1024, 0x3000, 0x04);
        try {
            var pending = new Stack<IntPtr>();
            pending.Push(SendMessageW(tree, 0x110A, IntPtr.Zero, IntPtr.Zero)); // TVGN_ROOT
            while (pending.Count > 0) {
                for (IntPtr item = pending.Pop(); item != IntPtr.Zero; item = SendMessageW(tree, 0x110A, (IntPtr)1, item)) {
                    if (TreeText(tree, process, memory, item) == text) return item;
                    SendMessageW(tree, 0x1102, (IntPtr)2, item);                    // TVM_EXPAND, TVE_EXPAND
                    IntPtr child = SendMessageW(tree, 0x110A, (IntPtr)4, item);    // TVGN_CHILD
                    if (child != IntPtr.Zero) pending.Push(child);
                }
            }
            return IntPtr.Zero;
        } finally {
            VirtualFreeEx(process, memory, IntPtr.Zero, 0x8000);
            CloseHandle(process);
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
function Probe($view, [int]$what) { [int64][W]::SendMessageW($view, 0x8009, [IntPtr]$what, [IntPtr]::Zero) }
function LParam([int]$x, [int]$y) { [IntPtr](($y -shl 16) -bor ($x -band 0xFFFF)) }
function PointOf($view, [int]$what) {
    $packed = Probe $view $what
    if ($packed -lt 0) { return $null }
    @([int]($packed -band 0xFFFF), [int](($packed -shr 16) -band 0xFFFF))
}
# Sent rather than posted, so that each is handled before the next check: Windows hands a
# thread the messages sent to it ahead of the ones posted.
function Click($view, $point, [switch]$Double) {
    $l = LParam $point[0] $point[1]
    [void][W]::SendMessageW($view, 0x201, [IntPtr]1, $l)
    [void][W]::SendMessageW($view, 0x202, [IntPtr]0, $l)
    if ($Double) {
        [void][W]::SendMessageW($view, 0x203, [IntPtr]1, $l)
        [void][W]::SendMessageW($view, 0x202, [IntPtr]0, $l)
    }
}
function Key($view, [int]$vk) {
    [void][W]::SendMessageW($view, 0x100, [IntPtr]$vk, [IntPtr]1)
    [void][W]::SendMessageW($view, 0x101, [IntPtr]$vk, [IntPtr]0xC0000001)
}
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
Copy-Item $PluginPath "$wa\Plugins\gen_playlist_covers.dll" -Force
New-Item -ItemType Directory -Force $shots | Out-Null

# Starts each run as the first time: no settings of the plug-in's, no tag cache.
function ForgetPlugin {
    $iniFile = Join-Path $ini "winamp.ini"
    $kept = New-Object System.Collections.Generic.List[string]
    $skipping = $false
    foreach ($line in [IO.File]::ReadAllLines($iniFile, [Text.Encoding]::Default)) {
        if ($line -match '^\[(.*)\]\s*$') { $skipping = $Matches[1] -eq "PlaylistCovers" }
        if (-not $skipping) { $kept.Add($line) }
    }
    [IO.File]::WriteAllLines($iniFile, $kept, [Text.Encoding]::Default)
    Remove-Item (Join-Path $ini "Plugins\playlist_covers_tags.txt") -ErrorAction SilentlyContinue
}

# ---- Songs, silent ones (a hidden desktop doesn't hide sound)

$ffmpeg = "C:\ffmpeg\bin\ffmpeg.exe"
function MakeCover([string]$source, [string]$path) {
    & $ffmpeg -hide_banner -loglevel error -y -f lavfi -i $source -frames:v 1 $path
}
function MakeSong([string]$path, [hashtable]$tags, [string]$cover) {
    New-Item -ItemType Directory -Force (Split-Path $path) | Out-Null
    $arguments = @("-hide_banner", "-loglevel", "error", "-y", "-f", "lavfi", "-t", "4", "-i", "anullsrc=r=22050:cl=mono")
    if ($cover) { $arguments += @("-i", $cover, "-map", "0:a", "-map", "1:v", "-c:v", "copy", "-disposition:v", "attached_pic",
                                  "-metadata:s:v", "title=Album cover", "-metadata:s:v", "comment=Cover (front)") }
    $arguments += @("-c:a", "libmp3lame", "-b:a", "32k", "-id3v2_version", "3")
    foreach ($key in $tags.Keys) { $arguments += @("-metadata", "$key=$($tags[$key])") }
    $arguments += $path
    & $ffmpeg @arguments
}

$songs = @()
if (-not $MusicFolder) {
    if (-not (Test-Path "$media\done.txt")) {
        "Making silent test songs in $media ..."
        Remove-Item $media -Recurse -Force -ErrorAction SilentlyContinue
        New-Item -ItemType Directory -Force "$media\covers" | Out-Null
        MakeCover "testsrc=size=500x500" "$media\covers\red.jpg"
        MakeCover "smptebars=size=640x360" "$media\covers\wide.jpg"
        MakeCover "testsrc2=size=300x300" "$media\covers\single1.jpg"
        MakeCover "rgbtestsrc=size=400x400" "$media\covers\single2.jpg"
        New-Item -ItemType Directory -Force "$media\Folder Band - Folder Art" | Out-Null
        MakeCover "mandelbrot=size=600x600" "$media\Folder Band - Folder Art\folder.jpg"
        foreach ($n in 1..3) {
            MakeSong "$media\Band A - Red Album\0$n Red $n.mp3" @{title = "Red Song $n"; artist = "Band A"; album = "Red Album"; album_artist = "Band A"} "$media\covers\red.jpg"
        }
        $artists = @("Xenia", "Yusuf", "Zora")
        foreach ($n in 1..3) {
            MakeSong "$media\Compilation\0$n Wide $n.mp3" @{title = "Wide Song $n"; artist = $artists[$n - 1]; album = "Wide Compilation"} "$media\covers\wide.jpg"
        }
        foreach ($n in 1..2) {
            MakeSong "$media\Folder Band - Folder Art\0$n Folder $n.mp3" @{title = "Folder Song $n"; artist = "Folder Band"; album = "Folder Art"} ""
        }
        MakeSong "$media\singles\Solo - Single One.mp3" @{title = "Single One"; artist = "Solo"} "$media\covers\single1.jpg"
        MakeSong "$media\singles\Duo - Single Two.mp3" @{title = "Single Two"; artist = "Duo"} "$media\covers\single2.jpg"
        MakeSong "$media\singles\Some Artist - No Tags.mp3" @{} ""
        MakeSong "$media\singles\Title Only.mp3" @{title = "Title Only"} ""
        MakeSong "$media\Band A bonus\Red Bonus.mp3" @{title = "Red Bonus"; artist = "Band A"; album = "Red Album"; album_artist = "Band A"} "$media\covers\red.jpg"
        Set-Content "$media\done.txt" "made"
    }
    $songs = @(
        "$media\Band A - Red Album\01 Red 1.mp3", "$media\Band A - Red Album\02 Red 2.mp3", "$media\Band A - Red Album\03 Red 3.mp3",
        "$media\Compilation\01 Wide 1.mp3", "$media\Compilation\02 Wide 2.mp3", "$media\Compilation\03 Wide 3.mp3",
        "$media\Folder Band - Folder Art\01 Folder 1.mp3", "$media\Folder Band - Folder Art\02 Folder 2.mp3",
        "$media\singles\Solo - Single One.mp3", "$media\singles\Duo - Single Two.mp3",
        "$media\singles\Some Artist - No Tags.mp3", "$media\singles\Title Only.mp3",
        "$media\Band A bonus\Red Bonus.mp3")
} else {
    # (-Include does nothing with -LiteralPath in Windows PowerShell.)
    $songs = @(Get-ChildItem -LiteralPath $MusicFolder -Recurse -File |
               Where-Object { $_.Extension -in ".mp3", ".m4a", ".flac", ".ogg" } | Sort-Object FullName | ForEach-Object FullName)
    "Playlist: $($songs.Count) songs from $MusicFolder"
}


# ---- The media library's playlists: the test's own, in place of the user's (in the copy)

$playlistsDir = Join-Path $ini "Plugins\ml\playlists"
function WriteLibraryPlaylist([string]$file, [string[]]$entries) {
    $lines = @("#EXTM3U")
    foreach ($entry in $entries) {
        $lines += "#EXTINF:4,$([IO.Path]::GetFileNameWithoutExtension($entry))"
        $lines += $entry
    }
    [IO.File]::WriteAllLines((Join-Path $playlistsDir $file), $lines, (New-Object Text.UTF8Encoding($true)))
}
function WriteLibrary([hashtable[]]$lists) {
    New-Item -ItemType Directory -Force $playlistsDir | Out-Null
    Get-ChildItem $playlistsDir -File | Remove-Item
    $xml = "<?xml version=`"1.0`" encoding=`"UTF-16`"?><playlists playlists=`"$($lists.Count)`">"
    foreach ($list in $lists) {
        WriteLibraryPlaylist $list.file $list.songs
        $xml += "<playlist filename=`"$($list.file)`" title=`"$($list.title)`" id=`"{$([guid]::NewGuid().ToString().ToUpper())}`" songs=`"$($list.songs.Count)`" seconds=`"$(4 * $list.songs.Count)`"/>"
    }
    $xml += "</playlists>"
    [IO.File]::WriteAllText((Join-Path $playlistsDir "playlists.xml"), $xml, (New-Object Text.UnicodeEncoding($false, $true)))
}
# The library opens on this playlist, and doesn't scan the user's music folder meanwhile.
function OpenLibraryOn([string]$title) {
    $file = Join-Path $ini "Plugins\gen_ml.ini"
    $lines = [IO.File]::ReadAllLines($file, [Text.Encoding]::Default) | Where-Object { $_ -notmatch '^(last_view|bgrescan_do|visible)=' }
    $lines = @($lines[0], "last_view=Playlists/$title", "bgrescan_do=0", "visible=1") + ($lines | Select-Object -Skip 1)
    [IO.File]::WriteAllLines($file, $lines, [Text.Encoding]::Default)
}
# Winamp's own playlist: one silent song, so that nothing of the user's is ever played.
function WriteWinampPlaylist([string]$song) {
    $lines = if ($song) { @("#EXTM3U", $song) } else { @("#EXTM3U") }
    [IO.File]::WriteAllLines((Join-Path $ini "winamp.m3u8"), $lines, (New-Object Text.UTF8Encoding($true)))
    Remove-Item (Join-Path $ini "Winamp.m3u") -ErrorAction SilentlyContinue
    [IO.File]::WriteAllText((Join-Path $ini "gen_jumpex.m3u8"), "#EXTM3U`r`n")
}

$script:proc = $null
function StartWinamp {
    $script:proc = Start-Process "$wa\winamp.exe" -ArgumentList "/CLASS=$Instance" -PassThru
    if (-not (WaitFor { [W]::TopWindows([uint32]$script:proc.Id) | Where-Object { [W]::ClassOf($_) -eq $Instance } } 30)) { throw "Winamp didn't start" }
    $script:main = [W]::TopWindows([uint32]$script:proc.Id) | Where-Object { [W]::ClassOf($_) -eq $Instance } | Select-Object -First 1
    [void](WaitFor { $script:library = [W]::TopWindows([uint32]$script:proc.Id) | Where-Object { [W]::ClassOf($_) -eq "Winamp Gen" -and [W]::TextOf($_) -eq "Winamp Library" } | Select-Object -First 1; $script:library } 20)
    FindPane
}
function FindPane {
    $script:pane = if ($script:library) { [W]::ChildOfClass($script:library, "PlaylistCoversPane") } else { [IntPtr]::Zero }
    [void](WaitFor { $script:pane = [W]::ChildOfClass($script:library, "PlaylistCoversPane"); $script:pane -ne [IntPtr]::Zero } 15)
    # The covers themselves, which scroll, inside the pane.
    $script:grid = if ($script:pane -ne [IntPtr]::Zero) { [W]::ChildOfClass($script:pane, "PlaylistCoversGrid") } else { [IntPtr]::Zero }
}
function StopWinamp {
    [void][W]::PostMessageW($script:main, 0x10, [IntPtr]::Zero, [IntPtr]::Zero)
    $exited = $script:proc.WaitForExit(30000)
    if (-not $exited) { $script:proc.Kill(); return $null }
    return $script:proc.ExitCode
}
# Nothing of the user's is to be heard: whatever Winamp starts playing by itself is stopped.
function KeepQuiet {
    if ((Ipc $script:main 0 104) -ne 0) { [void][W]::SendMessageW($script:main, 0x111, [IntPtr]40047, [IntPtr]::Zero); "  (stopped what Winamp started playing by itself)" }
}
function SelectTreeItem([string]$text) {
    $tree = [W]::ChildOfClass($script:library, "SysTreeView32")
    $item = [W]::FindTreeItem($tree, [uint32]$script:proc.Id, $text)
    if ($item -eq [IntPtr]::Zero) { return $false }
    [void][W]::SendMessageW($tree, 0x110B, [IntPtr]9, $item) # TVM_SELECTITEM, TVGN_CARET
    return $true
}

ForgetPlugin

if ($MusicFolder) {
    WriteLibrary @(@{file = "plfMUSIC.m3u8"; title = "Covers Music"; songs = $songs})
    OpenLibraryOn "Covers Music"
    WriteWinampPlaylist ""
    foreach ($round in @("first time", "from the cache")) {
        "Starting Winamp ($round) ..."
        $started = Get-Date
        StartWinamp
        KeepQuiet
        Check ($script:pane -ne [IntPtr]::Zero) "the covers are in the library's view of the playlist"
        if ($script:pane -eq [IntPtr]::Zero) { [void](StopWinamp); break }
        [void](WaitFor { (Probe $script:pane 0) -eq $songs.Count } 60)
        "        playlist read $([int]((Get-Date) - $started).TotalMilliseconds) ms after start: $(Probe $script:pane 0) songs, $(Probe $script:pane 10) rows in the list"
        $read = Get-Date
        [void](WaitFor { (Probe $script:pane 4) -eq 0 } 600)
        $tagged = Get-Date
        KeepQuiet
        "        tags all in $([int]($tagged - $read).TotalMilliseconds) ms later: $(Probe $script:pane 1) covers"
        [void](WaitFor { (Probe $script:pane 9) -eq 0 } 120)
        "        covers in sight and near loaded $([int]((Get-Date) - $tagged).TotalMilliseconds) ms later: $(Probe $script:pane 6) with pictures, $(Probe $script:pane 7) without"
        "        Winamp's private memory: $([int]((Get-Process -Id $script:proc.Id).PrivateMemorySize64 / 1MB)) MB"
        Shot $script:library "library-music-$($round -replace ' ', '-').png"
        # A page down, through the library's scroll bar's own message.
        [void][W]::SendMessageW($script:grid, 0x115, [IntPtr]3, [IntPtr]::Zero) # WM_VSCROLL, SB_PAGEDOWN
        Check ((Probe $script:pane 20) -gt 0) "the covers scroll a page down ($(Probe $script:pane 20) pixels)"
        [void](WaitFor { (Probe $script:pane 9) -eq 0 } 30)
        Start-Sleep -Milliseconds 300
        Shot $script:library "library-music-$($round -replace ' ', '-')-scrolled.png"
        $code = StopWinamp
        Check ($code -eq 0) "Winamp closes cleanly (exit code $code)"
    }
    ""
    if ($failures) { "$failures FAILED" } else { "all passed" }
    exit $failures
}

WriteLibrary @(
    @{file = "plfCOVERS.m3u8"; title = "Covers Test"; songs = $songs},
    @{file = "plfCOVER2.m3u8"; title = "Covers Two"; songs = @($songs[8], $songs[9])})
OpenLibraryOn "Covers Test"
WriteWinampPlaylist $songs[11]

if ($Look) {
    StartWinamp
    KeepQuiet
    [void](WaitFor { $script:pane -ne [IntPtr]::Zero -and (Probe $script:pane 9) -eq 0 -and (Probe $script:pane 4) -eq 0 -and (Probe $script:pane 1) -eq 7 } 20)
    Start-Sleep -Milliseconds 500
    "  the covers' parents:"
    for ($w = [W]::GetParent($script:pane); $w -ne [IntPtr]::Zero; $w = [W]::GetParent($w)) { "    $w $([W]::ClassOf($w)) '$([W]::TextOf($w))'" }
    Shot $script:library "look-playlist.png"
    [void](SelectTreeItem "Audio")
    Start-Sleep -Seconds 4
    Shot $script:library "look-audio.png"
    $code = StopWinamp
    Check ($code -eq 0) "Winamp closes cleanly (exit code $code)"
    exit $failures
}

"Starting Winamp ..."
StartWinamp
KeepQuiet
Check ($null -ne $script:library) "the library is open"
Check ($script:pane -ne [IntPtr]::Zero) "the covers are in its view of the playlist"
if ($script:pane -eq [IntPtr]::Zero) { [void](StopWinamp); "1 FAILED, stopping"; exit 1 }
Check (WaitFor { (Probe $script:pane 0) -eq 13 }) "they come from the playlist's 13 songs (it has $(Probe $script:pane 0))"
Check ((Probe $script:pane 10) -eq 13 -and (Probe $script:pane 3) -eq 13) "each of the list's 13 rows is matched to its song ($(Probe $script:pane 3) of $(Probe $script:pane 10))"
Check (WaitFor { (Probe $script:pane 4) -eq 0 -and (Probe $script:pane 1) -eq 7 }) "the tags make 7 covers: 3 albums, 4 songs of their own (it has $(Probe $script:pane 1))"
Check (WaitFor { (Probe $script:pane 6) -eq 5 -and (Probe $script:pane 7) -eq 2 }) "5 covers have pictures (one from folder.jpg), 2 have none (it has $(Probe $script:pane 6) and $(Probe $script:pane 7))"
$paneBottom = Probe $script:pane 12; $listTop = Probe $script:pane 13
Check ($listTop -eq $paneBottom + 1) "the list starts right under the covers (covers end at $paneBottom, list starts at $listTop)"
# The user's Audio view (in the copy) shows medium album icons, which are 90 pixels.
Check ((Probe $script:pane 15) -eq 90) "the covers are the size the Audio view shows its albums at ($(Probe $script:pane 15))"
Check ((Probe $script:pane 18) -eq 94 -and (Probe $script:pane 19) -eq 111) "laid out as the Audio view lays them out: 94 by 111 pixels a cover ($(Probe $script:pane 18) by $(Probe $script:pane 19))"
Check ((Probe $script:pane 16) -eq 1) "with the library's own scroll bar"
Check ($script:grid -ne [IntPtr]::Zero) "(the covers are in a window of their own, which scrolls)"
Shot $script:library "library-1-loaded.png"

Click $script:grid (PointOf $script:pane 100)
Check ((Probe $script:pane 11) -eq 4) "a click on the Red Album picks its 4 songs in the list, the one from another folder too (it picked $(Probe $script:pane 11))"
Check ((Probe $script:pane 5) -eq 1) "and the cover shows as picked"
[void][W]::SendMessageW($script:grid, 0x201, [IntPtr]9, (LParam (PointOf $script:pane 102)[0] (PointOf $script:pane 102)[1])) # Ctrl+click
[void][W]::SendMessageW($script:grid, 0x202, [IntPtr]0, (LParam (PointOf $script:pane 102)[0] (PointOf $script:pane 102)[1]))
Check ((Probe $script:pane 11) -eq 6 -and (Probe $script:pane 5) -eq 2) "Ctrl+click adds the Folder Art's 2 ($(Probe $script:pane 11) songs, $(Probe $script:pane 5) covers)"
Shot $script:library "library-2-picked.png"

Click $script:grid (PointOf $script:pane 101) -Double
Check (WaitFor { (Ipc $script:main 0 104) -eq 1 } 5) "a double click plays the cover's songs (status $(Ipc $script:main 0 104))"
Check ((Ipc $script:main 0 124) -eq 3) "through the library's Play, which puts its 3 songs in Winamp's playlist (it has $(Ipc $script:main 0 124))"
Check (WaitFor { (Probe $script:pane 14) -eq 1 } 3) "and that cover shows it is playing"
Start-Sleep -Milliseconds 600
Shot $script:library "library-3-playing.png"
[void][W]::SendMessageW($script:main, 0x111, [IntPtr]40047, [IntPtr]::Zero) # stop

# The bar under the covers, dragged 80 pixels down.
[W+RECT]$box = New-Object W+RECT
[void][W]::GetClientRect($script:pane, [ref]$box)
$x = [int]($box.Right / 2); $y = $box.Bottom - 3
[void][W]::SendMessageW($script:pane, 0x201, [IntPtr]1, (LParam $x $y))
[void][W]::SendMessageW($script:pane, 0x200, [IntPtr]1, (LParam $x ($y + 80)))
[void][W]::SendMessageW($script:pane, 0x202, [IntPtr]0, (LParam $x ($y + 80)))
$moved = (Probe $script:pane 12) - $paneBottom
Check ($moved -ge 75 -and $moved -le 85) "dragging the bar under the covers gives them more room ($moved pixels)"
Check ((Probe $script:pane 13) -eq (Probe $script:pane 12) + 1) "and the list starts right under them still"

[W+RECT]$frame = New-Object W+RECT
[void][W]::GetWindowRect($script:library, [ref]$frame)
[void][W]::SetWindowPos($script:library, [IntPtr]::Zero, 30, 30, 900, 900, 0x14)
Start-Sleep -Milliseconds 400
$dialog = [W]::GetParent($script:pane)
[W+RECT]$view = New-Object W+RECT
[void][W]::GetClientRect($dialog, [ref]$view)
Check ((Probe $script:pane 13) -eq (Probe $script:pane 12) + 1) "after the library is resized, the list is still right under the covers"
[W+RECT]$paneBox = New-Object W+RECT
[void][W]::GetWindowRect($script:pane, [ref]$paneBox)
Check ($paneBox.Right - $paneBox.Left -ge $view.Right - 12) "and the covers are as wide as the view ($($paneBox.Right - $paneBox.Left) of $($view.Right))"
Shot $script:library "library-4-resized.png"

Check (SelectTreeItem "Covers Two") "the other playlist can be picked in the library's tree"
FindPane
Check (WaitFor { (Probe $script:pane 0) -eq 2 -and (Probe $script:pane 1) -eq 2 } 10) "its view gets covers too: 2 songs, 2 covers (it has $(Probe $script:pane 0) and $(Probe $script:pane 1))"
Check (WaitFor { (Probe $script:pane 6) -eq 2 } 10) "with their pictures, already read ($(Probe $script:pane 6))"
Shot $script:library "library-5-other-playlist.png"
Check (SelectTreeItem "Audio") "the library's own views are left alone"
Start-Sleep -Milliseconds 500
Check ([W]::ChildOfClass($script:library, "PlaylistCoversPane") -eq [IntPtr]::Zero) "(no covers outside the playlists)"
[void](SelectTreeItem "Covers Test")
FindPane
Check (WaitFor { (Probe $script:pane 1) -eq 7 } 10) "back on the first playlist, its covers are back"

"Closing Winamp ..."
$code = StopWinamp
Check ($code -eq 0) "Winamp closes cleanly (exit code $code)"
$saved = Get-Content (Join-Path $ini "winamp.ini") -Raw
Check ($saved -match "\[PlaylistCovers\][^\[]*covers_share=(\d+)" -and [int]$Matches[1] -gt 450) "the room given to the covers is remembered (covers_share=$($Matches[1]))"
$cache = Join-Path $ini "Plugins\playlist_covers_tags.txt"
Check ((Test-Path $cache) -and ((Get-Content $cache).Count -ge 13)) "the tags are kept for next time"

""
if ($failures) { "$failures FAILED" } else { "all passed" }
exit $failures
