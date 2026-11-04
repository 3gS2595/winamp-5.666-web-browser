# Exodium

A tabbed web browser that lives inside Winamp. It is a Winamp general purpose plugin
(`gen_browser.dll`) that puts Microsoft's WebView2 in one of Winamp's own skinned windows.

- Tabs that can be reordered, dragged between windows, and torn off into windows of their own,
  several at once: Ctrl+click and Shift+click select them, as in Chrome
- A close button on each tab, as in Chrome, which can be turned off for closing tabs from
  the keyboard
- A speaker on each tab whose page is making sound, as in Chrome
- An address bar that completes and suggests from the history, the way Chrome's does
- A history page (`exodium://history`), per-site zoom, reopening of closed tabs, fullscreen video
- A downloads panel of its own, in place of WebView2's download dialog, that never takes the
  keyboard: the shortcuts keep working while it is open, and Escape closes it
- Windows that snap (dragged by the title bar to the screen's edges and corners, or with
  Win+arrow keys), maximize, minimize and have their own taskbar buttons
- Ironwork ornaments on each window's corners, bulging out past the frame. The one on
  the top right has the buttons that minimize, maximize and close the window
- Can be the default browser in Windows: a link opens in a new window, and Winamp is
  started for it if it isn't running
- Can be in the Start menu, and started from Win+R as `exodium`: a new window on the home
  page, with Winamp started for it if it isn't running
- Can be driven by Claude Code, to test sites in a window of its own
- Winamp's visualizers (the main window's analyzer, MilkDrop, AVS) show the sound a page
  makes while Winamp has nothing else to play, with the input plugin `in_exodium.dll`

A second, unrelated plugin lives beside it: `gen_llama.dll` plays Winamp's llama sound
at start-up when the splash screen is enabled.

## Keyboard

| Keys | |
| --- | --- |
| Ctrl+T / Ctrl+W | New tab / close tab |
| Ctrl+Shift+T | Reopen the last closed tab |
| Ctrl+Tab / Ctrl+Shift+Tab | Next / previous tab |
| Ctrl+N | New window, in Winamp's own windows as well |
| Ctrl+Shift+W, Alt+F4 | Close window |
| Ctrl+L | Address bar |
| Ctrl+H | History |
| Alt+Left / Alt+Right | Back / forward |
| Ctrl+0 | Put the site back to its normal zoom |
| Esc | Close the downloads panel, while it is open; otherwise Escape is the page's or the address bar's, as before |

A tab is also closed by the × at its right end, or by middle-clicking it. Either closes
that tab alone, even one of several selected, as in Chrome. The × works as a button:
pressing it neither brings the tab into view nor starts a drag, and letting go anywhere
but on it closes nothing. A tab that gets narrow loses its ×, except the one in view,
and on a very narrow tab in view the × is all there is. After a tab is closed with the
mouse, the others keep their width until the mouse leaves the strip, so that the next
tab's × comes under the mouse. The settings can take the ×s away.

Ctrl+N opens a browser window from Winamp's main window, the equalizer, the video window
and the windows in Winamp's frames, such as the Media Library, and brings it to the front.
It opens where a link's window does: down and right of the browser's saved place, a step
for each window open. In the playlist editor Ctrl+N
stays Winamp's own *New playlist*. Nothing opens while a dialog, a message box, a menu or
the Preferences are open, nor with a modern skin, whose windows aren't known. The setting
*Ctrl+N in Winamp opens a new Exodium window* turns it off.

Winamp 5.666 has Ctrl+N in the playlist editor's shortcut table and in no other, and none
of the plugins it comes with uses it. The plugin reads Winamp's tables when Ctrl+N is first
pressed, the language pack's if it has its own (the German one does), so it follows any
that has Ctrl+N somewhere else (`winamp_keys.h`). Ctrl+Alt+N is still Winamp's: it starts
another Winamp.

Ctrl+click on Back or Forward, or the middle button, opens the page it leads to in a new
tab behind the one in view, and Ctrl+Shift+click in front of it; Shift+click opens it in
a new window. That tab is given the same history, so its own Back and Forward lead where
the first tab's would from that page, and the first tab stays where it is. It goes where
a tab a link on the page opens would, as in Chrome. WebView2 can't copy a tab's history,
so the new tab makes it an entry at a time from blank stand-ins, which never reach the
sites (`back_forward.cpp`); an entry that was a form's answer loads again without the
form, as a plain address.

Ctrl+click a tab to add it to the selected tabs, or take it out; Shift+click to select
every tab from the last one clicked, and Ctrl+Shift+click to add those. Dragging any
selected tab takes them all, and Ctrl+W or the close button on the toolbar closes them
all. A plain click on one of them picks it alone.

## Downloads

WebView2's own download dialog took the window over: while it was open the browser's
shortcuts stopped working. Exodium follows downloads in a panel of its own instead, in the
top right corner of the page, as Chrome's download bubble is. It never takes the keyboard
or makes a window active, so the page keeps the keyboard and the shortcuts keep working.

- It comes up by itself when a download starts, in the window it was started from, and
  goes again after a few seconds, unless the mouse is on it or a download waits in it to
  be kept.
- A button at the right end of the toolbar, there while there are downloads, opens and
  closes it. A ring round it fills as the downloads come in.
- Each download shows its file's name, how much has come, its speed and the time left, or
  how it ended. A click on a finished one opens its file; its own buttons pause, resume or
  cancel one in progress, or show a finished one in its folder.
- Escape closes it, wherever the keyboard is in the window: the page, the address bar, a
  button. So does a click anywhere else, its own close button, and the toolbar button.
  While it is closed, Escape is the page's or the address bar's, as it always was.
- A type of file that can harm the computer (a program, an installer, a script) waits in
  the panel until it is kept or deleted, as it waited in WebView2's dialog.
- Every window lists every download: they are the browser's, not a tab's. A download goes
  on when its tab moves to another window, and when its window is closed.

Files still go to the profile's download folder, the user's Downloads folder unless it has
been changed, without asking. Claude Code's windows are left to WebView2's own dialog, as
before: Playwright, which drives them, has its own way with downloads.

## Building

Requirements: Visual Studio 2026 with the C++ desktop workload (toolset v145), and the
WebView2 NuGet package, which Visual Studio restores into `packages\`.

Open `exodium.slnx`, or build from a command prompt:

```
msbuild exodium\exodium.vcxproj /p:Configuration=Release /p:Platform=Win32
```

Classic Winamp is a 32-bit program, so **Win32 is the build to use**. x64 builds as well,
for 64-bit hosts.

The C++ runtime is linked in (`/MT`, set in `build\Common.props`), so nothing built here
needs a Visual C++ redistributable on the PC it runs on.

Everything is built into one tree, wherever the build is started from:

```
bin\<Platform>\<Configuration>\gen_browser.dll
bin\<Platform>\<Configuration>\WebView2Loader.dll
bin\<Platform>\<Configuration>\exodium_open.exe
bin\<Platform>\<Configuration>\in_exodium.dll
```

`in_exodium.dll` is a project of its own too (`msbuild in_exodium\in_exodium.vcxproj`),
since Winamp only takes input plugins from `in_*.dll` files; see
[The pages' sound in Winamp's visualizers](#the-pages-sound-in-winamps-visualizers).

`exodium_open.exe` is a project of its own (`msbuild exodium_open\exodium_open.vcxproj`
with the same options). Its icon, `exodium_open\exodium_open.ico`, is drawn by
`tools\make-icon.ps1` from the corner ornament's picture; run that again to change it, with
`-Preview <file.png>` to see every size first.

## Installing

With Winamp closed, copy `gen_browser.dll` and `WebView2Loader.dll` into Winamp's
`Plugins` folder. The browser is then in Winamp's main menu and its Windows menu as
*Exodium*. It needs the WebView2 runtime, which Windows 10 and 11 normally have, and
nothing else. Copy `in_exodium.dll` there as well for the pages' sound in Winamp's
visualizers; the browser works the same without it.

The plugin keeps its data in `Plugins\exodium_data`: `history.txt`, `zoom.txt` and the
browser profile (cookies, sign-ins, cache). The window's place and the settings are kept
in `winamp.ini`.

## Settings

Exodium has a page in Winamp's Preferences (Ctrl+P in Winamp), under *General
Preferences*. Exodium's *Configure* button under *Plug-ins > General Purpose* opens it too.
A change takes effect at once, in every window: a checkbox or a choice of engine as it is
clicked, typed text once the box is left or the Preferences are closed.

- **Home page**: where new windows and tabs open. It is taken the way the address bar
  takes it, so `example.com` is `https://example.com`. Left empty, it is Google again.
- **Search engine**: where words typed in the address bar are searched for: Google, Bing,
  DuckDuckGo, Brave Search, Ecosia, Startpage or Yahoo, or *Custom*, an address of your own
  with `%s` where the words go, such as `https://kagi.com/search?q=%s`. A search made
  earlier with any of these, whichever engine it was, is still offered in the address
  bar's list as a search, and is made again with the engine picked now.
- **Show close buttons on tabs**: the × at each tab's right end. Without them, a click
  there is a click on the tab, and tabs are closed with Ctrl+W, the toolbar's close
  button or a middle-click.
- **Ctrl+N in Winamp opens a new Exodium window**: see [Keyboard](#keyboard). On to begin with.
- **Show Exodium in the Start menu**: makes or removes the Start menu's entry at once (see
  [The Start menu](#the-start-menu)). It shows whether the entry is there, however it got
  there or went, and isn't kept in `winamp.ini`. It needs `exodium_open.exe` in `Plugins`;
  without it, the page says so and the box can only be unticked.
- **Winamp's visualizers show page sound while Winamp is stopped**: see
  [The pages' sound in Winamp's visualizers](#the-pages-sound-in-winamps-visualizers). On
  to begin with; unticked while Winamp is playing a page's sound, Winamp stops.

The rest are kept in the `[Exodium]` section of `winamp.ini`, beside `remote_debugging_port`.
A setting is only written there once it has been changed:

```
[Exodium]
home_page=https://example.com
search_engine=custom
custom_search_url=https://kagi.com/search?q=%s
tab_close_buttons=0
ctrl_n_in_winamp=0
sound_in_visualizers=0
```

`search_engine` is `google`, `bing`, `duckduckgo`, `brave`, `ecosia`, `startpage`, `yahoo`
or `custom`.

To add a checkbox to the page:

1. In `settings.h`, a `bool` in `Settings` with its default, and a line in `kFlagSettings`
   with its key in `winamp.ini`. Reading and writing it come with that, and the unit test
   `EveryFlagIsKeptUnderItsOwnKey` checks it.
2. An id in `resource.h`, after the last checkbox's, and a `CONTROL` line in
   `res\exodium.rc` under the last checkbox, 13 units further down. The *Options* box has
   room for two more, above the note at its foot (`IDC_PREFS_START_MENU_NOTE`).
3. A line in `kCheckboxes` in `settings_page.cpp`.
4. The browser reads it from `g_app.settings`. If something has to happen the moment it
   changes, that goes in `ApplySettings()` in `settings_page.cpp`.

## The pages' sound in Winamp's visualizers

With `in_exodium.dll` in `Plugins`, Winamp's visualizers show the sound Exodium's pages
make: the analyzer or oscilloscope in Winamp's main window, and whichever visualization
plugin is running, such as MilkDrop or AVS. It happens by itself, under these rules
(`sound_rule.h`):

- It starts when a page starts making sound and Winamp is stopped. While Winamp plays or
  is paused on something of its own, that is left alone.
- Winamp then shows it as a track, *Exodium:* and the page's title, which follows the page
  (the next video's title, say). Its time counts up from when the sound started.
- It stops once the pages have been quiet for 3 seconds, so a pause between two videos
  doesn't stop it, and Winamp goes back to showing the playlist's track.
- Play in Winamp plays the track the playlist was on, as it would have: the playlist is
  never changed. Stop stops it. Either way, it isn't started again until the pages have
  gone quiet and then make sound again.
- The setting *Winamp's visualizers show page sound while Winamp is stopped* turns it off.

Nothing is played twice: the pages are heard as before, and `in_exodium.dll` only hands
Winamp's visualizers a copy. It records the pages with Windows' process loopback
recording, aimed at WebView2's browser process and the processes it started, so it hears
the pages and nothing else, not Winamp's own playing nor other programs. Microsoft
documents that for Windows 10 build 20348 and later; it was found to work on 19045 too.
On a Windows without it, Winamp shows the track with nothing in the visualizers.

How it is put together:

- Winamp's visualizers only see what the input plugin playing hands them, and Winamp only
  plays what is in its playlist. So `winamp_sound.cpp` adds `exodium://sound` to the end of
  the playlist, plays it, and takes it out again at once, putting the playlist's position
  back; Winamp plays on. `in_exodium.dll` is the input plugin for `exodium://`.
- `in_exodium.dll` plays nothing itself and never uses Winamp's output plugin. It tells
  Winamp a time 60 ms behind the sound it has, which Winamp needs in order to find the
  visualization plugins' data every time (see `kVisLagMs`), and the pictures trail what
  is heard by less than that.
- The two DLLs talk by plain calls (`sound_ipc.h`): the plugin gives `in_exodium.dll` the
  page's title and asks it whether it is what Winamp plays.

## Making it the default browser

Copy `exodium_open.exe` into Winamp's `Plugins` folder too, then tell Windows about it:

```
powershell -ExecutionPolicy Bypass -File tools\register-browser.ps1
```

Exodium then shows up in *Settings > Apps > Default apps > Web browser*, and picking it
there is the last step. Windows doesn't let a program make that choice for itself. The
script also puts Exodium in the Start menu (next section). It takes `-WinampDir` if Winamp
isn't in `Documents\Winamp`, and `-Unregister` takes everything out again, the Start
menu's entry included.

`exodium_open.exe` is what Windows runs for a link. It looks for Winamp, starts it if it
isn't running, and sends the link to the plugin as a `WM_COPYDATA` (`link_ipc.h`). Every
link gets a new window, and `.htm` and `.html` files open the same way. The plugin
turns links down until Winamp has finished starting, and `exodium_open.exe` keeps
asking, because Winamp's main window takes the windows it owns to the front as it
starts, and that would bury a browser window, which it doesn't own.

## The Start menu

*Exodium* in the Start menu opens a new window on the home page, and starts Winamp first
if it isn't running. Started that way, Winamp comes up as it always does, with its splash
screen and its own windows, and the browser's window in front of them. Win+R `exodium`
does the same, and so does `start exodium` at a command prompt.

The entry is a shortcut, `Exodium.lnk` in the Start menu's own folder for the user
(`%APPDATA%\Microsoft\Windows\Start Menu\Programs`), to `exodium_open.exe` with no link.
Two things make it, and either removes it:

- **Show Exodium in the Start menu** on Exodium's page in Winamp's Preferences.
- `tools\register-browser.ps1`, which makes it along with the rest (`-StartMenuDir` puts it
  somewhere else). Win+R's `exodium` is its doing too: an *App Paths* entry,
  `HKCU\Software\Microsoft\Windows\CurrentVersion\App Paths\exodium.exe`, which leads to
  `exodium_open.exe`. `-Unregister` takes out both.

The shortcut has no icon of its own, so it shows `exodium_open.exe`'s, and nothing ties it
to the taskbar: the browser's windows belong to Winamp, and are grouped with it there. A
Winamp started under a window class of its own (`/CLASS=`), as the smoke test's is, makes a
shortcut that names that class, so that it reaches that Winamp and not the usual one. The
`EXODIUM_START_MENU_FOLDER` environment variable, which the smoke test sets, has the
plugin put the shortcut in that folder instead of the Start menu.

## Letting Claude Code drive it

Claude in Chrome's extension can't do this here: WebView2 has no real tab strip or
windows for it to work with, and calling `chrome.sidePanel.open` takes the whole browser
process down. Exodium opens the browser's DevTools port instead, and Claude Code uses
Playwright's MCP server on that. It needs Node, and these steps:

1. Copy `tools\exodium_mcp.mjs` and `tools\exodium_start.html` into Winamp's `Plugins`
   folder, beside `exodium_open.exe`.
2. Add this to `winamp.ini`, and restart Winamp:

   ```
   [Exodium]
   remote_debugging_port=9333
   ```

3. Tell Claude Code about it:

   ```
   claude mcp add --scope user exodium -- node "<Winamp>\Plugins\exodium_mcp.mjs"
   ```

The port is off unless `winamp.ini` names one, because **any program on this PC can use
it while it is open, and with it every site the browser is signed in to**. It listens on
127.0.0.1 only, and refuses web pages, so sites can't reach it. Remove the setting and
restart Winamp to close it. While it is open, a closed tab is closed outright rather than
kept, so Ctrl+Shift+T brings it back without Back and Forward.

`exodium_mcp.mjs` sits between Claude Code and Playwright's server (see the top of the
file for why). Claude gets a window of its own and keeps out of the tabs in use, each new
page is an Exodium window, and Winamp is started if it isn't running. `browser_close`
closes Claude's windows and nothing else.

What Claude sees of the rest is kept down:

- Playwright lists every tab in its answers. Claude is shown only its own pages, and the
  pages those opened, with a count of the rest; `browser_tabs` with `all: true` lists the
  user's too, for when they point Claude at one.
- Video is recorded of Claude's page only, where Playwright would record every tab.
- A console message repeated thousands of times is shown once, with its count.
- Playwright keeps a page's file picker from opening, so that `browser_file_upload` can
  answer it, and left to itself it does that to every tab; only the page Claude is
  working in has its picker caught, so upload buttons in the other tabs open the usual one.

Exodium knows Claude's windows from the page they open on, and treats them differently:

- They come up without the keyboard, behind every other window (in front of the
  desktop), so a session opening one doesn't interrupt whatever the user is doing. Behind
  other windows a page carries on drawing; one can be brought forward from the taskbar.
  Claude's pages never take the keyboard into a window the user isn't in.
- They close when the session that opened them ends. The wrapper gives its process id in
  the start page's address (`exodium_start.html#pid=1234&...`), and Exodium closes that
  session's tabs, the ones its pages opened included, once the process has gone, however
  it went. The wrapper also closes them itself when Claude Code ends the session.
- Their pages are always at 100%, whatever zoom the user has set for the site, so that
  the window is the size Claude asked for and screenshots are in the pixels it clicks in.
  Zooming one of them lasts until its page changes, and isn't saved.
- Closed, they don't go into the list Ctrl+Shift+T reopens from.

Playwright only reads and writes files in its output folder (`%TEMP%\exodium-mcp`, which
it keeps under 5 GB by removing the oldest) and the project. Files Claude names in its
own scratchpad, under `%TEMP%\claude`, are passed through the output folder.

Playwright connects to every page there is, and can't while one of them is showing a
dialog or is stuck in a script. The wrapper asks each page first, and if one doesn't
answer, Claude is told which at once instead of failing after a minute.

Besides Playwright's own tools, with its `vision`, `pdf` and `devtools` capabilities on
(the mouse by coordinates, the wheel, PDF, video, tracing), it adds four:

- `browser_zoom` takes a magnified picture of one region of the page. Page content is
  drawn afresh at the larger size; a canvas or WebGL scene is enlarged from its own pixels.
- `browser_batch` runs several tools in one call and stops at the first error.
- `browser_emulate_device` makes the page a phone's, a tablet's or a small laptop's: its
  screen size and pixel density, touch (`browser_mouse_click_xy` taps, `(pointer: coarse)`
  matches), its user agent, and a CPU slowdown. The GPU stays the PC's.
- `browser_filmstrip` watches the page for a while, optionally after an action such as a
  click, and gives back frames evenly spaced in time.

A page only draws while it is in view. Covered by another window it carries on, but while
its window is minimized or its tab is hidden, screenshots and clicks don't work.

## Moving to another PC

Winamp needs no installing: its folder runs wherever it is copied to. Two scripts move
it, Exodium and the settings of both. On the old PC, with Winamp closed:

```
powershell -ExecutionPolicy Bypass -File tools\export-setup.ps1 -Destination E:\winamp-move
```

with the letter of the drive to copy to (a folder on `C:` works too, to be moved on some
other way). It checks the drive is there and has room before copying anything. That copies Winamp's folder, its settings folder (wherever `paths.ini` sends them, here
`%APPDATA%\Winamp`), Claude Code's `CLAUDE.md`, `settings.json` and its memory of this
repo, and the scripts to set it all up again. On the new PC, run the copy of
`import-setup.ps1` in that folder. It puts Winamp in `Documents\Winamp`, or in
`%USERPROFILE%\Winamp` if Documents is kept in OneDrive, which would be syncing the
browser profile as it changes. It changes the old PC's paths in `winamp.ini`, playlists
and bookmarks wherever the user or Winamp folder differ, installs the WebView2 runtime if
it's missing, registers the browser and puts it in the Start menu, adds the MCP server,
and lists what's left to do.
It won't copy over a Winamp or settings that are already there.

What doesn't move:

- **Sign-ins.** The browser encrypts cookies and saved passwords with a key that Windows
  (DPAPI) locks to the PC's user account, so the export leaves them out, along with
  `Local State`, which holds that key. The new PC makes a key of its own. The rest of a
  site's data, such as localStorage and IndexedDB, comes along.
- **Caches** (page, code, GPU and shader caches, `shared_proto_db`), which fill again, the
  `*.bak` files in `Plugins`, and `wv2_userdata`, an older build's browser profile.
- **Music.** Playlists and the media library point at it, so put it in the same place
  under the new user folder. The media library's database is binary, so its paths aren't
  changed; rescan it if they differ.
- **Choosing Exodium as the default browser**, and Winamp's own file types, which
  Windows and Winamp leave to the user.

## Testing

Unit tests cover everything that needs neither a window nor a webview: addresses, the
history, suggestions, zoom, the files they are kept in, a tab's Back and Forward and how
they are copied, search engines, the settings and how `winamp.ini` keeps them, where a
tab's icon, speaker, close button and title go, where Ctrl+N in Winamp's windows opens a
browser window, what the downloads panel says about a download, `exodium_open.exe`'s
command line, and when Winamp plays the pages' sound and how `in_exodium.dll` cuts it
into blocks for the visualizers.

```
msbuild tests\exodium_tests.vcxproj /p:Configuration=Release /p:Platform=Win32
bin\Win32\Release\exodium_tests.exe
```

The rest is covered by a smoke test that runs the built plugin in a scratch copy of
Winamp, beside the one in use and without touching it, and works it by window messages:
pages, tabs and their close buttons, selecting several tabs and dragging them between
windows, Back and Forward opened in new tabs, pages that open tabs and windows of their
own, a download and the downloads panel, minimizing and maximizing, the corner ornaments
and their buttons, closing, what is on disk after Winamp exits, links from outside with
Winamp running and closed, and Exodium's page in Winamp's Preferences: opening it,
changing what is on it, what that writes to `winamp.ini`, and the browser following it.
Its Start menu checkbox makes and removes a shortcut in a scratch folder, which is then
opened with Winamp running and closed. Its downloads go to a folder in its scratch copy,
never to the Downloads folder in use.

It also has a page play a tone too quiet to hear and checks that Winamp plays it for the
visualizers and stops, leaves the playlist alone, and gives way to Stop and Play. What
reaches the visualizers is written down by `vis_probe.dll` (`tests\vis_probe`), a
visualization plugin of the test's own, and `in_exodium.dll` turns the tone up for them
(`EXODIUM_SOUND_TEST_GAIN`). The scratch Winamp's playlist only ever holds silent WAVs
then, since its settings are a copy of the real ones and Play is pressed. Without
`in_exodium.dll` and `vis_probe.dll` beside the plugin under test, that part is skipped.

```
powershell -ExecutionPolicy Bypass -File tools\smoke-test.ps1
```

It runs on a desktop of its own, which Windows never shows, so it can be left running
while you work: its windows don't come up in front of yours or take the keyboard, and
yours can't get in the way of its drags. `-OnScreen` runs it where it can be watched.

Keyboard shortcuts are the one thing neither covers; those are tried by hand. The keys
held with a click on Back or Forward, Ctrl+Shift+T, and Ctrl+N in Winamp's windows and the
browser's, are the exception: the smoke test shares the browser thread's keyboard state
for a moment, which leaves the real keyboard alone. So is Escape, which needs no other
key and can be sent to a window.

## Layout

```
build\Common.props        compiler and linker settings shared by every project
exodium\src\              the browser plugin
exodium\res\              the corner ornaments' pictures and the settings page, built into the plugin
exodium\third_party\      Winamp SDK headers, as they came, and in2.h and vis.h written out to the SDK's layout
exodium_open\             what Windows runs for a link, and the Start menu's entry runs, to get it to the plugin
in_exodium\               the input plugin that gives Winamp's visualizers the pages' sound
gen_llama\                the llama sound plugin
tests\                    unit tests and the small runner they use
tests\vis_probe\          the visualization plugin the smoke test writes down what reaches the visualizers with
tools\smoke-test.ps1      the smoke test
tools\register-browser.ps1  registers Exodium with Windows as a browser, and puts it in the Start menu
tools\make-icon.ps1       draws exodium_open.exe's icon
tools\export-setup.ps1    packs up Winamp, Exodium and their settings for another PC
tools\import-setup.ps1    and sets them up there
tools\exodium_mcp.mjs     what Claude Code runs for its browser tools
docs\                     design notes for individual features
```

Inside `exodium\src`:

| File | |
| --- | --- |
| `plugin.cpp` | What Winamp calls: start-up, preferences, exit; and links coming in from outside |
| `link_ipc.h` | How a link gets from `exodium_open.exe` to the plugin |
| `browser.h` | The model (`App`, `BrowserWindow`, `Tab`) and every function shared between the files below |
| `browser_window.cpp` | Making and closing windows, their window procedure, shortcuts |
| `frame.cpp`, `embed_frame.cpp` | Winamp's frame around a window: taskbar button, snapping, menu entries, saved place |
| `layout.cpp`, `drawing.cpp` | Where the controls go and how they are painted |
| `ornament.cpp` | The ornaments on a window's corners, and the minimize, maximize and close buttons on the top right one |
| `tabs.cpp`, `tab_drag.cpp` | Opening, closing, reopening, selecting, moving and dragging tabs, and the close buttons on them |
| `address_bar.cpp` | The address bar, its suggestion list, and the keyboard handling all controls share |
| `webview.cpp`, `webview_events.cpp` | Creating webviews and reacting to what they report |
| `back_forward.cpp` | Back and Forward, and the page they lead to in a new tab or window with a copy of the tab's history |
| `fullscreen.cpp`, `zoom.cpp` | Fullscreen video, per-site zoom |
| `downloads.cpp` | Downloads: following them in place of WebView2's dialog, and what the panel's buttons do to them |
| `downloads_panel.cpp` | The downloads button on the toolbar and the panel it opens |
| `settings_page.cpp` | Exodium's page in Winamp's Preferences, and what the browser does when a setting changes |
| `start_menu.cpp` | Exodium's entry in the Start menu, which the page's checkbox makes and removes |
| `suggestions.cpp`, `history.cpp`, `history_page.cpp`, `session_history.cpp`, `site_zoom.cpp`, `tab_order.cpp`, `tab_parts.cpp`, `url_util.cpp`, `text_util.cpp`, `download_status.cpp` | Logic with no window in it; this is what the unit tests build |
| `search_engines.cpp`, `settings.cpp` | The same: the search engines on offer and the one in use, and the settings and how `winamp.ini` keeps them |
| `winamp_keys.cpp` | The same: whether Ctrl+N pressed in one of Winamp's windows opens a browser window |
| `winamp_key_hook.cpp` | The hook on Winamp's thread that watches its windows for Ctrl+N |
| `winamp_sound.cpp` | Has Winamp play the pages' sound for its visualizers, and stop |
| `sound_rule.cpp` | The same as the logic files: when it is to start and stop |
| `sound_ipc.h` | What the plugin and `in_exodium.dll` know about each other |
| `win32_util.h` | Owning wrappers for window, GDI and COM handles |

## Conventions

- C++23, warning level 4, and no warnings.
- Everything runs on Winamp's UI thread; nothing is locked.
- Handles are owned by RAII types (`UniqueWindow`, `UniqueIcon`, `WebViewController`, ...),
  never closed by hand.
- Webview events find their tab by id each time they fire, because the tab may have moved
  to another window or been closed in the meantime.
- Work that would destroy a window or webview from inside its own event is posted to the
  window's client instead (`ClientMessage` in `browser.h`).
- `std::format` is kept out of the plugin (it is fine in the tests): it brings about
  190 KB of formatting tables into a DLL whose code is otherwise about 200 KB.
- Formatting is `clang-format` with the `.clang-format` at the root.
