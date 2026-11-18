<#
.SYNOPSIS
    Tests that the browser picks up where it left off: the windows and tabs open when
    Winamp closed come back when it starts again, each tab with its Back and Forward, and
    Ctrl+Shift+T brings back a closed window whole.

.DESCRIPTION
    Runs gen_browser.dll in a scratch copy of Winamp, as tools\smoke-test.ps1 does, on a
    desktop of its own that Windows never shows, so that nothing comes up in front of
    whoever is at the machine. It has a scratch folder and a window class of its own, so
    it can run beside a smoke test.

    Every page is a file the test writes, which says in its title how it was loaded, so a
    title tells a page that loaded for real from a blank stand-in, and a page loaded by a
    step back or forward ("back_forward") from one loaded afresh ("navigate").

    The exit code is the number of checks that failed.

.EXAMPLE
    tools\session-test.ps1
#>
param(
    [string]$PluginPath = (Join-Path $PSScriptRoot "..\bin\Win32\Release\gen_browser.dll"),
    [string]$WinampDir = (Join-Path ([Environment]::GetFolderPath("MyDocuments")) "Winamp"),
    [string]$SettingsDir = (Join-Path $env:APPDATA "Winamp"),
    [string]$Instance = "ExodiumSessionTest",
    [string]$ScratchDir = (Join-Path $env:TEMP "exodium-session-test"),
    [switch]$OnScreen,
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
            IntPtr kept = process.Handle;
            return process;
        } finally {
            CloseHandle(info.hThread);
            CloseHandle(info.hProcess);
        }
    }
}
"@
    $desktop = [OwnDesktop]::CreateDesktopW($Instance, [IntPtr]::Zero, [IntPtr]::Zero, 0, 0x10000000, [IntPtr]::Zero)  # GENERIC_ALL
    if ($desktop -eq [IntPtr]::Zero) { throw "Couldn't make a desktop to run on. -OnScreen runs without one." }
    $log = Join-Path ([IO.Path]::GetTempPath()) "exodium-session-test-$Instance.log"
    Remove-Item $log -ErrorAction SilentlyContinue
    $quote = { param($text) "'" + ($text -replace "'", "''") + "'" }
    $arguments = "-OnOwnDesktop -PluginPath $(& $quote (Resolve-Path $PluginPath).Path) -WinampDir $(& $quote $WinampDir) " +
                 "-SettingsDir $(& $quote $SettingsDir) -ScratchDir $(& $quote $ScratchDir) -Instance $(& $quote $Instance)"
    $command = "& $(& $quote $PSCommandPath) $arguments *> $(& $quote $log); exit `$LASTEXITCODE"
    $commandLine = "`"$PSHOME\powershell.exe`" -NoProfile -ExecutionPolicy Bypass -Command `"$command`""
    "(running on a desktop of its own, out of sight; -OnScreen to watch it)"
    $child = $null; $reader = $null; $exitCode = 1
    try {
        $child = [OwnDesktop]::Start($Instance, $commandLine, (Get-Location).Path)
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
        [void][OwnDesktop]::CloseDesktop($desktop)
    }
    exit $exitCode
}

if (-not (Test-Path "$wa\winamp.exe")) {
    "Copying $WinampDir to $ScratchDir ..."
    # As the smoke test: without the browser's data, and without the plugins that make a noise or reach outside.
    $skipDirs = (Join-Path $WinampDir "Plugins\exodium_data"), (Join-Path $WinampDir "wv2_userdata")
    robocopy $WinampDir $wa /E /XD $skipDirs /XF gen_llama.dll gen_tray.dll gen_hotkeys.dll gen_crasher.dll "gen_browser.dll*" /NFL /NDL /NJH /NJS /NP /R:1 /W:1 | Out-Null
    robocopy $SettingsDir $ini /E /NFL /NDL /NJH /NJS /NP /R:1 /W:1 | Out-Null
    Set-Content -Path "$wa\paths.ini" -Value "[Winamp]`r`ninidir=$ini" -Encoding ascii
}
Copy-Item $PluginPath "$wa\Plugins\gen_browser.dll" -Force
$loader = Join-Path (Split-Path -Parent $PluginPath) "WebView2Loader.dll"
if (Test-Path $loader) { Copy-Item $loader "$wa\Plugins\WebView2Loader.dll" -Force }
$opener = Join-Path (Split-Path -Parent $PluginPath) "exodium_open.exe"
if (Test-Path $opener) { Copy-Item $opener "$wa\Plugins\exodium_open.exe" -Force }
# Nothing here plays, so the visualizers' input plugin isn't wanted.
Remove-Item "$wa\Plugins\in_exodium.dll", "$wa\Plugins\vis_probe.dll" -ErrorAction SilentlyContinue

# A modest window place, and none of the settings of earlier runs.
$iniText = [System.IO.File]::ReadAllText("$ini\winamp.ini")
$iniText = [regex]::Replace($iniText, '(?ms)^\[EmbedWndExample\]\r?\n.*?(?=^\[|\z)', "[EmbedWndExample]`r`nxpos=100`r`nypos=90`r`nwidth=900`r`nheight=638`r`n")
$iniText = [regex]::Replace($iniText, '(?ms)^\[Exodium\]\r?\n.*?(?=^\[|\z)', "")
# Bitwarden left out, as in the smoke test: it would be fetched from the Chrome Web Store.
$iniText = $iniText.TrimEnd() + "`r`n[Exodium]`r`nbitwarden=0`r`n"
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
    [DllImport("user32.dll")] public static extern bool IsWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool IsZoomed(IntPtr h);
    [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr h);
    [DllImport("user32.dll")] public static extern bool ShowWindowAsync(IntPtr h, int cmd);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr SendMessage(IntPtr h, uint m, IntPtr w, string l);
    [DllImport("user32.dll", CharSet=CharSet.Unicode, EntryPoint="SendMessageW")] public static extern IntPtr SendMessageSb(IntPtr h, uint m, IntPtr w, StringBuilder l);
    [DllImport("user32.dll", EntryPoint="SendMessageW")] public static extern IntPtr SendMessageP(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);
    [DllImport("user32.dll")] public static extern IntPtr GetParent(IntPtr h);
    [DllImport("user32.dll")] public static extern int GetDlgCtrlID(IntPtr h);
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode)] public static extern bool WritePrivateProfileString(string section, string key, string value, string file);
    [DllImport("user32.dll")] public static extern bool AttachThreadInput(uint attach, uint attachTo, bool on);
    [DllImport("kernel32.dll")] public static extern uint GetCurrentThreadId();
    [DllImport("user32.dll")] public static extern bool GetKeyboardState(byte[] state);
    [DllImport("user32.dll")] public static extern bool SetKeyboardState(byte[] state);
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
[void][W]::SetProcessDPIAware()
$script:failures = 0

function Check([bool]$ok, [string]$what) {
    if ($ok) { "  PASS  $what" } else { $script:failures++; "  FAIL  $what" }
}
function WaitFor([scriptblock]$cond, [int]$seconds = 30) {
    $until = (Get-Date).AddSeconds($seconds)
    while ((Get-Date) -lt $until) { if (& $cond) { return $true }; Start-Sleep -Milliseconds 250 }
    return $false
}
function ClientOf($frame) {
    foreach ($c in [W]::Children($frame)) { if ([W]::Class($c) -eq "ExodiumClient") { return $c } }
    return [IntPtr]::Zero
}
function Frames($procId) {
    @([W]::TopLevel($procId) | Where-Object { [W]::Class($_) -eq "Winamp Gen" -and (ClientOf $_) -ne [IntPtr]::Zero })
}
function Shown($procId) { @(Frames $procId | Where-Object { [W]::IsWindowVisible($_) }) }
function Control($client, [int]$id) {
    foreach ($c in [W]::Children($client)) { if ([W]::GetParent($c) -eq $client -and [W]::GetDlgCtrlID($c) -eq $id) { return $c } }
    return [IntPtr]::Zero
}
$TabBase = 0x8000 + 43
function TabButtons($client) {
    @([W]::Children($client) | Where-Object { [W]::GetParent($_) -eq $client -and [W]::GetDlgCtrlID($_) -ge $TabBase -and [W]::GetDlgCtrlID($_) -lt ($TabBase + 4096) } | Sort-Object { [W]::GetDlgCtrlID($_) })
}
function TabTexts($client) { @(TabButtons $client | ForEach-Object { [W]::Text($_) }) }
function UrlOf($client) { [W]::Text((Control $client 1003)) }
function Navigate($client, [string]$url) {
    $edit = Control $client 1003
    [void][W]::SendMessage($edit, 0x000C, [IntPtr]::Zero, $url)          # WM_SETTEXT
    [void][W]::PostMessage($edit, 0x0100, [IntPtr]0x0D, [IntPtr]::Zero)  # WM_KEYDOWN VK_RETURN
}
# As in the smoke test: keys held as far as the browser's thread can tell, and none other.
function WithKeys($window, [int[]]$keys, [scriptblock]$action) {
    $owner = [uint32]0
    $thread = [W]::GetWindowThreadProcessId($window, [ref]$owner)
    $me = [W]::GetCurrentThreadId()
    [void][W]::AttachThreadInput($me, $thread, $true)
    try {
        $saved = New-Object byte[] 256
        [void][W]::GetKeyboardState($saved)
        $held = [byte[]]$saved.Clone()
        foreach ($modifier in 0x10, 0x11, 0x12, 0x5B, 0x5C, 0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5) { $held[$modifier] = 0 }
        foreach ($key in $keys) { $held[$key] = 0x80 }
        [void][W]::SetKeyboardState($held)
        try { & $action } finally { [void][W]::SetKeyboardState($saved) }
    } finally {
        [void][W]::AttachThreadInput($me, $thread, $false)
    }
}
$VK_SHIFT = 0x10; $VK_CONTROL = 0x11
# A key pressed in a browser window with modifiers held. The script block sees the variables
# of WithKeys before this function's, so the key mustn't be called $key there.
function PressKeys($client, [int[]]$mods, [int]$vk) {
    WithKeys $client $mods { [void][W]::SendMessageP($client, 0x0100, [IntPtr]$vk, [IntPtr]::Zero) }  # WM_KEYDOWN
}
# Waits until the tab in view has stopped loading for a second: its reload button says reload,
# not stop. A tab given its pages has its title from the start, and only takes Back and
# Forward once it has made them its history and loaded the page it shows.
$reloadGlyph = [string][char]0x21BA
function Settle($client) {
    $quiet = 0
    $until = (Get-Date).AddSeconds(20)
    while ($quiet -lt 4 -and (Get-Date) -lt $until) {
        if ([W]::Text((Control $client 1005)) -eq $reloadGlyph) { $quiet++ } else { $quiet = 0 }
        Start-Sleep -Milliseconds 250
    }
}
# Back (1001) or Forward (1002) with no key held, once the tab is ready for it.
function Click($client, [int]$id) {
    Settle $client
    WithKeys $client @() { [void][W]::SendMessageP($client, 0x0111, [IntPtr]$id, (Control $client $id)) }  # WM_COMMAND
}
function NewTab($client) {
    [void][W]::PostMessage($client, 0x0111, [IntPtr]1101, [IntPtr]::Zero)  # WM_COMMAND kIdNewTab
}
function ClickTab($button) {
    [void][W]::PostMessage($button, 0x0201, [IntPtr]1, [IntPtr]0x000A000A)  # WM_LBUTTONDOWN at 10,10
    [void][W]::PostMessage($button, 0x0202, [IntPtr]0, [IntPtr]0x000A000A)  # WM_LBUTTONUP
    Start-Sleep -Milliseconds 300
}
function PlaceOf($frame) { $r = New-Object W+RECT; [void][W]::GetWindowRect($frame, [ref]$r); $r }
# The browser window whose top left corner is there. Frames are listed from the front, so
# the first of them is only the first window while it is the only one.
function FrameAt($procId, [int]$left, [int]$top) {
    @(Frames $procId | Where-Object { $r = PlaceOf $_; $r.L -eq $left -and $r.T -eq $top })[0]
}
function StartWinamp {
    $process = Start-Process "$wa\winamp.exe" -ArgumentList "/CLASS=$Instance" -PassThru
    [void]$process.Handle
    $process
}
function QuitWinamp($process) {
    if ($process.HasExited) { return }
    $main = @([W]::TopLevel([uint32]$process.Id) | Where-Object { [W]::Class($_) -eq $Instance })
    if ($main.Count) { [void][W]::PostMessage($main[0], 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) }  # WM_CLOSE
    if (-not $process.WaitForExit(30000)) { Check $false "Winamp exits within 30 s; killing it"; $process.Kill(); $process.WaitForExit() }
}
$sessionFile = "$wa\Plugins\exodium_data\session.txt"
function SessionLines { if (Test-Path $sessionFile) { @([IO.File]::ReadAllLines($sessionFile)) } else { @() } }
function WindowLines { @(SessionLines | Where-Object { $_ -like "window`t*" }) }

# Pages that say in their titles how they were loaded.
$pages = Join-Path $ScratchDir "pages"
New-Item -ItemType Directory -Force $pages | Out-Null
foreach ($name in "S", "T", "U", "V") {
    foreach ($n in 0..2) {
        Set-Content "$pages\$name$n.html" ("<!doctype html><title>$name $n</title><script>document.title += ' (' + performance.getEntriesByType('navigation')[0].type + ')'</script><p>$name $n</p>") -Encoding ascii
    }
}
$page = { param([string]$name, [int]$n) ([Uri]"$pages\$name$n.html").AbsoluteUri }
# Goes to the pages name 0 to last in the active tab of a window.
function Visit($frame, [string]$name, [int]$last) {
    $client = ClientOf $frame
    foreach ($n in 0..$last) {
        Navigate $client (& $page $name $n)
        [void](WaitFor { [W]::Title($frame) -eq "$name $n (navigate)" } 20)
    }
}

"=== session test: $PluginPath ==="
# No history, profile or session of an earlier run.
Remove-Item "$wa\Plugins\exodium_data" -Recurse -Force -ErrorAction SilentlyContinue

# ---- the first run: windows and tabs with pages behind and ahead of them ----
$p = StartWinamp
$procId = [uint32]$p.Id
$secondPlace = $null
try {
    Check (WaitFor { (Frames $procId).Count -ge 1 } 30) "Winamp starts with the plugin"
    $first = (Frames $procId)[0]
    $firstClient = ClientOf $first
    Check (-not [W]::IsWindowVisible($first)) "with no session kept, the first window starts hidden as before"
    [void][W]::ShowWindowAsync($first, 8)  # SW_SHOWNA
    Check (WaitFor { [W]::IsWindowVisible($first) } 5) "the first window shows"
    Start-Sleep -Seconds 2

    # Its first tab: S 0, S 1, S 2 and back to S 1, so a page is ahead of it too.
    Visit $first "S" 2
    Click $firstClient 1001
    Check (WaitFor { [W]::Title($first) -eq "S 1 (back_forward)" } 20) "a tab with pages behind and ahead of it: '$([W]::Title($first))'"
    # A second tab: T 0, T 1.
    NewTab $firstClient
    [void](WaitFor { (TabButtons $firstClient).Count -eq 2 } 10)
    Start-Sleep -Seconds 2
    Visit $first "T" 1
    Check ([W]::Title($first) -eq "T 1 (navigate)") "and a second tab: '$([W]::Title($first))'"
    # The first tab in view again.
    ClickTab (TabButtons $firstClient)[0]
    Check (WaitFor { [W]::Title($first) -eq "S 1 (back_forward)" } 10) "the first tab in view again"

    # A second window, moved somewhere of its own: U 0 to U 2.
    PressKeys $firstClient @($VK_CONTROL) 0x4E  # Ctrl+N
    Check (WaitFor { (Frames $procId).Count -eq 2 } 10) "Ctrl+N opens a second window"
    $second = @(Frames $procId | Where-Object { $_ -ne $first })[0]
    Start-Sleep -Seconds 2
    [void][W]::SetWindowPos($second, [IntPtr]::Zero, 300, 200, 900, 638, 0x0014)  # SWP_NOZORDER | SWP_NOACTIVATE
    Visit $second "U" 2
    Check ([W]::Title($second) -eq "U 2 (navigate)") "with pages of its own: '$([W]::Title($second))'"
    $secondPlace = PlaceOf $second

    # A third window, closed: V 0, V 1.
    PressKeys (ClientOf $second) @($VK_CONTROL) 0x4E
    Check (WaitFor { (Frames $procId).Count -eq 3 } 10) "and a third"
    $third = @(Frames $procId | Where-Object { $_ -ne $first -and $_ -ne $second })[0]
    Start-Sleep -Seconds 2
    Visit $third "V" 1
    NewTab (ClientOf $third)
    Start-Sleep -Seconds 2
    $thirdPlace = PlaceOf $third
    [void][W]::PostMessage($third, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)  # WM_CLOSE
    Check (WaitFor { (Frames $procId).Count -eq 2 } 10) "the third window closes"

    Check (WaitFor { (WindowLines).Count -eq 2 -and @(SessionLines | Where-Object { $_ -like "closed-window`t*" }).Count -eq 1 } 10) "within seconds the session file has both windows and the closed one: $((SessionLines) -join ' | ')"
} catch { Check $false "the run went to its end: $_ at line $($_.InvocationInfo.ScriptLineNumber)" } finally {
    QuitWinamp $p
}
Check ($p.ExitCode -eq 0) ("Winamp exited cleanly: exit code 0x{0:X8}" -f $p.ExitCode)
$atExit = WindowLines
Check ($atExit.Count -eq 2 -and @($atExit | Where-Object { $_ -like "window`tnormal`t*" }).Count -eq 2) "as Winamp closed, the file kept both windows as on show: $($atExit -join ' | ')"
$secondLine = @($atExit | Where-Object { $_ -like "window`tnormal`t300`t200`t*" })
Check ($secondLine.Count -eq 1) "and the second window's place"

# ---- the second run: they come back ----
$p = StartWinamp
$procId = [uint32]$p.Id
try {
    Check (WaitFor { (Shown $procId).Count -eq 2 } 30) "Winamp starting brings both windows back on show: $((Shown $procId).Count)"
    $first = FrameAt $procId 100 90
    $second = FrameAt $procId $secondPlace.L $secondPlace.T
    Check ($first -and $second) "one where the first window was, one where the second was"
    Check ((Frames $procId)[0] -eq $second) "the one that was in front, the second, in front again"
    $firstClient = ClientOf $first
    $secondClient = ClientOf $second
    Check ((TabButtons $firstClient).Count -eq 2) "the first window has its two tabs: '$((TabTexts $firstClient) -join "' | '")'"
    Check (WaitFor { [W]::Title($first) -eq "S 1 (back_forward)" } 30) "the tab that was in view is in view, on its page, loaded for real: '$([W]::Title($first))'"
    Check ((UrlOf $firstClient) -eq (& $page "S" 1)) "its address: '$(UrlOf $firstClient)'"
    Check ((TabTexts $firstClient)[1] -eq "T 1 (navigate)") "the other tab has its page's title before it is looked at: '$((TabTexts $firstClient)[1])'"
    Click $firstClient 1001
    Check (WaitFor { [W]::Title($first) -eq "S 0 (back_forward)" } 20) "Back leads where it did before Winamp closed: '$([W]::Title($first))'"
    Click $firstClient 1002
    [void](WaitFor { [W]::Title($first) -eq "S 1 (back_forward)" } 20)
    Click $firstClient 1002
    Check (WaitFor { [W]::Title($first) -eq "S 2 (back_forward)" } 20) "and Forward on to the page that was ahead of it: '$([W]::Title($first))'"

    ClickTab (TabButtons $firstClient)[1]
    Check (WaitFor { [W]::Title($first) -like "T 1 (*" -and [W]::Title($first) -ne "T 1 (navigate)" } 20) "the other tab loads its page once looked at: '$([W]::Title($first))'"
    Click $firstClient 1001
    Check (WaitFor { [W]::Title($first) -eq "T 0 (back_forward)" } 20) "with its Back as well: '$([W]::Title($first))'"

    $r = PlaceOf $second
    Check ($r.R -eq $secondPlace.R -and $r.B -eq $secondPlace.B) "the second window is the size it was: ($($r.L),$($r.T))-($($r.R),$($r.B))"
    Check (WaitFor { [W]::Title($second) -like "U 2 (*" } 30) "on its page: '$([W]::Title($second))'"
    Click $secondClient 1001
    Check (WaitFor { [W]::Title($second) -eq "U 1 (back_forward)" } 20) "with its Back: '$([W]::Title($second))'"

    # The window closed in the last run comes back whole with Ctrl+Shift+T.
    PressKeys $firstClient @($VK_CONTROL, $VK_SHIFT) 0x54  # Ctrl+Shift+T
    Check (WaitFor { (Shown $procId).Count -eq 3 } 10) "Ctrl+Shift+T brings back the window closed before Winamp closed"
    $third = @(Frames $procId | Where-Object { $_ -ne $first -and $_ -ne $second })[0]
    if ($third) {
        $thirdClient = ClientOf $third
        $r = PlaceOf $third
        Check ($r.L -eq $thirdPlace.L -and $r.T -eq $thirdPlace.T) "where it was: ($($r.L),$($r.T))"
        Check (WaitFor { (TabButtons $thirdClient).Count -eq 2 } 10) "with both its tabs: '$((TabTexts $thirdClient) -join "' | '")'"
        ClickTab (TabButtons $thirdClient)[0]
        Check (WaitFor { [W]::Title($third) -like "V 1 (*" } 20) "the first on its page: '$([W]::Title($third))'"
        Click $thirdClient 1001
        Check (WaitFor { [W]::Title($third) -eq "V 0 (back_forward)" } 20) "with its Back: '$([W]::Title($third))'"
        [void][W]::PostMessage($third, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)
        [void](WaitFor { (Frames $procId).Count -eq 2 } 10)
    }

    # A window closed now comes back whole, history and all.
    [void][W]::PostMessage($second, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)
    Check (WaitFor { (Frames $procId).Count -eq 1 } 10) "the second window closes"
    PressKeys $firstClient @($VK_CONTROL, $VK_SHIFT) 0x54
    Check (WaitFor { (Shown $procId).Count -eq 2 } 10) "Ctrl+Shift+T brings it back"
    $second = @(Frames $procId | Where-Object { $_ -ne $first })[0]
    if ($second) {
        $secondClient = ClientOf $second
        $r = PlaceOf $second
        Check ($r.L -eq $secondPlace.L -and $r.T -eq $secondPlace.T) "where it was: ($($r.L),$($r.T))"
        Check (WaitFor { [W]::Title($second) -like "U 1 (*" } 20) "on the page it was on: '$([W]::Title($second))'"
        Click $secondClient 1002
        Check (WaitFor { [W]::Title($second) -eq "U 2 (back_forward)" } 20) "with its Forward: '$([W]::Title($second))'"
        Click $secondClient 1001
        [void](WaitFor { [W]::Title($second) -like "U 1 (*" } 20)
        Click $secondClient 1001
        Check (WaitFor { [W]::Title($second) -eq "U 0 (back_forward)" } 20) "and its Back: '$([W]::Title($second))'"
    }

    # The first window closed, then shown again from Winamp's menu as it is: a blank tab,
    # which Ctrl+Shift+T fills with its tabs.
    [void][W]::PostMessage($first, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)
    Check (WaitFor { -not [W]::IsWindowVisible($first) -and (TabButtons $firstClient).Count -eq 1 } 10) "the first window closes to a blank tab"
    [void][W]::ShowWindowAsync($first, 8)
    [void](WaitFor { [W]::IsWindowVisible($first) } 5)
    Start-Sleep -Seconds 2
    $framesBefore = (Frames $procId).Count
    PressKeys $firstClient @($VK_CONTROL, $VK_SHIFT) 0x54
    Check (WaitFor { (TabButtons $firstClient).Count -eq 2 } 10) "shown again, Ctrl+Shift+T puts its tabs back in it: '$((TabTexts $firstClient) -join "' | '")'"
    Check ((Frames $procId).Count -eq $framesBefore) "in place of its blank tab, with no window of their own"
    Check (WaitFor { [W]::Title($first) -like "T 0 (*" } 20) "the tab that was in view in view: '$([W]::Title($first))'"
    Click $firstClient 1002
    Check (WaitFor { [W]::Title($first) -eq "T 1 (back_forward)" } 20) "with its Forward: '$([W]::Title($first))'"

    # A tab closed on its own comes back with its Back and Forward too, its webview kept meanwhile.
    NewTab $firstClient
    [void](WaitFor { (TabButtons $firstClient).Count -eq 3 } 10)
    Start-Sleep -Seconds 2
    Visit $first "V" 2
    Click $firstClient 1006  # kIdCloseTab
    [void](WaitFor { (TabButtons $firstClient).Count -eq 2 } 10)
    PressKeys $firstClient @($VK_CONTROL, $VK_SHIFT) 0x54
    Check (WaitFor { (TabButtons $firstClient).Count -eq 3 -and [W]::Title($first) -like "V 2 (*" } 20) "a closed tab comes back: '$([W]::Title($first))'"
    Click $firstClient 1001
    Check (WaitFor { [W]::Title($first) -eq "V 1 (back_forward)" } 20) "with its Back: '$([W]::Title($first))'"

    # Left with the second maximized and the first minimized.
    [void][W]::ShowWindowAsync($second, 3)  # SW_MAXIMIZE
    [void](WaitFor { [W]::IsZoomed($second) } 5)
    [void][W]::ShowWindowAsync($first, 6)   # SW_MINIMIZE
    [void](WaitFor { [W]::IsIconic($first) } 5)
    Start-Sleep -Seconds 3
} catch { Check $false "the run went to its end: $_ at line $($_.InvocationInfo.ScriptLineNumber)" } finally {
    QuitWinamp $p
}
Check ($p.ExitCode -eq 0) ("Winamp exited cleanly: exit code 0x{0:X8}" -f $p.ExitCode)
Check ((WindowLines).Count -eq 2 -and @(WindowLines | Where-Object { $_ -like "window`tminimized`t100`t90`t*" }).Count -eq 1 -and
       @(WindowLines | Where-Object { $_ -like "window`tmaximized`t$($secondPlace.L)`t$($secondPlace.T)`t*" }).Count -eq 1) "the file kept both windows again, minimized and maximized, at the places they go back to: $((WindowLines) -join ' | ')"

# ---- the third run, started from the Start menu's entry: no window of its own on top ----
if (Test-Path "$wa\Plugins\exodium_open.exe") {
    $open = Start-Process "$wa\Plugins\exodium_open.exe" -ArgumentList "--class=$Instance" -PassThru
    [void]$open.Handle
    $running = { @(Get-Process winamp -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq "$wa\winamp.exe" }) }
    Check (WaitFor { (& $running).Count -ge 1 } 15) "exodium_open.exe with no link starts Winamp"
    $p = @(& $running)[0]
    if ($p) {
        [void]$p.Handle
        $procId = [uint32]$p.Id
        try {
            Check ($open.WaitForExit(40000) -and $open.ExitCode -eq 0) "and is handed over"
            Check (WaitFor { (Shown $procId).Count -eq 2 } 30) "the windows come back"
            Start-Sleep -Seconds 3
            Check ((Frames $procId).Count -eq 2) "and stand in for the window on the home page that was asked for: $((Frames $procId).Count) windows"
            $zoomed = @(Frames $procId | Where-Object { [W]::IsZoomed($_) })
            $iconic = @(Frames $procId | Where-Object { [W]::IsIconic($_) })
            Check ($zoomed.Count -eq 1 -and $iconic.Count -eq 1) "one maximized and one minimized, as they were left: $($zoomed.Count), $($iconic.Count)"
            Check ($zoomed.Count -eq 1 -and (Frames $procId)[0] -eq $zoomed[0]) "the maximized one, which was in front, in front"
            foreach ($window in $zoomed + $iconic) { [void][W]::ShowWindowAsync($window, 9) }  # SW_RESTORE
            Check (WaitFor { -not (@(Frames $procId | Where-Object { [W]::IsZoomed($_) -or [W]::IsIconic($_) })) } 5) "and both can be restored"
            Start-Sleep -Milliseconds 500
            Check ((FrameAt $procId $secondPlace.L $secondPlace.T) -and (FrameAt $procId 100 90)) "to the places they had"
        } catch { Check $false "the run went to its end: $_ at line $($_.InvocationInfo.ScriptLineNumber)" } finally {
            QuitWinamp $p
        }
    }
} else { "  SKIP  started from the Start menu's entry: no exodium_open.exe beside the plugin" }

# ---- the fourth run, with the setting off: nothing comes back by itself, Ctrl+Shift+T brings it ----
[void][W]::WritePrivateProfileString("Exodium", "restore_windows", "0", "$ini\winamp.ini")
$p = StartWinamp
$procId = [uint32]$p.Id
try {
    Check (WaitFor { (Frames $procId).Count -ge 1 } 30) "Winamp starts"
    Start-Sleep -Seconds 12  # past the time it can take to start, after which windows come up
    Check ((Shown $procId).Count -eq 0 -and (Frames $procId).Count -eq 1) "with the setting off, no window comes back by itself"
    $first = (Frames $procId)[0]
    $firstClient = ClientOf $first
    [void][W]::ShowWindowAsync($first, 8)
    [void](WaitFor { [W]::IsWindowVisible($first) } 5)
    Start-Sleep -Seconds 2
    PressKeys $firstClient @($VK_CONTROL, $VK_SHIFT) 0x54
    PressKeys $firstClient @($VK_CONTROL, $VK_SHIFT) 0x54
    $titles = { @(Frames $procId | ForEach-Object { TabTexts (ClientOf $_) }) -join " | " }
    Check (WaitFor { (Shown $procId).Count -eq 2 -and (& $titles) -match "U " -and (& $titles) -match "S " } 20) "Ctrl+Shift+T brings back the windows open when Winamp closed: $(& $titles)"
} catch { Check $false "the run went to its end: $_ at line $($_.InvocationInfo.ScriptLineNumber)" } finally {
    QuitWinamp $p
}
Check ($p.ExitCode -eq 0) ("Winamp exited cleanly: exit code 0x{0:X8}" -f $p.ExitCode)

"=== session test: $($script:failures) failure(s) ==="
exit $script:failures
