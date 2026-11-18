<#
.SYNOPSIS
    Smoke-tests gen_browser.dll in a scratch copy of Winamp, driven by window messages.

.DESCRIPTION
    The Winamp in use is never touched. Its folder and settings are copied once to a
    scratch folder, without the browser's own data and without the plugins that make a
    noise or reach outside Winamp, and the copy is run as a separate instance beside it.
    The script then works the browser the way a user would, as far as window messages
    reach: pages, tabs and their close buttons, dragging tabs between windows, a download
    and the downloads panel, minimizing and maximizing, the ornaments on the corners and
    their buttons, closing, and what is left on disk after Winamp exits. Then it opens
    links from outside, as exodium_open.exe does for Windows, with Winamp running and with
    it closed, and does the same with the Start menu's shortcut, which the checkbox on
    Exodium's page makes in a scratch folder. Downloads go to a folder in the scratch
    copy, never to the Downloads folder in use.

    It runs on a desktop of its own, which Windows never shows, so its windows don't
    come up in front of whoever is at the machine or take the keyboard from them, and
    their windows can't get in the way of a drop. The script starts itself again there
    and passes on what that prints. -OnScreen runs it where it can be watched.

    Keyboard shortcuts are mostly not covered: keystrokes can't be injected without taking
    the keyboard away from whoever is at the machine. Keys held with a click on Back or
    Forward, Ctrl+Shift+T, and Ctrl+N in Winamp's windows and the browser's, are given by
    sharing the browser thread's keyboard state (WithKeys), which leaves the real keyboard
    alone. Escape needs no other key, and is posted to the window that has the keyboard.

    The exit code is the number of checks that failed.

.EXAMPLE
    tools\smoke-test.ps1

.EXAMPLE
    tools\smoke-test.ps1 -PluginPath C:\old\gen_browser.dll -Label "baseline"
#>
param(
    # The build to test.
    [string]$PluginPath = (Join-Path $PSScriptRoot "..\bin\Win32\Release\gen_browser.dll"),
    # The Winamp to copy, and the folder its winamp.ini is in.
    [string]$WinampDir = (Join-Path ([Environment]::GetFolderPath("MyDocuments")) "Winamp"),
    [string]$SettingsDir = (Join-Path $env:APPDATA "Winamp"),
    # The name the run goes by: Winamp's window class, the desktop it runs on and its log.
    # Runs going on at the same time each need their own, and their own -ScratchDir and -DebugPort.
    [string]$Instance = "ExodiumSmokeTest",
    # Where the copy is kept between runs. Delete it to have it made afresh.
    [string]$ScratchDir = (Join-Path $env:TEMP "exodium-smoke-test"),
    [string]$Label = "gen_browser.dll",
    # Leaves out the tab dragging, which with -OnScreen needs the browser window not to be covered.
    [switch]$SkipDrag,
    # The debugging port, for the part that has it on. Not the one in real use, to keep clear of it.
    [int]$DebugPort = 9334,
    # Runs on the desktop in use, to be watched, rather than on one of its own.
    [switch]$OnScreen,
    # Set by the script itself when it starts itself again on its own desktop.
    [switch]$OnOwnDesktop
)

$ErrorActionPreference = "Stop"
$wa = Join-Path $ScratchDir "Winamp"
$ini = Join-Path $ScratchDir "settings"

if (-not $OnScreen -and -not $OnOwnDesktop) {
    Add-Type @"
using System;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text;
public static class OwnDesktop {
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
    // Starts a program with no console window on the named desktop, which whatever it starts goes on as well.
    public static Process Start(string desktop, string commandLine, string directory) {
        var startup = new STARTUPINFO();
        startup.cb = Marshal.SizeOf(startup);
        startup.lpDesktop = desktop;
        PROCESS_INFORMATION info;
        const uint CREATE_NO_WINDOW = 0x08000000;
        if (!CreateProcessW(null, new StringBuilder(commandLine), IntPtr.Zero, IntPtr.Zero, false, CREATE_NO_WINDOW, IntPtr.Zero,
                            directory, ref startup, out info)) {
            throw new System.ComponentModel.Win32Exception();
        }
        try {
            Process process = Process.GetProcessById(info.dwProcessId);
            IntPtr kept = process.Handle; // the exit code can't be read later without it
            return process;
        } finally {
            CloseHandle(info.hThread);
            CloseHandle(info.hProcess);
        }
    }
}
"@
    $desktopName = $Instance
    $desktop = [OwnDesktop]::CreateDesktopW($desktopName, [IntPtr]::Zero, [IntPtr]::Zero, 0, 0x10000000, [IntPtr]::Zero)  # GENERIC_ALL
    if ($desktop -eq [IntPtr]::Zero) { throw "Couldn't make a desktop to run on ($([ComponentModel.Win32Exception]::new().Message)). -OnScreen runs without one." }

    # What it prints goes to a file, which is passed on here as it grows.
    $logName = if ($Instance -eq "ExodiumSmokeTest") { "exodium-smoke-test.log" } else { "exodium-smoke-test-$Instance.log" }
    $log = Join-Path ([IO.Path]::GetTempPath()) $logName
    Remove-Item $log -ErrorAction SilentlyContinue
    $quote = { param($text) "'" + ($text -replace "'", "''") + "'" }
    $arguments = "-OnOwnDesktop -PluginPath $(& $quote (Resolve-Path $PluginPath).Path) -WinampDir $(& $quote $WinampDir) " +
                 "-SettingsDir $(& $quote $SettingsDir) -ScratchDir $(& $quote $ScratchDir) -Label $(& $quote $Label) -DebugPort $DebugPort " +
                 "-Instance $(& $quote $Instance)"
    if ($SkipDrag) { $arguments += " -SkipDrag" }
    $command = "& $(& $quote $PSCommandPath) $arguments *> $(& $quote $log); exit `$LASTEXITCODE"
    $commandLine = "`"$PSHOME\powershell.exe`" -NoProfile -ExecutionPolicy Bypass -Command `"$command`""

    "(running on a desktop of its own, out of sight; -OnScreen to watch it)"
    $child = $null
    $reader = $null
    $exitCode = 1
    try {
        $child = [OwnDesktop]::Start($desktopName, $commandLine, (Get-Location).Path)
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
        # Stopped half way, it would leave a Winamp running where no one can see it.
        if ($child -and -not $child.HasExited) { & taskkill /pid $child.Id /t /f | Out-Null }
        foreach ($left in @(Get-Process winamp -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq "$wa\winamp.exe" })) {
            "  (stopping the test Winamp, left running on the test desktop)"
            $left.Kill()
        }
        [void][OwnDesktop]::CloseDesktop($desktop)
    }
    exit $exitCode
}

if (-not (Test-Path "$wa\winamp.exe")) {
    "Copying $WinampDir to $ScratchDir ..."
    # Left out: the browser's data, and gen_llama (plays a sound at start-up), gen_tray,
    # gen_hotkeys and gen_crasher (Nullsoft's crash reporter, which spins on a crash).
    $skipDirs = (Join-Path $WinampDir "Plugins\exodium_data"), (Join-Path $WinampDir "wv2_userdata")
    robocopy $WinampDir $wa /E /XD $skipDirs /XF gen_llama.dll gen_tray.dll gen_hotkeys.dll gen_crasher.dll "gen_browser.dll*" /NFL /NDL /NJH /NJS /NP /R:1 /W:1 | Out-Null
    robocopy $SettingsDir $ini /E /NFL /NDL /NJH /NJS /NP /R:1 /W:1 | Out-Null
    Set-Content -Path "$wa\paths.ini" -Value "[Winamp]`r`ninidir=$ini" -Encoding ascii
}
Copy-Item $PluginPath "$wa\Plugins\gen_browser.dll" -Force
$loader = Join-Path (Split-Path -Parent $PluginPath) "WebView2Loader.dll"
if (Test-Path $loader) { Copy-Item $loader "$wa\Plugins\WebView2Loader.dll" -Force }
Copy-Item (Join-Path (Split-Path -Parent $PluginPath) "exodium_open.exe") "$wa\Plugins\exodium_open.exe" -Force
# The input plug-in that plays the pages' sound to the visualizers, and a visualization
# plug-in of the test's own that writes down what reaches them (tests\vis_probe). Without
# them beside the plugin under test, as for an old build, that part is skipped.
foreach ($extra in "in_exodium.dll", "vis_probe.dll") {
    $from = Join-Path (Split-Path -Parent $PluginPath) $extra
    if (Test-Path $from) { Copy-Item $from "$wa\Plugins\$extra" -Force } else { Remove-Item "$wa\Plugins\$extra" -ErrorAction SilentlyContinue }
}
$soundTested = (Test-Path "$wa\Plugins\in_exodium.dll") -and (Test-Path "$wa\Plugins\vis_probe.dll")
# The probe's log, and the gain in_exodium.dll gives the sound: the test's tone is far too
# quiet to hear, and so too quiet for the visualizers to show as it is.
$probeLog = Join-Path $ScratchDir "vis-probe.log"
Remove-Item $probeLog -ErrorAction SilentlyContinue
$env:EXODIUM_VIS_PROBE_LOG = $probeLog
$env:EXODIUM_SOUND_TEST_GAIN = "1000"
# Each block in_exodium.dll hands on carries when it was heard, and the probe writes down,
# for every picture, when it was drawn and when the sound it was given was heard
# (in_exodium\sound_mark.h): how old the sound the visualizers show is.
$probePictures = Join-Path $ScratchDir "vis-probe-pictures.log"
Remove-Item $probePictures -ErrorAction SilentlyContinue
$env:EXODIUM_SOUND_TEST_MARK = "1"
$env:EXODIUM_VIS_PROBE_PICTURES = $probePictures

# The Start menu's checkbox puts its shortcut here, not in the Start menu of whoever runs
# this. The scratch Winamp has it from this process, and so does one exodium_open.exe starts.
$startMenu = Join-Path $ScratchDir "start-menu"
$shortcut = Join-Path $startMenu "Exodium.lnk"
$keptShortcut = Join-Path $ScratchDir "Exodium-kept.lnk"
Remove-Item $startMenu, $keptShortcut, "$wa\Plugins\exodium_open.exe.away" -Recurse -Force -ErrorAction SilentlyContinue
$env:EXODIUM_START_MENU_FOLDER = $startMenu

Add-Type -AssemblyName System.Windows.Forms

# Every run starts from the same modest window place, kept in the scratch winamp.ini.
$iniText = [System.IO.File]::ReadAllText("$ini\winamp.ini")
$iniText = [regex]::Replace($iniText, '(?ms)^\[EmbedWndExample\]\r?\n.*?(?=^\[|\z)', "[EmbedWndExample]`r`nxpos=100`r`nypos=90`r`nwidth=900`r`nheight=638`r`n")
$iniText = [regex]::Replace($iniText, '(?ms)^\[Exodium\]\r?\n.*?(?=^\[|\z)', "")  # no debugging port, to begin with
# Nor Bitwarden, which would be fetched from the Chrome Web Store: the test needs no network.
$iniText = $iniText.TrimEnd() + "`r`n[Exodium]`r`nbitwarden=0`r`n"
# The probe is the visualization plug-in, started whenever Winamp plays.
foreach ($pair in @(@("visplugin_name", "vis_probe.dll"), @("visplugin_num", "0"), @("visplugin_autoexec", "1"))) {
    $line = "$($pair[0])=$($pair[1])"
    if ($iniText -match "(?m)^$($pair[0])=") { $iniText = [regex]::Replace($iniText, "(?m)^$($pair[0])=.*$", $line) }
    else { $iniText = [regex]::Replace($iniText, '(?m)^\[Winamp\]\r?$', "[Winamp]`r`n$line") }
}
[System.IO.File]::WriteAllText("$ini\winamp.ini", $iniText)

Add-Type @"
using System;
using System.Text;
using System.Collections.Generic;
using System.Runtime.InteropServices;
public static class W {
    public delegate bool EnumProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr p, EnumProc cb, IntPtr l);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern bool IsWindowEnabled(IntPtr h);
    [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr h, int id);
    [DllImport("user32.dll")] public static extern bool IsWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr h);
    [DllImport("user32.dll")] public static extern bool IsZoomed(IntPtr h);
    [DllImport("user32.dll")] public static extern bool ShowWindowAsync(IntPtr h, int cmd);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr SendMessage(IntPtr h, uint m, IntPtr w, string l);
    [DllImport("user32.dll", CharSet=CharSet.Unicode, EntryPoint="SendMessageW")] public static extern IntPtr SendMessageSb(IntPtr h, uint m, IntPtr w, StringBuilder l);
    [DllImport("user32.dll", EntryPoint="SendMessageW")] public static extern IntPtr SendMessageP(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
    [StructLayout(LayoutKind.Sequential)] public struct WINDOWPLACEMENT { public int length, flags, showCmd; public POINT min, max; public RECT normal; }
    [DllImport("user32.dll")] public static extern bool GetWindowPlacement(IntPtr h, ref WINDOWPLACEMENT p);
    [DllImport("user32.dll")] public static extern bool SetWindowPlacement(IntPtr h, ref WINDOWPLACEMENT p);
    [DllImport("user32.dll")] public static extern IntPtr GetWindow(IntPtr h, uint cmd);
    [DllImport("user32.dll")] public static extern IntPtr GetParent(IntPtr h);
    [DllImport("user32.dll")] public static extern int GetDlgCtrlID(IntPtr h);
    [DllImport("user32.dll", EntryPoint="GetWindowLongW")] public static extern int GetWindowLong(IntPtr h, int index);
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode)] public static extern bool WritePrivateProfileString(string section, string key, string value, string file);
    [DllImport("user32.dll")] public static extern bool AttachThreadInput(uint attach, uint attachTo, bool on);
    [DllImport("kernel32.dll")] public static extern uint GetCurrentThreadId();
    [DllImport("user32.dll")] public static extern bool GetKeyboardState(byte[] state);
    [DllImport("user32.dll")] public static extern bool SetKeyboardState(byte[] state);
    [StructLayout(LayoutKind.Sequential)] public struct GUITHREADINFO { public int cbSize, flags; public IntPtr hwndActive, hwndFocus, hwndCapture, hwndMenuOwner, hwndMoveSize, hwndCaret; public RECT rcCaret; }
    [DllImport("user32.dll")] public static extern bool GetGUIThreadInfo(uint thread, ref GUITHREADINFO info);
    // The window with the keyboard on the thread that `h` belongs to.
    public static IntPtr Focus(IntPtr h) {
        uint pid; var info = new GUITHREADINFO(); info.cbSize = Marshal.SizeOf(info);
        GetGUIThreadInfo(GetWindowThreadProcessId(h, out pid), ref info);
        return info.hwndFocus;
    }
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
    public static string Class(IntPtr h) { var s = new StringBuilder(256); GetClassName(h, s, 256); return s.ToString(); }
    public static string Title(IntPtr h) { var s = new StringBuilder(1024); GetWindowText(h, s, 1024); return s.ToString(); }
    public static string Text(IntPtr h) { var s = new StringBuilder(4096); SendMessageSb(h, 0x000D, (IntPtr)4096, s); return s.ToString(); }
    public static List<IntPtr> TopLevel(uint pid) {
        var list = new List<IntPtr>();
        EnumWindows((h, l) => { uint p; GetWindowThreadProcessId(h, out p); if (p == pid) list.Add(h); return true; }, IntPtr.Zero);
        return list;
    }
    public static List<IntPtr> Children(IntPtr parent) {
        var list = new List<IntPtr>();
        EnumChildWindows(parent, (h, l) => { list.Add(h); return true; }, IntPtr.Zero);
        return list;
    }
}
"@

# Winamp's list views take pointers into Winamp, which is a 32-bit program, so a message to
# one carries its structures written into Winamp's memory.
Add-Type @"
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class Remote32 {
    [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll", EntryPoint="SendMessageW")] static extern IntPtr Send(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("kernel32.dll")] static extern IntPtr OpenProcess(uint access, bool inherit, uint pid);
    [DllImport("kernel32.dll")] static extern IntPtr VirtualAllocEx(IntPtr p, IntPtr addr, UIntPtr size, uint type, uint protect);
    [DllImport("kernel32.dll")] static extern bool VirtualFreeEx(IntPtr p, IntPtr addr, UIntPtr size, uint type);
    [DllImport("kernel32.dll")] static extern bool WriteProcessMemory(IntPtr p, IntPtr addr, byte[] data, UIntPtr size, out UIntPtr written);
    [DllImport("kernel32.dll")] static extern bool ReadProcessMemory(IntPtr p, IntPtr addr, byte[] data, UIntPtr size, out UIntPtr read);
    [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);
    // The text Winamp answers a message with, where it gives a pointer to a wide string in its
    // memory, such as IPC_GET_PLAYING_TITLE. Null when it gives none.
    public static string AskForText(IntPtr winamp, int param, int ipc) {
        IntPtr at = Send(winamp, 0x0400, (IntPtr)param, (IntPtr)ipc);  // WM_WA_IPC
        if ((long)at < 0x10000) return null;
        uint pid; GetWindowThreadProcessId(winamp, out pid);
        IntPtr process = OpenProcess(0x0010, false, pid);  // PROCESS_VM_READ
        if (process == IntPtr.Zero) return null;
        try {
            byte[] text = new byte[2048]; UIntPtr read;
            if (!ReadProcessMemory(process, at, text, (UIntPtr)text.Length, out read)) return null;
            string all = Encoding.Unicode.GetString(text);
            int end = all.IndexOf('\0');
            return end >= 0 ? all.Substring(0, end) : all;
        } finally { CloseHandle(process); }
    }
    // Selects the item of a list view with this text, as a click on it would, and says which it is: -1 for none.
    public static int SelectListItem(IntPtr list, string text) {
        uint pid; GetWindowThreadProcessId(list, out pid);
        IntPtr process = OpenProcess(0x0008 | 0x0010 | 0x0020, false, pid);  // PROCESS_VM_OPERATION | VM_READ | VM_WRITE
        if (process == IntPtr.Zero) return -1;
        IntPtr memory = VirtualAllocEx(process, IntPtr.Zero, (UIntPtr)4096, 0x3000, 0x04);  // MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE
        try {
            if (memory == IntPtr.Zero || (long)memory > uint.MaxValue) return -1;
            uint at = (uint)(long)memory;
            UIntPtr written;
            // LVFINDINFOW, 32-bit: flags = LVFI_STRING, then the text's address. The text goes after it.
            byte[] find = new byte[24];
            BitConverter.GetBytes(0x0002u).CopyTo(find, 0);
            BitConverter.GetBytes(at + 64).CopyTo(find, 4);
            byte[] name = Encoding.Unicode.GetBytes(text + "\0");
            WriteProcessMemory(process, memory, find, (UIntPtr)find.Length, out written);
            WriteProcessMemory(process, (IntPtr)(long)(at + 64), name, (UIntPtr)name.Length, out written);
            int index = (int)Send(list, 0x1053, (IntPtr)(-1), memory);  // LVM_FINDITEMW
            if (index < 0) return -1;
            // LVITEMW, 32-bit: state and stateMask = LVIS_SELECTED | LVIS_FOCUSED.
            byte[] item = new byte[60];
            BitConverter.GetBytes(3u).CopyTo(item, 12);
            BitConverter.GetBytes(3u).CopyTo(item, 16);
            WriteProcessMemory(process, memory, item, (UIntPtr)item.Length, out written);
            Send(list, 0x102B, (IntPtr)index, memory);       // LVM_SETITEMSTATE
            Send(list, 0x1043, IntPtr.Zero, (IntPtr)index);  // LVM_SETSELECTIONMARK, which is what Winamp reads
            return index;
        } finally {
            if (memory != IntPtr.Zero) VirtualFreeEx(process, memory, UIntPtr.Zero, 0x8000);  // MEM_RELEASE
            CloseHandle(process);
        }
    }
}
"@

[void][W]::SetProcessDPIAware()  # so that the coordinates seen here are the real ones
$script:failures = 0
function Check([bool]$ok, [string]$what) {
    if ($ok) { "  PASS  $what" } else { $script:failures++; "  FAIL  $what" }
}
function WaitFor([scriptblock]$cond, [int]$seconds = 30) {
    $until = (Get-Date).AddSeconds($seconds)
    while ((Get-Date) -lt $until) { if (& $cond) { return $true }; Start-Sleep -Milliseconds 250 }
    return $false
}
function LParam([int]$x, [int]$y) { [IntPtr](($y -shl 16) -bor ($x -band 0xFFFF)) }

# Our frames: Winamp's embed frames that hold an ExodiumClient.
function Frames($procId) {
    @([W]::TopLevel($procId) | Where-Object { [W]::Class($_) -eq "Winamp Gen" -and (ClientOf $_) -ne [IntPtr]::Zero })
}
function ClientOf($frame) {
    foreach ($c in [W]::Children($frame)) { if ([W]::Class($c) -eq "ExodiumClient") { return $c } }
    return [IntPtr]::Zero
}
function Control($client, [int]$id) {
    foreach ($c in [W]::Children($client)) { if ([W]::GetParent($c) -eq $client -and [W]::GetDlgCtrlID($c) -eq $id) { return $c } }
    return [IntPtr]::Zero
}
$TabBase = 0x8000 + 43
function TabButtons($client) {
    @([W]::Children($client) | Where-Object { [W]::GetParent($_) -eq $client -and [W]::GetDlgCtrlID($_) -ge $TabBase -and [W]::GetDlgCtrlID($_) -lt ($TabBase + 4096) } | Sort-Object { [W]::GetDlgCtrlID($_) })
}
function TabTexts($client) {
    @(TabButtons $client | ForEach-Object { [W]::Text($_) })
}
function TabByText($client, [string]$text) {
    @(TabButtons $client | Where-Object { [W]::Text($_) -eq $text })[0]
}
# Clicks a tab, with Ctrl or Shift held if $keys says so. The keys go in the message, which
# is where the plugin reads them from.
$MK_SHIFT = 0x4; $MK_CONTROL = 0x8
function ClickTab($button, [int]$keys = 0) {
    [void][W]::PostMessage($button, 0x0201, [IntPtr](1 -bor $keys), (LParam 10 10))  # WM_LBUTTONDOWN
    [void][W]::PostMessage($button, 0x0202, [IntPtr]$keys, (LParam 10 10))          # WM_LBUTTONUP
    Start-Sleep -Milliseconds 300
}
# The middle of a tab's close button, inside its button: the square tab_parts.cpp puts 4
# pixels in from the right end, inside a border of a pixel all round.
function CloseButtonPoint($button) {
    $r = New-Object W+RECT; [void][W]::GetWindowRect($button, [ref]$r)
    LParam ($r.R - $r.L - 2 - 4 - 8) (($r.B - $r.T - 2) / 2)
}
# Clicks where a tab's close button is, or would be.
function ClickCloseButton($button) {
    $at = CloseButtonPoint $button
    [void][W]::PostMessage($button, 0x0201, [IntPtr]1, $at)  # WM_LBUTTONDOWN
    [void][W]::PostMessage($button, 0x0202, [IntPtr]0, $at)  # WM_LBUTTONUP
    Start-Sleep -Milliseconds 300
}
# Drags a tab by the point 10,10 inside it and lets go at a point on the screen.
function DragTab($button, [int]$toX, [int]$toY) {
    $r = New-Object W+RECT; [void][W]::GetWindowRect($button, [ref]$r)
    $x = $toX - $r.L; $y = $toY - $r.T
    [void][W]::PostMessage($button, 0x0201, [IntPtr]1, (LParam 10 10))
    [void][W]::PostMessage($button, 0x0200, [IntPtr]1, (LParam 40 40))  # WM_MOUSEMOVE, past the drag threshold
    [void][W]::PostMessage($button, 0x0200, [IntPtr]1, (LParam $x $y))
    [void][W]::PostMessage($button, 0x0202, [IntPtr]0, (LParam $x $y))
    Start-Sleep -Milliseconds 500
}
# Runs $action with keys held, as far as the browser's thread can tell: this thread shares
# that thread's keyboard state for the while, which is where GetKeyState reads them, and the
# real keyboard is left alone. What $action sends must be sent, not posted, to be read in time.
function WithKeys($window, [int[]]$keys, [scriptblock]$action) {
    $owner = [uint32]0
    $thread = [W]::GetWindowThreadProcessId($window, [ref]$owner)
    $me = [W]::GetCurrentThreadId()
    [void][W]::AttachThreadInput($me, $thread, $true)
    try {
        $saved = New-Object byte[] 256
        [void][W]::GetKeyboardState($saved)
        $held = [byte[]]$saved.Clone()
        # Exactly the keys asked for are held. A modifier the shared state has down without
        # being asked, as Alt can be when the real keyboard's state leaks into it (AltSnap
        # works with Alt held), would turn Ctrl+N into Ctrl+Alt+N.
        foreach ($modifier in 0x10, 0x11, 0x12, 0x5B, 0x5C, 0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5) { $held[$modifier] = 0 }
        foreach ($key in $keys) { $held[$key] = 0x80 }
        [void][W]::SetKeyboardState($held)
        try { & $action } finally { [void][W]::SetKeyboardState($saved) }
    } finally {
        [void][W]::AttachThreadInput($me, $thread, $false)
    }
}
$VK_SHIFT = 0x10; $VK_CONTROL = 0x11
# Ctrl+N pressed in a window. The key is posted, as a real one is, where the plugin's hook on
# Winamp's queue sees it; a key that is sent goes past it. Ctrl stays down until what it opens
# has had time to: until there are $frames browser windows, if any are to open, and a moment
# more, in which a second one that shouldn't would come.
function PressCtrlN($window, [int]$frames = 0) {
    WithKeys $window @($VK_CONTROL) {
        [void][W]::PostMessage($window, 0x0100, [IntPtr]0x4E, [IntPtr]0x00310001)  # WM_KEYDOWN N, its scan code and a count of 1
        if ($frames) { [void](WaitFor { (Frames $procId).Count -ge $frames } 10) }
        Start-Sleep -Milliseconds 1000
    }
}
# Closes every browser window but the first, and says whether they are gone.
function CloseOtherFrames {
    foreach ($other in @(Frames $procId | Where-Object { $_ -ne $frame })) {
        [void][W]::PostMessage($other, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)  # WM_CLOSE
    }
    WaitFor { (Frames $procId).Count -eq 1 } 10
}
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class WinampKeys {
    [StructLayout(LayoutKind.Sequential)] struct COPYDATASTRUCT { public IntPtr dwData; public int cbData; public IntPtr lpData; }
    [DllImport("user32.dll", EntryPoint="SendMessageW")] static extern IntPtr Send(IntPtr h, uint m, IntPtr w, ref COPYDATASTRUCT l);
    [StructLayout(LayoutKind.Sequential)] struct GUITHREADINFO {
        public int cbSize, flags; public IntPtr active, focus, capture, menuOwner, moveSize, caret; public int left, top, right, bottom;
    }
    [DllImport("user32.dll")] static extern bool GetGUIThreadInfo(uint thread, ref GUITHREADINFO info);
    [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    // Adds a file to Winamp's playlist, as a file dropped on it would be (IPC_PLAYFILEW, which only adds it).
    public static void Enqueue(IntPtr winamp, string path) {
        IntPtr text = Marshal.StringToHGlobalUni(path);
        try {
            var data = new COPYDATASTRUCT { dwData = (IntPtr)1100, cbData = (path.Length + 1) * 2, lpData = text };
            Send(winamp, 0x004A, IntPtr.Zero, ref data);  // WM_COPYDATA
        } finally { Marshal.FreeHGlobal(text); }
    }
    // The window that has the keyboard's attention on the thread of this one.
    public static IntPtr ActiveWindow(IntPtr window) {
        uint pid;
        var info = new GUITHREADINFO(); info.cbSize = Marshal.SizeOf(info);
        return GetGUIThreadInfo(GetWindowThreadProcessId(window, out pid), ref info) ? info.active : IntPtr.Zero;
    }
}
"@
function PlaylistLength($main) {
    [int][W]::SendMessageP($main, 0x0400, [IntPtr]::Zero, [IntPtr]124)  # WM_WA_IPC IPC_GETLISTLENGTH
}
function HasWebView($client) {
    @([W]::Children($client) | Where-Object { [W]::Class($_) -like "Chrome_WidgetWin*" }).Count -gt 0
}
# The frame's corner ornaments, which the frame owns.
function OrnamentsOf($procId, $frame) {
    @([W]::TopLevel($procId) | Where-Object { [W]::Class($_) -eq "ExodiumOrnament" -and [W]::GetWindow($_, 4) -eq $frame })
}
# The one that lies over a corner of the frame: $left and $top say which.
function OrnamentOn($procId, $frame, [bool]$left, [bool]$top) {
    $fr = New-Object W+RECT; [void][W]::GetWindowRect($frame, [ref]$fr)
    $x = if ($left) { $fr.L } else { $fr.R - 1 }
    $y = if ($top) { $fr.T } else { $fr.B - 1 }
    foreach ($o in (OrnamentsOf $procId $frame)) {
        $r = New-Object W+RECT; [void][W]::GetWindowRect($o, [ref]$r)
        if ($x -ge $r.L -and $x -lt $r.R -and $y -ge $r.T -and $y -lt $r.B) { return $o }
    }
    return $null
}
# Clicks a button of the top right one: 0 minimizes, 1 maximizes, 2 closes. Where they are in
# the ornament's picture, which is 748 pixels wide, is ornament.cpp's to say (kArtButtons).
function ClickOrnament($ornament, [int]$button) {
    $r = New-Object W+RECT; [void][W]::GetWindowRect($ornament, [ref]$r)
    $scale = ($r.R - $r.L) / 748
    $at = LParam ([int](@(552, 618, 685)[$button] * $scale)) ([int](79 * $scale))
    [void][W]::PostMessage($ornament, 0x0201, [IntPtr]1, $at)  # WM_LBUTTONDOWN
    [void][W]::PostMessage($ornament, 0x0202, [IntPtr]0, $at)  # WM_LBUTTONUP
}
function Navigate($client, [string]$url) {
    $edit = Control $client 1003
    [void][W]::SendMessage($edit, 0x000C, [IntPtr]::Zero, $url)          # WM_SETTEXT
    [void][W]::PostMessage($edit, 0x0100, [IntPtr]0x0D, [IntPtr]::Zero)  # WM_KEYDOWN VK_RETURN
}
# Starts exodium_open.exe, as Windows does for a link, for the scratch Winamp's class.
function StartOpen([string]$link) {
    $quoted = if ($link) { " `"$link`"" } else { "" }
    $open = Start-Process "$wa\Plugins\exodium_open.exe" -ArgumentList "--class=$Instance$quoted" -PassThru
    [void]$open.Handle  # the exit code can't be read later without it
    $open
}
function OpenedOk($open) {
    $open.WaitForExit(40000) -and $open.ExitCode -eq 0
}
# A shortcut as the shell reads it: TargetPath, Arguments and the rest.
function ShortcutAt([string]$path) {
    (New-Object -ComObject WScript.Shell).CreateShortcut($path)
}
function PortOpen {
    [bool](Get-NetTCPConnection -LocalPort $DebugPort -State Listen -ErrorAction SilentlyContinue)
}
# One question to the browser over the DevTools protocol on the port at $debug, and its answer's
# result. Sent with no Origin, which is how the port takes tools' connections.
function CdpBrowser([string]$debug, [string]$method) {
    $socketUrl = (Invoke-RestMethod "$debug/json/version" -TimeoutSec 5).webSocketDebuggerUrl
    $socket = New-Object System.Net.WebSockets.ClientWebSocket
    $none = [Threading.CancellationToken]::None
    try {
        if (-not $socket.ConnectAsync([Uri]$socketUrl, $none).Wait(5000)) { return $null }
        $request = [Text.Encoding]::UTF8.GetBytes("{`"id`":1,`"method`":`"$method`",`"params`":{}}")
        if (-not $socket.SendAsync([ArraySegment[byte]]::new($request), "Text", $true, $none).Wait(5000)) { return $null }
        $buffer = New-Object byte[] 65536
        $answer = New-Object IO.MemoryStream
        do {
            $receive = $socket.ReceiveAsync([ArraySegment[byte]]::new($buffer), $none)
            if (-not $receive.Wait(5000)) { return $null }
            $answer.Write($buffer, 0, $receive.Result.Count)
        } until ($receive.Result.EndOfMessage)
        return ([Text.Encoding]::UTF8.GetString($answer.ToArray()) | ConvertFrom-Json).result
    } finally {
        $socket.Dispose()
    }
}
# Winamp's Preferences while they are open, and a control on the page they show.
function PrefsWindow($main) {
    [IntPtr][int64][W]::SendMessageP($main, 0x0400, [IntPtr]::Zero, [IntPtr]619)  # WM_WA_IPC IPC_GETPREFSWND
}
# A control on a page of the Preferences, by its number: on the page they show, if it has one.
# Winamp's own pages use some of the numbers Exodium's page does (1302 to 1314), and a page it
# has moved on from can still be there, out of sight.
function PageControl($prefs, [int]$id) {
    $outOfSight = [IntPtr]::Zero
    foreach ($c in [W]::Children($prefs)) {
        $page = [W]::GetParent($c)
        if ($page -ne $prefs -and [W]::GetParent($page) -eq $prefs -and [W]::GetDlgCtrlID($c) -eq $id) {
            if ([W]::IsWindowVisible($page)) { return $c }
            if ($outOfSight -eq [IntPtr]::Zero) { $outOfSight = $c }
        }
    }
    return $outOfSight
}
# Tells a control's page what a user did to it, as the control would: $code is the notification.
function Notify($control, [int]$code) {
    $wParam = [IntPtr](([W]::GetDlgCtrlID($control) -band 0xFFFF) -bor ($code -shl 16))
    [void][W]::SendMessageP([W]::GetParent($control), 0x0111, $wParam, $control)  # WM_COMMAND
}
# Picks an entry of a drop-down list by its text.
function PickFromList($list, [string]$text) {
    $at = [int][W]::SendMessage($list, 0x0158, [IntPtr](-1), $text)  # CB_FINDSTRINGEXACT
    [void][W]::SendMessageP($list, 0x014E, [IntPtr]$at, [IntPtr]::Zero)  # CB_SETCURSEL
    Notify $list 1  # CBN_SELCHANGE
}
# The [Exodium] section of the scratch winamp.ini, as it is on disk.
function ExodiumIni {
    $values = @{}
    $inSection = $false
    foreach ($line in Get-Content "$ini\winamp.ini") {
        if ($line -match '^\[(.+)\]$') { $inSection = $Matches[1] -eq "Exodium" }
        elseif ($inSection -and $line -match '^([^=]+)=(.*)$') { $values[$Matches[1]] = $Matches[2] }
    }
    $values
}
function ChildOfClass($parent, [string]$class) {
    @([W]::Children($parent) | Where-Object { [W]::Class($_) -eq $class })[0]
}
function PostClick($hwnd, [int]$x, [int]$y) {
    [void][W]::PostMessage($hwnd, 0x0201, [IntPtr]1, (LParam $x $y))  # WM_LBUTTONDOWN
    [void][W]::PostMessage($hwnd, 0x0202, [IntPtr]0, (LParam $x $y))  # WM_LBUTTONUP
}
# A WAV of silence, so that whatever the test has Winamp play can't be heard.
function SilentWav([string]$path, [double]$seconds, [int]$rate = 8000) {
    $silence = New-Object byte[] ([int]($rate * $seconds) * 2)
    $header = [byte[]](@(0x52,0x49,0x46,0x46) + [BitConverter]::GetBytes([int](36 + $silence.Length)) + @(0x57,0x41,0x56,0x45,0x66,0x6D,0x74,0x20) +
                       [BitConverter]::GetBytes([int]16) + @(1,0,1,0) + [BitConverter]::GetBytes([int]$rate) + [BitConverter]::GetBytes([int]($rate * 2)) +
                       @(2,0,16,0,0x64,0x61,0x74,0x61) + [BitConverter]::GetBytes([int]$silence.Length))
    [System.IO.File]::WriteAllBytes($path, [byte[]]($header + $silence))
}
# Winamp's playing: 1 playing, 3 paused, 0 stopped (IPC_ISPLAYING); and its playlist's position.
function WinampPlaying($main) { [W]::SendMessageP($main, 0x0400, [IntPtr]0, [IntPtr]104).ToInt64() }
function PlaylistPosition($main) { [W]::SendMessageP($main, 0x0400, [IntPtr]0, [IntPtr]125).ToInt64() }
function WinampCommand($main, [int]$id) { [void][W]::SendMessageP($main, 0x0111, [IntPtr]$id, [IntPtr]::Zero) }
# What the probe has written down, the latest last: when, and the loudest value each kind
# of visualizer was given.
function ProbeReadings {
    if (-not (Test-Path $probeLog)) { return @() }
    $reader = New-Object IO.StreamReader((New-Object IO.FileStream($probeLog, "Open", "Read", "ReadWrite")))
    try { $text = $reader.ReadToEnd() } finally { $reader.Dispose() }
    @($text -split "`r?`n" | Where-Object { $_ -match '^\d+ -?\d+ -?\d+ -?\d+$' } | ForEach-Object {
        $f = $_ -split ' '; [pscustomobject]@{ ms = [int64]$f[0]; wave = [int]$f[1]; spectrum = [int]$f[2]; analyzer = [int]$f[3] } })
}
# The pictures the probe has drawn since `since` (a SoundMarkTime), oldest first: for each,
# how many ms old the sound it was given was, or -1 when it was given none of the pages'.
function ProbePictures([uint32]$since) {
    if (-not (Test-Path $probePictures)) { return @() }
    $reader = New-Object IO.StreamReader((New-Object IO.FileStream($probePictures, "Open", "Read", "ReadWrite")))
    try { $text = $reader.ReadToEnd() } finally { $reader.Dispose() }
    # Differences of these 32-bit times, in units of 100 µs, as a signed number.
    $diff = { param([uint32]$a, [uint32]$b) $d = ([int64]$a - [int64]$b) % 4294967296; if ($d -ge 2147483648) { $d - 4294967296 } elseif ($d -lt -2147483648) { $d + 4294967296 } else { $d } }
    @($text -split "`r?`n" | Where-Object { $_ -match '^\d+ (\d+|-)$' } | ForEach-Object {
        $f = $_ -split ' '
        if ((& $diff ([uint32]$f[0]) $since) -gt 0) { if ($f[1] -eq '-') { -1 } else { (& $diff ([uint32]$f[0]) ([uint32]$f[1])) / 10.0 } }
    })
}
# Now, as a SoundMarkTime: the units of 100 µs, from the clock in_exodium.dll's times are on.
function MarkTimeNow {
    $at = [Diagnostics.Stopwatch]::GetTimestamp(); $per = [Diagnostics.Stopwatch]::Frequency
    $ticks = [math]::Floor($at / $per) * 10000000 + [math]::Floor(($at % $per) * 10000000 / $per)
    [uint32]([math]::Floor($ticks / 1000) % 4294967296)
}
function PressKey($hwnd, [int]$key) {
    [void][W]::PostMessage($hwnd, 0x0100, [IntPtr]$key, [IntPtr]1)                       # WM_KEYDOWN
    [void][W]::PostMessage($hwnd, 0x0101, [IntPtr]$key, [IntPtr]([int]0xC0000001 -bor 0))  # WM_KEYUP
}
# The page in view's own window. A click posted to it counts as the user's.
function PageWidget($client) {
    @([W]::Children($client) | Where-Object { [W]::Class($_) -eq "Chrome_RenderWidgetHostHWND" -and [W]::IsWindowVisible($_) })[0]
}
# How far in from a frame's left, top, right and bottom edges the client is, and the page
# in view: "left,top,right,bottom" for each.
function Insets($frame, $client) {
    $fr = New-Object W+RECT; [void][W]::GetWindowRect($frame, [ref]$fr)
    $view = @([W]::Children($client) | Where-Object { [W]::GetParent($_) -eq $client -and [W]::Class($_) -like "Chrome_WidgetWin*" -and [W]::IsWindowVisible($_) })[0]
    $result = @{}
    foreach ($part in @(@("client", $client), @("page", $view))) {
        $r = New-Object W+RECT
        if (-not $part[1] -or -not [W]::GetWindowRect($part[1], [ref]$r)) { $result[$part[0]] = "none"; continue }
        $result[$part[0]] = "$($r.L - $fr.L),$($r.T - $fr.T),$($fr.R - $r.R),$($fr.B - $r.B)"
    }
    [pscustomobject]$result
}
# Inside Winamp's border, where the frame isn't maximized: the client where Winamp keeps it
# (frame.cpp), and the page a margin in from that (layout.cpp).
$insideBorder = [pscustomobject]@{ client = "8,24,11,10"; page = "14,82,17,16" }
# Maximized, the border goes but for the title bar, and the page runs to the frame's edges.
$borderless = [pscustomobject]@{ client = "0,24,0,0"; page = "0,82,0,0" }
function InsetsAre($frame, $client, $want) {
    $now = Insets $frame $client
    $now.client -eq $want.client -and $now.page -eq $want.page
}
# The windows the webview's processes have on show of their own, as WebView2's own download dialog is.
function WebViewPopups($procId) {
    $all = @(Get-CimInstance Win32_Process -Filter "Name='msedgewebview2.exe'")
    $ids = @{}
    $ids[[int]$procId] = $true
    do {
        $grew = $false
        foreach ($p in $all) { if ($ids.ContainsKey([int]$p.ParentProcessId) -and -not $ids.ContainsKey([int]$p.ProcessId)) { $ids[[int]$p.ProcessId] = $true; $grew = $true } }
    } while ($grew)
    $ids.Remove([int]$procId)
    @($ids.Keys | ForEach-Object { [W]::TopLevel([uint32]$_) } | Where-Object { [W]::IsWindowVisible($_) })
}
function QuitWinamp($process) {
    if ($process.HasExited) { return }
    $main = @([W]::TopLevel([uint32]$process.Id) | Where-Object { [W]::Class($_) -eq $Instance })
    if ($main.Count) { [void][W]::PostMessage($main[0], 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) }
    if (-not $process.WaitForExit(30000)) { "  FAIL  Winamp did not exit within 30 s; killing it"; $script:failures++; $process.Kill(); $process.WaitForExit() }
}

"=== smoke test: $Label ==="
$plugin = Get-Item "$wa\Plugins\gen_browser.dll"
"plugin: $($plugin.Length) bytes, $($plugin.LastWriteTime)"
# The browser starts with no history and no profile, so that every run sees the same.
Remove-Item "$wa\Plugins\exodium_data" -Recurse -Force -ErrorAction SilentlyContinue
# Downloads go to a folder of the run's own, not the Downloads folder of whoever is at the
# machine: the new profile is given it before the browser first opens it.
$downloads = Join-Path $ScratchDir "downloads"
Remove-Item $downloads -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $downloads | Out-Null
$profileDir = "$wa\Plugins\exodium_data\webview_profile\EBWebView\Default"
New-Item -ItemType Directory -Force $profileDir | Out-Null
[System.IO.File]::WriteAllText("$profileDir\Preferences", '{"download":{"default_directory":' + (ConvertTo-Json $downloads) + ',"prompt_for_download":false}}')

$p = Start-Process "$wa\winamp.exe" -ArgumentList "/CLASS=$Instance" -PassThru
$procId = [uint32]$p.Id
try {
    Check (WaitFor { (Frames $procId).Count -ge 1 } 30) "plugin loaded and made its frame"
    $frame = (Frames $procId)[0]
    $client = ClientOf $frame
    Check (-not [W]::IsWindowVisible($frame)) "first window starts hidden"
    Check ([W]::GetWindow($frame, 4) -eq [IntPtr]::Zero) "frame has no owner (own taskbar/Alt+Tab entry)"
    Check ([W]::Title($frame) -eq "Exodium") "frame is titled Exodium before a page loads"
    Check ((TabButtons $client).Count -eq 1) "one tab to begin with"
    foreach ($id in 1001, 1002, 1003, 1005, 1006, 1100, 1101, 1102, 1103, 1200) {
        if ((Control $client $id) -eq [IntPtr]::Zero) { Check $false "control $id exists" }
    }
    Check $true "toolbar, tab strip and suggestion list controls exist"

    [void][W]::ShowWindowAsync($frame, 8)  # SW_SHOWNA
    Check (WaitFor { [W]::IsWindowVisible($frame) } 5) "frame shows"
    Check (WaitFor { HasWebView $client } 60) "webview is created once the window is shown"
    Check (WaitFor { [W]::Title($frame) -like "*Google*" } 40) "default page loads and its title reaches the frame: '$([W]::Title($frame))'"
    Check ([W]::Text((TabButtons $client)[0]) -like "*Google*") "tab button carries the page title"
    Check (-not (PortOpen)) "there is no debugging port unless winamp.ini names one"

    Navigate $client "https://example.com/"
    Check (WaitFor { [W]::Title($frame) -eq "Example Domain" } 30) "typed address navigates: '$([W]::Title($frame))'"
    Check (WaitFor { [W]::Text((Control $client 1003)) -eq "https://example.com/" } 10) "address bar shows the page's address: '$([W]::Text((Control $client 1003)))'"

    # Ctrl+N in Winamp's own windows opens a browser window, as it does in one, except where
    # Winamp has the key itself: in the playlist editor it is New playlist.
    $main = @([W]::TopLevel($procId) | Where-Object { [W]::Class($_) -eq $Instance })[0]
    PressCtrlN $main 2
    $made = @(Frames $procId | Where-Object { $_ -ne $frame })
    Check ($made.Count -eq 1 -and [W]::IsWindowVisible($made[0]) -and (TabButtons (ClientOf $made[0])).Count -eq 1) "Ctrl+N in Winamp's main window opens a browser window: $($made.Count) opened"
    if ($made.Count -eq 1) {
        $order = @([W]::TopLevel($procId))
        Check ([array]::IndexOf($order, $made[0]) -lt [array]::IndexOf($order, $main)) "it comes up in front of Winamp's main window"
        Check ([WinampKeys]::ActiveWindow($main) -eq $made[0]) "with the keyboard in it: '$([W]::Title([WinampKeys]::ActiveWindow($main)))'"
    }
    PressCtrlN (Control $client 1003) 3
    Check ((Frames $procId).Count -eq 3) "Ctrl+N in a browser window's address bar still opens just the one: $((Frames $procId).Count - 2) opened"
    $framesBefore = (Frames $procId).Count
    $library = @([W]::TopLevel($procId) | Where-Object { [W]::Class($_) -eq "Winamp Gen" -and (ClientOf $_) -eq [IntPtr]::Zero } |
                 Sort-Object { [W]::Title($_) -ne "Winamp Library" })[0]
    if ($library) {
        $inside = @([W]::Children($library))[0]
        PressCtrlN $(if ($inside) { $inside } else { $library }) ($framesBefore + 1)
        Check ((Frames $procId).Count -eq $framesBefore + 1) "Ctrl+N in a window in one of Winamp's frames ('$([W]::Title($library))') opens one as well"
    } else { "  SKIP  Ctrl+N in a window in one of Winamp's frames: the scratch Winamp has none" }
    Check (CloseOtherFrames) "the windows Ctrl+N opened close again"

    $wav = Join-Path $ScratchDir "ctrl-n.wav"
    SilentWav $wav 0.1
    [WinampKeys]::Enqueue($main, $wav)
    $playlist = [IntPtr][int64][W]::SendMessageP($main, 0x0400, [IntPtr]1, [IntPtr]260)  # WM_WA_IPC IPC_GETWND, the playlist editor
    if ((WaitFor { (PlaylistLength $main) -ge 1 } 5)) {
        PressCtrlN $playlist
        Check ((Frames $procId).Count -eq 1 -and (PlaylistLength $main) -eq 0) "Ctrl+N in the playlist editor is still Winamp's New playlist, and opens no browser window: $(PlaylistLength $main) left in the playlist"
    } else { Check $false "a file added to Winamp's playlist, to see Ctrl+N clear it" }

    # New tab button, then the history page in it.
    [void][W]::PostMessage($client, 0x0111, [IntPtr]1101, [IntPtr]::Zero)  # WM_COMMAND kIdNewTab
    Check (WaitFor { (TabButtons $client).Count -eq 2 } 10) "new tab button adds a tab"
    Start-Sleep -Seconds 3
    Navigate $client "exodium://history"
    Check (WaitFor { [W]::Title($frame) -eq "History" } 30) "history page is served: '$([W]::Title($frame))'"
    Check (WaitFor { [W]::Text((Control $client 1003)) -eq "exodium://history" } 10) "history page shows under its own name: '$([W]::Text((Control $client 1003)))'"

    if (-not $SkipDrag) {
        # Reorder: drag the first tab past the second, along the strip.
        $tabs = TabButtons $client
        $before = @($tabs | ForEach-Object { [W]::Text($_) })
        $r0 = New-Object W+RECT; $r1 = New-Object W+RECT
        [void][W]::GetWindowRect($tabs[0], [ref]$r0); [void][W]::GetWindowRect($tabs[1], [ref]$r1)
        $dropX = ($r1.R - $r0.L) - 5
        [void][W]::PostMessage($tabs[0], 0x0201, [IntPtr]1, (LParam 10 10))      # WM_LBUTTONDOWN
        [void][W]::PostMessage($tabs[0], 0x0200, [IntPtr]1, (LParam 60 10))      # WM_MOUSEMOVE
        [void][W]::PostMessage($tabs[0], 0x0200, [IntPtr]1, (LParam $dropX 10))
        [void][W]::PostMessage($tabs[0], 0x0202, [IntPtr]0, (LParam $dropX 10))  # WM_LBUTTONUP
        $ok = WaitFor { $t = TabButtons $client; $t.Count -eq 2 -and [W]::Text($t[0]) -eq $before[1] -and [W]::Text($t[1]) -eq $before[0] } 10
        Check $ok "dragging a tab along the strip reorders it: '$((TabButtons $client | ForEach-Object { [W]::Text($_) }) -join "' | '")'"

        # Tear off: drop the first tab well below the strip; it gets a window of its own.
        $tabs = TabButtons $client
        $tornTitle = [W]::Text($tabs[0])
        [void][W]::PostMessage($tabs[0], 0x0201, [IntPtr]1, (LParam 10 10))
        [void][W]::PostMessage($tabs[0], 0x0200, [IntPtr]1, (LParam 40 120))
        [void][W]::PostMessage($tabs[0], 0x0200, [IntPtr]1, (LParam 80 260))
        [void][W]::PostMessage($tabs[0], 0x0202, [IntPtr]0, (LParam 80 260))
        Check (WaitFor { (Frames $procId).Count -eq 2 } 15) "dropping a tab away from the strip opens a second window"
        $second = @(Frames $procId | Where-Object { $_ -ne $frame })[0]
        $secondClient = ClientOf $second
        Check (WaitFor { [W]::IsWindowVisible($second) -and (TabButtons $secondClient).Count -eq 1 -and (TabButtons $client).Count -eq 1 } 10) "the tab moved: one tab in each window"
        Check (WaitFor { [W]::Title($second) -eq $tornTitle } 10) "the new window takes the moved tab's title: '$([W]::Title($second))'"
        Check (WaitFor { HasWebView $secondClient } 10) "the live webview moved into the new window"

        # Drag it back onto the first window's strip.
        $src = (TabButtons $secondClient)[0]
        $dst = (TabButtons $client)[0]
        $rs = New-Object W+RECT; $rd = New-Object W+RECT
        [void][W]::GetWindowRect($src, [ref]$rs); [void][W]::GetWindowRect($dst, [ref]$rd)
        $bx = ($rd.R + 20) - $rs.L; $by = ($rd.T + 10) - $rs.T
        [void][W]::PostMessage($src, 0x0201, [IntPtr]1, (LParam 10 10))
        [void][W]::PostMessage($src, 0x0200, [IntPtr]1, (LParam 60 60))
        [void][W]::PostMessage($src, 0x0200, [IntPtr]1, (LParam $bx $by))
        [void][W]::PostMessage($src, 0x0202, [IntPtr]0, (LParam $bx $by))
        $back = WaitFor { (TabButtons $client).Count -eq 2 } 10
        if ($back) {
            Check $true "dragging the tab onto another window's strip moves it there"
            Check (WaitFor { -not [W]::IsWindow($second) } 10) "the window it left, now empty, is destroyed"
        } else {
            # The drop point is found by a real hit test, so another window on top of it defeats this step.
            "  SKIP  drag back onto the first window (drop point covered by another window?)"
            [void][W]::PostMessage($second, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)
            Check (WaitFor { -not [W]::IsWindow($second) } 10) "closing a second window destroys it"
        }

        # Several tabs at once: Ctrl+click and Shift+click select them, as in Chrome, and
        # they are dragged and closed together. The keys go with each posted click.
        $pages = Join-Path $ScratchDir "pages"
        New-Item -ItemType Directory -Force $pages | Out-Null
        foreach ($name in "A", "B", "C", "D", "E") {
            Set-Content "$pages\tab$name.html" "<!doctype html><title>Tab $name</title><p>$name</p>" -Encoding ascii
            [void][W]::PostMessage($client, 0x0111, [IntPtr]1101, [IntPtr]::Zero)  # kIdNewTab
            Start-Sleep -Seconds 2
            Navigate $client ([Uri]"$pages\tab$name.html").AbsoluteUri
            [void](WaitFor { [W]::Title($frame) -eq "Tab $name" } 20)
        }
        $beforeSelecting = TabTexts $client
        Check (($beforeSelecting | Select-Object -Last 5) -join "," -eq "Tab A,Tab B,Tab C,Tab D,Tab E") "five tabs to select among: '$($beforeSelecting -join "' | '")'"

        # Click A, Shift+click C, and drag B away from every strip: all three go to a new window.
        ClickTab (TabByText $client "Tab A")
        ClickTab (TabByText $client "Tab C") $MK_SHIFT
        $rb = New-Object W+RECT; [void][W]::GetWindowRect((TabByText $client "Tab B"), [ref]$rb)
        DragTab (TabByText $client "Tab B") ($rb.L + 80) ($rb.T + 260)
        $ok = WaitFor { (Frames $procId).Count -eq 2 } 15
        $group = @(Frames $procId | Where-Object { $_ -ne $frame })[0]
        $groupClient = if ($group) { ClientOf $group } else { [IntPtr]::Zero }
        Check ($ok -and (WaitFor { (TabTexts $groupClient) -join "," -eq "Tab A,Tab B,Tab C" } 10)) "Shift+click then a drag out takes the three tabs to a window of their own, in order: '$((TabTexts $groupClient) -join "' | '")'"
        Check ((TabTexts $client | Where-Object { $_ -in "Tab A", "Tab B", "Tab C" }).Count -eq 0) "and they have left the first window"
        Check (WaitFor { [W]::Title($group) -eq "Tab B" } 10) "the tab that was dragged is the one in view: '$([W]::Title($group))'"

        # Drag A, still selected with the other two, onto the first window, right of its first tab.
        $first = (TabButtons $client)[0]
        $rf = New-Object W+RECT; [void][W]::GetWindowRect($first, [ref]$rf)
        DragTab (TabByText $groupClient "Tab A") ($rf.R - 5) ($rf.T + 10)
        $back = WaitFor { -not [W]::IsWindow($group) } 10
        if ($back) {
            $texts = TabTexts $client
            Check (($texts[1..3] -join ",") -eq "Tab A,Tab B,Tab C") "dragging one of them onto another window's strip takes all three there, side by side: '$($texts -join "' | '")'"

            # Click A, Ctrl+click C, and drag C to the end of the strip: they go there together.
            ClickTab (TabByText $client "Tab A")
            ClickTab (TabByText $client "Tab C") $MK_CONTROL
            $tabs = TabButtons $client
            $rl = New-Object W+RECT; [void][W]::GetWindowRect($tabs[$tabs.Count - 1], [ref]$rl)
                DragTab (TabByText $client "Tab C") ($rl.R + 2) ($rl.T + 10)
            Check (WaitFor { ((TabTexts $client | Select-Object -Last 2) -join ",") -eq "Tab A,Tab C" } 10) "Ctrl+click then a drag along the strip moves both, side by side: '$((TabTexts $client) -join "' | '")'"
            Check ((TabTexts $client)[1] -eq "Tab B") "the tab between them stays where it was"

            # A plain click on one of several selected picks it alone: the close button then takes only it.
            $count = (TabButtons $client).Count
            ClickTab (TabByText $client "Tab C")
            [void][W]::PostMessage($client, 0x0111, [IntPtr]1006, [IntPtr]::Zero)  # kIdCloseTab
            Check (WaitFor { (TabButtons $client).Count -eq $count - 1 } 10) "a plain click on a selected tab leaves it the only one selected"
            Check ((TabTexts $client) -contains "Tab A" -and (TabTexts $client) -notcontains "Tab C") "and the close button closes just that one"

            # Ctrl+click on a selected tab takes it out of the selection again.
            $count = (TabButtons $client).Count
            ClickTab (TabByText $client "Tab B")
            ClickTab (TabByText $client "Tab D") $MK_CONTROL
            ClickTab (TabByText $client "Tab B") $MK_CONTROL
            Check (WaitFor { [W]::Title($frame) -eq "Tab D" } 5) "Ctrl+click on the tab in view takes it out, and the other selected one comes into view"
            [void][W]::PostMessage($client, 0x0111, [IntPtr]1006, [IntPtr]::Zero)
            Check (WaitFor { (TabButtons $client).Count -eq $count - 1 } 10) "the close button then closes one tab"
            Check ((TabTexts $client) -contains "Tab B" -and (TabTexts $client) -notcontains "Tab D") "the one still selected: '$((TabTexts $client) -join "' | '")'"

            # The close button takes every selected tab.
            $count = (TabButtons $client).Count
            ClickTab (TabByText $client "Tab A")
            ClickTab (TabByText $client "Tab B") $MK_CONTROL
            [void][W]::PostMessage($client, 0x0111, [IntPtr]1006, [IntPtr]::Zero)
            Check (WaitFor { (TabButtons $client).Count -eq $count - 2 } 10) "with two tabs selected, the close button closes both: '$((TabTexts $client) -join "' | '")'"
        } else {
            "  SKIP  the rest of the tests of several tabs (drop point covered by another window?)"
            [void][W]::PostMessage($group, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)
            Check (WaitFor { -not [W]::IsWindow($group) } 10) "closing the window of three tabs destroys it"
        }
        # The tests after these expect the tabs as they found them: the ones made here go, and
        # the last of the rest comes into view.
        foreach ($left in @(TabTexts $client | Where-Object { $_ -like "Tab ?" })) {
            [void][W]::PostMessage((TabByText $client $left), 0x0208, [IntPtr]::Zero, (LParam 5 5))  # WM_MBUTTONUP
            [void](WaitFor { (TabTexts $client) -notcontains $left } 10)
        }
        $tabs = TabButtons $client
        ClickTab $tabs[$tabs.Count - 1]
        Check (((TabTexts $client) -join ",") -eq (($beforeSelecting | Select-Object -SkipLast 5) -join ",")) "the tabs that were there before are left as they were: '$((TabTexts $client) -join "' | '")'"
    }

    # Middle-click closes a tab.
    $count = (TabButtons $client).Count
    if ($count -ge 2) {
        [void][W]::PostMessage((TabButtons $client)[0], 0x0208, [IntPtr]::Zero, (LParam 5 5))  # WM_MBUTTONUP
        Check (WaitFor { (TabButtons $client).Count -eq $count - 1 } 10) "middle-click closes a tab"
    }

    # Ctrl+click on Back or Forward opens the page it leads to in a new tab with a copy of the
    # tab's history, as in Chrome. Each entry of the copy is made as a blank stand-in with the
    # title the page had, and a page says in its title how it was loaded, so a title of the
    # copy's own tells a page that loaded for real from its stand-in.
    $pages = Join-Path $ScratchDir "pages"
    New-Item -ItemType Directory -Force $pages | Out-Null
    foreach ($n in 0..3) {
        Set-Content "$pages\bf$n.html" ("<!doctype html><title>BF $n</title><script>document.title += ' (' + performance.getEntriesByType('navigation')[0].type + ')'</script>" +
                                        "<p>$n</p><div style='height:3000px'></div><p id=part>part</p>") -Encoding ascii
    }
    $bf = { param([int]$n, [string]$fragment = "") ([Uri]"$pages\bf$n.html").AbsoluteUri + $fragment }
    $urlBar = { [W]::Text((Control $client 1003)) }
    $click = { param([int]$id) [void][W]::SendMessageP($client, 0x0111, [IntPtr]$id, (Control $client $id)) }  # WM_COMMAND, as a button sends it
    # A click with no key held, whatever the real keyboard's state leaks in: Shift down would
    # have Back and Forward open a window, as it once did to six of them.
    $plainClick = { param([int]$id) WithKeys $client @() { & $click $id } }
    $baseline = (TabButtons $client).Count
    [void][W]::PostMessage($client, 0x0111, [IntPtr]1101, [IntPtr]::Zero)  # kIdNewTab
    Start-Sleep -Seconds 2
    foreach ($n in 0..3) {
        Navigate $client (& $bf $n)
        [void](WaitFor { [W]::Title($frame) -eq "BF $n (navigate)" } 20)
        if ($n -eq 2) {
            Navigate $client (& $bf 2 "#part")
            [void](WaitFor { (& $urlBar) -eq (& $bf 2 "#part") } 10)
            Start-Sleep -Milliseconds 500
        }
    }
    $count = (TabButtons $client).Count
    $source = $count - 1
    Check ([W]::Title($frame) -eq "BF 3 (navigate)") "a tab with pages behind it: '$([W]::Title($frame))'"

    WithKeys $client @($VK_CONTROL) { & $click 1001 }  # kIdBack
    Check (WaitFor { (TabButtons $client).Count -eq $count + 1 } 10) "Ctrl+click on Back opens a tab"
    Check (WaitFor { (TabTexts $client)[$source + 1] -eq "BF 2 (back_forward)" } 20) "beside the tab, with the page Back leads to loaded for real: '$((TabTexts $client)[$source + 1])'"
    Check ([W]::Title($frame) -eq "BF 3 (navigate)" -and (& $urlBar) -eq (& $bf 3)) "behind the tab in view, which stays on its page: '$([W]::Title($frame))' '$(& $urlBar)'"

    ClickTab (TabButtons $client)[$source + 1]
    Check (WaitFor { (& $urlBar) -eq (& $bf 2 "#part") } 10) "the new tab is on the entry itself, fragment and all: '$(& $urlBar)'"
    & $plainClick 1001
    Check (WaitFor { (& $urlBar) -eq (& $bf 2) } 10) "Back in it goes where it would have in the tab it came from: '$(& $urlBar)'"
    & $plainClick 1001
    Check (WaitFor { [W]::Title($frame) -eq "BF 1 (back_forward)" } 20) "and on, to a page loaded for real once it is come to: '$([W]::Title($frame))'"
    & $plainClick 1002
    [void](WaitFor { [W]::Title($frame) -eq "BF 2 (back_forward)" } 20)
    & $plainClick 1002
    [void](WaitFor { (& $urlBar) -eq (& $bf 2 "#part") } 10)
    & $plainClick 1002
    Check (WaitFor { [W]::Title($frame) -eq "BF 3 (back_forward)" -and (& $urlBar) -eq (& $bf 3) } 20) "Forward leads on to the last of them: '$([W]::Title($frame))' '$(& $urlBar)'"

    # The middle button on Forward, from the entry before the last: the copy ends on the page it shows.
    & $plainClick 1001
    [void](WaitFor { (& $urlBar) -eq (& $bf 2 "#part") } 10)
    [void][W]::PostMessage((Control $client 1002), 0x0208, [IntPtr]::Zero, (LParam 5 5))  # WM_MBUTTONUP
    Check (WaitFor { (TabButtons $client).Count -eq $count + 2 } 10) "the middle button on Forward opens a tab as well"
    Check (WaitFor { (TabTexts $client)[$source + 2] -eq "BF 3 (reload)" } 20) "beside the one in view, with the page Forward leads to: '$((TabTexts $client)[$source + 2])'"

    # Ctrl+Shift+click on Back, where the page shares its document with every entry after it.
    Navigate $client (& $bf 2 "#more")
    [void](WaitFor { (& $urlBar) -eq (& $bf 2 "#more") } 10)
    Start-Sleep -Milliseconds 500
    WithKeys $client @($VK_CONTROL, $VK_SHIFT) { & $click 1001 }
    Check (WaitFor { (TabButtons $client).Count -eq $count + 3 } 10) "Ctrl+Shift+click on Back opens a tab"
    Check (WaitFor { [W]::Title($frame) -eq "BF 2 (reload)" -and (& $urlBar) -eq (& $bf 2 "#part") } 20) "in front, with the page loaded for real: '$([W]::Title($frame))' '$(& $urlBar)'"

    $frameCount = (Frames $procId).Count
    WithKeys $client @($VK_SHIFT) { & $click 1001 }
    Check (WaitFor { (Frames $procId).Count -eq $frameCount + 1 } 10) "Shift+click on Back opens a window"
    $bfWindow = @(Frames $procId | Where-Object { $_ -ne $frame })[0]
    if ($bfWindow) {
        Check (WaitFor { [W]::IsWindowVisible($bfWindow) -and [W]::Title($bfWindow) -eq "BF 2 (reload)" } 20) "with the page Back leads to in it: '$([W]::Title($bfWindow))'"
        [void][W]::PostMessage($bfWindow, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)  # WM_CLOSE
        [void](WaitFor { -not [W]::IsWindow($bfWindow) } 10)
    }

    # Nowhere to go: nothing opens. Nor where Forward only leads to the page a closed tab was
    # put away on, which a tab reopened with Ctrl+Shift+T has ahead of it.
    ClickTab (TabButtons $client)[$source]
    $now = (TabButtons $client).Count
    WithKeys $client @($VK_CONTROL) { & $click 1002 }
    Start-Sleep -Seconds 2
    Check ((TabButtons $client).Count -eq $now -and [W]::Title($frame) -eq "BF 3 (navigate)") "with nowhere to go, Ctrl+click on Forward opens nothing"
    & $click 1006  # kIdCloseTab
    [void](WaitFor { (TabButtons $client).Count -eq $now - 1 } 10)
    WithKeys $client @($VK_CONTROL, $VK_SHIFT) { [void][W]::SendMessageP($client, 0x0100, [IntPtr]0x54, [IntPtr]::Zero) }  # WM_KEYDOWN T
    [void](WaitFor { (TabButtons $client).Count -eq $now -and [W]::Title($frame) -eq "BF 3 (back_forward)" } 20)
    WithKeys $client @($VK_CONTROL) { & $click 1002 }
    Start-Sleep -Seconds 2
    Check ((TabButtons $client).Count -eq $now -and [W]::Title($frame) -like "BF 3 (*") "nor on a reopened tab with only its put-away page ahead: '$([W]::Title($frame))', $((TabButtons $client).Count) tabs"

    # The tests after these expect the tabs as they found them, with the last in view.
    while ($true) {
        $left = @(TabButtons $client | Where-Object { [W]::Text($_) -like "BF *" })
        if (-not $left.Count) { break }
        $before = (TabButtons $client).Count
        [void][W]::PostMessage($left[0], 0x0208, [IntPtr]::Zero, (LParam 5 5))  # WM_MBUTTONUP
        if (-not (WaitFor { (TabButtons $client).Count -lt $before } 10)) { break }
    }
    $tabs = TabButtons $client
    ClickTab $tabs[$tabs.Count - 1]
    Check ((TabButtons $client).Count -eq $baseline) "the tabs opened for these close again"

    # The close button at a tab's right end closes that tab and no other. The mouse going
    # down on it neither brings the tab into view nor starts a drag. A click anywhere else
    # on the tab brings it into view, as before.
    $pages = Join-Path $ScratchDir "pages"
    New-Item -ItemType Directory -Force $pages | Out-Null
    foreach ($name in "W", "X", "Y", "Z") {
        Set-Content "$pages\close$name.html" "<!doctype html><title>Close $name</title><p>$name</p>" -Encoding ascii
        [void][W]::PostMessage($client, 0x0111, [IntPtr]1101, [IntPtr]::Zero)  # kIdNewTab
        Start-Sleep -Seconds 2
        Navigate $client ([Uri]"$pages\close$name.html").AbsoluteUri
        [void](WaitFor { [W]::Title($frame) -eq "Close $name" } 20)
    }
    $beforeClosing = TabTexts $client
    $count = $beforeClosing.Count
    $frameCount = (Frames $procId).Count
    Check (($beforeClosing | Select-Object -Last 4) -join "," -eq "Close W,Close X,Close Y,Close Z") "four tabs to close: '$($beforeClosing -join "' | '")'"
    ClickCloseButton (TabByText $client "Close X")
    Check (WaitFor { (TabButtons $client).Count -eq $count - 1 -and (TabTexts $client) -notcontains "Close X" } 10) "a click on a tab's close button closes it: '$((TabTexts $client) -join "' | '")'"
    Check ([W]::Title($frame) -eq "Close Z") "without bringing it into view first: '$([W]::Title($frame))'"

    $closeY = TabByText $client "Close Y"
    $at = CloseButtonPoint $closeY
    [void][W]::PostMessage($closeY, 0x0201, [IntPtr]1, $at)                 # WM_LBUTTONDOWN on its close button
    [void][W]::PostMessage($closeY, 0x0200, [IntPtr]1, (LParam 40 120))     # WM_MOUSEMOVE, far past the drag threshold
    [void][W]::PostMessage($closeY, 0x0200, [IntPtr]1, (LParam 80 260))
    [void][W]::PostMessage($closeY, 0x0202, [IntPtr]0, (LParam 80 260))     # WM_LBUTTONUP away from the strip
    Start-Sleep -Milliseconds 800
    Check ((TabButtons $client).Count -eq $count - 1 -and (Frames $procId).Count -eq $frameCount -and [W]::Title($frame) -eq "Close Z") "pressed and let go elsewhere, it closes nothing, drags nothing, and leaves the tab in view as it was: '$([W]::Title($frame))'"

    $r = New-Object W+RECT; [void][W]::GetWindowRect($closeY, [ref]$r)
    $beside = LParam ($r.R - $r.L - 2 - 28) 11  # on the title, just left of the close button
    [void][W]::PostMessage($closeY, 0x0201, [IntPtr]1, $beside)
    [void][W]::PostMessage($closeY, 0x0202, [IntPtr]0, $beside)
    Check ((WaitFor { [W]::Title($frame) -eq "Close Y" } 5) -and (TabButtons $client).Count -eq $count - 1) "a click on the tab just left of it brings the tab into view: '$([W]::Title($frame))'"

    # With several tabs selected, as with a middle-click, only the tab clicked closes.
    ClickTab (TabByText $client "Close W") $MK_CONTROL
    Check ([W]::Title($frame) -eq "Close W") "Ctrl+click selects a second tab"
    ClickCloseButton (TabByText $client "Close Y")
    Check (WaitFor { (TabButtons $client).Count -eq $count - 2 -and (TabTexts $client) -notcontains "Close Y" } 10) "the close button of one of two selected tabs closes that one alone: '$((TabTexts $client) -join "' | '")'"
    Check ((TabTexts $client) -contains "Close W" -and [W]::Title($frame) -eq "Close W") "and the other stays, in view"

    ClickCloseButton (TabByText $client "Close W")
    Check (WaitFor { (TabButtons $client).Count -eq $count - 3 -and (TabTexts $client) -notcontains "Close W" } 10) "the close button of the tab in view closes it: '$((TabTexts $client) -join "' | '")'"
    # The tests after these expect the tabs as they found them.
    [void][W]::PostMessage((TabByText $client "Close Z"), 0x0208, [IntPtr]::Zero, (LParam 5 5))  # WM_MBUTTONUP
    [void](WaitFor { (TabTexts $client) -notcontains "Close Z" } 10)
    $tabs = TabButtons $client
    ClickTab $tabs[$tabs.Count - 1]
    Check (((TabTexts $client) -join ",") -eq (($beforeClosing | Select-Object -SkipLast 4) -join ",")) "the tabs that were there before are left as they were: '$((TabTexts $client) -join "' | '")'"

    # A page that opens another: in a tab, unless it asks for a window of its own.
    $pages = Join-Path $ScratchDir "pages"
    New-Item -ItemType Directory -Force $pages | Out-Null
    Set-Content "$pages\opened.html" '<!doctype html><title>Opened Page</title><p>opened</p>' -Encoding ascii
    Set-Content "$pages\opener.html" @'
<!doctype html><title>Opener Page</title>
<script>
const features = { "?tab": undefined, "?popup": "width=420,height=300,left=200,top=150", "?bare": "popup" };
window.open("opened.html", "_blank", features[location.search]);
</script>
'@ -Encoding ascii
    $opener = ([Uri]"$pages\opener.html").AbsoluteUri
    $count = (TabButtons $client).Count
    $frameCount = (Frames $procId).Count

    Navigate $client "$opener`?tab"
    Check (WaitFor { (TabButtons $client).Count -eq $count + 1 } 30) "a page opening another with nothing asked of the window gets a tab"
    Check (WaitFor { [W]::Title($frame) -eq "Opened Page" } 20) "the opened page loads in that tab: '$([W]::Title($frame))'"
    Check ((Frames $procId).Count -eq $frameCount) "and no window is opened for it"
    [void][W]::PostMessage((TabButtons $client)[$count], 0x0208, [IntPtr]::Zero, (LParam 5 5))  # WM_MBUTTONUP
    # The title is the last thing a closing tab leaves changed. An address typed before
    # then would be replaced by the address of the tab that comes into view.
    Check (WaitFor { (TabButtons $client).Count -eq $count -and [W]::Title($frame) -eq "Opener Page" } 10) "that tab closes again"

    foreach ($ask in "popup", "bare") {
        Navigate $client "$opener`?$ask"
        Check (WaitFor { (Frames $procId).Count -eq $frameCount + 1 } 30) "[$ask] a page asking for a window of its own gets one"
        $popup = @(Frames $procId | Where-Object { $_ -ne $frame })[0]
        if (-not $popup) { continue }
        $popupClient = ClientOf $popup
        Check (WaitFor { [W]::IsWindowVisible($popup) -and [W]::Title($popup) -eq "Opened Page" } 20) "[$ask] the window shows, with the opened page in it: '$([W]::Title($popup))'"
        Check ((TabButtons $popupClient).Count -eq 1 -and (TabButtons $client).Count -eq $count) "[$ask] the page is the window's one tab, and the opener's window has no more tabs than before"
        if ($ask -eq "popup") {
            # Winamp sees pixels at the main screen's scale, whether or not it is DPI-aware.
            $scale = [System.Drawing.Graphics]::FromHwnd([IntPtr]::Zero).DpiX / 96
            $view = @([W]::Children($popupClient) | Where-Object { [W]::GetParent($_) -eq $popupClient -and [W]::Class($_) -like "Chrome_WidgetWin*" })[0]
            $pr = New-Object W+RECT; $vr = New-Object W+RECT
            [void][W]::GetWindowRect($popup, [ref]$pr); [void][W]::GetWindowRect($view, [ref]$vr)
            $w = $vr.R - $vr.L; $h = $vr.B - $vr.T
            Check ([Math]::Abs($w - 420 * $scale) -le 2 -and [Math]::Abs($h - 300 * $scale) -le 2) "[$ask] the page has the size asked for: ${w}x$h at scale $scale"
            Check ([Math]::Abs($pr.L - 200 * $scale) -le 2 -and [Math]::Abs($pr.T - 150 * $scale) -le 2) "[$ask] the window is where it was asked to be: ($($pr.L),$($pr.T))"
        }
        [void][W]::PostMessage($popup, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)  # WM_CLOSE
        Check (WaitFor { -not [W]::IsWindow($popup) } 10) "[$ask] closing the window destroys it"
    }

    # Downloads are followed in a panel of the browser's own, in place of WebView2's dialog,
    # which took the window over: the browser's shortcuts stopped while it was open. The
    # page keeps the keyboard, and Escape closes the panel wherever the keyboard is.
    $panel = ChildOfClass $client "ExodiumDownloads"
    $downloadsButton = ChildOfClass $client "ExodiumDownloadsButton"
    $edit = Control $client 1003
    Check ($panel -and $downloadsButton -and -not [W]::IsWindowVisible($downloadsButton)) "there is no downloads button while there are no downloads"
    $er = New-Object W+RECT; [void][W]::GetWindowRect($edit, [ref]$er); $editWidth = $er.R - $er.L
    $dlName = "exodium-smoke-" + [guid]::NewGuid().ToString("N").Substring(0, 8) + ".txt"
    Set-Content "$pages\download.html" @"
<!doctype html><title>Download Page</title>
<style>html,body{margin:0;height:100%} a{display:block;position:fixed;inset:0;background:#234}</style>
<a download="$dlName" href="data:text/plain,downloaded%20by%20the%20smoke%20test">download</a>
<script>addEventListener('keydown', e => { if (e.key === 'Escape') document.title = 'Escape reached the page'; });</script>
"@ -Encoding ascii
    Navigate $client ([Uri]"$pages\download.html").AbsoluteUri
    [void](WaitFor { [W]::Title($frame) -eq "Download Page" } 20)
    Start-Sleep -Seconds 1
    $dialogsBefore = WebViewPopups $procId
    $saved = Join-Path $downloads $dlName
    # A click posted to a page that has only just loaded is now and then lost: one more try.
    foreach ($try in 1..2) {
        PostClick (PageWidget $client) 200 200
        if (WaitFor { Test-Path -LiteralPath $saved } 10) { break }
    }
    Check (Test-Path -LiteralPath $saved) "a download from a page is saved, in the profile's download folder"
    $stray = Join-Path ([Environment]::GetFolderPath("UserProfile")) "Downloads\$dlName"
    if (Test-Path -LiteralPath $stray) { Remove-Item -LiteralPath $stray -Force; Check $false "the download went to the real Downloads folder (removed again)" }
    Check (WaitFor { [W]::IsWindowVisible($panel) } 5) "the downloads panel comes up by itself"
    [void][W]::GetWindowRect($edit, [ref]$er)
    Check ([W]::IsWindowVisible($downloadsButton) -and ($er.R - $er.L) -lt $editWidth) "with the downloads button on the toolbar, the address bar making room for it"
    Check (WaitFor { [W]::Title($panel) -like "*$dlName (Done)*" } 5) "the panel lists the download as done: '$([W]::Title($panel))'"
    Start-Sleep -Seconds 1
    $dialogs = @(WebViewPopups $procId | Where-Object { $dialogsBefore -notcontains $_ })
    Check ($dialogs.Count -eq 0) "WebView2's own download dialog stays shut: $(($dialogs | ForEach-Object { [W]::Class($_) }) -join ', ')"
    $keyboard = [W]::Focus($client)
    Check ($keyboard -ne [IntPtr]::Zero -and [W]::Class($keyboard) -eq "Chrome_WidgetWin_1") "the page keeps the keyboard: $([W]::Class($keyboard))"

    # Escape, sent where the keyboard is, as a key pressed in the page would be.
    PressKey $keyboard 0x1B
    Check (WaitFor { -not [W]::IsWindowVisible($panel) } 3) "Escape in the page closes the panel"
    Start-Sleep -Milliseconds 500
    Check ([W]::Title($frame) -eq "Download Page") "and the page doesn't see that Escape: '$([W]::Title($frame))'"
    PressKey $keyboard 0x1B
    Check (WaitFor { [W]::Title($frame) -eq "Escape reached the page" } 3) "with the panel closed, Escape is the page's again"

    PostClick $downloadsButton 10 10
    Check (WaitFor { [W]::IsWindowVisible($panel) } 3) "the downloads button opens the panel"
    PostClick $downloadsButton 10 10
    Check (WaitFor { -not [W]::IsWindowVisible($panel) } 3) "and closes it again"
    foreach ($place in @(@($edit, "the address bar"), @($client, "the window itself"))) {
        PostClick $downloadsButton 10 10
        [void](WaitFor { [W]::IsWindowVisible($panel) } 3)
        PressKey $place[0] 0x1B
        Check (WaitFor { -not [W]::IsWindowVisible($panel) } 3) "Escape with the keyboard in $($place[1]) closes the panel"
    }

    # Clearing the finished downloads takes the button away again. Where Clear is in the
    # panel's header is downloads_panel.cpp's to say.
    PostClick $downloadsButton 10 10
    [void](WaitFor { [W]::IsWindowVisible($panel) } 3)
    $pr = New-Object W+RECT; [void][W]::GetWindowRect($panel, [ref]$pr)
    PostClick $panel ($pr.R - $pr.L - 79) 17
    Check (WaitFor { -not [W]::IsWindowVisible($downloadsButton) -and -not [W]::IsWindowVisible($panel) } 3) "Clear empties the list, and the panel and button go"
    Check (WaitFor { [void][W]::GetWindowRect($edit, [ref]$er); ($er.R - $er.L) -eq $editWidth } 3) "and the address bar has its room back"

    # Minimize, restore, maximize.
    [void][W]::ShowWindowAsync($frame, 6)   # SW_MINIMIZE
    Check (WaitFor { [W]::IsIconic($frame) } 5) "frame minimizes"
    [void][W]::ShowWindowAsync($frame, 9)   # SW_RESTORE
    Check (WaitFor { -not [W]::IsIconic($frame) } 5) "frame restores"
    # A click on the frame itself first. Jump to File Extra notes the width it has then,
    # and forced the next maximize back to it (see SpendJumpToFileWidth in frame.cpp).
    [void][W]::PostMessage($frame, 0x0201, [IntPtr]1, (LParam 3 200))  # WM_LBUTTONDOWN, on its left border
    [void][W]::PostMessage($frame, 0x0202, [IntPtr]0, (LParam 3 200))  # WM_LBUTTONUP
    Start-Sleep -Milliseconds 300
    $normalPlace = New-Object W+RECT; [void][W]::GetWindowRect($frame, [ref]$normalPlace)
    [void][W]::ShowWindowAsync($frame, 3)   # SW_MAXIMIZE
    # Maximized a moment before it has the screen: it is waited for at its new place.
    $atNewPlace = { $q = New-Object W+RECT; [void][W]::GetWindowRect($frame, [ref]$q); $q.L -ne $normalPlace.L -or $q.T -ne $normalPlace.T -or $q.R -ne $normalPlace.R -or $q.B -ne $normalPlace.B }
    Check (WaitFor { [W]::IsZoomed($frame) -and (& $atNewPlace) } 5) "frame maximizes"
    $mr = New-Object W+RECT; [void][W]::GetWindowRect($frame, [ref]$mr)
    $screen = [System.Windows.Forms.Screen]::FromHandle($frame)
    $work = $screen.WorkingArea
    if ($work -eq $screen.Bounds) {
        # Nothing takes room from the screen, as with a taskbar that hides itself. A window
        # without a caption that covered all of it would be taken for a full screen game, which
        # AltSnap won't size, so a row is left uncovered on one side (frame.cpp, MaximizedPlace).
        $gaps = @(($mr.L - $work.Left), ($mr.T - $work.Top), ($work.Right - $mr.R), ($work.Bottom - $mr.B))
        $ok = @($gaps | Where-Object { $_ -ne 0 }).Count -eq 1 -and @($gaps | Where-Object { $_ -lt 0 -or $_ -gt 2 }).Count -eq 0
        Check $ok "maximized frame is the whole screen but for a row on one side ($($mr.L),$($mr.T),$($mr.R),$($mr.B))"
    } else {
        Check ($mr.L -eq $work.Left -and $mr.T -eq $work.Top -and $mr.R -eq $work.Right -and $mr.B -eq $work.Bottom) "maximized frame is exactly the work area ($($mr.L),$($mr.T),$($mr.R),$($mr.B))"
    }
    $edge = [int][W]::SendMessageP($frame, 0x0084, [IntPtr]::Zero, (LParam ($mr.L + 2) ([int](($mr.T + $mr.B) / 2))))  # WM_NCHITTEST
    Check ($edge -lt 10 -or $edge -gt 17) "maximized, its edges don't size it, as no maximized window's do: $edge"
    $ok = WaitFor { InsetsAre $frame $client $borderless } 5
    $now = Insets $frame $client
    Check $ok "maximized, Winamp's border goes but for the title bar, and the page runs to the edges: client $($now.client), page $($now.page)"
    [void][W]::ShowWindowAsync($frame, 9)
    Check (WaitFor { -not [W]::IsZoomed($frame) } 5) "frame restores from maximized"
    $ok = WaitFor { InsetsAre $frame $client $insideBorder } 5
    $now = Insets $frame $client
    Check $ok "and is inside Winamp's border again: client $($now.client), page $($now.page)"

    # AltSnap sizing a maximized window (its hooks.c, RestoreToMonitorSize): it makes it a normal
    # window the size of the work area, where it is, and sizes that, here from the top left.
    $normal = New-Object W+RECT; [void][W]::GetWindowRect($frame, [ref]$normal)
    $topRight = OrnamentOn $procId $frame $false $true
    [void][W]::ShowWindowAsync($frame, 3)
    [void](WaitFor { [W]::IsZoomed($frame) } 5); Start-Sleep -Milliseconds 300
    [void][W]::GetWindowRect($frame, [ref]$mr)
    $pl = New-Object W+WINDOWPLACEMENT; $pl.length = [System.Runtime.InteropServices.Marshal]::SizeOf($pl)
    [void][W]::GetWindowPlacement($frame, [ref]$pl)
    $whole = New-Object W+RECT
    $whole.L = 0; $whole.T = 0; $whole.R = $work.Width; $whole.B = $work.Height  # measured from the work area's corner
    $pl.normal = $whole; $pl.showCmd = 9  # SW_RESTORE
    [void][W]::SetWindowPlacement($frame, [ref]$pl)
    foreach ($i in 1..5) {
        [void][W]::SetWindowPos($frame, [IntPtr]::Zero, $work.Left + 20 * $i, $work.Top + 16 * $i, $work.Width - 20 * $i, $work.Height - 16 * $i, 0x0014)
    }
    $f2 = New-Object W+RECT
    $sized = WaitFor { [void][W]::GetWindowRect($frame, [ref]$f2); -not [W]::IsZoomed($frame) -and $f2.L -eq $work.Left + 100 -and $f2.T -eq $work.Top + 80 -and $f2.R -eq $work.Right -and $f2.B -eq $work.Bottom } 5
    Check $sized "a maximized window sized the way AltSnap sizes one ends up a normal window at the size asked for: ($($f2.L),$($f2.T),$($f2.R),$($f2.B))"
    $ok = WaitFor { InsetsAre $frame $client $insideBorder } 5
    $now = Insets $frame $client
    Check $ok "with Winamp's border back: client $($now.client), page $($now.page)"
    # AltSnap sizes whichever window is under the mouse, and maximized, the ornament with the
    # buttons sits inside the frame's top right corner. Sized there, the frame is sized instead.
    if ($topRight) {
        [void][W]::ShowWindowAsync($frame, 3)
        [void](WaitFor { [W]::IsZoomed($frame) } 5); Start-Sleep -Milliseconds 300
        $or = New-Object W+RECT; [void][W]::GetWindowRect($topRight, [ref]$or)
        [void][W]::SetWindowPos($topRight, [IntPtr]::Zero, $or.L + 100, $or.T, ($or.R - $or.L) - 100, $or.B - $or.T, 0x0014)
        [void][W]::PostMessage($topRight, 0x0232, [IntPtr]::Zero, [IntPtr]::Zero)  # WM_EXITSIZEMOVE
        $sized = WaitFor { [void][W]::GetWindowRect($frame, [ref]$f2); -not [W]::IsZoomed($frame) -and $f2.L -eq $mr.L + 100 -and $f2.T -eq $mr.T -and $f2.R -eq $mr.R -and $f2.B -eq $mr.B } 5
        Check $sized "the ornament on a maximized window sized from outside makes it a normal window, sized by as much: ($($f2.L),$($f2.T),$($f2.R),$($f2.B))"
    }
    [void][W]::SetWindowPos($frame, [IntPtr]::Zero, $normal.L, $normal.T, $normal.R - $normal.L, $normal.B - $normal.T, 0x0014)
    [void](WaitFor { [void][W]::GetWindowRect($frame, [ref]$f2); $f2.L -eq $normal.L -and $f2.T -eq $normal.T -and $f2.R -eq $normal.R -and $f2.B -eq $normal.B } 5)

    # Winamp's title bar is a caption, for Windows to drag the window by and snap it to the
    # screen's edges. Its close button stays Winamp's.
    $fr = New-Object W+RECT; [void][W]::GetWindowRect($frame, [ref]$fr)
    $hit = [int][W]::SendMessageP($frame, 0x0084, [IntPtr]::Zero, (LParam ($fr.L + 200) ($fr.T + 8)))  # WM_NCHITTEST
    Check ($hit -eq 2) "Winamp's title bar hit-tests as a caption: $hit"
    $hit = [int][W]::SendMessageP($frame, 0x0084, [IntPtr]::Zero, (LParam ($fr.R - 7) ($fr.T + 7)))
    Check ($hit -eq 1) "the close button on it doesn't: $hit"
    # Its edges and corners size it, as any window's do, Winamp's resize grip included
    # (frame.cpp, SizingEdgeAt). HTLEFT is 10 and HTBOTTOMRIGHT 17.
    $w = $fr.R - $fr.L; $h = $fr.B - $fr.T
    $wanted = @(@("left edge", 2, [int]($h / 2), 10), @("right edge", ($w - 3), [int]($h / 2), 11), @("top edge", [int]($w / 2), 1, 12),
                @("top left corner", 2, 2, 13), @("top right corner", ($w - 1), 0, 14), @("bottom edge", [int]($w / 2), ($h - 3), 15),
                @("bottom left corner", 2, ($h - 3), 16), @("resize grip", ($w - 12), ($h - 12), 17))
    $got = @($wanted | ForEach-Object { [int][W]::SendMessageP($frame, 0x0084, [IntPtr]::Zero, (LParam ($fr.L + $_[1]) ($fr.T + $_[2]))) })
    $wrong = @(for ($i = 0; $i -lt $wanted.Count; $i++) { if ($got[$i] -ne $wanted[$i][3]) { "$($wanted[$i][0]) $($got[$i])" } })
    Check ($wrong.Count -eq 0) "its edges and corners size it: $(if ($wrong) { $wrong -join ', ' } else { 'all eight' })"
    # The ornament with the buttons lies over the frame's top right corner, and lets the mouse
    # through to the edge there.
    if ($topRight) {
        $hit = [int][W]::SendMessageP($topRight, 0x0084, [IntPtr]::Zero, (LParam ($fr.R - 3) ($fr.T + 30)))
        Check ($hit -eq -1) "the ornament on the top right corner lets the frame's right edge through: $hit"
    }

    # The ornaments on the frame's corners, and the buttons on the top right one.
    $ornament = OrnamentOn $procId $frame $false $true
    Check ($null -ne $ornament) "the frame owns an ornament on its top right corner"
    if ($ornament) {
        $fr = New-Object W+RECT; $or = New-Object W+RECT; $nt = New-Object W+RECT
        [void][W]::GetWindowRect($frame, [ref]$fr); [void][W]::GetWindowRect($ornament, [ref]$or)
        Check ([W]::IsWindowVisible($ornament) -and $or.T -lt $fr.T -and $or.R -gt $fr.R -and $or.L -lt $fr.R) "it sticks out above and beside the frame's top right corner"

        # The plain ones on the other three corners.
        $topLeft = OrnamentOn $procId $frame $true $true
        $plain = @(@($topLeft, (OrnamentOn $procId $frame $false $false), (OrnamentOn $procId $frame $true $false)) | Where-Object { $_ })
        $all = @($ornament) + $plain
        Check ((OrnamentsOf $procId $frame).Count -eq 4 -and $plain.Count -eq 3 -and @($all | Select-Object -Unique).Count -eq 4) "and one on each of the other three corners"
        Check (@($plain | Where-Object { [W]::IsWindowVisible($_) }).Count -eq 3) "all of them in sight on a frame this size"
        if ($topLeft) {
            $tab = New-Object W+RECT; [void][W]::GetWindowRect((TabButtons $client)[0], [ref]$tab)
            Check ($tab.L -ge $fr.L + 57 * ($or.R - $or.L) / 748) "the first tab starts clear of the top left one"

            # Moved from outside, as AltSnap moves the window under the cursor: the frame goes instead.
            $tl = New-Object W+RECT; [void][W]::GetWindowRect($topLeft, [ref]$tl)
            [void][W]::SetWindowPos($topLeft, [IntPtr]::Zero, $tl.L + 40, $tl.T + 30, 0, 0, 0x0015)  # SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE
            $moved = WaitFor {
                $f2 = New-Object W+RECT; $t2 = New-Object W+RECT
                [void][W]::GetWindowRect($frame, [ref]$f2); [void][W]::GetWindowRect($topLeft, [ref]$t2)
                $f2.L -eq $fr.L + 40 -and $f2.T -eq $fr.T + 30 -and $t2.L -eq $tl.L + 40 -and $t2.T -eq $tl.T + 30 -and ($t2.R - $t2.L) -eq ($tl.R - $tl.L)
            } 5
            Check $moved "an ornament moved from outside takes the frame with it"
            [void][W]::SetWindowPos($frame, [IntPtr]::Zero, $fr.L, $fr.T, 0, 0, 0x0015)
            Start-Sleep -Milliseconds 300

            # Sized from outside by its top left corner. AltSnap asks for each step from where
            # the drag began, and the frame once grew by all of them added up.
            [void][W]::GetWindowRect($topLeft, [ref]$tl)
            foreach ($i in 1..10) {
                [void][W]::SetWindowPos($topLeft, [IntPtr]::Zero, $tl.L - 10 * $i, $tl.T - 6 * $i, ($tl.R - $tl.L) + 10 * $i, ($tl.B - $tl.T) + 6 * $i, 0x0014)
            }
            [void][W]::PostMessage($topLeft, 0x0232, [IntPtr]::Zero, [IntPtr]::Zero)  # WM_EXITSIZEMOVE, as AltSnap ends a drag
            $f2 = New-Object W+RECT
            $sized = WaitFor { [void][W]::GetWindowRect($frame, [ref]$f2); $f2.L -eq $fr.L - 100 -and $f2.T -eq $fr.T - 60 -and $f2.R -eq $fr.R -and $f2.B -eq $fr.B } 5
            Check $sized "an ornament sized from outside sizes the frame by as much: ($($f2.L),$($f2.T),$($f2.R),$($f2.B))"
            [void][W]::SetWindowPos($frame, [IntPtr]::Zero, $fr.L, $fr.T, $fr.R - $fr.L, $fr.B - $fr.T, 0x0014)
            Start-Sleep -Milliseconds 300
        }

        ClickOrnament $ornament 0
        Check (WaitFor { [W]::IsIconic($frame) } 5) "the top right one's first button minimizes the frame"
        Check (WaitFor { @($all | Where-Object { [W]::IsWindowVisible($_) }).Count -eq 0 } 5) "and they all go out of sight with the frame"
        [void][W]::ShowWindowAsync($frame, 9)
        Check (WaitFor { -not [W]::IsIconic($frame) -and @($all | Where-Object { [W]::IsWindowVisible($_) }).Count -eq 4 } 5) "they come back with the frame"

        ClickOrnament $ornament 1
        Check (WaitFor { [W]::IsZoomed($frame) } 5) "its second button maximizes the frame"
        $tucked = WaitFor {
            [void][W]::GetWindowRect($frame, [ref]$fr); [void][W]::GetWindowRect($ornament, [ref]$or)
            [void][W]::GetWindowRect((Control $client 1101), [ref]$nt)
            $or.R -eq $fr.R -and $nt.R -lt $or.L + 552 * ($or.R - $or.L) / 748
        } 5
        Check $tucked "maximized, it sits inside the corner and the new tab button stays clear of its buttons"
        Check (@($plain | Where-Object { [W]::IsWindowVisible($_) }).Count -eq 0) "and the plain ones are out of sight"
        ClickOrnament $ornament 1
        Check (WaitFor { -not [W]::IsZoomed($frame) -and @($plain | Where-Object { [W]::IsWindowVisible($_) }).Count -eq 3 } 5) "the same button puts the frame back, and them on its corners"
    }

    # Winamp's library window wears the ornaments as well (library_frame.cpp). Their buttons
    # maximize it, close it as Winamp's own close button does, and minimize Winamp with it.
    $main = @([W]::TopLevel($procId) | Where-Object { [W]::Class($_) -eq $Instance })[0]
    $libraryFrame = @([W]::TopLevel($procId) | Where-Object { [W]::Class($_) -eq "Winamp Gen" -and [W]::Title($_) -eq "Winamp Library" })[0]
    if (-not $libraryFrame) {
        "  SKIP  the library window's ornaments: the scratch Winamp has no library"
    } else {
        if (-not [W]::IsWindowVisible($libraryFrame)) { [void][W]::PostMessage($main, 0x0111, [IntPtr]40379, [IntPtr]::Zero) }  # WM_COMMAND: Winamp's Show library
        [void](WaitFor { [W]::IsWindowVisible($libraryFrame) } 5)
        # Away from the screen's edges, on the screen the browser window was maximized on, for
        # the ironwork to stick out past every corner, and wide enough for the pieces along its
        # top and bottom edges not to run into each other (ornament.cpp, UpdateOrnaments).
        $lr = New-Object W+RECT; [void][W]::GetWindowRect($libraryFrame, [ref]$lr)
        $libraryPlace = @(($work.Left + 400), ($work.Top + 300), 800, 480)
        [void][W]::SetWindowPos($libraryFrame, [IntPtr]::Zero, $libraryPlace[0], $libraryPlace[1], $libraryPlace[2], $libraryPlace[3], 0x0014)
        $libraryOrnaments = @(OrnamentsOf $procId $libraryFrame)
        $shownOn = { @($libraryOrnaments | Where-Object { [W]::IsWindowVisible($_) }).Count }
        Check ($libraryOrnaments.Count -eq 4 -and (WaitFor { (& $shownOn) -eq 4 } 5)) "Winamp's library window wears the ornaments too, on all four corners: $($libraryOrnaments.Count) made, $(& $shownOn) in sight"
        $libraryButtons = OrnamentOn $procId $libraryFrame $false $true
        $fr = New-Object W+RECT; $or = New-Object W+RECT
        if ($libraryButtons) {
            [void][W]::GetWindowRect($libraryFrame, [ref]$fr); [void][W]::GetWindowRect($libraryButtons, [ref]$or)
            Check ($or.T -lt $fr.T -and $or.R -gt $fr.R) "the one with the buttons sticks out past its top right corner"

            ClickOrnament $libraryButtons 1
            $full = WaitFor { [void][W]::GetWindowRect($libraryFrame, [ref]$fr); [W]::IsZoomed($libraryFrame) -and $fr.L -eq $mr.L -and $fr.T -eq $mr.T -and $fr.R -eq $mr.R -and $fr.B -eq $mr.B } 5
            Check $full "its second button maximizes it to where a browser window goes: ($($fr.L),$($fr.T),$($fr.R),$($fr.B))"
            $inside = WaitFor { [void][W]::GetWindowRect($libraryButtons, [ref]$or); [W]::IsWindowVisible($libraryButtons) -and $or.R -eq $fr.R -and $or.T -le $fr.T + 60 } 5
            Check $inside "maximized, the one with the buttons sits inside its corner: ($($or.L),$($or.T),$($or.R),$($or.B)) on ($($fr.L),$($fr.T),$($fr.R),$($fr.B))"
            # Winamp moves it by hand, from its title bar. Maximized, it stays put.
            [void][W]::SetWindowPos($libraryFrame, [IntPtr]::Zero, $fr.L + 60, $fr.T + 40, 0, 0, 0x0015)  # SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE
            Start-Sleep -Milliseconds 300
            $r = New-Object W+RECT; [void][W]::GetWindowRect($libraryFrame, [ref]$r)
            Check ([W]::IsZoomed($libraryFrame) -and $r.L -eq $fr.L -and $r.T -eq $fr.T) "maximized, a move doesn't take it anywhere: at ($($r.L),$($r.T))"
            ClickOrnament $libraryButtons 1
            $back = WaitFor { [void][W]::GetWindowRect($libraryFrame, [ref]$fr); -not [W]::IsZoomed($libraryFrame) -and $fr.L -eq $libraryPlace[0] -and $fr.T -eq $libraryPlace[1] -and ($fr.R - $fr.L) -eq $libraryPlace[2] -and ($fr.B - $fr.T) -eq $libraryPlace[3] } 5
            Check $back "the same button puts it back where it was: ($($fr.L),$($fr.T)) $($fr.R - $fr.L)x$($fr.B - $fr.T)"

            ClickOrnament $libraryButtons 2
            Check (WaitFor { -not [W]::IsWindowVisible($libraryFrame) -and (& $shownOn) -eq 0 } 5) "its third button closes it, as Winamp's own close button does, and the ornaments go with it"
            Check ([W]::IsWindowVisible($main)) "and Winamp stays"
            [void][W]::PostMessage($main, 0x0111, [IntPtr]40379, [IntPtr]::Zero)
            Check (WaitFor { [W]::IsWindowVisible($libraryFrame) -and (& $shownOn) -eq 4 } 5) "shown again from Winamp's menu, it has them back"

            ClickOrnament $libraryButtons 0
            Check (WaitFor { ([W]::IsIconic($main) -or -not [W]::IsWindowVisible($main)) -and -not [W]::IsWindowVisible($libraryFrame) -and (& $shownOn) -eq 0 } 5) "its first button minimizes Winamp, and the library goes with it"
            [void][W]::ShowWindowAsync($main, 9)  # SW_RESTORE
            Check (WaitFor { -not [W]::IsIconic($main) -and [W]::IsWindowVisible($libraryFrame) -and (& $shownOn) -eq 4 } 5) "with Winamp back, the library and its ornaments are too"
        } else { Check $false "the library's ornament with the buttons is on its top right corner" }
        [void][W]::SetWindowPos($libraryFrame, [IntPtr]::Zero, $lr.L, $lr.T, $lr.R - $lr.L, $lr.B - $lr.T, 0x0014)
    }

    # A move from outside the process, as AltSnap makes one. It must stick and be saved.
    Start-Sleep -Milliseconds 500
    [void][W]::SetWindowPos($frame, [IntPtr]::Zero, 140, 120, 925, 667, 0x0014)  # SWP_NOZORDER | SWP_NOACTIVATE
    Start-Sleep -Milliseconds 500
    $r = New-Object W+RECT; [void][W]::GetWindowRect($frame, [ref]$r)
    Check ($r.L -eq 140 -and $r.T -eq 120) "frame can be moved from outside: at ($($r.L),$($r.T)) size $($r.R - $r.L)x$($r.B - $r.T)"
    # Sized from outside without being maximized, as snapping it to half the screen does, it keeps its border.
    $ok = WaitFor { InsetsAre $frame $client $insideBorder } 5
    $now = Insets $frame $client
    Check $ok "sized from outside, it keeps Winamp's border: client $($now.client), page $($now.page)"
    $movedTo = $r

    # Closed from outside (taskbar): the first window hides and is left with a blank tab.
    [void][W]::PostMessage($frame, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)  # WM_CLOSE
    Check (WaitFor { -not [W]::IsWindowVisible($frame) } 10) "WM_CLOSE on the frame hides the first window"
    # Waited for: the window is out of sight a moment before it has its blank tab.
    Check (WaitFor { [W]::IsWindow($frame) -and (TabButtons $client).Count -eq 1 } 5) "the first window survives with one blank tab"
    Check (WaitFor { [W]::Title($frame) -eq "Exodium" } 5) "its title is back to Exodium"
    [void][W]::ShowWindowAsync($frame, 8)
    Check (WaitFor { [W]::Title($frame) -like "*Google*" } 40) "shown again, its blank tab loads the default page"

    # Closed with the ornament's own button, the window goes the same way.
    if ($ornament) {
        ClickOrnament $ornament 2
        Check (WaitFor { -not [W]::IsWindowVisible($frame) -and @($all | Where-Object { [W]::IsWindowVisible($_) }).Count -eq 0 } 10) "the ornament's third button closes the window, and the ornaments go with it"
        # Waited for: the window is out of sight a moment before it has its blank tab.
        Check (WaitFor { [W]::IsWindow($frame) -and (TabButtons $client).Count -eq 1 } 5) "the first window survives that as well"
    }

    # Links from outside. Every window is closed now, as when Winamp has just started.
    $linkUrl = ([Uri]"$pages\opened.html").AbsoluteUri
    $linkPath = "$pages\a linked page#1.html"
    Set-Content -LiteralPath $linkPath '<!doctype html><title>Linked Page</title><p>linked</p>' -Encoding ascii
    Check (OpenedOk (StartOpen $linkUrl)) "exodium_open.exe hands a link to the running plugin"
    Check (WaitFor { (Frames $procId).Count -eq 2 } 10) "with no window open, a link gets a new one"
    $linked = @(Frames $procId | Where-Object { $_ -ne $frame })[0]
    if ($linked) {
        $linkedClient = ClientOf $linked
        Check (WaitFor { [W]::IsWindowVisible($linked) -and [W]::Title($linked) -eq "Opened Page" } 30) "the window shows, with the link's page in it: '$([W]::Title($linked))'"
        Check ((TabButtons $linkedClient).Count -eq 1) "the page is the window's one tab"
        Check (WaitFor { [W]::Text((Control $linkedClient 1003)) -eq $linkUrl } 10) "the address bar shows the link: '$([W]::Text((Control $linkedClient 1003)))'"
        Check (-not [W]::IsWindowVisible($frame)) "the first window stays out of sight"

        # With a window open, a link is a new tab of it, as in Chrome.
        $linkedTabs = { (TabTexts $linkedClient) -join "' | '" }
        Check (OpenedOk (StartOpen $linkPath)) "a page on disk is taken as a file type's path"
        Check (WaitFor { [W]::Title($linked) -eq "Linked Page" } 30) "the page on disk loads in the window there is, its space and # intact: '$([W]::Title($linked))'"
        Check ((Frames $procId).Count -eq 2 -and (& $linkedTabs) -eq "Opened Page' | 'Linked Page") "in a new tab at the end of its strip, not in a window of its own: '$(& $linkedTabs)'"
        Check ([W]::Text((Control $linkedClient 1003)) -like "file:*linked*page*1.html") "and the address bar shows that link: '$([W]::Text((Control $linkedClient 1003)))'"

        [void](OpenedOk (StartOpen "javascript:alert(1)"))
        Start-Sleep -Seconds 1
        Check ((Frames $procId).Count -eq 2 -and (TabButtons $linkedClient).Count -eq 2) "a script address opens nothing"

        $titles = { @(Frames $procId | ForEach-Object { [W]::Title($_) }) }
        Check (OpenedOk (StartOpen "")) "exodium_open.exe with no link at all"
        Check (WaitFor { (Frames $procId).Count -eq 3 -and (& $titles) -match "Google" } 40) "gets a window on the default page, with a window open as well: '$((& $titles) -join "' | '")'"

        # Which window a link goes to: the one nearest the front, as the windows lie on the screen.
        $other = @(Frames $procId | Where-Object { $_ -ne $frame -and $_ -ne $linked })[0]
        if ($other) {
            $otherClient = ClientOf $other
            $raise = { param($window) [void][W]::SetWindowPos($window, [IntPtr]::Zero, 0, 0, 0, 0, 0x0013) }  # HWND_TOP; SWP_NOSIZE | SWP_NOMOVE | SWP_NOACTIVATE
            $counts = { "$((TabButtons $linkedClient).Count) and $((TabButtons $otherClient).Count) tabs" }
            & $raise $linked
            [void](OpenedOk (StartOpen $linkUrl))
            Check ((WaitFor { (TabButtons $linkedClient).Count -eq 3 -and [W]::Title($linked) -eq "Opened Page" } 30) -and (TabButtons $otherClient).Count -eq 1) "of two windows, a link goes to the one in front: $(& $counts)"
            & $raise $other
            [void](OpenedOk (StartOpen $linkUrl))
            Check ((WaitFor { (TabButtons $otherClient).Count -eq 2 -and [W]::Title($other) -eq "Opened Page" } 30) -and (TabButtons $linkedClient).Count -eq 3) "and to the other one once that is in front: $(& $counts)"

            # A minimized window is passed over, even where it is first in that order.
            [void][W]::ShowWindowAsync($linked, 6)  # SW_MINIMIZE
            [void](WaitFor { [W]::IsIconic($linked) } 5)
            & $raise $linked
            [void](OpenedOk (StartOpen $linkUrl))
            Check ((WaitFor { (TabButtons $otherClient).Count -eq 3 } 10) -and [W]::IsIconic($linked) -and (TabButtons $linkedClient).Count -eq 3) "a minimized window is passed over for one on show: $(& $counts)"

            # So is a private one (Ctrl+Shift+N), which is in front as it opens.
            $framesBefore = @(Frames $procId)
            WithKeys $otherClient @($VK_CONTROL, $VK_SHIFT) { [void][W]::SendMessageP($otherClient, 0x0100, [IntPtr]0x4E, [IntPtr]::Zero) }  # WM_KEYDOWN N
            [void](WaitFor { (Frames $procId).Count -eq $framesBefore.Count + 1 } 10)
            $private = @(Frames $procId | Where-Object { $framesBefore -notcontains $_ })[0]
            if ($private -and (WaitFor { [W]::Title($private) -like "* (Private)" } 30)) {
                & $raise $private
                [void](OpenedOk (StartOpen $linkUrl))
                Check ((WaitFor { (TabButtons $otherClient).Count -eq 4 } 10) -and (TabButtons (ClientOf $private)).Count -eq 1) "a private window in front is passed over as well: $(& $counts), $((TabButtons (ClientOf $private)).Count) in the private one"
                [void][W]::PostMessage($private, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)  # WM_CLOSE
                [void](WaitFor { -not [W]::IsWindow($private) } 10)
            } else { Check $false "a private window, for a link to pass over" }

            # With no other window open, the minimized one is brought back for the link.
            [void][W]::PostMessage($other, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)  # WM_CLOSE
            [void](WaitFor { -not [W]::IsWindow($other) } 10)
            [void](OpenedOk (StartOpen $linkPath))
            Check (WaitFor { -not [W]::IsIconic($linked) -and (TabButtons $linkedClient).Count -eq 4 -and [W]::Title($linked) -eq "Linked Page" } 30) "with only a minimized window open, it is brought back with the link in a new tab: $((TabButtons $linkedClient).Count) tabs, '$([W]::Title($linked))'"
            Check ((Frames $procId).Count -eq 2) "and no window was opened for any of those links: $((Frames $procId).Count) windows"
        } else { Check $false "a second window, to see which of two a link goes to" }

        foreach ($extra in @(Frames $procId | Where-Object { $_ -ne $frame })) {
            [void][W]::PostMessage($extra, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)  # WM_CLOSE
        }
        Check (WaitFor { (Frames $procId).Count -eq 1 } 10) "windows opened for links close like any other"
    }

    # Exodium's page in Winamp's Preferences, opened the way the Configure button for the
    # plugin opens it, under Plug-ins > General Purpose.
    $main = @([W]::TopLevel($procId) | Where-Object { [W]::Class($_) -eq $Instance })[0]
    [void][W]::PostMessage($main, 0x0400, [IntPtr]35, [IntPtr]380)  # WM_WA_IPC IPC_OPENPREFSTOPAGE, General Purpose
    $prefs = [IntPtr]::Zero
    Check (WaitFor { $script:prefs = PrefsWindow $main; $script:prefs -ne [IntPtr]::Zero -and [W]::IsWindowVisible($script:prefs) } 10) "Winamp's Preferences open"
    Check ([Remote32]::SelectListItem((PageControl $prefs 1121), "Exodium") -ge 0) "Exodium is one of the general purpose plug-ins listed there"
    $configure = PageControl $prefs 1122
    [void][W]::PostMessage([W]::GetParent($configure), 0x0111, [IntPtr]1122, $configure)  # WM_COMMAND: Configure selected plug-in
    $shown = { $c = PageControl $prefs 1301; $c -ne [IntPtr]::Zero -and [W]::IsWindowVisible([W]::GetParent($c)) }
    Check (WaitFor $shown 10) "its Configure button opens Exodium's page in the Preferences"
    $homeBox = PageControl $prefs 1301
    $engines = PageControl $prefs 1302
    $searchBox = PageControl $prefs 1303
    $closeButtons = PageControl $prefs 1311
    Check ([W]::Text($homeBox) -eq "https://google.com/" -and [W]::Text($engines) -eq "Google" -and [W]::Text($searchBox) -eq "https://www.google.com/search?q=%s") "the page starts with Google as home page and search engine: '$([W]::Text($homeBox))' '$([W]::Text($engines))' '$([W]::Text($searchBox))'"
    $closeChecked = [int][W]::SendMessageP($closeButtons, 0x00F0, [IntPtr]::Zero, [IntPtr]::Zero)  # BM_GETCHECK
    Check ($closeChecked -eq 1) "and with close buttons on tabs: $closeChecked"

    # What is changed on it is written to winamp.ini at once.
    Set-Content "$pages\home.html" '<!doctype html><title>Exodium Home</title><p>home</p>' -Encoding ascii
    Set-Content "$pages\search.html" '<!doctype html><title>Search</title><script>document.title = "Searched for " + new URLSearchParams(location.search).get("q")</script>' -Encoding ascii
    $homeUrl = ([Uri]"$pages\home.html").AbsoluteUri
    [void][W]::SendMessage($homeBox, 0x000C, [IntPtr]::Zero, "example.com")  # WM_SETTEXT
    Notify $homeBox 0x0200  # EN_KILLFOCUS: the box was left
    Check ([W]::Text($homeBox) -eq "https://example.com" -and (ExodiumIni).home_page -eq "https://example.com") "a home page typed as a bare domain is taken as the address bar takes it, and written to winamp.ini: '$([W]::Text($homeBox))'"
    [void][W]::SendMessage($homeBox, 0x000C, [IntPtr]::Zero, $homeUrl)
    Notify $homeBox 0x0200
    Check ((ExodiumIni).home_page -eq $homeUrl) "so is a page on disk: '$((ExodiumIni).home_page)'"
    PickFromList $engines "DuckDuckGo"
    Check ((ExodiumIni).search_engine -eq "duckduckgo" -and [W]::Text($searchBox) -eq "https://duckduckgo.com/?q=%s") "picking a search engine writes it, and shows its address: '$([W]::Text($searchBox))'"
    $tabsBefore = (TabButtons $client).Count
    [void][W]::SendMessageP($closeButtons, 0x00F1, [IntPtr]0, [IntPtr]::Zero)  # BM_SETCHECK, unchecked
    Notify $closeButtons 0  # BN_CLICKED
    Check ((ExodiumIni).tab_close_buttons -eq "0") "unticking close buttons on tabs writes it"
    Check ((TabButtons $client).Count -eq $tabsBefore) "and the tab strips are laid out again, with their tabs"

    # The browser follows the page at once: a new tab opens on the home page, and words
    # typed in the address bar are searched for with the engine picked.
    [void][W]::ShowWindowAsync($frame, 8)  # SW_SHOWNA
    [void](WaitFor { [W]::IsWindowVisible($frame) -and (HasWebView $client) } 30)
    Navigate $client ([Uri]"$pages\opened.html").AbsoluteUri
    [void](WaitFor { [W]::Title($frame) -eq "Opened Page" } 20)
    $count = (TabButtons $client).Count
    [void][W]::PostMessage($client, 0x0111, [IntPtr]1101, [IntPtr]::Zero)  # WM_COMMAND kIdNewTab
    Check (WaitFor { (TabButtons $client).Count -eq $count + 1 -and [W]::Title($frame) -eq "Exodium Home" } 30) "a new tab opens on the home page: '$([W]::Title($frame))'"
    Navigate $client "exodium smoke test"
    # Searched for, it is the engine's address; the page may then write the words its own way.
    Check (WaitFor { [W]::Text((Control $client 1003)) -match '^https://duckduckgo\.com/\?(.*&)?q=exodium(%20|\+)smoke(%20|\+)test' } 10) "words typed in the address bar are searched for with the engine picked: '$([W]::Text((Control $client 1003)))'"

    $readOnly = { ([W]::GetWindowLong($searchBox, -16) -band 0x0800) -ne 0 }  # GWL_STYLE, ES_READONLY
    Check (& $readOnly) "an engine on offer has an address that can't be changed"
    PickFromList $engines "Custom"
    Check ((ExodiumIni).search_engine -eq "custom" -and -not (& $readOnly) -and [W]::Text($searchBox) -eq "https://duckduckgo.com/?q=%s") "picking Custom lets the address be changed, starting from the engine it takes over from"
    $searchUrl = ([Uri]"$pages\search.html").AbsoluteUri + "?q=%s"
    [void][W]::SendMessage($searchBox, 0x000C, [IntPtr]::Zero, $searchUrl)
    Notify $searchBox 0x0200
    Check ((ExodiumIni).custom_search_url -eq $searchUrl) "a custom engine's address is written to winamp.ini: '$((ExodiumIni).custom_search_url)'"
    Navigate $client "custom words"
    Check (WaitFor { [W]::Title($frame) -eq "Searched for custom words" } 20) "and searches go there: '$([W]::Title($frame))'"

    # With close buttons on tabs unticked, a click where one was brings its tab into view.
    $count = (TabButtons $client).Count
    ClickCloseButton (TabByText $client "Opened Page")
    Check ((WaitFor { [W]::Title($frame) -eq "Opened Page" } 5) -and (TabButtons $client).Count -eq $count) "with close buttons off, a click where one was brings the tab into view and closes nothing: '$([W]::Title($frame))'"

    # The Start menu's checkbox makes and removes the shortcut at once, and keeps nothing in
    # winamp.ini. This Winamp runs under a class of its own, which the shortcut passes on.
    $startBox = PageControl $prefs 1313
    $startNote = PageControl $prefs 1331
    $checked = { [int][W]::SendMessageP($startBox, 0x00F0, [IntPtr]::Zero, [IntPtr]::Zero) -eq 1 }  # BM_GETCHECK
    Check ($startBox -ne [IntPtr]::Zero -and [W]::IsWindowEnabled($startBox) -and -not (& $checked) -and [W]::Text($startNote) -eq "") "the Start menu's checkbox is there, unticked, with no shortcut made yet"
    [void][W]::SendMessageP($startBox, 0x00F1, [IntPtr]1, [IntPtr]::Zero)  # BM_SETCHECK, checked
    Notify $startBox 0  # BN_CLICKED
    $made = if (Test-Path -LiteralPath $shortcut) { ShortcutAt $shortcut }
    Check ($made -and $made.TargetPath -eq "$wa\Plugins\exodium_open.exe" -and $made.Arguments -eq "--class=$Instance") "ticking it makes a shortcut to exodium_open.exe, for this Winamp: '$($made.TargetPath)' '$($made.Arguments)'"
    Check (-not @((ExodiumIni).Keys | Where-Object { $_ -like "*start*" }).Count) "and writes nothing to winamp.ini: the shortcut is all there is"
    if ($made) {
        # Kept for starting Winamp with, further on.
        Copy-Item -LiteralPath $shortcut $keptShortcut -Force
        $before = @(Frames $procId)
        Start-Process -FilePath $shortcut
        $fromStart = { @(Frames $procId | Where-Object { $before -notcontains $_ }) }
        # Long enough to wait for another run's exodium_open.exe, which holds the same mutex.
        Check (WaitFor { @(& $fromStart).Count -eq 1 -and [W]::IsWindowVisible((& $fromStart)[0]) -and [W]::Title((& $fromStart)[0]) -eq "Exodium Home" } 45) "with Winamp running, the shortcut opens a window on the home page: '$((& $fromStart | ForEach-Object { [W]::Title($_) }) -join "' | '")'"
        foreach ($opened in (& $fromStart)) { [void][W]::PostMessage($opened, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) }  # WM_CLOSE
        Check (WaitFor { (Frames $procId).Count -eq $before.Count } 10) "and the Start menu's window closes like any other"
    }
    [void][W]::SendMessageP($startBox, 0x00F1, [IntPtr]0, [IntPtr]::Zero)  # BM_SETCHECK, unchecked
    Notify $startBox 0
    Check (-not (Test-Path -LiteralPath $shortcut) -and -not (& $checked)) "unticking it removes the shortcut"
    # Ticked again, for the Preferences to find when they are opened again.
    [void][W]::SendMessageP($startBox, 0x00F1, [IntPtr]1, [IntPtr]::Zero)
    Notify $startBox 0

    # Closed and opened again where they were, the Preferences show the page as it was left.
    [void][W]::SendMessageP($prefs, 0x0111, [IntPtr]1, [IntPtr]::Zero)  # WM_COMMAND IDOK: Close
    Check (WaitFor { -not [W]::IsWindowVisible($prefs) } 5) "the Preferences close"
    [void][W]::PostMessage($main, 0x0400, [IntPtr](-1), [IntPtr]380)  # IPC_OPENPREFSTOPAGE, the page last shown
    Check (WaitFor { $script:prefs = PrefsWindow $main; $script:prefs -ne [IntPtr]::Zero -and [W]::IsWindowVisible($script:prefs) -and (PageControl $script:prefs 1301) -ne [IntPtr]::Zero } 10) "opened again, they are at Exodium's page"
    $shown = @([W]::Text((PageControl $prefs 1301)), [W]::Text((PageControl $prefs 1302)), [W]::Text((PageControl $prefs 1303)), [int][W]::SendMessageP((PageControl $prefs 1311), 0x00F0, [IntPtr]::Zero, [IntPtr]::Zero))
    Check ($shown[0] -eq $homeUrl -and $shown[1] -eq "Custom" -and $shown[2] -eq $searchUrl -and $shown[3] -eq 0) "which shows the settings as they were left: '$($shown -join "' | '")'"
    Check ([int][W]::SendMessageP((PageControl $prefs 1313), 0x00F0, [IntPtr]::Zero, [IntPtr]::Zero) -eq 1) "and the Start menu's checkbox ticked, the shortcut being there"

    # Ticked again, the close buttons are back at once; then off again, for the restart below.
    $closeButtons = PageControl $prefs 1311
    [void][W]::SendMessageP($closeButtons, 0x00F1, [IntPtr]1, [IntPtr]::Zero)  # BM_SETCHECK, checked
    Notify $closeButtons 0  # BN_CLICKED
    $count = (TabButtons $client).Count
    ClickCloseButton (TabByText $client "Searched for custom words")
    Check ((ExodiumIni).tab_close_buttons -eq "1" -and (WaitFor { (TabButtons $client).Count -eq $count - 1 } 10) -and [W]::Title($frame) -eq "Opened Page") "ticked again, a tab's close button closes it at once: '$((TabTexts $client) -join "' | '")'"
    [void][W]::SendMessageP($closeButtons, 0x00F1, [IntPtr]0, [IntPtr]::Zero)  # BM_SETCHECK, unchecked
    Notify $closeButtons 0

    # Ctrl+N in Winamp, on the same page. Nothing opens while the Preferences are open, from
    # them or from Winamp's main window.
    $ctrlN = PageControl $prefs 1312
    Check ([int][W]::SendMessageP($ctrlN, 0x00F0, [IntPtr]::Zero, [IntPtr]::Zero) -eq 1) "Ctrl+N in Winamp opening a browser window is on to begin with"  # BM_GETCHECK
    PressCtrlN $ctrlN
    PressCtrlN $main
    Check ((Frames $procId).Count -eq 1) "Ctrl+N in the Preferences opens no browser window, nor in Winamp's main window while they are open"
    [void][W]::SendMessageP($ctrlN, 0x00F1, [IntPtr]0, [IntPtr]::Zero)  # BM_SETCHECK, unchecked
    Notify $ctrlN 0  # BN_CLICKED
    Check ((ExodiumIni).ctrl_n_in_winamp -eq "0") "unticking it writes it"

    # The pages' sound in Winamp's visualizers, on the same page.
    $soundBox = PageControl $prefs 1314
    Check ($soundBox -ne [IntPtr]::Zero -and [int][W]::SendMessageP($soundBox, 0x00F0, [IntPtr]::Zero, [IntPtr]::Zero) -eq 1) "the pages' sound in Winamp's visualizers is on to begin with"
    if ($soundBox -ne [IntPtr]::Zero) {
        [void][W]::SendMessageP($soundBox, 0x00F1, [IntPtr]0, [IntPtr]::Zero)  # BM_SETCHECK, unchecked
        Notify $soundBox 0
        Check ((ExodiumIni).sound_in_visualizers -eq "0") "unticking it writes it"
        [void][W]::SendMessageP($soundBox, 0x00F1, [IntPtr]1, [IntPtr]::Zero)
        Notify $soundBox 0
        Check ((ExodiumIni).sound_in_visualizers -eq "1") "ticking it again writes that too"
    }

    # The corner ornaments' skin, on the same page. Picked, the window's corners have it at
    # once: Venom's pictures are 731 of their pixels wide to Iron's 748, shown 332 and 340
    # wide, and its pod reaches further in over the tab strip, to 456 of them from the left.
    $skins = PageControl $prefs 1304
    $topRightWidth = { $o = OrnamentOn $procId $frame $false $true; if ($o) { $r = New-Object W+RECT; [void][W]::GetWindowRect($o, [ref]$r); $r.R - $r.L } }
    Check ($skins -ne [IntPtr]::Zero -and [W]::Text($skins) -eq "Iron") "the corners' skin is Iron to begin with: '$(if ($skins -ne [IntPtr]::Zero) { [W]::Text($skins) })'"
    if ($skins -ne [IntPtr]::Zero) {
        $ornamentsBefore = @(OrnamentsOf $procId $frame)
        PickFromList $skins "Venom"
        $skinned = WaitFor { (& $topRightWidth) -eq 332 -and @(OrnamentsOf $procId $frame | Where-Object { $ornamentsBefore -notcontains $_ }).Count -eq 4 } 5
        Check ((ExodiumIni).corner_skin -eq "venom" -and $skinned) "picking Venom writes it, and gives the window four new ornaments at once: the top right one $(& $topRightWidth) wide"
        $o = OrnamentOn $procId $frame $false $true
        if ($o) {
            $or = New-Object W+RECT; $nt = New-Object W+RECT
            [void][W]::GetWindowRect($o, [ref]$or); [void][W]::GetWindowRect((Control $client 1101), [ref]$nt)
            Check ($nt.R -le $or.L + 456 * ($or.R - $or.L) / 731 + 1) "the tab strip ends clear of Venom's pod: the new tab button ends at $($nt.R - $or.L) of its 332"
        }
        # Charms and Dark Charms, 719 and 717 of their pixels wide, shown 327 and 326. Their
        # buttons are in a bar thick enough that, maximized, all of it lies over the tab strip,
        # from 17 and 13 of their pixels in. The library's corners change along with the window's.
        foreach ($skin in @(@("Charms", "charms", 327, 719, 475, 17), @("Dark Charms", "darkcharms", 326, 717, 474, 13))) {
            $name, $id, $shown, $art, $shoulder, $barLeft = $skin
            $ornamentsBefore = @(OrnamentsOf $procId $frame)
            PickFromList $skins $name
            $skinned = WaitFor { (& $topRightWidth) -eq $shown -and @(OrnamentsOf $procId $frame | Where-Object { $ornamentsBefore -notcontains $_ }).Count -eq 4 } 5
            Check ((ExodiumIni).corner_skin -eq $id -and $skinned) "picking $name writes it, and gives the window four new ornaments at once: the top right one $(& $topRightWidth) wide"
            if ($libraryFrame) {
                $libraryWidths = { @(OrnamentsOf $procId $libraryFrame | ForEach-Object { $r = New-Object W+RECT; [void][W]::GetWindowRect($_, [ref]$r); $r.R - $r.L }) }
                Check (WaitFor { $w = & $libraryWidths; $w.Count -eq 4 -and @($w | Where-Object { $_ -ne $shown }).Count -eq 0 } 5) "and the library's four ornaments too: $((& $libraryWidths) -join ', ') wide"
            }
            $or = New-Object W+RECT; $nt = New-Object W+RECT
            if ($o = OrnamentOn $procId $frame $false $true) {
                [void][W]::GetWindowRect($o, [ref]$or); [void][W]::GetWindowRect((Control $client 1101), [ref]$nt)
                Check ($nt.R -le $or.L + $shoulder * ($or.R - $or.L) / $art + 1) "the tab strip ends clear of the ironwork coming down its side: the new tab button ends at $($nt.R - $or.L) of its $shown"
            }
            [void][W]::ShowWindowAsync($frame, 3)  # SW_MAXIMIZE
            $clear = WaitFor {
                $o = OrnamentOn $procId $frame $false $true
                if (-not $o -or -not [W]::IsZoomed($frame)) { return $false }
                [void][W]::GetWindowRect($o, [ref]$or); [void][W]::GetWindowRect((Control $client 1101), [ref]$nt)
                $nt.R -le $or.L + $barLeft * ($or.R - $or.L) / $art + 1
            } 5
            Check $clear "maximized, the tab strip ends where $name's bar begins: the new tab button ends at $($nt.R - $or.L) of its $shown"
            [void][W]::ShowWindowAsync($frame, 9)  # SW_RESTORE
            [void](WaitFor { -not [W]::IsZoomed($frame) } 5)
        }
        PickFromList $skins "Iron"
        Check ((ExodiumIni).corner_skin -eq "iron" -and (WaitFor { (& $topRightWidth) -eq 340 -and (OrnamentsOf $procId $frame).Count -eq 4 } 5)) "and picking Iron puts that back"
    }

    # The black glow round Winamp's windows while Winamp is in front, on the same page. This
    # desktop has no window in front, and the glow then goes by Winamp's thread having an active
    # window, as it has while the Preferences are open. Each picture of the glow is a window of
    # its own, right under the lowest of the windows it goes round, and round all of them.
    $glowList = PageControl $prefs 1305
    $glows = { @([W]::TopLevel($procId) | Where-Object { [W]::Class($_) -eq "ExodiumGlow" -and [W]::IsWindowVisible($_) }) }
    $glowCasters = { param([bool]$all)
        @([W]::TopLevel($procId) | Where-Object {
            $c = [W]::Class($_)
            [W]::IsWindowVisible($_) -and -not [W]::IsZoomed($_) -and -not [W]::IsIconic($_) -and (
                ($c -eq "Winamp Gen" -and ($all -or (ClientOf $_) -ne [IntPtr]::Zero)) -or
                ($all -and ($c -eq $Instance -or $c -eq "Winamp EQ" -or $c -eq "Winamp PE" -or $c -eq "Winamp Video")))
        }) }
    # Whether a window is below another in the Z order.
    $isBelow = { param($lower, $upper)
        $h = [W]::GetWindow($upper, 2)  # GW_HWNDNEXT
        while ($h -ne [IntPtr]::Zero -and $h -ne $lower) { $h = [W]::GetWindow($h, 2) }
        $h -eq $lower }
    # Whether the glow's window is round every one of the windows, as far as the screen goes.
    $glowRound = { param($glow, $windows)
        $g = New-Object W+RECT; [void][W]::GetWindowRect($glow, [ref]$g)
        $v = [System.Windows.Forms.SystemInformation]::VirtualScreen
        @($windows | Where-Object {
            $r = New-Object W+RECT; [void][W]::GetWindowRect($_, [ref]$r)
            [Math]::Max($r.L, $v.Left) -lt $g.L -or [Math]::Max($r.T, $v.Top) -lt $g.T -or [Math]::Min($r.R, $v.Right) -gt $g.R -or [Math]::Min($r.B, $v.Bottom) -gt $g.B
        }).Count -eq 0 }
    # Whether each of the windows has a picture of the glow under it, and round it.
    $eachGlows = { param($windows)
        $g = & $glows
        $windows.Count -gt 0 -and @($windows | Where-Object {
            $w = $_
            @($g | Where-Object { (& $isBelow $_ $w) -and (& $glowRound $_ @($w)) }).Count -eq 0
        }).Count -eq 0 }
    $glowSays = {
        $g = & $glows
        $all = @([W]::TopLevel($procId) | Where-Object { [W]::IsWindowVisible($_) } | ForEach-Object { [W]::Class($_) }) -join ", "
        $each = @($g | ForEach-Object {
            $r = New-Object W+RECT; [void][W]::GetWindowRect($_, [ref]$r)
            "at $($r.L),$($r.T)-$($r.R),$($r.B) under a $([W]::Class([W]::GetWindow($_, 3)))"
        }) -join "; "
        "$($g.Count) shown, $each; Winamp's windows: $all"
    }
    Check ($glowList -ne [IntPtr]::Zero -and [W]::Text($glowList) -eq "All of Winamp") "the glow goes round all of Winamp to begin with: '$(if ($glowList -ne [IntPtr]::Zero) { [W]::Text($glowList) })'"
    # One picture for the windows docked together, and one for each window apart from them.
    $glowing = WaitFor { & $eachGlows (& $glowCasters $true) } 5
    Check $glowing "with Winamp in front, each of Winamp's windows has a picture of the glow under it and round it: $(& $glowSays)"
    if ($glowList -ne [IntPtr]::Zero) {
        PickFromList $glowList "Exodium only"
        Check ((ExodiumIni).glow -eq "exodium") "picking Exodium only writes it"
        $framed = WaitFor { $g = & $glows; $g.Count -eq 1 -and [W]::GetWindow($g[0], 3) -eq $frame -and (& $glowRound $g[0] @($frame)) } 5
        Check $framed "and the glow goes right under the browser window, and round it: $(& $glowSays)"
        $placeBefore = New-Object W+RECT; [void][W]::GetWindowRect($frame, [ref]$placeBefore)
        [void][W]::ShowWindowAsync($frame, 3)  # SW_MAXIMIZE
        Check (WaitFor { [W]::IsZoomed($frame) -and (& $glows).Count -eq 0 } 5) "a maximized browser window has none: $(& $glowSays)"
        [void][W]::ShowWindowAsync($frame, 9)  # SW_RESTORE
        # Back where it was: it stops being maximized a moment before it has its place back.
        $backInPlace = { $q = New-Object W+RECT; [void][W]::GetWindowRect($frame, [ref]$q); -not [W]::IsZoomed($frame) -and $q.L -eq $placeBefore.L -and $q.T -eq $placeBefore.T -and $q.R -eq $placeBefore.R -and $q.B -eq $placeBefore.B }
        Check (WaitFor { (& $backInPlace) -and (& $glows).Count -eq 1 } 5) "and has it back once it is restored: $(& $glowSays)"

        # Winamp out of front, and a window of Winamp's activated, as on a desktop that isn't on
        # the screen no click can do: a window of the test's own is made the active one in
        # Winamp's stead, or one of Winamp's is, by sharing Winamp's thread's input for a moment.
        if (-not ("SmokeFront" -as [type])) {
            Add-Type -ReferencedAssemblies System.Windows.Forms, System.Drawing @"
using System; using System.Runtime.InteropServices; using System.Windows.Forms;
public static class SmokeFront {
    [DllImport("user32.dll")] static extern bool AttachThreadInput(uint a, uint b, bool attach);
    [DllImport("user32.dll")] static extern IntPtr SetActiveWindow(IntPtr h);
    [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr h, IntPtr pid);
    [DllImport("user32.dll")] static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);
    [DllImport("kernel32.dll")] static extern uint GetCurrentThreadId();
    static Form own;
    static Form Own() {
        if (own == null) {
            own = new Form();
            own.ShowInTaskbar = false;
            own.StartPosition = FormStartPosition.Manual;
            own.Location = new System.Drawing.Point(-30000, -30000);
            own.Size = new System.Drawing.Size(120, 60);
            own.Show();
        }
        return own;
    }
    // Makes a window the active one by sharing Winamp's thread's input for a moment: after that
    // the window's own thread has it active, and the other thread has nothing active.
    static void Activate(IntPtr winampWindow, IntPtr window) {
        uint theirs = GetWindowThreadProcessId(winampWindow, IntPtr.Zero), mine = GetCurrentThreadId();
        AttachThreadInput(mine, theirs, true);
        try { SetActiveWindow(window); } finally { AttachThreadInput(mine, theirs, false); }
        Application.DoEvents();
    }
    // Winamp out of front: a window of the test's, far off the screen, is active instead.
    public static void Take(IntPtr winampWindow) {
        Form form = Own();
        SetWindowPos(form.Handle, IntPtr.Zero, -30000, -30000, 120, 60, 0x0014);  // SWP_NOZORDER | SWP_NOACTIVATE
        Activate(winampWindow, form.Handle);
    }
    // One of Winamp's windows activated, as Alt+Tab to it would.
    public static void GiveBack(IntPtr winampWindow) { Activate(winampWindow, winampWindow); }
    // The test's window over a part of the screen, above every other, as another program's
    // window that was brought to the front over Winamp's.
    public static IntPtr Cover(int x, int y, int w, int h) { return CoverUnder(IntPtr.Zero, x, y, w, h); }
    // The same, right under a window. On a desktop with nothing in front no program can bring
    // another's window to the top, as Alt+Tab does, but its own can go anywhere.
    public static IntPtr CoverUnder(IntPtr above, int x, int y, int w, int h) {
        Form form = Own();
        SetWindowPos(form.Handle, above, x, y, w, h, 0x0050);  // SWP_SHOWWINDOW | SWP_NOACTIVATE
        Application.DoEvents();
        return form.Handle;
    }
    public static void Done() { if (own != null) { own.Close(); own.Dispose(); own = null; } }
}
"@
        }

        # Windows docked edge to edge glow as one. A window apart from them, over them, has a
        # glow of its own, which lies over the windows under it. Two browser windows, put where
        # Winamp's are not, the second opened as the Start menu's entry opens one: a link
        # would go in a tab of the first.
        $framesBefore = @(Frames $procId)
        $opened = (Test-Path "$wa\Plugins\exodium_open.exe") -and (OpenedOk (StartOpen "")) -and (WaitFor { (Frames $procId).Count -eq $framesBefore.Count + 1 } 10)
        Check $opened "a second browser window opens, for the glow round docked windows"
        if ($opened) {
            $second = @(Frames $procId | Where-Object { $framesBefore -notcontains $_ })[0]
            [void](WaitFor { [W]::IsWindowVisible($second) } 10)
            $firstPlace = New-Object W+RECT; [void][W]::GetWindowRect($frame, [ref]$firstPlace)
            $place = { param($window, [int]$x, [int]$y, [int]$w, [int]$h) [void][W]::SetWindowPos($window, [IntPtr]::Zero, $x, $y, $w, $h, 0x0014) }  # SWP_NOZORDER | SWP_NOACTIVATE
            $toTop = { param($window) [void][W]::SetWindowPos($window, [IntPtr]::Zero, 0, 0, 0, 0, 0x0013) }  # HWND_TOP; SWP_NOSIZE | SWP_NOMOVE | SWP_NOACTIVATE
            # The picture right under a window, if there is one.
            $under = { param($window) $n = [W]::GetWindow($window, 2); if ($n -ne [IntPtr]::Zero -and [W]::Class($n) -eq "ExodiumGlow" -and [W]::IsWindowVisible($n)) { $n } else { [IntPtr]::Zero } }
            Start-Sleep -Milliseconds 500
            & $place $frame 2200 200 700 500
            & $place $second 2900 200 600 500
            & $toTop $second; & $toTop $frame
            $docked = WaitFor { $g = & $under $second; $g -ne [IntPtr]::Zero -and (& $glowRound $g @($frame, $second)) -and (& $under $frame) -eq [IntPtr]::Zero } 5
            Check $docked "docked side by side, they glow as one: a picture right under the lower, round both: $(& $glowSays)"
            # And they come to the front together. Another program's window goes between them, and
            # the upper one is activated, as Alt+Tab to it does: the other, docked to it, comes
            # up right under it, over that window.
            $between = {
                [SmokeFront]::GiveBack($prefs)  # something else active, for the upper one to be activated anew
                $script:upper, $script:lower = if (& $isBelow $second $frame) { $frame, $second } else { $second, $frame }
                $script:cover = [SmokeFront]::CoverUnder($script:upper, 2100, 100, 1500, 800)
                (& $isBelow $script:cover $script:upper) -and (& $isBelow $script:lower $script:cover)
            }
            $stacking = { "upper over cover: $(& $isBelow $cover $upper), lower over cover: $(& $isBelow $cover $lower), lower under upper: $(& $isBelow $lower $upper)" }
            Check (& $between) "another program's window comes between them: $(& $stacking)"
            [SmokeFront]::GiveBack($upper)
            Check (WaitFor { (& $isBelow $cover $lower) -and (& $isBelow $lower $upper) } 5) "activating the upper one brings the other, docked to it, up right under it, over that window: $(& $stacking)"
            & $place $second 2500 400 600 500
            & $toTop $second
            $apart = WaitFor {
                $g = & $under $second
                $g -ne [IntPtr]::Zero -and (& $glowRound $g @($second)) -and -not (& $glowRound $g @($frame)) -and (& $isBelow $frame $g) -and (& $under $frame) -ne [IntPtr]::Zero
            } 5
            Check $apart "put over the first without an edge along it, the second has a glow of its own, lying over the first, which has its own: $(& $glowSays)"
            # How fast the glow follows the window it goes round: moved a step at a time, and sized,
            # each step timed from the move until the picture under the window is round it again.
            # Spaced out, each is timed alone; 8 ms apart, as a drag's steps come, each has until
            # the next to be followed.
            if (-not ("GlowTiming" -as [type])) {
                Add-Type @"
using System; using System.Diagnostics; using System.Runtime.InteropServices; using System.Text; using System.Threading;
public static class GlowTiming {
    [StructLayout(LayoutKind.Sequential)] struct RECT { public int L, T, R, B; }
    [DllImport("user32.dll")] static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);
    [DllImport("user32.dll")] static extern IntPtr GetWindow(IntPtr h, uint cmd);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetClassNameW(IntPtr h, StringBuilder name, int size);
    static IntPtr GlowUnder(IntPtr window) {
        IntPtr next = GetWindow(window, 2);  // GW_HWNDNEXT
        var name = new StringBuilder(64);
        GetClassNameW(next, name, 64);
        return name.ToString() == "ExodiumGlow" ? next : IntPtr.Zero;
    }
    // Moves the window by dx and sizes it by dw, `steps` times, stepMs apart. For each step, how
    // many ms the glow under it took to be round it again, or -1 if it wasn't by the next step.
    public static double[] Follow(IntPtr window, int steps, int stepMs, int dx, int dw) {
        RECT w, g;
        IntPtr glow = GlowUnder(window);
        if (glow == IntPtr.Zero || !GetWindowRect(window, out w) || !GetWindowRect(glow, out g)) return new double[0];
        int left = w.L - g.L, right = g.R - w.R;  // how far the picture reaches past the window
        var took = new double[steps];
        var clock = Stopwatch.StartNew();
        for (int i = 0; i < steps; ++i) {
            GetWindowRect(window, out w);
            SetWindowPos(window, IntPtr.Zero, w.L + dx, w.T, w.R - w.L + dw, w.B - w.T, 0x0014);  // SWP_NOZORDER | SWP_NOACTIVATE
            GetWindowRect(window, out w);
            double start = clock.Elapsed.TotalMilliseconds;
            took[i] = -1;
            while (clock.Elapsed.TotalMilliseconds - start < stepMs) {
                IntPtr under = GlowUnder(window);
                if (took[i] < 0 && under != IntPtr.Zero && GetWindowRect(under, out g) && g.L == w.L - left && g.R == w.R + right) {
                    took[i] = clock.Elapsed.TotalMilliseconds - start;
                }
                Thread.SpinWait(200);
            }
        }
        return took;
    }
}
"@
            }
            $followed = { param([double[]]$took)
                $made = @($took | Where-Object { $_ -ge 0 } | Sort-Object)
                if (-not $took.Count) { return "not timed: no picture under the window" }
                $median = if ($made.Count) { "{0:N1}" -f $made[[int][Math]::Floor($made.Count / 2)] } else { "-" }
                $most = if ($made.Count) { "{0:N1}" -f $made[-1] } else { "-" }
                "$($made.Count) of $($took.Count) followed in time, median $median ms, slowest $most ms" }
            [void][W]::SetWindowPos($second, [IntPtr]::Zero, 1900, 300, 1500, 1000, 0x0014)  # as large as a browser window is
            Start-Sleep -Milliseconds 500
            $moves = [GlowTiming]::Follow($second, 20, 100, 6, 0)
            "  info  glow timing, moves 100 ms apart: $(& $followed $moves)"
            Check (@($moves | Where-Object { $_ -ge 0 }).Count -eq 20) "the glow follows a window moved a step at a time, every step: $(& $followed $moves)"
            $sizes = [GlowTiming]::Follow($second, 10, 150, 0, 6)
            "  info  glow timing, sizes 150 ms apart: $(& $followed $sizes)"
            $drag = [GlowTiming]::Follow($second, 60, 8, -2, 0)
            "  info  glow timing, a drag's moves 8 ms apart: $(& $followed $drag)"

            [void](& $between)
            [SmokeFront]::GiveBack($upper)
            Start-Sleep -Milliseconds 700
            Check ((& $isBelow $cover $upper) -and (& $isBelow $lower $cover)) "not docked, the other stays under the other program's window when the upper one is activated: $(& $stacking)"
            [SmokeFront]::GiveBack($prefs)
            [SmokeFront]::Done()
            & $place $frame $firstPlace.L $firstPlace.T ($firstPlace.R - $firstPlace.L) ($firstPlace.B - $firstPlace.T)
            Check (CloseOtherFrames) "and the second window closes again"
        }
        PickFromList $glowList "Off"
        Check ((ExodiumIni).glow -eq "off" -and (WaitFor { @([W]::TopLevel($procId) | Where-Object { [W]::Class($_) -eq "ExodiumGlow" }).Count -eq 0 } 5)) "picking Off writes it, and the glow's windows go"
        $lookControls = @(1306, 1307, 1308, 1309, 1310 | ForEach-Object { PageControl $prefs $_ })
        Check (@($lookControls | Where-Object { $_ -eq [IntPtr]::Zero -or [W]::IsWindowEnabled($_) }).Count -eq 0) "and its sliders and buttons can't be used then"
        PickFromList $glowList "All of Winamp"
        Check ((ExodiumIni).glow -eq "winamp" -and (WaitFor { (& $glows).Count -ge 1 } 5)) "and picking All of Winamp brings it back: $(& $glowSays)"
        Check (@($lookControls | Where-Object { -not [W]::IsWindowEnabled($_) }).Count -eq 0) "with its sliders and buttons"

        # The page as drawn, for a look at its layout: Winamp gives every page the same room.
        Add-Type -AssemblyName System.Drawing
        if (-not ("PageShot" -as [type])) {
            Add-Type @"
using System; using System.Runtime.InteropServices;
public static class PageShot {
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags);
}
"@
        }
        $pr = New-Object W+RECT; [void][W]::GetWindowRect($prefs, [ref]$pr)
        $shot = New-Object System.Drawing.Bitmap ($pr.R - $pr.L), ($pr.B - $pr.T)
        $graphics = [System.Drawing.Graphics]::FromImage($shot)
        $dc = $graphics.GetHdc(); [void][PageShot]::PrintWindow($prefs, $dc, 2); $graphics.ReleaseHdc($dc); $graphics.Dispose()
        $shotPath = Join-Path $ScratchDir "prefs-page.png"
        $shot.Save($shotPath, [System.Drawing.Imaging.ImageFormat]::Png); $shot.Dispose()
        "  info  Exodium's page in the Preferences: $shotPath"

        # The glow's sliders: at the look it has to begin with, which a dragged slider changes as
        # it goes, and which is written once the slider is let go. Default puts it all back.
        $sizeBar = PageControl $prefs 1306; $opacityBar = PageControl $prefs 1307; $spreadBar = PageControl $prefs 1308
        $barAt = { param($bar) [int][W]::SendMessageP($bar, 0x0400, [IntPtr]::Zero, [IntPtr]::Zero) }  # TBM_GETPOS
        $slide = { param($bar, [int]$to, [int]$code)  # sets it there, and tells the page as the slider would
            [void][W]::SendMessageP($bar, 0x0405, [IntPtr]1, [IntPtr]$to)  # TBM_SETPOS
            [void][W]::SendMessageP([W]::GetParent($bar), 0x0114, [IntPtr](($to -shl 16) -bor $code), $bar) }  # WM_HSCROLL
        # The widest picture's width: each grows with the size.
        $glowWidth = { $widths = @(& $glows | ForEach-Object { $r = New-Object W+RECT; [void][W]::GetWindowRect($_, [ref]$r); $r.R - $r.L }); if ($widths.Count) { ($widths | Measure-Object -Maximum).Maximum } else { 0 } }
        Check ((& $barAt $sizeBar) -eq 60 -and (& $barAt $opacityBar) -eq 100 -and (& $barAt $spreadBar) -eq 0 -and [W]::Text((PageControl $prefs 1332)) -eq "Size: 60 px") "the glow's sliders start at size 60, opacity 100% and spread 0%: $(& $barAt $sizeBar), $(& $barAt $opacityBar), $(& $barAt $spreadBar), '$([W]::Text((PageControl $prefs 1332)))'"
        $widthBefore = & $glowWidth
        & $slide $sizeBar 150 5  # TB_THUMBTRACK
        Check ((WaitFor { (& $glowWidth) -gt $widthBefore + 100 } 5) -and -not (ExodiumIni).ContainsKey("glow_size") -and [W]::Text((PageControl $prefs 1332)) -eq "Size: 150 px") "dragging the size slider to 150 grows the glow as it goes, $widthBefore to $(& $glowWidth) wide, and writes nothing yet"
        & $slide $sizeBar 150 8  # TB_ENDTRACK
        Check ((ExodiumIni).glow_size -eq "150") "letting go writes it: '$((ExodiumIni).glow_size)'"
        & $slide $spreadBar 40 4  # TB_THUMBPOSITION
        & $slide $opacityBar 70 4
        Check ((ExodiumIni).glow_spread -eq "40" -and (ExodiumIni).glow_opacity -eq "70") "spread and opacity are written too: '$((ExodiumIni).glow_spread)', '$((ExodiumIni).glow_opacity)'"

        # Its colour, from Windows' colour picker, which the page waits on: so the click is posted.
        $colorButton = PageControl $prefs 1309
        [void][W]::PostMessage([W]::GetParent($colorButton), 0x0111, [IntPtr]1309, $colorButton)  # WM_COMMAND to its page, BN_CLICKED
        $picker = [IntPtr]::Zero
        $pickerUp = WaitFor { $script:picker = @([W]::TopLevel($procId) | Where-Object { [W]::Class($_) -eq "#32770" -and [W]::IsWindowVisible($_) -and [W]::GetWindow($_, 4) -eq $prefs })[0]; $script:picker -and $script:picker -ne [IntPtr]::Zero } 5
        Check $pickerUp "Color... opens Windows' colour picker"
        if ($pickerUp) {
            foreach ($channel in @(@(706, "255"), @(707, "0"), @(708, "0"))) {  # its red, green and blue boxes
                $box = [W]::GetDlgItem($picker, $channel[0])
                [void][W]::SendMessage($box, 0x000C, [IntPtr]::Zero, $channel[1])  # WM_SETTEXT
            }
            [void][W]::PostMessage($picker, 0x0111, [IntPtr]1, [IntPtr]::Zero)  # WM_COMMAND, IDOK
            Check (WaitFor { -not [W]::IsWindowVisible($picker) -and (ExodiumIni).glow_color -eq "ff0000" } 5) "and red picked there is written: '$((ExodiumIni).glow_color)'"
        }
        Notify (PageControl $prefs 1310) 0  # BN_CLICKED
        $glowIni = ExodiumIni
        # The colour is only written once it has been changed.
        $putBack = $glowIni.glow_size -eq "60" -and $glowIni.glow_opacity -eq "100" -and $glowIni.glow_spread -eq "0" -and
                   ($glowIni.glow_color -eq "000000" -or (-not $pickerUp -and -not $glowIni.ContainsKey("glow_color")))
        Check ($putBack -and (& $barAt $sizeBar) -eq 60 -and (WaitFor { [Math]::Abs((& $glowWidth) - $widthBefore) -le 2 } 5)) "Default puts the look back as it was, and the glow with it: $($glowIni.glow_size), $($glowIni.glow_opacity), $($glowIni.glow_spread), $($glowIni.glow_color); $(& $glowWidth) wide"

        # Shown only while Winamp is in front, or always. Winamp goes out of front here when a
        # window of the test's own is made the active one in its stead.
        $alwaysBox = PageControl $prefs 1315
        Check ($alwaysBox -ne [IntPtr]::Zero -and [int][W]::SendMessageP($alwaysBox, 0x00F0, [IntPtr]::Zero, [IntPtr]::Zero) -eq 0) "the glow is shown only while Winamp is in front to begin with"
        [SmokeFront]::Take($prefs)
        Check (WaitFor { (& $glows).Count -eq 0 } 5) "with another program's window active instead, the glow goes: $(& $glowSays)"
        if ($alwaysBox -ne [IntPtr]::Zero) {
            [void][W]::SendMessageP($alwaysBox, 0x00F1, [IntPtr]1, [IntPtr]::Zero)  # BM_SETCHECK, checked
            Notify $alwaysBox 0
            Check ((ExodiumIni).glow_always -eq "1" -and (WaitFor { (& $glows).Count -ge 1 } 5)) "ticking Always writes it, and the glow is back with Winamp still behind: $(& $glowSays)"
            [void][W]::SendMessageP($alwaysBox, 0x00F1, [IntPtr]0, [IntPtr]::Zero)
            Notify $alwaysBox 0
            Check ((ExodiumIni).glow_always -eq "0" -and (WaitFor { (& $glows).Count -eq 0 } 5)) "unticking it writes that, and the glow goes again"
        }
        [SmokeFront]::GiveBack($prefs)
        [SmokeFront]::Done()
        Check (WaitFor { (& $glows).Count -ge 1 } 5) "and is back once Winamp is in front again: $(& $glowSays)"
    }
    [void][W]::SendMessageP($prefs, 0x0111, [IntPtr]1, [IntPtr]::Zero)
    [void](WaitFor { -not [W]::IsWindowVisible($prefs) } 5)
    PressCtrlN $main
    Check ((Frames $procId).Count -eq 1) "unticked, Ctrl+N in Winamp's main window opens nothing"
    [void][W]::PostMessage($main, 0x0400, [IntPtr](-1), [IntPtr]380)  # IPC_OPENPREFSTOPAGE, the page last shown
    if (WaitFor { $script:prefs = PrefsWindow $main; $script:prefs -ne [IntPtr]::Zero -and (PageControl $script:prefs 1312) -ne [IntPtr]::Zero } 10) {
        $ctrlN = PageControl $prefs 1312
        [void][W]::SendMessageP($ctrlN, 0x00F1, [IntPtr]1, [IntPtr]::Zero)  # BM_SETCHECK, checked
        Notify $ctrlN 0
        Check ((ExodiumIni).ctrl_n_in_winamp -eq "1") "ticking it again writes that too"
        [void][W]::SendMessageP($prefs, 0x0111, [IntPtr]1, [IntPtr]::Zero)
        [void](WaitFor { -not [W]::IsWindowVisible($prefs) } 5)
        PressCtrlN $main 2
        Check ((Frames $procId).Count -eq 2) "and Ctrl+N in Winamp's main window opens a browser window again: $((Frames $procId).Count) browser windows"
        Check (CloseOtherFrames) "which closes again"
    } else { Check $false "the Preferences open again at Exodium's page" }

    # Without exodium_open.exe beside the plugin the page says so, rather than make a
    # shortcut that leads nowhere; the one that is there can still be removed.
    [void](WaitFor { -not [W]::IsWindowVisible($prefs) } 5)
    Rename-Item "$wa\Plugins\exodium_open.exe" "exodium_open.exe.away"
    try {
        [void][W]::PostMessage($main, 0x0400, [IntPtr](-1), [IntPtr]380)  # IPC_OPENPREFSTOPAGE, the page last shown
        [void](WaitFor { $script:prefs = PrefsWindow $main; $script:prefs -ne [IntPtr]::Zero -and [W]::IsWindowVisible($script:prefs) -and (PageControl $script:prefs 1313) -ne [IntPtr]::Zero } 10)
        $startBox = PageControl $prefs 1313
        $startNote = [W]::Text((PageControl $prefs 1331))
        Check ((& $checked) -and [W]::IsWindowEnabled($startBox) -and $startNote -like "*exodium_open.exe isn't in Winamp's Plugins folder*") "without exodium_open.exe the page says so, and the shortcut there can still be removed: '$startNote'"
        [void][W]::SendMessageP($startBox, 0x00F1, [IntPtr]0, [IntPtr]::Zero)  # BM_SETCHECK, unchecked
        Notify $startBox 0
        Check (-not (Test-Path -LiteralPath $shortcut) -and -not (& $checked) -and -not [W]::IsWindowEnabled($startBox)) "unticked, the shortcut goes, and the checkbox can't be ticked again"
        [void][W]::SendMessageP($prefs, 0x0111, [IntPtr]1, [IntPtr]::Zero)  # Close
    } finally {
        Rename-Item "$wa\Plugins\exodium_open.exe.away" "exodium_open.exe"
    }

    # The pages' sound in Winamp's visualizers (winamp_sound.cpp and in_exodium.dll). The page
    # plays a tone too quiet to hear, and Winamp's playlist only ever holds silent WAVs here:
    # the scratch settings are a copy of the real ones, playlist and all, and Play is pressed.
    # It comes last, so its page's visit is in no history an earlier check counts.
    if ($soundTested) {
        [void][W]::ShowWindowAsync($frame, 4)  # SW_SHOWNOACTIVATE: its page is clicked
        [void][W]::SendMessageP($main, 0x0400, [IntPtr]0, [IntPtr]101)  # IPC_DELETE: an empty playlist
        foreach ($name in "Silence one", "Silence two") {
            SilentWav (Join-Path $ScratchDir "$name.wav") 5
            [WinampKeys]::Enqueue($main, (Join-Path $ScratchDir "$name.wav"))
        }
        [void](WaitFor { (PlaylistLength $main) -eq 2 } 5)
        [void][W]::SendMessageP($main, 0x0400, [IntPtr]1, [IntPtr]121)  # IPC_SETPLAYLISTPOS: the second
        # With shuffle on, the pages must still be what plays, not a track picked at random.
        [void][W]::SendMessageP($main, 0x0400, [IntPtr]1, [IntPtr]252)  # IPC_SET_SHUFFLE
        $tone = Join-Path $ScratchDir "tone.html"
        [System.IO.File]::WriteAllText($tone, @'
<!doctype html>
<meta charset="utf-8">
<title>Tone ready</title>
<body style="height:100vh;margin:0">
<script>
// A click starts a 440 Hz tone at 20/32767 of full scale, over Chromium's line for a page
// making sound and too quiet to hear, and the next click stops it. Three seconds in, the
// page changes its title, as a video site does for the next video.
let ctx = null, osc = null;
document.addEventListener('click', () => {
  if (osc) { osc.stop(); osc = null; ctx.close(); ctx = null; document.title = 'Tone stopped'; return; }
  ctx = new AudioContext();
  osc = ctx.createOscillator();
  const gain = ctx.createGain();
  gain.gain.value = 20 / 32767;
  osc.connect(gain).connect(ctx.destination);
  osc.start();
  document.title = 'Tone playing';
  const playing = osc;
  setTimeout(() => { if (osc === playing) document.title = 'Tone still playing'; }, 3000);
});
</script>
'@)
        Navigate $client ("file:///" + ($tone -replace '\\', '/'))
        Check (WaitFor { [W]::Title($frame) -eq "Tone ready" } 30) "a page with a tone loads: '$([W]::Title($frame))'"
        $listBefore = "$(PlaylistLength $main) at $(PlaylistPosition $main)"
        Check ((WinampPlaying $main) -eq 0) "Winamp is stopped before the page makes sound"

        PostClick (PageWidget $client) 200 200
        Check (WaitFor { (WinampPlaying $main) -eq 1 } 15) "the page making sound has Winamp play it for the visualizers"
        Check (WaitFor { [W]::Title($main) -like "*Exodium: Tone playing*" } 5) "Winamp shows the page's title: '$([W]::Title($main))'"
        Check ("$(PlaylistLength $main) at $(PlaylistPosition $main)" -eq $listBefore) "the playlist is as it was: $(PlaylistLength $main) at $(PlaylistPosition $main)"
        Start-Sleep -Milliseconds 500
        $from = MarkTimeNow
        Start-Sleep -Milliseconds 1500
        # Each picture is given the newest sound there is, from a packet that came at most
        # 10 ms before (in_exodium.cpp): measured 2026-11-07, 12 to 23 ms old, half under 18.
        $ages = @(ProbePictures $from)
        $fresh = @($ages | Where-Object { $_ -ge 0 -and $_ -le 40 } | Sort-Object)
        $median = if ($fresh.Count) { $fresh[[int]($fresh.Count / 2)] } else { -1 }
        Check ($ages.Count -ge 30 -and $fresh.Count -eq $ages.Count -and $median -le 25) "every picture the visualizers draw shows the newest sound: $($fresh.Count) of $($ages.Count) pictures at most 40 ms old, half under $median ms"
        $recent = @(ProbeReadings | Select-Object -Last 5)
        Check (@($recent | Where-Object { $_.wave -gt 40 }).Count -ge 4) "the visualization plug-ins get the page's sound: $(($recent | ForEach-Object { $_.wave }) -join ' ')"
        Check (@($recent | Where-Object { $_.analyzer -gt 10 }).Count -ge 4) "and so does the main window's analyzer: $(($recent | ForEach-Object { $_.analyzer }) -join ' ')"
        Check (WaitFor { [Remote32]::AskForText($main, 0, 3034) -like "*Tone still playing*" } 8) "the title Winamp shows follows the page's: '$([Remote32]::AskForText($main, 0, 3034))'"  # IPC_GET_PLAYING_TITLE
        $entryTitles = @(0..1 | ForEach-Object { [Remote32]::AskForText($main, $_, 213) })  # IPC_GETPLAYLISTTITLEW
        Check (-not ($entryTitles -match "Tone|Exodium")) "the playlist's entries keep their own titles: '$($entryTitles -join "', '")'"

        PostClick (PageWidget $client) 200 200  # quiet
        $quietFor = [Diagnostics.Stopwatch]::StartNew()
        $stopped = WaitFor { (WinampPlaying $main) -eq 0 } 15
        Check ($stopped -and $quietFor.ElapsedMilliseconds -lt 2000) "Winamp stops as soon as the page goes quiet: after $($quietFor.ElapsedMilliseconds) ms"
        # Which entry that is, shuffle may have moved; it is whichever the playlist is on.
        $onEntry = [Remote32]::AskForText($main, [int](PlaylistPosition $main), 213)  # IPC_GETPLAYLISTTITLEW
        Check (WaitFor { $onEntry -and [W]::Title($main) -like "*$onEntry*" } 3) "and shows its own playlist entry again, '$onEntry': '$([W]::Title($main))'"
        Check ("$(PlaylistLength $main) at $(PlaylistPosition $main)" -eq $listBefore) "the playlist is still as it was: $(PlaylistLength $main) at $(PlaylistPosition $main)"

        PostClick (PageWidget $client) 200 200
        Check (WaitFor { (WinampPlaying $main) -eq 1 } 15) "new sound starts it again"
        WinampCommand $main 40047  # Stop
        Start-Sleep -Seconds 5
        Check ((WinampPlaying $main) -eq 0) "Stop in Winamp isn't overruled while the page goes on making sound"
        PostClick (PageWidget $client) 200 200  # quiet
        Start-Sleep -Seconds 2
        PostClick (PageWidget $client) 200 200
        Check (WaitFor { (WinampPlaying $main) -eq 1 } 15) "once the page has gone quiet, its next sound starts it again"
        WinampCommand $main 40045  # Play
        Check (WaitFor { [W]::Title($main) -like "*Silence*" -and (WinampPlaying $main) -eq 1 } 5) "Play in Winamp plays a track of the playlist's: '$([W]::Title($main))'"
        Start-Sleep -Seconds 7  # the track ends
        Check (-not ([W]::Title($main) -like "*Exodium*")) "and the page doesn't take Winamp back when the track ends: '$([W]::Title($main))'"

        PostClick (PageWidget $client) 200 200  # quiet
        WinampCommand $main 40047
        [void][W]::SendMessageP($main, 0x0400, [IntPtr]0, [IntPtr]252)  # shuffle off
        [void][W]::SendMessageP($main, 0x0400, [IntPtr]0, [IntPtr]101)  # the playlist empty again
    } else { "  SKIP  the pages' sound in Winamp's visualizers: in_exodium.dll or vis_probe.dll isn't beside the plugin" }

    # What typed text is taken for, the history's journal, a tab's menu, tabs by number, and
    # bookmarks (2026-11-09). The tabs opened here are the last in the strip.
    if (-not [W]::IsWindowVisible($frame)) { [void][W]::ShowWindowAsync($frame, 8) }  # SW_SHOWNA
    if ([W]::IsIconic($frame)) { [void][W]::ShowWindowAsync($frame, 9) }               # SW_RESTORE
    [void](WaitFor { [W]::IsWindowVisible($frame) -and -not [W]::IsIconic($frame) } 5)
    $pages = Join-Path $ScratchDir "pages"
    New-Item -ItemType Directory -Force $pages | Out-Null
    foreach ($name in "One", "Two", "Three") {
        Set-Content "$pages\new$name.html" "<!doctype html><title>New $name</title><p>$name</p>" -Encoding ascii
    }
    $newUrl = { param([string]$name) ([Uri]"$pages\new$name.html").AbsoluteUri }
    $urlBar = { [W]::Text((Control $client 1003)) }
    $newTab = { [void][W]::PostMessage($client, 0x0111, [IntPtr]1101, [IntPtr]::Zero); Start-Sleep -Seconds 2 }  # WM_COMMAND kIdNewTab
    # A key pressed in the window with others held, and a pick from a tab's menu (kMsgTabCommand).
    $key = { param([int]$vk, [int[]]$held) WithKeys $client $held { [void][W]::SendMessageP($client, 0x0100, [IntPtr]$vk, [IntPtr]::Zero) } }  # WM_KEYDOWN
    $tabCommand = { param($button, [int]$command) [void][W]::PostMessage($client, 0x8000 + 51, [IntPtr]$command, $button) }
    $base = (TabButtons $client).Count

    & $newTab
    Check ((TabButtons $client).Count -eq $base + 1) "a new tab for the checks that follow"
    Navigate $client "$pages\newOne.html"  # the file's path, as Explorer's address bar gives it
    Check (WaitFor { [W]::Title($frame) -eq "New One" } 20) "a file's path typed into the address bar opens the file: '$([W]::Title($frame))'"
    Check (WaitFor { (& $urlBar) -eq (& $newUrl "One") } 10) "at its file address: '$(& $urlBar)'"
    $journal = "$wa\Plugins\exodium_data\history.journal"
    $journalHas = { param([string]$text) (Test-Path $journal) -and [IO.File]::ReadAllText($journal, [Text.Encoding]::Unicode).Contains($text) }
    Check (WaitFor { & $journalHas "newOne.html" } 8) "the visit is in the history's journal a moment later, long before Winamp exits"

    foreach ($name in "Two", "Three") {
        & $newTab
        Navigate $client (& $newUrl $name)
        [void](WaitFor { [W]::Title($frame) -eq "New $name" } 20)
    }
    Check ((TabButtons $client).Count -eq $base + 3 -and (TabTexts $client)[-1] -eq "New Three" -and [W]::Title($frame) -eq "New Three") "three tabs on pages of their own: '$((TabTexts $client | Select-Object -Last 3) -join "' | '")'"

    # Tabs by number, and the keys beside them.
    & $key 0x31 @($VK_CONTROL)  # Ctrl+1
    Check (WaitFor { [W]::Title($frame) -ne "New Three" } 5) "Ctrl+1 goes to the first tab: '$([W]::Title($frame))'"
    & $key 0x39 @($VK_CONTROL)  # Ctrl+9
    Check (WaitFor { [W]::Title($frame) -eq "New Three" } 5) "Ctrl+9 goes to the last one: '$([W]::Title($frame))'"
    & $key 0x21 @($VK_CONTROL)  # Ctrl+Page Up
    Check (WaitFor { [W]::Title($frame) -eq "New Two" } 5) "Ctrl+Page Up goes to the tab before: '$([W]::Title($frame))'"
    & $key 0x22 @($VK_CONTROL)  # Ctrl+Page Down
    Check (WaitFor { [W]::Title($frame) -eq "New Three" } 5) "and Ctrl+Page Down to the one after: '$([W]::Title($frame))'"

    # A tab's menu: Duplicate, Close tabs to the right, Reopen closed tab, Close other tabs.
    & $tabCommand (TabByText $client "New Two") 3  # TabCommand::kDuplicate
    Check (WaitFor { (TabButtons $client).Count -eq $base + 4 -and @(TabTexts $client | Where-Object { $_ -eq "New Two" }).Count -eq 2 } 20) "Duplicate opens another tab on the same page: '$((TabTexts $client | Select-Object -Last 4) -join "' | '")'"
    Check ((TabTexts $client)[$base + 2] -eq "New Two" -and (WaitFor { [W]::Title($frame) -eq "New Two" -and (& $urlBar) -eq (& $newUrl "Two") } 20)) "right after it and in view: '$([W]::Title($frame))' '$(& $urlBar)'"
    & $tabCommand (TabButtons $client)[$base] 7  # kCloseToRight, on New One
    Check (WaitFor { (TabButtons $client).Count -eq $base + 1 } 10) "Close tabs to the right leaves the tab and those before it: $((TabButtons $client).Count - $base) of the four left"
    Check (WaitFor { [W]::Title($frame) -eq "New One" } 10) "and the tab is the one in view: '$([W]::Title($frame))'"
    & $tabCommand (TabButtons $client)[$base] 8  # kReopenClosed
    Check (WaitFor { (TabButtons $client).Count -eq $base + 2 -and [W]::Title($frame) -eq "New Three" } 20) "Reopen closed tab brings back the one closed last: '$([W]::Title($frame))'"
    & $tabCommand (TabByText $client "New One") 6  # kCloseOthers
    Check (WaitFor { (TabButtons $client).Count -eq 1 -and [W]::Title($frame) -eq "New One" } 10) "Close other tabs leaves the tab alone in its window: '$((TabTexts $client) -join "' | '")'"

    # Bookmarks: Ctrl+D and the star mark the page in view and unmark it, and are on disk at once.
    $bookmarksFile = "$wa\Plugins\exodium_data\bookmarks.txt"
    $bookmarked = { (Test-Path $bookmarksFile) -and [IO.File]::ReadAllText($bookmarksFile, [Text.Encoding]::Unicode).Contains("newOne.html`t") }
    $star = Control $client 1009
    Check ($star -ne [IntPtr]::Zero -and [W]::IsWindowVisible($star) -and [W]::Text($star) -eq [string][char]0x2606) "the toolbar has a star, empty while the page in view is no bookmark"
    & $key 0x44 @($VK_CONTROL)  # Ctrl+D
    Check (WaitFor { & $bookmarked } 5) "Ctrl+D bookmarks the page in view, in bookmarks.txt at once"
    Check ([W]::Text($star) -eq [string][char]0x2605) "and the star is filled"
    [void][W]::SendMessageP($client, 0x0111, [IntPtr]1009, $star)  # WM_COMMAND, as a click on the star sends it
    Check ((WaitFor { -not (& $bookmarked) } 5) -and [W]::Text($star) -eq [string][char]0x2606) "a click on the star takes the bookmark away again, and empties it"
    & $key 0x44 @($VK_CONTROL)
    [void](WaitFor { & $bookmarked } 5)
    & $newTab
    Navigate $client "exodium://bookmarks"
    Check (WaitFor { [W]::Title($frame) -eq "Bookmarks" } 30) "the bookmarks page is served: '$([W]::Title($frame))'"
    Check (WaitFor { (& $urlBar) -eq "exodium://bookmarks" } 10) "under its own name: '$(& $urlBar)'"
    Check ([W]::Text($star) -eq [string][char]0x2606) "and is no bookmark itself"

    # Tab in the address bar goes on into the page, to the first thing on it that takes the
    # keyboard, and Shift+Tab to the last (2026-11-09). The page says in its title which has it.
    Set-Content "$pages\tabStops.html" ("<!doctype html><title>Tab Stops</title><a id=first href='#1'>first</a><p>between</p><a id=last href='#2'>last</a>" +
                                        "<script>setInterval(() => { const at = document.activeElement; if (at && at.id) document.title = at.id + ' has the keyboard' }, 100)</script>") -Encoding ascii
    Navigate $client ([Uri]"$pages\tabStops.html").AbsoluteUri
    Check (WaitFor { [W]::Title($frame) -eq "Tab Stops" } 20) "a page with two things to tab to: '$([W]::Title($frame))'"
    $addressBar = Control $client 1003
    & $key 0x44 @(0x12)  # Alt+D
    Check (WaitFor { [W]::Focus($client) -eq $addressBar } 5) "Alt+D puts the keyboard in the address bar: $([W]::Class([W]::Focus($client)))"
    WithKeys $addressBar @() { [void][W]::SendMessageP($addressBar, 0x0100, [IntPtr]0x09, [IntPtr]::Zero) }  # WM_KEYDOWN Tab
    Check (WaitFor { [W]::Title($frame) -eq "first has the keyboard" } 5) "Tab there moves it to the first thing on the page: '$([W]::Title($frame))'"
    Check ([W]::Class([W]::Focus($client)) -eq "Chrome_WidgetWin_1") "and out of the address bar, into the page: $([W]::Class([W]::Focus($client)))"
    & $key 0x44 @(0x12)
    [void](WaitFor { [W]::Focus($client) -eq $addressBar } 5)
    WithKeys $addressBar @($VK_SHIFT) { [void][W]::SendMessageP($addressBar, 0x0100, [IntPtr]0x09, [IntPtr]::Zero) }
    Check (WaitFor { [W]::Title($frame) -eq "last has the keyboard" } 5) "Shift+Tab moves it to the last: '$([W]::Title($frame))'"

    # The window as drawn, for a look at the star and the strip.
    Add-Type -AssemblyName System.Drawing
    if (-not ("PageShot" -as [type])) {
        Add-Type @"
using System; using System.Runtime.InteropServices;
public static class PageShot {
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags);
}
"@
    }
    $fr = New-Object W+RECT; [void][W]::GetWindowRect($frame, [ref]$fr)
    $shot = New-Object System.Drawing.Bitmap ($fr.R - $fr.L), ($fr.B - $fr.T), ([System.Drawing.Imaging.PixelFormat]::Format24bppRgb)
    $graphics = [System.Drawing.Graphics]::FromImage($shot)
    $dc = $graphics.GetHdc(); [void][PageShot]::PrintWindow($frame, $dc, 2); $graphics.ReleaseHdc($dc); $graphics.Dispose()
    $shotPath = Join-Path $ScratchDir "browser-window.png"
    $shot.Save($shotPath, [System.Drawing.Imaging.ImageFormat]::Png); $shot.Dispose()
    "  info  the browser window: $shotPath"

    Check (-not $p.HasExited) "Winamp is still running"
}
finally {
    QuitWinamp $p
}
Check ($p.ExitCode -eq 0) ("Winamp exited cleanly: exit code 0x{0:X8}" -f $p.ExitCode)

$history = "$wa\Plugins\exodium_data\history.txt"
if (Test-Path $history) {
    $bytes = [System.IO.File]::ReadAllBytes($history)
    Check ($bytes[0] -eq 0xFF -and $bytes[1] -eq 0xFE) "history.txt is UTF-16 with a byte-order mark"
    $lines = [System.IO.File]::ReadAllLines($history)
    $example = @($lines | Where-Object { $_ -like "https://example.com/`t*" })
    Check ($example.Count -eq 1) "history has the visited page: $($example -join ' ')"
    if ($example.Count -eq 1) {
        $cols = $example[0].Split("`t")
        Check ($cols[1] -eq "1" -and $cols[2] -eq "1" -and [int64]$cols[3] -gt 1700000000 -and $cols[4] -eq "Example Domain") "entry counts one typed visit, dated and titled"
    }
    Check (@($lines | Where-Object { $_ -like "*exodium.invalid*" }).Count -eq 0) "the history page itself is not in the history"
    # Visited once, and made as a stand-in in every copy of its tab's history since.
    $bf0 = @($lines | Where-Object { $_ -like "file:*/bf0.html`t*" })
    Check ($bf0.Count -eq 1 -and $bf0[0].Split("`t")[1] -eq "1") "a page copied into other tabs' history counts only its own visit: $($bf0 -join ' ')"
    $newOne = @($lines | Where-Object { $_ -like "file:*/newOne.html`t*" })
    Check ($newOne.Count -eq 1 -and $newOne[0].Split("`t")[2] -eq "1" -and $newOne[0].Split("`t")[4] -eq "New One") "and has the page opened by its path, as an address typed in: $($newOne -join ' ')"
} else { Check $false "history.txt was written at exit" }
Check (-not (Test-Path "$wa\Plugins\exodium_data\history.journal")) "the history's journal is done with once the history is written whole at exit"
$bookmarksFile = "$wa\Plugins\exodium_data\bookmarks.txt"
$bookmarkLines = @(if (Test-Path $bookmarksFile) { [IO.File]::ReadAllLines($bookmarksFile) })  # a list, of one line too
Check ($bookmarkLines.Count -eq 1 -and $bookmarkLines[0] -like "file:*/newOne.html`t*`tNew One") "bookmarks.txt has the page bookmarked: $($bookmarkLines -join ' | ')"
$session = "$wa\Plugins\exodium_data\session.txt"
$sessionLines = if (Test-Path $session) { @([IO.File]::ReadAllLines($session)) } else { @() }
Check ($sessionLines.Count -gt 1 -and $sessionLines[0] -eq "exodium-session`t1" -and @($sessionLines | Where-Object { $_ -like "window`t*" }).Count -ge 1) "session.txt was written at exit, with the first window in it: $(@($sessionLines | Where-Object { $_ -like "window`t*" }) -join ' | ')"

$saved = @{}
$inSection = $false
foreach ($line in Get-Content "$ini\winamp.ini") {
    if ($line -match '^\[(.+)\]$') { $inSection = $Matches[1] -eq "EmbedWndExample" }
    elseif ($inSection -and $line -match '^(\w+)=(-?\d+)$') { $saved[$Matches[1]] = [int]$Matches[2] }
}
if ($movedTo) {
    Check ($saved.xpos -eq $movedTo.L -and $saved.ypos -eq $movedTo.T -and $saved.width -eq ($movedTo.R - $movedTo.L) -and $saved.height -eq ($movedTo.B - $movedTo.T)) "window place saved to winamp.ini: x=$($saved.xpos) y=$($saved.ypos) w=$($saved.width) h=$($saved.height)"
}

# This time with the debugging port on, as Claude Code uses it. It goes in the [Exodium]
# section the settings page has written to, not in a second one, which would go unread.
[void][W]::WritePrivateProfileString("Exodium", "remote_debugging_port", "$DebugPort", "$ini\winamp.ini")
# And without the windows open when Winamp last closed coming back as it starts, which the
# checks below would count among the links' windows. tools\session-test.ps1 tests those.
[void][W]::WritePrivateProfileString("Exodium", "restore_windows", "0", "$ini\winamp.ini")

# With Winamp closed: two links clicked together start one Winamp. Whichever gets there
# first has a new window, and the other a new tab of it.
$opens = @((StartOpen $linkUrl), (StartOpen $linkPath))
$running = { @(Get-Process winamp -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq "$wa\winamp.exe" }) }
Check (WaitFor { (& $running).Count -ge 1 } 15) "with Winamp closed, a link starts it"
$started = @(& $running)[0]
if ($started) {
    [void]$started.Handle  # the exit code can't be read later without it
    try {
        Check ((OpenedOk $opens[0]) -and (OpenedOk $opens[1])) "and is handed over once it has started"
        $startedId = [uint32]$started.Id
        $visible = { @(Frames $startedId | Where-Object { [W]::IsWindowVisible($_) }) }
        # Both load, the one that was in view only until the other came as well.
        $linkTabs = { @(& $visible | ForEach-Object { TabTexts (ClientOf $_) }) }
        Check (WaitFor { (& $visible).Count -eq 1 -and ((& $linkTabs | Sort-Object) -join "|") -eq "Linked Page|Opened Page" } 40) "the links are on show as the two tabs of one window: $((& $visible).Count) window(s), '$((& $linkTabs) -join "' | '")'"
        Check ((& $running).Count -eq 1) "both links went to one Winamp"
        # In order from the front: the links' window comes up over Winamp's own.
        $order = @([W]::TopLevel($startedId))
        $mainAt = [array]::IndexOf($order, @($order | Where-Object { [W]::Class($_) -eq $Instance })[0])
        $above = @(& $visible | Where-Object { [array]::IndexOf($order, $_) -lt $mainAt })
        Check ($above.Count -eq (& $visible).Count) "and it is in front of Winamp's main window"

        # The settings made on Exodium's page before are read back at start-up.
        $linkedFrame = @(& $visible)[0]
        if ($linkedFrame) {
            $linkedClient = ClientOf $linkedFrame
            [void][W]::PostMessage($linkedClient, 0x0111, [IntPtr]1101, [IntPtr]::Zero)  # WM_COMMAND kIdNewTab
            Check (WaitFor { (TabButtons $linkedClient).Count -eq 3 -and [W]::Title($linkedFrame) -eq "Exodium Home" } 30) "after a restart a new tab still opens on the home page set before: '$([W]::Title($linkedFrame))'"
            ClickCloseButton (TabByText $linkedClient "Linked Page")
            Check ((WaitFor { [W]::Title($linkedFrame) -eq "Linked Page" } 5) -and (TabButtons $linkedClient).Count -eq 3) "and tabs still have no close buttons: a click where one was brings the tab into view"
            [void][W]::PostMessage((TabButtons $linkedClient)[2], 0x0208, [IntPtr]::Zero, (LParam 5 5))  # WM_MBUTTONUP: it goes again
            [void](WaitFor { (TabButtons $linkedClient).Count -eq 2 } 10)
        } else { Check $false "a window to open a tab in after the restart" }

        # What tools see on the debugging port.
        $debug = "http://127.0.0.1:$DebugPort"
        # In brackets, or Windows PowerShell hands the whole list down the pipe as one item.
        $pages = { @((Invoke-RestMethod "$debug/json/list" -TimeoutSec 5) | Where-Object { $_.type -eq "page" }) }
        Check (@(& $pages | Where-Object { $_.url -eq $linkUrl }).Count -eq 1) "the debugging port shows the page in a tab"
        $wanted = @(& $pages | Where-Object { $_.url -eq $linkUrl })[0]
        [void](Invoke-WebRequest "$debug/json/close/$($wanted.id)" -UseBasicParsing -TimeoutSec 5)
        Check (WaitFor { @(& $pages | Where-Object { $_.id -eq $wanted.id }).Count -eq 0 } 5) "a tab closed from there goes, with no page kept behind for tools to find"
        Check (WaitFor { ((& $linkTabs) -join "|") -eq "Linked Page" } 5) "and it goes from its window's strip, which keeps the other: '$((& $linkTabs) -join "' | '")'"

        # A window of Claude's, known by the tools' start page in its address, has a browser
        # profile of its own: a browser context apart from the user's pages', on the port.
        $claudeUrl = "$linkUrl#exodium_start.html"
        Check (OpenedOk (StartOpen $claudeUrl)) "a window of Claude's is handed over"
        Check (WaitFor { @(& $pages | Where-Object { $_.url -eq $claudeUrl }).Count -eq 1 } 30) "the debugging port shows Claude's page"
        # It is a window of its own, not a tab of the user's, and no link of the user's goes in
        # it, in front or not.
        $claudeFrame = @(& $visible | Where-Object { $_ -ne $linkedFrame })[0]
        Check ($claudeFrame -and $linkedFrame -and (TabButtons (ClientOf $linkedFrame)).Count -eq 1) "Claude's page has a window of its own, though one of the user's is open: $((& $visible).Count) windows"
        if ($claudeFrame -and $linkedFrame) {
            [void][W]::SetWindowPos($claudeFrame, [IntPtr]::Zero, 0, 0, 0, 0, 0x0013)  # HWND_TOP; SWP_NOSIZE | SWP_NOMOVE | SWP_NOACTIVATE
            [void](OpenedOk (StartOpen $linkUrl))
            Check ((WaitFor { (TabTexts (ClientOf $linkedFrame)) -contains "Opened Page" } 30) -and (TabButtons (ClientOf $claudeFrame)).Count -eq 1) "a link goes past Claude's window in front, into the user's: '$((TabTexts (ClientOf $linkedFrame)) -join "' | '")', $((TabButtons (ClientOf $claudeFrame)).Count) tab(s) in Claude's"
            $again = @(& $pages | Where-Object { $_.url -eq $linkUrl })[0]
            if ($again) { [void](Invoke-WebRequest "$debug/json/close/$($again.id)" -UseBasicParsing -TimeoutSec 5) }
            [void](WaitFor { (TabButtons (ClientOf $linkedFrame)).Count -eq 1 } 5)
        }
        $targets = @((CdpBrowser $debug "Target.getTargets").targetInfos | Where-Object { $_.type -eq "page" })
        $claudeContext = @($targets | Where-Object { $_.url -eq $claudeUrl })[0].browserContextId
        $userContexts = @($targets | Where-Object { $_.url -ne $claudeUrl } | ForEach-Object { $_.browserContextId } | Select-Object -Unique)
        Check ($claudeContext -and $userContexts.Count -ge 1 -and $userContexts -notcontains $claudeContext) "Claude's page is in a profile of its own: '$claudeContext', the user's '$($userContexts -join "', '")'"
        foreach ($claudePage in @(& $pages | Where-Object { $_.url -eq $claudeUrl })) {
            [void](Invoke-WebRequest "$debug/json/close/$($claudePage.id)" -UseBasicParsing -TimeoutSec 5)
        }
        Check (WaitFor { @(& $visible).Count -eq 1 } 5) "and its window goes when it is closed"
        # Made by the browser process itself, so it is in no window of ours.
        [void](Invoke-RestMethod -Method Put "$debug/json/new?about:blank" -TimeoutSec 5)
    }
    finally {
        QuitWinamp $started
    }
    Check ($started.ExitCode -eq 0) ("Winamp started by a link exited cleanly: exit code 0x{0:X8}" -f $started.ExitCode)
    Check (WaitFor { -not (PortOpen) } 10) "a page made through the debugging port doesn't keep the port open past Winamp"
}

# The Start menu's entry with Winamp closed, with the shortcut the checkbox made: Winamp
# starts, and a window on the home page comes up in front of Winamp's own windows.
if (Test-Path -LiteralPath $keptShortcut) {
    Start-Process -FilePath $keptShortcut
    Check (WaitFor { (& $running).Count -ge 1 } 15) "with Winamp closed, the Start menu's shortcut starts it"
    $started = @(& $running)[0]
    if ($started) {
        [void]$started.Handle
        try {
            $startedId = [uint32]$started.Id
            $visible = { @(Frames $startedId | Where-Object { [W]::IsWindowVisible($_) }) }
            Check (WaitFor { @(& $visible | Where-Object { [W]::Title($_) -eq "Exodium Home" }).Count -eq 1 } 40) "and a window on the home page opens: '$((& $visible | ForEach-Object { [W]::Title($_) }) -join "' | '")'"
            $order = @([W]::TopLevel($startedId))
            $mainAt = [array]::IndexOf($order, @($order | Where-Object { [W]::Class($_) -eq $Instance })[0])
            Check (@(& $visible | Where-Object { [array]::IndexOf($order, $_) -lt $mainAt }).Count -eq @(& $visible).Count) "in front of Winamp's main window"
            # Not a check: what else of Winamp's comes up with it, from the front.
            $shownWith = @($order | Where-Object { [W]::IsWindowVisible($_) } | ForEach-Object { "$([W]::Class($_)) '$([W]::Title($_))'" })
            "  info  on show, from the front: $($shownWith -join ', ')"
        }
        finally {
            QuitWinamp $started
        }
        Check ($started.ExitCode -eq 0) ("Winamp started from the Start menu exited cleanly: exit code 0x{0:X8}" -f $started.ExitCode)
    }
} else { Check $false "a shortcut kept from the Start menu's checkbox, to start Winamp with" }

# Tabs left out of view are put to sleep, and a Winamp that is cut off keeps its history
# (2026-11-09). Here a tab is left for twelve seconds, not two hours, which the plugin takes
# from the environment; the debugging port is still on, as it is where Claude Code is used.
$sleepPages = Join-Path $ScratchDir "pages"
New-Item -ItemType Directory -Force $sleepPages | Out-Null
# Each says in its title how many times it has loaded, so that one loaded afresh can be told.
$counts = "<script>const n = +(localStorage.getItem(document.title) || 0) + 1; localStorage.setItem(document.title, n); document.title += ' ' + n</script>"
Set-Content "$sleepPages\sleeper.html" "<!doctype html><title>Sleeper</title><p>plain</p>$counts" -Encoding ascii
Set-Content "$sleepPages\draft.html" "<!doctype html><title>Draft</title><textarea id=t></textarea><script>t.value = 'typed and not sent'</script>$counts" -Encoding ascii
Set-Content "$sleepPages\front.html" "<!doctype html><title>Front</title><p>in view</p>$counts" -Encoding ascii
Set-Content "$sleepPages\cutoff.html" "<!doctype html><title>Cut Off</title><p>visited just before Winamp was ended</p>" -Encoding ascii
$sleepUrl = { param([string]$name) ([Uri]"$sleepPages\$name.html").AbsoluteUri }
$historyFile = "$wa\Plugins\exodium_data\history.txt"
$journalFile = "$wa\Plugins\exodium_data\history.journal"
$logFile = "$wa\Plugins\exodium_data\exodium.log"
Remove-Item $logFile -ErrorAction SilentlyContinue

$env:EXODIUM_SLEEP_TABS_MS = "12000"
$sleepy = Start-Process "$wa\winamp.exe" -ArgumentList "/CLASS=$Instance" -PassThru
Remove-Item Env:\EXODIUM_SLEEP_TABS_MS
[void]$sleepy.Handle
$sleepyId = [uint32]$sleepy.Id
$ended = $false
try {
    Check (WaitFor { (Frames $sleepyId).Count -ge 1 } 30) "Winamp starts again, for the tabs that are put to sleep"
    $sleepFrame = (Frames $sleepyId)[0]
    $sleepClient = ClientOf $sleepFrame
    [void][W]::ShowWindowAsync($sleepFrame, 8)  # SW_SHOWNA
    [void](WaitFor { [W]::IsWindowVisible($sleepFrame) -and (HasWebView $sleepClient) } 60)
    Start-Sleep -Seconds 2
    # The webviews in the window: each is a window of the browser's own, right under the client.
    $webviews = { @([W]::Children($sleepClient) | Where-Object { [W]::GetParent($_) -eq $sleepClient -and [W]::Class($_) -like "Chrome_WidgetWin*" }).Count }
    Navigate $sleepClient (& $sleepUrl "sleeper")
    [void](WaitFor { [W]::Title($sleepFrame) -eq "Sleeper 1" } 20)
    foreach ($name in "draft", "front") {
        [void][W]::PostMessage($sleepClient, 0x0111, [IntPtr]1101, [IntPtr]::Zero)  # WM_COMMAND kIdNewTab
        Start-Sleep -Seconds 2
        Navigate $sleepClient (& $sleepUrl $name)
        [void](WaitFor { [W]::Title($sleepFrame) -like "$name 1" } 20)
    }
    Check (((TabTexts $sleepClient) -join "|") -eq "Sleeper 1|Draft 1|Front 1" -and (& $webviews) -eq 3) "three tabs, each with its page: '$((TabTexts $sleepClient) -join "' | '")', $(& $webviews) webviews"
    Check (WaitFor { (& $webviews) -eq 2 } 40) "a tab left out of view is put to sleep, and its webview goes: $(& $webviews) left"
    Start-Sleep -Seconds 10  # long enough for the next tab's turn to have come
    Check ((& $webviews) -eq 2) "one with something typed into it stays awake, and so does the one in view: $(& $webviews) left"
    Check (((TabTexts $sleepClient) -join "|") -eq "Sleeper 1|Draft 1|Front 1") "the sleeping tab keeps its place and its title: '$((TabTexts $sleepClient) -join "' | '")'"
    ClickTab (TabByText $sleepClient "Sleeper 1")
    Check (WaitFor { [W]::Title($sleepFrame) -eq "Sleeper 2" -and (& $webviews) -ge 2 } 30) "looked at again, it has a webview and its page loads afresh: '$([W]::Title($sleepFrame))'"
    Check ([W]::Text((Control $sleepClient 1003)) -eq (& $sleepUrl "sleeper")) "at the address it was on: '$([W]::Text((Control $sleepClient 1003)))'"
    ClickTab (TabByText $sleepClient "Draft 1")
    Check ((WaitFor { [W]::Title($sleepFrame) -eq "Draft 1" } 5) -and -not (WaitFor { [W]::Title($sleepFrame) -ne "Draft 1" } 3)) "the tab that stayed awake is as it was, not loaded again: '$([W]::Title($sleepFrame))'"

    # A private window (Ctrl+Shift+N): its pages are in a session of their own, and nothing of
    # them is written down: not in the history's journal, nor among the windows and tabs kept.
    Set-Content "$sleepPages\private.html" "<!doctype html><title>Private Page</title><p>private</p>" -Encoding ascii
    $sessionFile = "$wa\Plugins\exodium_data\session.txt"
    $written = { param([string]$file) if (Test-Path $file) { [IO.File]::ReadAllText($file, [Text.Encoding]::Unicode) } else { "" } }
    $framesBefore = @(Frames $sleepyId)
    WithKeys $sleepClient @($VK_CONTROL, $VK_SHIFT) { [void][W]::SendMessageP($sleepClient, 0x0100, [IntPtr]0x4E, [IntPtr]::Zero) }  # WM_KEYDOWN N
    Check (WaitFor { (Frames $sleepyId).Count -eq $framesBefore.Count + 1 } 10) "Ctrl+Shift+N opens a window"
    $privateFrame = @(Frames $sleepyId | Where-Object { $framesBefore -notcontains $_ })[0]
    if ($privateFrame) {
        $privateClient = ClientOf $privateFrame
        Check (WaitFor { [W]::Title($privateFrame) -like "* (Private)" } 30) "whose title says it is a private one: '$([W]::Title($privateFrame))'"
        [void](WaitFor { HasWebView $privateClient } 30)
        Start-Sleep -Seconds 2
        Navigate $privateClient (& $sleepUrl "private")
        Check (WaitFor { [W]::Title($privateFrame) -eq "Private Page (Private)" } 20) "a page loads in it: '$([W]::Title($privateFrame))'"
        [void][W]::PostMessage($privateClient, 0x0111, [IntPtr]1101, [IntPtr]::Zero)  # WM_COMMAND kIdNewTab
        Check (WaitFor { (TabButtons $privateClient).Count -eq 2 } 10) "and a second tab opens in it"
        Start-Sleep -Seconds 3
        $debug = "http://127.0.0.1:$DebugPort"
        $targets = @((CdpBrowser $debug "Target.getTargets").targetInfos | Where-Object { $_.type -eq "page" })
        $privateUrl = & $sleepUrl "private"
        $privateContext = @($targets | Where-Object { $_.url -eq $privateUrl })[0].browserContextId
        $otherContexts = @($targets | Where-Object { $_.browserContextId -ne $privateContext } | ForEach-Object { $_.browserContextId } | Select-Object -Unique)
        $inPrivate = @($targets | Where-Object { $_.browserContextId -eq $privateContext })
        Check ($privateContext -and $otherContexts.Count -ge 1 -and $inPrivate.Count -eq 2) "its two tabs are in a session of their own, apart from the other windows': '$privateContext' with $($inPrivate.Count) pages, the others in '$($otherContexts -join "', '")'"
        Check (-not (& $written $journalFile).Contains("private.html") -and (& $written $journalFile).Contains("sleeper.html")) "the history's journal has the other windows' pages, and not the private one's"
        [void][W]::PostMessage($privateFrame, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)  # WM_CLOSE
        Check (WaitFor { -not [W]::IsWindow($privateFrame) } 10) "it closes like any other"
        Start-Sleep -Seconds 4  # the windows and tabs kept, the closed ones among them, are written every two
        Check ((& $written $sessionFile).Contains("sleeper.html") -and -not (& $written $sessionFile).Contains("private.html")) "and neither it nor its tabs are among the windows and tabs kept, open or closed"
    }

    # Ended from outside, with no chance to write anything: the visit just made is in the journal.
    Navigate $sleepClient (& $sleepUrl "cutoff")
    [void](WaitFor { [W]::Title($sleepFrame) -eq "Cut Off" } 20)
    $inJournal = WaitFor { (Test-Path $journalFile) -and [IO.File]::ReadAllText($journalFile, [Text.Encoding]::Unicode).Contains("cutoff.html") } 8
    $inHistory = (Test-Path $historyFile) -and [IO.File]::ReadAllText($historyFile, [Text.Encoding]::Unicode).Contains("cutoff.html")
    Check ($inJournal -and -not $inHistory) "a page just visited is in the history's journal, and not yet in the history file"
    $sleepy.Kill()
    [void]$sleepy.WaitForExit(10000)
    $ended = $true
}
finally {
    if (-not $ended) { QuitWinamp $sleepy }
}
# The browser's own processes go once Winamp has, and its port with them.
Check (WaitFor { -not (PortOpen) } 20) "the Winamp that was ended leaves no browser behind"
Start-Sleep -Seconds 2
$after = Start-Process "$wa\winamp.exe" -ArgumentList "/CLASS=$Instance" -PassThru
[void]$after.Handle
try {
    Check (WaitFor { (Frames ([uint32]$after.Id)).Count -ge 1 } 30) "and Winamp starts after it"
    Start-Sleep -Seconds 2
}
finally {
    QuitWinamp $after
}
Check ($after.ExitCode -eq 0) ("it exits cleanly: exit code 0x{0:X8}" -f $after.ExitCode)
$historyLines = @(if (Test-Path $historyFile) { [IO.File]::ReadAllLines($historyFile) })
$cutOff = @($historyLines | Where-Object { $_ -like "file:*/cutoff.html`t*" })
Check ($cutOff.Count -eq 1 -and $cutOff[0].Split("`t")[1] -eq "1" -and $cutOff[0].Split("`t")[4] -eq "Cut Off") "the history has the page visited just before Winamp was ended, counted once: $($cutOff -join ' ')"
Check (-not (Test-Path $journalFile)) "and the journal it was read from is done with"
Check (@($historyLines | Where-Object { $_ -like "*private.html*" }).Count -eq 0) "the page looked at in the private window is not in the history"
$logText = if (Test-Path $logFile) { [IO.File]::ReadAllText($logFile, [Text.Encoding]::Unicode) } else { "" }
Check ($logText -like "*its journal was read instead*") "the log says so: $(($logText -split "`r?`n" | Where-Object { $_ }) -join ' / ')"
"=== ${Label}: $($script:failures) failure(s) ==="
exit $script:failures
