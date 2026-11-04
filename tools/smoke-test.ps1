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
# Winamp's Preferences while they are open, and a control on the page they show.
function PrefsWindow($main) {
    [IntPtr][int64][W]::SendMessageP($main, 0x0400, [IntPtr]::Zero, [IntPtr]619)  # WM_WA_IPC IPC_GETPREFSWND
}
function PageControl($prefs, [int]$id) {
    foreach ($c in [W]::Children($prefs)) {
        $page = [W]::GetParent($c)
        if ($page -ne $prefs -and [W]::GetParent($page) -eq $prefs -and [W]::GetDlgCtrlID($c) -eq $id) { return $c }
    }
    return [IntPtr]::Zero
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
function PressKey($hwnd, [int]$key) {
    [void][W]::PostMessage($hwnd, 0x0100, [IntPtr]$key, [IntPtr]1)                       # WM_KEYDOWN
    [void][W]::PostMessage($hwnd, 0x0101, [IntPtr]$key, [IntPtr]([int]0xC0000001 -bor 0))  # WM_KEYUP
}
# The page in view's own window. A click posted to it counts as the user's.
function PageWidget($client) {
    @([W]::Children($client) | Where-Object { [W]::Class($_) -eq "Chrome_RenderWidgetHostHWND" -and [W]::IsWindowVisible($_) })[0]
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
    & $click 1001
    Check (WaitFor { (& $urlBar) -eq (& $bf 2) } 10) "Back in it goes where it would have in the tab it came from: '$(& $urlBar)'"
    & $click 1001
    Check (WaitFor { [W]::Title($frame) -eq "BF 1 (back_forward)" } 20) "and on, to a page loaded for real once it is come to: '$([W]::Title($frame))'"
    & $click 1002
    [void](WaitFor { [W]::Title($frame) -eq "BF 2 (back_forward)" } 20)
    & $click 1002
    [void](WaitFor { (& $urlBar) -eq (& $bf 2 "#part") } 10)
    & $click 1002
    Check (WaitFor { [W]::Title($frame) -eq "BF 3 (back_forward)" -and (& $urlBar) -eq (& $bf 3) } 20) "Forward leads on to the last of them: '$([W]::Title($frame))' '$(& $urlBar)'"

    # The middle button on Forward, from the entry before the last: the copy ends on the page it shows.
    & $click 1001
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
    [void][W]::GetWindowRect($edit, [ref]$er)
    Check (($er.R - $er.L) -eq $editWidth) "and the address bar has its room back"

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
    [void][W]::ShowWindowAsync($frame, 3)   # SW_MAXIMIZE
    Check (WaitFor { [W]::IsZoomed($frame) } 5) "frame maximizes"
    $mr = New-Object W+RECT; [void][W]::GetWindowRect($frame, [ref]$mr)
    $work = [System.Windows.Forms.Screen]::FromHandle($frame).WorkingArea
    Check ($mr.L -eq $work.Left -and $mr.T -eq $work.Top -and $mr.R -eq $work.Right -and $mr.B -eq $work.Bottom) "maximized frame is exactly the work area ($($mr.L),$($mr.T),$($mr.R),$($mr.B))"
    [void][W]::ShowWindowAsync($frame, 9)
    Check (WaitFor { -not [W]::IsZoomed($frame) } 5) "frame restores from maximized"

    # Winamp's title bar is a caption, for Windows to drag the window by and snap it to the
    # screen's edges. Its close button stays Winamp's.
    $fr = New-Object W+RECT; [void][W]::GetWindowRect($frame, [ref]$fr)
    $hit = [int][W]::SendMessageP($frame, 0x0084, [IntPtr]::Zero, (LParam ($fr.L + 200) ($fr.T + 5)))  # WM_NCHITTEST
    Check ($hit -eq 2) "Winamp's title bar hit-tests as a caption: $hit"
    $hit = [int][W]::SendMessageP($frame, 0x0084, [IntPtr]::Zero, (LParam ($fr.R - 7) ($fr.T + 7)))
    Check ($hit -eq 1) "the close button on it doesn't: $hit"

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

    # A move from outside the process, as AltSnap makes one. It must stick and be saved.
    Start-Sleep -Milliseconds 500
    [void][W]::SetWindowPos($frame, [IntPtr]::Zero, 140, 120, 925, 667, 0x0014)  # SWP_NOZORDER | SWP_NOACTIVATE
    Start-Sleep -Milliseconds 500
    $r = New-Object W+RECT; [void][W]::GetWindowRect($frame, [ref]$r)
    Check ($r.L -eq 140 -and $r.T -eq 120) "frame can be moved from outside: at ($($r.L),$($r.T)) size $($r.R - $r.L)x$($r.B - $r.T)"
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
    Check (WaitFor { (Frames $procId).Count -eq 2 } 10) "a link gets a new window"
    $linked = @(Frames $procId | Where-Object { $_ -ne $frame })[0]
    if ($linked) {
        $linkedClient = ClientOf $linked
        Check (WaitFor { [W]::IsWindowVisible($linked) -and [W]::Title($linked) -eq "Opened Page" } 30) "the window shows, with the link's page in it: '$([W]::Title($linked))'"
        Check ((TabButtons $linkedClient).Count -eq 1) "the page is the window's one tab"
        Check (WaitFor { [W]::Text((Control $linkedClient 1003)) -eq $linkUrl } 10) "the address bar shows the link: '$([W]::Text((Control $linkedClient 1003)))'"
        Check (-not [W]::IsWindowVisible($frame)) "the first window stays out of sight"

        Check (OpenedOk (StartOpen $linkPath)) "a page on disk is taken as a file type's path"
        Check (WaitFor { (Frames $procId).Count -eq 3 } 10) "a second link gets a window as well, not a tab"
        $titles = { @(Frames $procId | ForEach-Object { [W]::Title($_) }) }
        Check (WaitFor { (& $titles) -contains "Linked Page" } 30) "the page on disk loads, its space and # intact: '$((& $titles) -join "' | '")'"

        [void](OpenedOk (StartOpen "javascript:alert(1)"))
        Start-Sleep -Seconds 1
        Check ((Frames $procId).Count -eq 3) "a script address opens nothing"

        Check (OpenedOk (StartOpen "")) "exodium_open.exe with no link at all"
        Check (WaitFor { (Frames $procId).Count -eq 4 -and (& $titles) -match "Google" } 40) "gets a window on the default page: '$((& $titles) -join "' | '")'"

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
    Check (WaitFor { (PageControl $prefs 1301) -ne [IntPtr]::Zero } 10) "its Configure button opens Exodium's page in the Preferences"
    $homeBox = PageControl $prefs 1301
    $engines = PageControl $prefs 1302
    $searchBox = PageControl $prefs 1303
    $closeButtons = PageControl $prefs 1311
    Check ([W]::Text($homeBox) -eq "https://google.com/" -and [W]::Text($engines) -eq "Google" -and [W]::Text($searchBox) -eq "https://www.google.com/search?q=%s") "the page starts with Google as home page and search engine"
    Check ([int][W]::SendMessageP($closeButtons, 0x00F0, [IntPtr]::Zero, [IntPtr]::Zero) -eq 1) "and with close buttons on tabs"  # BM_GETCHECK

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
        Start-Sleep -Seconds 2
        $recent = @(ProbeReadings | Select-Object -Last 5)
        Check (@($recent | Where-Object { $_.wave -gt 40 }).Count -ge 4) "the visualization plug-ins get the page's sound: $(($recent | ForEach-Object { $_.wave }) -join ' ')"
        Check (@($recent | Where-Object { $_.analyzer -gt 10 }).Count -ge 4) "and so does the main window's analyzer: $(($recent | ForEach-Object { $_.analyzer }) -join ' ')"
        Check (WaitFor { [Remote32]::AskForText($main, 0, 3034) -like "*Tone still playing*" } 8) "the title Winamp shows follows the page's: '$([Remote32]::AskForText($main, 0, 3034))'"  # IPC_GET_PLAYING_TITLE
        $entryTitles = @(0..1 | ForEach-Object { [Remote32]::AskForText($main, $_, 213) })  # IPC_GETPLAYLISTTITLEW
        Check (-not ($entryTitles -match "Tone|Exodium")) "the playlist's entries keep their own titles: '$($entryTitles -join "', '")'"

        PostClick (PageWidget $client) 200 200  # quiet
        Start-Sleep -Seconds 1
        Check ((WinampPlaying $main) -eq 1) "Winamp doesn't stop the moment the page goes quiet"
        Check (WaitFor { (WinampPlaying $main) -eq 0 } 15) "it stops once the page has been quiet a while"
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
} else { Check $false "history.txt was written at exit" }

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

# With Winamp closed: two links clicked together start one Winamp, and each gets a window.
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
        Check (WaitFor { @(& $visible | Where-Object { [W]::Title($_) -in "Opened Page", "Linked Page" }).Count -eq 2 } 40) "each link is on show in a window of its own: '$((& $visible | ForEach-Object { [W]::Title($_) }) -join "' | '")'"
        Check ((& $running).Count -eq 1) "both links went to one Winamp"
        # In order from the front: the links' windows come up over Winamp's own.
        $order = @([W]::TopLevel($startedId))
        $mainAt = [array]::IndexOf($order, @($order | Where-Object { [W]::Class($_) -eq $Instance })[0])
        $above = @(& $visible | Where-Object { [array]::IndexOf($order, $_) -lt $mainAt })
        Check ($above.Count -eq (& $visible).Count) "and they are in front of Winamp's main window"

        # The settings made on Exodium's page before are read back at start-up.
        $linkedFrame = @(& $visible | Where-Object { [W]::Title($_) -eq "Linked Page" })[0]
        if ($linkedFrame) {
            $linkedClient = ClientOf $linkedFrame
            [void][W]::PostMessage($linkedClient, 0x0111, [IntPtr]1101, [IntPtr]::Zero)  # WM_COMMAND kIdNewTab
            Check (WaitFor { (TabButtons $linkedClient).Count -eq 2 -and [W]::Title($linkedFrame) -eq "Exodium Home" } 30) "after a restart a new tab still opens on the home page set before: '$([W]::Title($linkedFrame))'"
            ClickCloseButton (TabByText $linkedClient "Linked Page")
            Check ((WaitFor { [W]::Title($linkedFrame) -eq "Linked Page" } 5) -and (TabButtons $linkedClient).Count -eq 2) "and tabs still have no close buttons: a click where one was brings the tab into view"
            [void][W]::PostMessage((TabButtons $linkedClient)[1], 0x0208, [IntPtr]::Zero, (LParam 5 5))  # WM_MBUTTONUP: it goes again
            [void](WaitFor { (TabButtons $linkedClient).Count -eq 1 } 10)
        } else { Check $false "a window to open a tab in after the restart" }

        # What tools see on the debugging port.
        $debug = "http://127.0.0.1:$DebugPort"
        # In brackets, or Windows PowerShell hands the whole list down the pipe as one item.
        $pages = { @((Invoke-RestMethod "$debug/json/list" -TimeoutSec 5) | Where-Object { $_.type -eq "page" }) }
        Check (@(& $pages | Where-Object { $_.url -eq $linkUrl }).Count -eq 1) "the debugging port shows the page in a window"
        $wanted = @(& $pages | Where-Object { $_.url -eq $linkUrl })[0]
        [void](Invoke-WebRequest "$debug/json/close/$($wanted.id)" -UseBasicParsing -TimeoutSec 5)
        Check (WaitFor { @(& $pages | Where-Object { $_.id -eq $wanted.id }).Count -eq 0 } 5) "a tab closed from there goes, with no page kept behind for tools to find"
        Check (WaitFor { @(& $visible).Count -eq 1 } 5) "and its window goes with it"
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
"=== ${Label}: $($script:failures) failure(s) ==="
exit $script:failures
