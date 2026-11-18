<#
.SYNOPSIS
    Tests the DevTools docked in a browser window: F12 and the other keys, the strip's
    buttons, the splitter, tabs with DevTools of their own, "Inspect" in a page's menu,
    and what winamp.ini keeps of them.

.DESCRIPTION
    Runs gen_browser.dll in a scratch copy of Winamp, as tools\smoke-test.ps1 does, on a
    desktop of its own that Windows never shows, so that nothing comes up in front of
    whoever is at the machine. It has a scratch folder, a window class and a debugging
    port of its own, so it can run beside a smoke test and beside the Winamp in use.

    The debugging port is on throughout: docked DevTools are served by it. What the
    DevTools themselves show is asked of their page over that port, as any tool would.

    The exit code is the number of checks that failed.

.EXAMPLE
    tools\devtools-test.ps1
#>
param(
    [string]$PluginPath = (Join-Path $PSScriptRoot "..\bin\Win32\Release\gen_browser.dll"),
    [string]$WinampDir = (Join-Path ([Environment]::GetFolderPath("MyDocuments")) "Winamp"),
    [string]$SettingsDir = (Join-Path $env:APPDATA "Winamp"),
    [string]$Instance = "ExodiumDevToolsTest",
    [string]$ScratchDir = (Join-Path $env:TEMP "exodium-devtools-test"),
    # Not the port in real use, nor the smoke test's.
    [int]$DebugPort = 9336,
    # A folder to save pictures of the window in, with its DevTools on each side: how the strip
    # and the splitter look. What the webviews show isn't in them on a desktop that isn't shown.
    [string]$ShotDir = "",
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
    $log = Join-Path ([IO.Path]::GetTempPath()) "exodium-devtools-test-$Instance.log"
    Remove-Item $log -ErrorAction SilentlyContinue
    $quote = { param($text) "'" + ($text -replace "'", "''") + "'" }
    $arguments = "-OnOwnDesktop -PluginPath $(& $quote (Resolve-Path $PluginPath).Path) -WinampDir $(& $quote $WinampDir) " +
                 "-SettingsDir $(& $quote $SettingsDir) -ScratchDir $(& $quote $ScratchDir) -Instance $(& $quote $Instance) -DebugPort $DebugPort"
    if ($ShotDir) { $arguments += " -ShotDir $(& $quote $ShotDir)" }
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
# The browser starts with no profile, so that the DevTools are as they are the first time.
Remove-Item "$wa\Plugins\exodium_data" -Recurse -Force -ErrorAction SilentlyContinue

# A modest window place, none of the settings of earlier runs, and the debugging port on.
# Bitwarden is left out, as in the smoke test: it would be fetched from the Chrome Web
# Store. The windows of the last run don't come back, so each run starts with the one.
$iniText = [System.IO.File]::ReadAllText("$ini\winamp.ini")
$iniText = [regex]::Replace($iniText, '(?ms)^\[EmbedWndExample\]\r?\n.*?(?=^\[|\z)', "[EmbedWndExample]`r`nxpos=100`r`nypos=90`r`nwidth=900`r`nheight=638`r`n")
$iniText = [regex]::Replace($iniText, '(?ms)^\[Exodium\]\r?\n.*?(?=^\[|\z)', "")
$iniText = $iniText.TrimEnd() + "`r`n[Exodium]`r`nbitwarden=0`r`nrestore_windows=0`r`nremote_debugging_port=$DebugPort`r`n"
[System.IO.File]::WriteAllText("$ini\winamp.ini", $iniText)

Add-Type -ReferencedAssemblies System.Drawing @"
using System;
using System.Text;
using System.Collections.Generic;
using System.Runtime.InteropServices;
public static class W {
    [DllImport("user32.dll")] static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags);
    // A picture of a window, saved as a PNG.
    public static void Shot(IntPtr h, string path) {
        RECT r; GetWindowRect(h, out r);
        using (var picture = new System.Drawing.Bitmap(r.R - r.L, r.B - r.T, System.Drawing.Imaging.PixelFormat.Format24bppRgb)) {
            using (var graphics = System.Drawing.Graphics.FromImage(picture)) {
                IntPtr dc = graphics.GetHdc();
                PrintWindow(h, dc, 2);  // PW_RENDERFULLCONTENT
                graphics.ReleaseHdc(dc);
            }
            picture.Save(path, System.Drawing.Imaging.ImageFormat.Png);
        }
    }
    public delegate bool EnumProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr p, EnumProc cb, IntPtr l);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern bool IsWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool ShowWindowAsync(IntPtr h, int cmd);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr SendMessage(IntPtr h, uint m, IntPtr w, string l);
    [DllImport("user32.dll", CharSet=CharSet.Unicode, EntryPoint="SendMessageW")] public static extern IntPtr SendMessageSb(IntPtr h, uint m, IntPtr w, StringBuilder l);
    [DllImport("user32.dll", EntryPoint="SendMessageW")] public static extern IntPtr SendMessageP(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern IntPtr GetParent(IntPtr h);
    [DllImport("user32.dll")] public static extern int GetDlgCtrlID(IntPtr h);
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode)] public static extern uint GetPrivateProfileString(string section, string key, string fallback, StringBuilder value, uint size, string file);
    [DllImport("user32.dll")] public static extern bool AttachThreadInput(uint attach, uint attachTo, bool on);
    [DllImport("kernel32.dll")] public static extern uint GetCurrentThreadId();
    [DllImport("user32.dll")] public static extern bool GetKeyboardState(byte[] state);
    [DllImport("user32.dll")] public static extern bool SetKeyboardState(byte[] state);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
    public static string Class(IntPtr h) { var s = new StringBuilder(256); GetClassName(h, s, 256); return s.ToString(); }
    public static string Title(IntPtr h) { var s = new StringBuilder(1024); GetWindowText(h, s, 1024); return s.ToString(); }
    public static string Text(IntPtr h) { var s = new StringBuilder(4096); SendMessageSb(h, 0x000D, (IntPtr)4096, s); return s.ToString(); }
    public static string Ini(string file, string key) { var s = new StringBuilder(256); GetPrivateProfileString("Exodium", key, "", s, 256, file); return s.ToString(); }
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
function LParam([int]$x, [int]$y) { [IntPtr](($y -shl 16) -bor ($x -band 0xFFFF)) }
function ClientOf($frame) {
    foreach ($c in [W]::Children($frame)) { if ([W]::Class($c) -eq "ExodiumClient") { return $c } }
    return [IntPtr]::Zero
}
function Frames($procId) {
    @([W]::TopLevel($procId) | Where-Object { [W]::Class($_) -eq "Winamp Gen" -and (ClientOf $_) -ne [IntPtr]::Zero })
}
function Control($client, [int]$id) {
    foreach ($c in [W]::Children($client)) { if ([W]::GetParent($c) -eq $client -and [W]::GetDlgCtrlID($c) -eq $id) { return $c } }
    return [IntPtr]::Zero
}
$TabBase = 0x8000 + 43
function TabButtons($client) {
    @([W]::Children($client) | Where-Object { [W]::GetParent($_) -eq $client -and [W]::GetDlgCtrlID($_) -ge $TabBase -and [W]::GetDlgCtrlID($_) -lt ($TabBase + 4096) } | Sort-Object { [W]::GetDlgCtrlID($_) })
}
function TabByText($client, [string]$text) { @(TabButtons $client | Where-Object { [W]::Text($_) -eq $text })[0] }
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
$VK_SHIFT = 0x10; $VK_CONTROL = 0x11; $VK_F12 = 0x7B
# A key pressed in a browser window with modifiers held. The script block sees the variables
# of WithKeys before this function's, so the key mustn't be called $key there.
function PressKeys($client, [int[]]$mods, [int]$vk) {
    WithKeys $client $mods { [void][W]::SendMessageP($client, 0x0100, [IntPtr]$vk, [IntPtr]::Zero) }  # WM_KEYDOWN
}
function ClickTab($button) {
    [void][W]::PostMessage($button, 0x0201, [IntPtr]1, (LParam 10 10))  # WM_LBUTTONDOWN
    [void][W]::PostMessage($button, 0x0202, [IntPtr]0, (LParam 10 10))  # WM_LBUTTONUP
    Start-Sleep -Milliseconds 300
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
function PortOpen { [bool](Get-NetTCPConnection -LocalPort $DebugPort -State Listen -ErrorAction SilentlyContinue) }

# ---- where things are in a window ----

# A window's place inside the browser window's client, as "left,top,right,bottom"; "hidden"
# for one that isn't on show.
function PlaceIn($client, $window) {
    if (-not $window -or $window -eq [IntPtr]::Zero -or -not [W]::IsWindowVisible($window)) { return $null }
    $c = New-Object W+RECT; $r = New-Object W+RECT
    [void][W]::GetWindowRect($client, [ref]$c); [void][W]::GetWindowRect($window, [ref]$r)
    [pscustomobject]@{ L = $r.L - $c.L; T = $r.T - $c.T; R = $r.R - $c.L; B = $r.B - $c.T; W = $r.R - $r.L; H = $r.B - $r.T }
}
function Show($place) { if ($place) { "$($place.L),$($place.T),$($place.R),$($place.B)" } else { "hidden" } }
# The webviews on show in a window: each is a window of the browser's own, right under the client.
function Views($client) {
    @([W]::Children($client) | Where-Object { [W]::GetParent($_) -eq $client -and [W]::Class($_) -like "Chrome_WidgetWin*" -and [W]::IsWindowVisible($_) })
}
function AllViews($client) {
    @([W]::Children($client) | Where-Object { [W]::GetParent($_) -eq $client -and [W]::Class($_) -like "Chrome_WidgetWin*" })
}
# What a window shows of its DevTools: the strip, the splitter, the DevTools' own webview
# (the one right under the strip) and the page's (the other one).
function Layout($client) {
    $bar = PlaceIn $client (Control $client 1202)
    $splitter = PlaceIn $client (Control $client 1203)
    $views = @(Views $client | ForEach-Object { PlaceIn $client $_ })
    $panel = if ($bar) { @($views | Where-Object { $_.T -eq $bar.B -and $_.L -eq $bar.L -and $_.R -eq $bar.R })[0] } else { $null }
    $page = @($views | Where-Object { $_ -ne $panel })[0]
    [pscustomobject]@{ Bar = $bar; Splitter = $splitter; Panel = $panel; Page = $page; Views = $views.Count }
}
# The webview at a place in a window, which keys for it are posted to.
function ViewAt($client, $place) {
    @(Views $client | Where-Object { (Show (PlaceIn $client $_)) -eq (Show $place) })[0]
}
function Shot($frame, [string]$name) {
    if (-not $ShotDir) { return }
    New-Item -ItemType Directory -Force $ShotDir | Out-Null
    Start-Sleep -Milliseconds 500
    [W]::Shot($frame, (Join-Path $ShotDir "$name.png"))
}
# Gives the DevTools in view the keyboard, by a click in their middle (on their console,
# there is nothing there for a click to set off), and says which window of their webview
# takes its keys: the browser's own inside it.
function ClickIntoTools($client) {
    $panel = (Layout $client).Panel
    $view = ViewAt $client $panel
    $widget = @([W]::Children($view) | Where-Object { [W]::Class($_) -eq "Chrome_RenderWidgetHostHWND" })[0]
    $middle = LParam ([int]($panel.W / 2)) ([int]($panel.H / 2))
    [void][W]::PostMessage($widget, 0x0201, [IntPtr]1, $middle)  # WM_LBUTTONDOWN
    [void][W]::PostMessage($widget, 0x0202, [IntPtr]0, $middle)  # WM_LBUTTONUP
    Start-Sleep -Seconds 1
    @([W]::Children($view) | Where-Object { [W]::Class($_) -eq "Chrome_WidgetWin_1" })[0]
}
# A key pressed in a webview, with these other keys held and no others, as far as both the
# webview's own thread and the browser window's can tell: the first decides whether the key
# is one to pass on, and the plugin reads the keys held on the second. They are held for
# long enough for the key to have been passed on.
function PressInView($client, $view, [int]$vk, [int[]]$mods = @()) {
    WithKeys $client $mods {
        WithKeys $view $mods {
            [void][W]::PostMessage($view, 0x0100, [IntPtr]$vk, [IntPtr]1)                          # WM_KEYDOWN
            [void][W]::PostMessage($view, 0x0101, [IntPtr]$vk, [IntPtr]([int]0xC0000001 -bor 0))   # WM_KEYUP
            Start-Sleep -Milliseconds 800
        }
    }
}
function ShowLayout($layout) {
    "page $(Show $layout.Page), splitter $(Show $layout.Splitter), strip $(Show $layout.Bar), DevTools $(Show $layout.Panel), $($layout.Views) webview(s) on show"
}
# The strip's buttons, counted from its right end: close, a window of their own, and the
# three sides (devtools_layout.h).
$BarClose = 0; $BarOwnWindow = 1; $BarRight = 2; $BarBottom = 3; $BarLeft = 4
function ClickBar($client, [int]$fromRight) {
    $bar = Control $client 1202
    $r = New-Object W+RECT; [void][W]::GetWindowRect($bar, [ref]$r)
    $at = LParam (($r.R - $r.L) - 2 - $fromRight * 22 - 10) 12
    [void][W]::PostMessage($bar, 0x0201, [IntPtr]1, $at)  # WM_LBUTTONDOWN
    [void][W]::PostMessage($bar, 0x0202, [IntPtr]0, $at)  # WM_LBUTTONUP
}
# Drags the splitter by a point 2,2 inside it, that far.
function DragSplitter($client, [int]$dx, [int]$dy) {
    $splitter = Control $client 1203
    [void][W]::PostMessage($splitter, 0x0201, [IntPtr]1, (LParam 2 2))                  # WM_LBUTTONDOWN
    [void][W]::PostMessage($splitter, 0x0200, [IntPtr]1, (LParam (2 + $dx) (2 + $dy)))  # WM_MOUSEMOVE
    Start-Sleep -Milliseconds 300
    # Where it is let go is nothing to it: it has moved under the mouse by then.
    [void][W]::PostMessage($splitter, 0x0202, [IntPtr]0, (LParam 2 2))                  # WM_LBUTTONUP
}

# ---- what the debugging port says ----

$debug = "http://127.0.0.1:$DebugPort"
# In brackets, or Windows PowerShell hands the whole list down the pipe as one item.
function Pages { @((Invoke-RestMethod "$debug/json/list" -TimeoutSec 5) | Where-Object { $_.type -eq "page" }) }
function DevToolsPages { @(Pages | Where-Object { $_.url -like "$debug/devtools/devtools_app.html*" }) }
# Counted here: one object on its own has no Count in Windows PowerShell.
function DevToolsCount { @(DevToolsPages).Count }
function PageWithUrl([string]$url) { @(Pages | Where-Object { $_.url -eq $url })[0] }
# One question over the DevTools protocol to a socket of the port's, and its answer's result.
function Cdp([string]$socketUrl, [string]$method, $params = @{}) {
    $socket = New-Object System.Net.WebSockets.ClientWebSocket
    $none = [Threading.CancellationToken]::None
    try {
        if (-not $socket.ConnectAsync([Uri]$socketUrl, $none).Wait(5000)) { return $null }
        $text = @{ id = 1; method = $method; params = $params } | ConvertTo-Json -Compress -Depth 6
        $request = [Text.Encoding]::UTF8.GetBytes($text)
        if (-not $socket.SendAsync([ArraySegment[byte]]::new($request), "Text", $true, $none).Wait(5000)) { return $null }
        $buffer = New-Object byte[] 65536
        while ($true) {
            $answer = New-Object IO.MemoryStream
            do {
                $receive = $socket.ReceiveAsync([ArraySegment[byte]]::new($buffer), $none)
                if (-not $receive.Wait(15000)) { return $null }
                $answer.Write($buffer, 0, $receive.Result.Count)
            } until ($receive.Result.EndOfMessage)
            $message = [Text.Encoding]::UTF8.GetString($answer.ToArray()) | ConvertFrom-Json
            if ($message.id -eq 1) { return $message.result }
        }
    } catch {
        return $null
    } finally {
        $socket.Dispose()
    }
}
function CdpBrowser([string]$method) { Cdp (Invoke-RestMethod "$debug/json/version" -TimeoutSec 5).webSocketDebuggerUrl $method }
# What a script comes to in a page the port knows by this id: its value, or $null.
function Evaluate([string]$targetId, [string]$script) {
    $result = Cdp "ws://127.0.0.1:$DebugPort/devtools/page/$targetId" "Runtime.evaluate" @{ expression = $script; awaitPromise = $true; returnByValue = $true }
    if ($result -and $result.result) { $result.result.value } else { $null }
}
# Scripts for the DevTools' own page, which has its modules to be imported again.
$inDevTools = "const at = name => new URL(name, location.href).href; const SDK = await import(at('core/sdk/sdk.js')); const UI = await import(at('ui/legacy/legacy.js'));"
function AskDevTools([string]$targetId, [string]$body) { Evaluate $targetId "(async () => { $inDevTools $body })()" }
# The page they are for, as they have it; nothing until they know of one.
function InspectedUrl([string]$targetId) {
    AskDevTools $targetId "const target = SDK.TargetManager.TargetManager.instance().primaryPageTarget(); return target ? target.inspectedURL() : '';"
}
function ShownTool([string]$targetId) { AskDevTools $targetId "return UI.InspectorView.InspectorView.instance().tabbedPane.selectedTabId;" }
function PickingElement([string]$targetId) {
    AskDevTools $targetId "return UI.ActionRegistry.ActionRegistry.instance().getAction('elements.toggle-element-search').toggled();"
}
# The id of the element selected in the Elements tool, or its tag if it has none.
function SelectedElement([string]$targetId) {
    AskDevTools $targetId "const node = UI.Context.Context.instance().flavor(SDK.DOMModel.DOMNode); return node ? (node.getAttribute('id') || node.nodeName()) : '';"
}

# ---- the pages ----

$pages = Join-Path $ScratchDir "pages"
New-Item -ItemType Directory -Force $pages | Out-Null
# A page longer than the window, with a box of a known place and size far down it.
Set-Content "$pages\first.html" @'
<!doctype html><title>First Page</title>
<style>body { margin: 0; height: 4000px; background: #223 } #target { position: absolute; left: 100px; top: 1400px; width: 300px; height: 200px; background: #c33 }</style>
<h1 id="heading">First</h1><div id="target"><span id="inside">inside the box</span></div>
'@ -Encoding ascii
Set-Content "$pages\second.html" "<!doctype html><title>Second Page</title><p id=only>second</p>" -Encoding ascii
$firstUrl = ([Uri]"$pages\first.html").AbsoluteUri
$secondUrl = ([Uri]"$pages\second.html").AbsoluteUri

"=== DevTools docked in the window ==="
$plugin = Get-Item "$wa\Plugins\gen_browser.dll"
"plugin: $($plugin.Length) bytes, $($plugin.LastWriteTime)"

$p = StartWinamp
$procId = [uint32]$p.Id
try {
    Check (WaitFor { (Frames $procId).Count -ge 1 } 30) "plugin loaded and made its frame"
    $frame = (Frames $procId)[0]
    $client = ClientOf $frame
    [void][W]::ShowWindowAsync($frame, 8)  # SW_SHOWNA
    [void](WaitFor { [W]::IsWindowVisible($frame) -and (Views $client).Count -ge 1 } 60)
    Start-Sleep -Seconds 2
    Navigate $client $firstUrl
    Check (WaitFor { [W]::Title($frame) -eq "First Page" } 30) "a page loads: '$([W]::Title($frame))'"
    Check (WaitFor { PortOpen } 10) "the debugging port is on"
    $firstPage = PageWithUrl $firstUrl
    Check ([bool]$firstPage) "and shows the page"

    $closed = Layout $client
    $area = $closed.Page
    Check ($closed.Views -eq 1 -and -not $closed.Bar -and -not $closed.Splitter) "before they are opened, the page has its place to itself: $(ShowLayout $closed)"
    Check ((DevToolsCount) -eq 0) "and the port has no DevTools page"
    Shot $frame "closed"

    # F12 opens them, docked on the right at two fifths of the page's place.
    [void](Evaluate $firstPage.id "window.exodiumBefore = 1; 0")
    PressKeys $client @() $VK_F12
    Check (WaitFor { (Layout $client).Panel } 30) "F12 opens the DevTools in the window"
    $open = Layout $client
    $wide = [math]::Floor($area.W * 2 / 5)
    Check ($open.Views -eq 2 -and $open.Page.L -eq $area.L -and $open.Page.T -eq $area.T -and $open.Page.B -eq $area.B -and
           $open.Page.R -eq $open.Splitter.L -and $open.Splitter.W -eq 5 -and $open.Splitter.R -eq $open.Bar.L -and
           $open.Bar.R -eq $area.R -and $open.Bar.T -eq $area.T -and $open.Bar.H -eq 24 -and $open.Bar.W -eq $wide -and
           $open.Panel.B -eq $area.B) "on the right of the page, with the splitter between them and their strip above: $(ShowLayout $open)"
    Check ([W]::Text((Control $client 1202)) -eq "DevTools") "the strip says what they are"
    Shot $frame "docked-right"
    Check (WaitFor { (DevToolsCount) -eq 1 } 10) "the port serves them as a page"
    $tools = @(DevToolsPages)[0]
    Check ($tools.url -eq "$debug/devtools/devtools_app.html?ws=127.0.0.1:$DebugPort/devtools/page/$($firstPage.id)") "for the page in the tab: $($tools.url)"
    Check (WaitFor { (InspectedUrl $tools.id) -eq $firstUrl } 30) "and they are connected to it: '$(InspectedUrl $tools.id)'"
    $targets = @((CdpBrowser "Target.getTargets").targetInfos | Where-Object { $_.type -eq "page" })
    $toolsContext = @($targets | Where-Object { $_.targetId -eq $tools.id })[0].browserContextId
    $pageContext = @($targets | Where-Object { $_.targetId -eq $firstPage.id })[0].browserContextId
    Check ($toolsContext -and $pageContext -and $toolsContext -ne $pageContext) "they are in a profile of their own: '$toolsContext', the page in '$pageContext'"
    Check ((Evaluate $firstPage.id "window.exodiumBefore || 0") -eq 1) "the page is as it was, not loaded again for them"
    Check ((Evaluate $firstPage.id "innerWidth * devicePixelRatio") -le ($open.Page.W + 1)) "the page is as wide as what is left of its place: $(Evaluate $firstPage.id 'Math.round(innerWidth * devicePixelRatio)') of $($open.Page.W)"
    # Something of the test's own among what they keep of themselves, to look for after a restart.
    [void](Evaluate $tools.id "localStorage.setItem('exodium-test-marker', 'kept')")

    # The splitter, dragged left by a hundred, makes them that much wider.
    DragSplitter $client -100 0
    Check (WaitFor { (Layout $client).Bar.W -eq $wide + 100 } 5) "dragging the splitter sizes them: $(ShowLayout (Layout $client))"
    $dragged = Layout $client
    Check ($dragged.Page.R -eq $dragged.Splitter.L -and $dragged.Panel -and $dragged.Panel.R -eq $area.R) "and the page with them"
    Check (WaitFor { [W]::Ini("$ini\winamp.ini", "devtools_width") -eq "$($wide + 100)" } 5) "winamp.ini keeps the width: '$([W]::Ini("$ini\winamp.ini", "devtools_width"))'"
    # Past the page's least, they stop.
    DragSplitter $client -3000 0
    Check (WaitFor { (Layout $client).Page.W -eq 120 } 5) "the page keeps some of its place however far the splitter is dragged: $(ShowLayout (Layout $client))"
    DragSplitter $client 3000 0
    Check (WaitFor { (Layout $client).Bar.W -eq 160 } 5) "and so do they: $(ShowLayout (Layout $client))"
    DragSplitter $client -200 0
    [void](WaitFor { (Layout $client).Bar.W -eq 360 } 5)

    # The strip's buttons dock them elsewhere.
    ClickBar $client $BarBottom
    Check (WaitFor { $l = Layout $client; $l.Panel -and $l.Bar.L -eq $area.L -and $l.Bar.R -eq $area.R } 5) "the strip's button docks them at the bottom"
    $bottom = Layout $client
    $high = [math]::Floor($area.H * 2 / 5)
    Check ($bottom.Page.T -eq $area.T -and $bottom.Page.R -eq $area.R -and $bottom.Page.B -eq $bottom.Splitter.T -and $bottom.Splitter.H -eq 5 -and
           $bottom.Splitter.B -eq $bottom.Bar.T -and $bottom.Panel.B -eq $area.B -and ($area.B - $bottom.Bar.T) -eq $high) "under the page, across its width: $(ShowLayout $bottom)"
    Check ([W]::Ini("$ini\winamp.ini", "devtools_dock") -eq "bottom") "winamp.ini keeps the side: '$([W]::Ini("$ini\winamp.ini", "devtools_dock"))'"
    Shot $frame "docked-bottom"
    DragSplitter $client 0 -50
    Check (WaitFor { $l = Layout $client; ($area.B - $l.Bar.T) -eq $high + 50 } 5) "there the splitter sizes them by their height: $(ShowLayout (Layout $client))"
    ClickBar $client $BarLeft
    Check (WaitFor { $l = Layout $client; $l.Panel -and $l.Bar.L -eq $area.L -and $l.Bar.W -eq 360 -and $l.Splitter.L -eq $l.Bar.R -and $l.Page.L -eq $l.Splitter.R -and $l.Page.R -eq $area.R } 5) "on the left, at the width they had on the right: $(ShowLayout (Layout $client))"
    Check ((DevToolsCount) -eq 1 -and @(DevToolsPages)[0].id -eq $tools.id) "they are the same DevTools wherever they are docked"
    Shot $frame "docked-left"
    ClickBar $client $BarRight
    [void](WaitFor { $l = Layout $client; $l.Panel -and $l.Bar.R -eq $area.R -and $l.Bar.W -eq 360 } 5)

    # Ctrl+Shift+J shows the console in the ones that are open, and Ctrl+Shift+C picks an element.
    PressKeys $client @($VK_CONTROL, $VK_SHIFT) 0x4A  # J
    Check (WaitFor { (ShownTool $tools.id) -eq "console" } 10) "Ctrl+Shift+J shows the console: '$(ShownTool $tools.id)'"
    Check ((DevToolsCount) -eq 1) "in the DevTools that were open"
    PressKeys $client @($VK_CONTROL, $VK_SHIFT) 0x43  # C
    Check (WaitFor { (PickingElement $tools.id) -eq $true } 10) "Ctrl+Shift+C has the mouse pick an element"
    PressKeys $client @($VK_CONTROL, $VK_SHIFT) 0x43
    Check (WaitFor { (PickingElement $tools.id) -eq $false } 10) "and pressed again, stops"

    # Each tab has its own, shown with the tab.
    [void][W]::PostMessage($client, 0x0111, [IntPtr]1101, [IntPtr]::Zero)  # WM_COMMAND kIdNewTab
    [void](WaitFor { (TabButtons $client).Count -eq 2 } 10)
    Start-Sleep -Seconds 2
    Navigate $client $secondUrl
    Check (WaitFor { [W]::Title($frame) -eq "Second Page" } 30) "a second tab, on another page"
    $other = Layout $client
    Check ($other.Views -eq 1 -and -not $other.Bar -and -not $other.Splitter -and (Show $other.Page) -eq (Show $area)) "has its place to itself, the first tab's DevTools out of sight: $(ShowLayout $other)"
    Check ((DevToolsCount) -eq 1) "though they are still there"
    PressKeys $client @($VK_CONTROL, $VK_SHIFT) 0x4A  # J
    Check (WaitFor { (DevToolsCount) -eq 2 -and (Layout $client).Panel } 30) "Ctrl+Shift+J opens the second tab's own"
    $secondPage = PageWithUrl $secondUrl
    $tools2 = @(DevToolsPages | Where-Object { $_.id -ne $tools.id })[0]
    Check ($tools2 -and $secondPage -and $tools2.url -eq "$debug/devtools/devtools_app.html?ws=127.0.0.1:$DebugPort/devtools/page/$($secondPage.id)&panel=console") "for that tab's page, on the console: $($tools2.url)"
    Check (WaitFor { (InspectedUrl $tools2.id) -eq $secondUrl -and (ShownTool $tools2.id) -eq "console" } 30) "which they show: '$(ShownTool $tools2.id)' for '$(InspectedUrl $tools2.id)'"
    ClickTab (TabByText $client "First Page")
    Check (WaitFor { $l = Layout $client; $l.Views -eq 2 -and $l.Panel -and [W]::Title($frame) -eq "First Page" } 10) "back on the first tab, its DevTools are in view again: $(ShowLayout (Layout $client))"
    Check ((ShownTool $tools.id) -eq "console") "as they were left: '$(ShownTool $tools.id)'"

    # The strip's close button, and F12 again, close the ones in view only.
    ClickBar $client $BarClose
    Check (WaitFor { (DevToolsCount) -eq 1 -and @(DevToolsPages)[0].id -eq $tools2.id } 10) "the strip's close button closes the DevTools in view, and no others"
    $after = Layout $client
    Check ($after.Views -eq 1 -and -not $after.Bar -and -not $after.Splitter -and (Show $after.Page) -eq (Show $area)) "and the page has its place back: $(ShowLayout $after)"
    PressKeys $client @() $VK_F12
    Check (WaitFor { (DevToolsCount) -eq 2 -and (Layout $client).Panel } 30) "F12 opens them again"
    $tools = @(DevToolsPages | Where-Object { $_.id -ne $tools2.id })[0]
    [void](WaitFor { (InspectedUrl $tools.id) -eq $firstUrl } 30)
    # With the keyboard in the DevTools, their own keys are theirs: F5 loads the page they are
    # for again, not the DevTools. And F12 closes them from there, as it opened them.
    [void](AskDevTools $tools.id "InspectorFrontendAPI.showPanel('console'); return 1;")
    Start-Sleep -Seconds 1
    $toolsKeys = ClickIntoTools $client
    $keysSeen = "window.exodiumKeys ? exodiumKeys.join() : 'gone'"
    $pageMark = "window.exodiumLoaded || 'again'"
    [void](Evaluate $tools.id "window.exodiumKeys = []; addEventListener('keydown', event => exodiumKeys.push(event.key), true); 0")
    [void](Evaluate $firstPage.id "window.exodiumLoaded = 'before'; 0")
    PressInView $client $toolsKeys 0x74  # F5
    Check (WaitFor { (Evaluate $tools.id $keysSeen) -eq "F5" } 5) "a key pressed in the DevTools reaches them, and their own page stays: '$(Evaluate $tools.id $keysSeen)'"
    Check (WaitFor { (Evaluate $firstPage.id $pageMark) -eq "again" } 10) "F5 there loads the page they are for again: '$(Evaluate $firstPage.id $pageMark)'"
    # Ctrl with + makes them larger, which is their webview's zoom, and Ctrl+0 normal again.
    $toolsScale = Evaluate $tools.id "devicePixelRatio"
    PressInView $client $toolsKeys 0xBB @($VK_CONTROL)  # VK_OEM_PLUS
    Check (WaitFor { [W]::Ini("$ini\winamp.ini", "devtools_zoom") -eq "110" } 5) "Ctrl with + in the DevTools makes them larger, which winamp.ini keeps: '$([W]::Ini("$ini\winamp.ini", "devtools_zoom"))'"
    Check (WaitFor { [math]::Abs((Evaluate $tools.id "devicePixelRatio") / $toolsScale - 1.1) -lt 0.01 } 5) "by a tenth: $(Evaluate $tools.id 'devicePixelRatio') from $toolsScale"
    PressInView $client $toolsKeys 0x30 @($VK_CONTROL)  # 0
    Check (WaitFor { [W]::Ini("$ini\winamp.ini", "devtools_zoom") -eq "100" -and [math]::Abs((Evaluate $tools.id "devicePixelRatio") - $toolsScale) -lt 0.01 } 5) "and Ctrl+0 puts them back: '$([W]::Ini("$ini\winamp.ini", "devtools_zoom"))', $(Evaluate $tools.id 'devicePixelRatio')"
    PressInView $client $toolsKeys $VK_F12
    Check (WaitFor { (DevToolsCount) -eq 1 -and -not (Layout $client).Bar } 10) "and F12 there closes them"
    # Closed from the window if that didn't, for the checks that follow.
    if ((Layout $client).Bar) { PressKeys $client @() $VK_F12; [void](WaitFor { -not (Layout $client).Bar } 10) }

    # A tab closed takes its DevTools with it: here from inside them, with Ctrl+W, which is the
    # browser's own key there as it is in a page.
    ClickTab (TabByText $client "Second Page")
    [void](WaitFor { (Layout $client).Panel } 10)
    $toolsKeys = ClickIntoTools $client
    PressInView $client $toolsKeys 0x57 @($VK_CONTROL)  # W
    Check (WaitFor { (TabButtons $client).Count -eq 1 -and (DevToolsCount) -eq 0 } 10) "a tab that is closed takes its DevTools with it: $((DevToolsCount)) left"
    Check (WaitFor { $l = Layout $client; $l.Views -eq 1 -and -not $l.Bar } 5) "and the tab that comes into view has none: $(ShowLayout (Layout $client))"
    Check ((AllViews $client).Count -eq 1) "nor is a webview of theirs left in the window: $((AllViews $client).Count)"

    # A tab dragged out into a window of its own takes its DevTools along.
    [void][W]::PostMessage($client, 0x0111, [IntPtr]1101, [IntPtr]::Zero)  # WM_COMMAND kIdNewTab
    [void](WaitFor { (TabButtons $client).Count -eq 2 } 10)
    Start-Sleep -Seconds 2
    Navigate $client $secondUrl
    [void](WaitFor { [W]::Title($frame) -eq "Second Page" } 30)
    PressKeys $client @() $VK_F12
    Check (WaitFor { (DevToolsCount) -eq 1 -and (Layout $client).Panel } 30) "a tab with its DevTools open"
    $tools2 = @(DevToolsPages)[0]
    [void](WaitFor { (InspectedUrl $tools2.id) -eq $secondUrl } 30)
    $tab = TabByText $client "Second Page"
    [void][W]::PostMessage($tab, 0x0201, [IntPtr]1, (LParam 10 10))    # WM_LBUTTONDOWN
    [void][W]::PostMessage($tab, 0x0200, [IntPtr]1, (LParam 40 120))   # WM_MOUSEMOVE
    [void][W]::PostMessage($tab, 0x0200, [IntPtr]1, (LParam 80 260))
    [void][W]::PostMessage($tab, 0x0202, [IntPtr]0, (LParam 80 260))   # WM_LBUTTONUP
    Check (WaitFor { (Frames $procId).Count -eq 2 } 15) "is dragged out into a window of its own"
    $second = @(Frames $procId | Where-Object { $_ -ne $frame })[0]
    if ($second) {
        $secondClient = ClientOf $second
        Check (WaitFor { $l = Layout $secondClient; $l.Views -eq 2 -and $l.Panel -and $l.Page.R -eq $l.Splitter.L } 10) "and its DevTools are docked there beside it: $(ShowLayout (Layout $secondClient))"
        Check ((DevToolsCount) -eq 1 -and @(DevToolsPages)[0].id -eq $tools2.id -and (InspectedUrl $tools2.id) -eq $secondUrl) "the same ones, still connected to its page"
        $left = Layout $client
        Check ($left.Views -eq 1 -and -not $left.Bar -and (AllViews $client).Count -eq 1) "the window it left has neither: $(ShowLayout $left)"
        # Closing that window closes them with the tab.
        [void][W]::PostMessage($second, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)  # WM_CLOSE
        Check (WaitFor { -not [W]::IsWindow($second) -and (DevToolsCount) -eq 0 } 10) "and go when that window is closed: $((DevToolsCount)) left"
    }

    # "Inspect" in the page's own menu opens them on the element the menu was opened on. The
    # page is scrolled, so that the place in the window is not the place in the document.
    [void](WaitFor { [W]::Title($frame) -eq "First Page" } 10)
    $firstPage = PageWithUrl $firstUrl
    [void](Evaluate $firstPage.id "scrollTo(0, 1200)")
    $scale = Evaluate $firstPage.id "devicePixelRatio"
    # The box is at 100,1400 in the document and 300 by 200: its middle is at 250,300 in the view.
    $widget = @([W]::Children($client) | Where-Object { [W]::Class($_) -eq "Chrome_RenderWidgetHostHWND" -and [W]::IsWindowVisible($_) })[0]
    $at = LParam ([int](250 * $scale)) ([int](350 * $scale))
    [void][W]::PostMessage($widget, 0x0204, [IntPtr]2, $at)  # WM_RBUTTONDOWN
    [void][W]::PostMessage($widget, 0x0205, [IntPtr]0, $at)  # WM_RBUTTONUP
    # The menu is a window of the browser's own processes. "Inspect" is its last entry.
    $browserIds = {
        $all = @(Get-CimInstance Win32_Process -Filter "Name='msedgewebview2.exe'")
        $ids = @{}; $ids[[int]$procId] = $true
        do {
            $grew = $false
            foreach ($one in $all) { if ($ids.ContainsKey([int]$one.ParentProcessId) -and -not $ids.ContainsKey([int]$one.ProcessId)) { $ids[[int]$one.ProcessId] = $true; $grew = $true } }
        } while ($grew)
        $ids.Remove([int]$procId)
        @($ids.Keys)
    }
    $menus = { @(& $browserIds | ForEach-Object { [W]::TopLevel([uint32]$_) } | Where-Object { [W]::IsWindowVisible($_) }) }
    if (WaitFor { (& $menus).Count -ge 1 } 10) {
        $menu = @(& $menus)[0]
        foreach ($key in 0x26, 0x0D) {  # Up to the last entry, and Enter: the menu takes keys posted to it
            [void][W]::PostMessage($menu, 0x0100, [IntPtr]$key, [IntPtr]1)                          # WM_KEYDOWN
            [void][W]::PostMessage($menu, 0x0101, [IntPtr]$key, [IntPtr]([int]0xC0000001 -bor 0))   # WM_KEYUP
            Start-Sleep -Milliseconds 300
        }
        Check (WaitFor { (DevToolsCount) -eq 1 -and (Layout $client).Panel } 30) "Inspect in the page's menu opens the DevTools in the window"
        $tools = @(DevToolsPages)[0]
        if ($tools) {
            Check ($tools.url -like "*&panel=elements") "on the Elements tool: $($tools.url)"
            Check (WaitFor { (SelectedElement $tools.id) -eq "inside" -or (SelectedElement $tools.id) -eq "target" } 30) "with the element the menu was opened on selected: '$(SelectedElement $tools.id)'"
            PressKeys $client @() $VK_F12
            [void](WaitFor { (DevToolsCount) -eq 0 } 10)
        }
    } else {
        "  SKIP  Inspect in the page's menu: the menu didn't come up on this desktop"
    }
}
finally {
    QuitWinamp $p
}
Check ($p.ExitCode -eq 0) ("Winamp exited cleanly: exit code 0x{0:X8}" -f $p.ExitCode)
Check (WaitFor { -not (PortOpen) } 10) "and left no browser behind with the port open"
Check ([W]::Ini("$ini\winamp.ini", "devtools_dock") -eq "right" -and [W]::Ini("$ini\winamp.ini", "devtools_width") -eq "360") "winamp.ini has them on the right, 360 wide: '$([W]::Ini("$ini\winamp.ini", "devtools_dock"))', '$([W]::Ini("$ini\winamp.ini", "devtools_width"))'"

# The next start has them where they were left; and with their DevTools open, Winamp closes cleanly.
$p = StartWinamp
$procId = [uint32]$p.Id
try {
    Check (WaitFor { (Frames $procId).Count -ge 1 } 30) "Winamp starts again"
    $frame = (Frames $procId)[0]
    $client = ClientOf $frame
    [void][W]::ShowWindowAsync($frame, 8)  # SW_SHOWNA
    [void](WaitFor { [W]::IsWindowVisible($frame) -and (Views $client).Count -ge 1 } 60)
    Start-Sleep -Seconds 2
    Navigate $client $firstUrl
    [void](WaitFor { [W]::Title($frame) -eq "First Page" } 30)
    $area = (Layout $client).Page
    PressKeys $client @($VK_CONTROL, $VK_SHIFT) 0x49  # I
    Check (WaitFor { (Layout $client).Panel } 30) "Ctrl+Shift+I opens the DevTools"
    $again = Layout $client
    Check ($again.Bar.R -eq $area.R -and $again.Bar.W -eq 360) "where they were left, and as wide: $(ShowLayout $again)"
    $tools = @(DevToolsPages)[0]
    Check ($tools -and (WaitFor { (InspectedUrl $tools.id) -eq $firstUrl } 30)) "connected to the page"
    $marker = "localStorage.getItem('exodium-test-marker')"
    Check ((Evaluate $tools.id $marker) -eq "kept") "what they kept of themselves before the restart is still theirs: '$(Evaluate $tools.id $marker)'"
    $targets = @((CdpBrowser "Target.getTargets").targetInfos | Where-Object { $_.type -eq "page" })
    $toolsContext = @($targets | Where-Object { $_.targetId -eq $tools.id })[0].browserContextId

    # A private window's DevTools keep nothing: they are in a private session of their own.
    $framesBefore = @(Frames $procId)
    PressKeys $client @($VK_CONTROL, $VK_SHIFT) 0x4E  # N
    Check (WaitFor { (Frames $procId).Count -eq $framesBefore.Count + 1 } 10) "a private window"
    $privateFrame = @(Frames $procId | Where-Object { $framesBefore -notcontains $_ })[0]
    if ($privateFrame) {
        $privateClient = ClientOf $privateFrame
        [void](WaitFor { (Views $privateClient).Count -ge 1 } 30)
        Start-Sleep -Seconds 2
        Navigate $privateClient $secondUrl
        [void](WaitFor { [W]::Title($privateFrame) -eq "Second Page (Private)" } 30)
        PressKeys $privateClient @() $VK_F12
        Check (WaitFor { (DevToolsCount) -eq 2 -and (Layout $privateClient).Panel } 30) "has its DevTools docked in it too: $(ShowLayout (Layout $privateClient))"
        $privateTools = @(DevToolsPages | Where-Object { $_.id -ne $tools.id })[0]
        if ($privateTools) {
            Check (WaitFor { (InspectedUrl $privateTools.id) -eq $secondUrl } 30) "connected to its page"
            $targets = @((CdpBrowser "Target.getTargets").targetInfos | Where-Object { $_.type -eq "page" })
            $privateToolsContext = @($targets | Where-Object { $_.targetId -eq $privateTools.id })[0].browserContextId
            $others = @($targets | Where-Object { $_.targetId -ne $privateTools.id } | ForEach-Object { $_.browserContextId })
            Check ($privateToolsContext -and $others -notcontains $privateToolsContext) "in a session apart from the other DevTools' and from every page's: '$privateToolsContext', the other DevTools in '$toolsContext'"
            Check ((Evaluate $privateTools.id $marker) -ne "kept") "with nothing of what the others kept: '$(Evaluate $privateTools.id $marker)'"
        }
        [void][W]::PostMessage($privateFrame, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)  # WM_CLOSE
        Check (WaitFor { -not [W]::IsWindow($privateFrame) -and (DevToolsCount) -eq 1 } 10) "and they go with it"
    }
    Start-Sleep -Seconds 1
}
finally {
    QuitWinamp $p
}
Check ($p.ExitCode -eq 0) ("Winamp exited cleanly with DevTools open: exit code 0x{0:X8}" -f $p.ExitCode)
Check (WaitFor { -not (PortOpen) } 10) "and left no browser behind"
$logFile = "$wa\Plugins\exodium_data\exodium.log"
$logText = if (Test-Path $logFile) { [IO.File]::ReadAllText($logFile, [Text.Encoding]::Unicode) } else { "" }
Check ($logText -notlike "*DevTools*") "the log has nothing against the DevTools: $(($logText -split "`r?`n" | Where-Object { $_ -like '*DevTools*' }) -join ' / ')"

"=== DevTools: $($script:failures) failure(s) ==="
exit $script:failures
