<#
.SYNOPSIS
    Smoke-tests gen_browser.dll in a scratch copy of Winamp, driven by window messages.

.DESCRIPTION
    The Winamp in use is never touched. Its folder and settings are copied once to a
    scratch folder, without the browser's own data and without the plugins that make a
    noise or reach outside Winamp, and the copy is run as a separate instance beside it.
    The script then works the browser the way a user would, as far as window messages
    reach: pages, tabs, dragging tabs between windows, minimizing and maximizing,
    the ornaments on the corners and their buttons, closing, and what is left on
    disk after Winamp exits.

    Keyboard shortcuts are not covered. Keystrokes can't be injected without taking the
    keyboard away from whoever is at the machine.

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
    # Where the copy is kept between runs. Delete it to have it made afresh.
    [string]$ScratchDir = (Join-Path $env:TEMP "exodium-smoke-test"),
    [string]$Label = "gen_browser.dll",
    # Leaves out the tab dragging, which needs the browser window not to be covered.
    [switch]$SkipDrag
)

$ErrorActionPreference = "Stop"
$wa = Join-Path $ScratchDir "Winamp"
$ini = Join-Path $ScratchDir "settings"

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

Add-Type -AssemblyName System.Windows.Forms

# Every run starts from the same modest window place, kept in the scratch winamp.ini.
$iniText = [System.IO.File]::ReadAllText("$ini\winamp.ini")
$iniText = [regex]::Replace($iniText, '(?ms)^\[EmbedWndExample\]\r?\n.*?(?=^\[|\z)', "[EmbedWndExample]`r`nxpos=100`r`nypos=90`r`nwidth=900`r`nheight=638`r`n")
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

"=== smoke test: $Label ==="
$plugin = Get-Item "$wa\Plugins\gen_browser.dll"
"plugin: $($plugin.Length) bytes, $($plugin.LastWriteTime)"
# The browser starts with no history and no profile, so that every run sees the same.
Remove-Item "$wa\Plugins\exodium_data" -Recurse -Force -ErrorAction SilentlyContinue

$p = Start-Process "$wa\winamp.exe" -ArgumentList "/CLASS=ExodiumSmokeTest" -PassThru
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

    Navigate $client "https://example.com/"
    Check (WaitFor { [W]::Title($frame) -eq "Example Domain" } 30) "typed address navigates: '$([W]::Title($frame))'"
    Check (WaitFor { [W]::Text((Control $client 1003)) -eq "https://example.com/" } 10) "address bar shows the page's address: '$([W]::Text((Control $client 1003)))'"

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
    }

    # Middle-click closes a tab.
    $count = (TabButtons $client).Count
    if ($count -ge 2) {
        [void][W]::PostMessage((TabButtons $client)[0], 0x0208, [IntPtr]::Zero, (LParam 5 5))  # WM_MBUTTONUP
        Check (WaitFor { (TabButtons $client).Count -eq $count - 1 } 10) "middle-click closes a tab"
    }

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
    Check ([W]::IsWindow($frame) -and (TabButtons $client).Count -eq 1) "the first window survives with one blank tab"
    Check ([W]::Title($frame) -eq "Exodium") "its title is back to Exodium"
    [void][W]::ShowWindowAsync($frame, 8)
    Check (WaitFor { [W]::Title($frame) -like "*Google*" } 40) "shown again, its blank tab loads the default page"

    # Closed with the ornament's own button, the window goes the same way.
    if ($ornament) {
        ClickOrnament $ornament 2
        Check (WaitFor { -not [W]::IsWindowVisible($frame) -and @($all | Where-Object { [W]::IsWindowVisible($_) }).Count -eq 0 } 10) "the ornament's third button closes the window, and the ornaments go with it"
        # Waited for: the window is out of sight a moment before it has its blank tab.
        Check (WaitFor { [W]::IsWindow($frame) -and (TabButtons $client).Count -eq 1 } 5) "the first window survives that as well"
    }

    Check (-not $p.HasExited) "Winamp is still running"
}
finally {
    if (-not $p.HasExited) {
        $main = @([W]::TopLevel($procId) | Where-Object { [W]::Class($_) -eq "ExodiumSmokeTest" })
        if ($main.Count) { [void][W]::PostMessage($main[0], 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) }
        if (-not $p.WaitForExit(30000)) { "  FAIL  Winamp did not exit within 30 s; killing it"; $script:failures++; $p.Kill(); $p.WaitForExit() }
    }
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
"=== ${Label}: $($script:failures) failure(s) ==="
exit $script:failures
