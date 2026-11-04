<#
.SYNOPSIS
    Registers Exodium with Windows as a web browser, or takes it out again.

.DESCRIPTION
    Everything goes in the current user's part of the registry and Start menu and leads
    to exodium_open.exe in Winamp's Plugins folder, so run this again if Winamp moves.

    This makes Exodium one of the browsers Windows offers. Making it the default stays a
    step for the user, in Settings > Apps > Default apps > Web browser: Windows keeps
    that choice out of reach of programs.

    It also puts Exodium in the Start menu, as the checkbox on Exodium's page in Winamp's
    Preferences does, and lets it be started by name from Win+R: "exodium". Either opens
    a window on the home page, and starts Winamp first if it isn't running.

.EXAMPLE
    tools\register-browser.ps1

.EXAMPLE
    tools\register-browser.ps1 -Unregister
#>
param(
    # The Winamp whose Plugins folder has exodium_open.exe in it.
    [string]$WinampDir = (Join-Path ([Environment]::GetFolderPath("MyDocuments")) "Winamp"),
    # Where the Start menu's entry goes: the current user's Programs folder, which is the
    # Start menu's list of apps.
    [string]$StartMenuDir = [Environment]::GetFolderPath("Programs"),
    [switch]$Unregister
)

$ErrorActionPreference = "Stop"

$appName = "Exodium"
$progId = "ExodiumHTML"
$clientKey = "Software\Clients\StartMenuInternet\$appName"
$capabilitiesKey = "$clientKey\Capabilities"
$registeredApps = "Software\RegisteredApplications"
$classes = "Software\Classes"
$fileTypes = ".htm", ".html"
# The name Win+R and the command prompt's "start" know it by. Windows looks the name up
# with .exe on the end.
$appPath = "Software\Microsoft\Windows\CurrentVersion\App Paths\exodium.exe"
# The same file the plugin's checkbox makes and removes (start_menu.cpp).
$shortcut = Join-Path $StartMenuDir "$appName.lnk"

$user = [Microsoft.Win32.Registry]::CurrentUser

Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class Shell {
    [DllImport("shell32.dll")] public static extern void SHChangeNotify(int eventId, uint flags, IntPtr item1, IntPtr item2);
}
"@
function NotifyShell {
    [Shell]::SHChangeNotify(0x08000000, 0, [IntPtr]::Zero, [IntPtr]::Zero)  # SHCNE_ASSOCCHANGED
}

# CreateSubKey leaves what is in a key alone. New-Item -Force would empty it, and some of
# these keys belong to every program on the machine.
function SetValues([string]$key, [hashtable]$values) {
    $opened = $user.CreateSubKey($key)
    try {
        foreach ($value in $values.GetEnumerator()) { $opened.SetValue($value.Key, $value.Value) }
    }
    finally {
        $opened.Close()
    }
}
function RemoveValue([string]$key, [string]$name) {
    $opened = $user.OpenSubKey($key, $true)
    if (-not $opened) { return }
    try { $opened.DeleteValue($name, $false) } finally { $opened.Close() }
}

if ($Unregister) {
    RemoveValue $registeredApps $appName
    foreach ($type in $fileTypes) { RemoveValue "$classes\$type\OpenWithProgids" $progId }
    $user.DeleteSubKeyTree($clientKey, $false)
    $user.DeleteSubKeyTree("$classes\$progId", $false)
    $user.DeleteSubKeyTree($appPath, $false)
    Remove-Item -LiteralPath $shortcut -ErrorAction SilentlyContinue
    NotifyShell
    "$appName is no longer registered as a browser, nor in the Start menu."
    exit 0
}

$open = Join-Path $WinampDir "Plugins\exodium_open.exe"
if (-not (Test-Path $open)) { throw "$open is not there. Copy it in first; see the README." }
# Exodium's own icon, which exodium_open.exe carries: in Default apps, and on the pages it
# opens. An exodium_open.exe from before it had one shows as a plain program.
$icon = "$open,0"
$description = "A tabbed web browser that lives inside Winamp"

# What links and files open with. The link is quoted, which exodium_open.exe relies on
# to tell it from its own option.
SetValues "$classes\$progId" @{ "" = "$appName HTML Document"; "FriendlyTypeName" = "$appName HTML Document"; "URL Protocol" = "" }
SetValues "$classes\$progId\Application" @{ "ApplicationName" = $appName; "ApplicationIcon" = $icon; "ApplicationDescription" = $description }
SetValues "$classes\$progId\DefaultIcon" @{ "" = $icon }
SetValues "$classes\$progId\shell\open\command" @{ "" = "`"$open`" `"%1`"" }
foreach ($type in $fileTypes) { SetValues "$classes\$type\OpenWithProgids" @{ $progId = "" } }

# The browser as Windows lists it.
SetValues $clientKey @{ "" = $appName }
SetValues "$clientKey\DefaultIcon" @{ "" = $icon }
SetValues "$clientKey\shell\open\command" @{ "" = "`"$open`"" }
SetValues $capabilitiesKey @{ "ApplicationName" = $appName; "ApplicationIcon" = $icon; "ApplicationDescription" = $description }
SetValues "$capabilitiesKey\URLAssociations" @{ "http" = $progId; "https" = $progId }
SetValues "$capabilitiesKey\FileAssociations" (& { $types = @{}; foreach ($type in $fileTypes) { $types[$type] = $progId }; $types })
SetValues "$capabilitiesKey\StartMenu" @{ "StartMenuInternet" = $appName }
SetValues $registeredApps @{ $appName = $capabilitiesKey }

# Started from the Start menu or by name, with no link, it opens a window on the home page.
# The shortcut has no icon of its own, so it shows exodium_open.exe's.
SetValues $appPath @{ "" = $open }
New-Item -ItemType Directory -Path $StartMenuDir -Force | Out-Null
$link = (New-Object -ComObject WScript.Shell).CreateShortcut($shortcut)
$link.TargetPath = $open
$link.WorkingDirectory = Split-Path -Parent $open
$link.Description = $description
$link.Save()

NotifyShell
"$appName is registered as a browser, opening links with $open"
"It is in the Start menu ($shortcut), and Win+R starts it as `"exodium`"."
"To make it the default: Settings > Apps > Default apps > Web browser > $appName"
