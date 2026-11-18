# Exodium

A tabbed web browser that lives inside Winamp. It is a Winamp general purpose plugin
(`gen_browser.dll`) that puts Microsoft's WebView2 in one of Winamp's own skinned windows.

- Tabs that can be reordered, dragged between windows, and torn off into windows of their own,
  several at once: Ctrl+click and Shift+click select them, as in Chrome
- A close button on each tab, as in Chrome, which can be turned off for closing tabs from
  the keyboard
- A speaker on each tab whose page is making sound, as in Chrome, and a menu on each tab
  (right click) that mutes it, duplicates it, loads it again, or closes it, the others or
  those to its right
- An address bar that completes and suggests from the history and the bookmarks, the way
  Chrome's does, and takes what is typed the way a browser should; a × on a suggestion
  forgets it (see [The address bar](#the-address-bar))
- Bookmarks: a star on the toolbar and Ctrl+D, a bookmarks page (`exodium://bookmarks`),
  and Chrome's, Edge's, Brave's or Vivaldi's brought in with a click (see [Bookmarks](#bookmarks))
- A history page (`exodium://history`) of the last 20,000 pages, reopening of closed tabs,
  fullscreen video
- Zoom that belongs to the site, as in Chrome, and is on the toolbar with a − and a + while
  the page in view is at anything but 100% (see [Zoom](#zoom))
- Tabs left out of view for two hours are put to sleep, which gives back all the memory
  their pages held (see [Sleeping tabs](#sleeping-tabs))
- Private windows (Ctrl+Shift+N), whose pages leave nothing behind (see
  [Private windows](#private-windows))
- Picks up where it left off: the windows open when Winamp closed come back when it starts
  again, where they were, each with its tabs, and each tab with its Back and Forward. A
  closed window comes back whole with Ctrl+Shift+T, from this run of Winamp or the one
  before (see [Picking up where it left off](#picking-up-where-it-left-off))
- A downloads panel of its own, in place of WebView2's download dialog, that never takes the
  keyboard: the shortcuts keep working while it is open, and Escape closes it
- Bitwarden, the password manager, fetched from the Chrome Web Store, checked and kept up to
  date, with its button on the toolbar and its popup under it, as in Chrome
- Windows that snap (dragged by the title bar to the screen's edges and corners, or with
  Win+arrow keys), maximize, minimize and have their own taskbar buttons. Maximized, a
  window drops Winamp's border on the left, right and bottom, and the page fills the screen
  below the title bar and toolbar
- Windows docked to Winamp's, edge to edge, come to the front with them: Alt+Tab to a
  browser window, a click on it or its taskbar button brings up the windows docked with it,
  Winamp's main window and all it owns included, right under it and above other programs'
  windows, without taking the keyboard; and the same brings a docked browser window up with
  Winamp's. Winamp's main window owns its other windows, so they always came up together; a
  browser window has a taskbar button of its own, so nothing owns it (`docked_windows.cpp`)
- Windows that size from their edges and corners like any other window, and that AltSnap
  can move and size, maximized ones included. Where the taskbar hides itself, a maximized
  window leaves the screen's bottom row of pixels uncovered (or the row on the taskbar's
  side): one that covered all of it would be taken for a full screen game or video, which
  AltSnap leaves alone and which a hidden taskbar won't come up over
- Ironwork ornaments on each window's corners, bulging out past the frame. The one on
  the top right has the buttons that minimize, maximize and close the window. Their skin
  can be picked in Winamp's Preferences: *Iron*, *Venom*'s purple and green chrome, or
  *Charms* and *Dark Charms*, filigree crusted with stars, hearts and flowers.
  Winamp's library window wears them too: there the buttons fill the screen with it and
  put it back, close it as Winamp's own close button does, and minimize Winamp, which
  takes the library along, as the library has no taskbar button of its own
- A black glow round Winamp's windows while Winamp is in front, or always: one outline round
  those docked together, ornaments and skin shapes included, fading out over the desktop and
  other programs' windows, and never lying over any of them. A window that isn't docked to
  the others, as a browser window put down over them, has a glow of its own that lies over
  the windows under it. Its size, opacity, spread and colour are set with sliders in
  Winamp's Preferences, where it can also go round Exodium's windows only, or be turned off
- Can be the default browser in Windows: a link opens in a new tab of the window in front,
  or in a new window if none is open, and Winamp is started for it if it isn't running
- Can be in the Start menu, and started from Win+R as `exodium`: a new window on the home
  page, with Winamp started for it if it isn't running
- Can be driven by Claude Code, to test sites in a window of its own
- DevTools docked in the window, beside the page or under it, as in Chrome, where the
  debugging port is on (see [DevTools](#devtools))
- Winamp's visualizers (the main window's analyzer, MilkDrop, AVS) show the sound a page
  makes while Winamp has nothing else to play, with the input plugin `in_exodium.dll`

A second, unrelated plugin lives beside it: `gen_llama.dll` plays Winamp's llama sound
at start-up when the splash screen is enabled.

## Keyboard

| Keys | |
| --- | --- |
| Ctrl+T / Ctrl+W | New tab / close tab |
| Ctrl+Shift+T | Reopen the last closed tab, or the last closed window with all its tabs |
| Ctrl+Tab / Ctrl+Shift+Tab, Ctrl+Page Down / Ctrl+Page Up | Next / previous tab |
| Ctrl+1 to Ctrl+8, Ctrl+9 | The tab with that number in the strip; the last tab |
| Ctrl+N | New window, in Winamp's own windows as well |
| Ctrl+Shift+N | New private window (see [Private windows](#private-windows)) |
| Ctrl+Shift+W, Alt+F4 | Close window |
| Ctrl+L, Alt+D, F6 | Address bar |
| Tab / Shift+Tab, in the address bar or on a toolbar button | On into the page: to the first thing on it that takes the keyboard / to the last. While the address bar's list is open, Tab goes down the list instead |
| Ctrl+H | History |
| Ctrl+D | Bookmark the page in view, or take its bookmark away |
| Ctrl+Shift+O | Bookmarks |
| Ctrl+J | Open or close the downloads panel, while there are downloads |
| Alt+Left / Alt+Right | Back / forward |
| F5, Ctrl+R / Ctrl+F5, Ctrl+Shift+R | Load the page again / without what was kept of it. In a page these are the webview's own; they now work from the address bar and the toolbar too |
| Ctrl+0 | Put the site back to its normal zoom |
| Ctrl+Shift+Y | Open or close Bitwarden's popup, as in Chrome (while Bitwarden is on) |
| F12, Ctrl+Shift+I | Open the DevTools of the tab in view, docked in the window, or close them (see [DevTools](#devtools)) |
| Ctrl+Shift+J / Ctrl+Shift+C | Open them on the console / to pick an element of the page with the mouse |
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

A right click on a tab brings up its menu (`tab_menu.cpp`): *New tab to the right*,
*Reload*, *Duplicate* (another tab on the same page, with the same Back and Forward),
*Mute tab*, *Close*, *Close other tabs*, *Close tabs to the right* and *Reopen closed tab*.
Reload, Mute and Close take every selected tab when the tab clicked is one of several. A
muted tab stays muted until it is unmuted, whatever page it goes to, and shows a speaker
crossed out; Winamp's visualizers don't show a muted page's sound. The mouse resting on a
tab is told its page's whole title and its site, which a tab squeezed among many has no
room for.

## The address bar

What is typed is taken for an address or a search by the text alone (`ResolveAddressInput`
in `url_util.h`):

- An address with its scheme goes as it stands: `https://example.com`, `edge://gpu`,
  `about:blank`. `exodium://history` and `exodium://bookmarks` are the browser's own pages.
- A site's name without one goes over https, with whatever follows it:
  `github.com/anthropics/claude-code`, `example.com:8443/app?x=1`.
- This computer and the ones beside it go over http: `localhost:3000`, `127.0.0.1:8000`,
  `192.168.1.1/admin`, `[::1]:8080`, a name ending in `.local`, `.localhost`, `.test`,
  `.internal` or `.lan`, and a bare name with a port, `nas:8080`.
- The path of a file, `C:\pages\a.html` or `\\server\share\a.html`, opens the file.
- Anything else is searched for. That includes text like `TypeError: x is not a function`,
  which reads as an address with a scheme to Windows; names that end as files do and no
  sites do, such as `three.js` or `notes.txt`; and `javascript:` addresses, which are not
  run from the address bar.

Until 2026-11-09 a path after a site's name made it a search (`example.com/docs` was only
an address if it ended like `.html`), `localhost` and addresses in numbers were searched
for, `localhost:3000` was handed to the webview without `http://`, and an error message
with a colon in it went nowhere.

A row of the list that the history put there ends in a ×, which forgets the row: what the
history has behind it is taken out of the history, in its journal at once, and the list is
made again without it. It is suggested again once it is visited again. What goes depends on
the row (`PagesBehind` in `suggestions.h`):

- A page: that page.
- A site, which is what is offered while the typing is still inside a site's name, and which
  every page visited on the site speaks for: every page of the site in the history, over
  http and https, with `www.` and without. Nothing less would stop it being suggested.
- A search made before: every results page of those words, on whichever search engine.

A row that is only there for the typed text, or for a bookmark, has no ×: a bookmark is
suggested for as long as it is one. A page that is in the history and a bookmark has one,
which forgets its visits and leaves it there with its star. The × works as a tab's does:
pressing it neither picks the row nor goes anywhere, and letting go anywhere but on it
forgets nothing. There is no undoing it, short of visiting the pages again.

## Zoom

Zoom belongs to the site, as it does in Chrome: Ctrl with the wheel, + or − zooms the page
for its host, every tab on that host follows, and it comes back whenever the host is
visited again (`zoom.txt`). Ctrl+0 puts the site back to 100%.

While the page in view is at anything but 100%, the toolbar says so between the address bar
and the star: the percentage, with a − on its left and a + on its right. They go through
the same sizes as the keys, Chrome's (25, 33, 50, 67, 75, 80, 90, 100, 110, 125, 150, 175,
200, 250, 300, 400 and 500%), and a click on the percentage puts the page back to 100%. At
100% none of the three is there and the address bar has their room, and a window too narrow
for them and the address bar both goes without them.

A page that has no site, such as a file, is zoomed for its tab alone, until the tab goes
somewhere else; so are a private window's pages and Claude Code's.

## Bookmarks

The star at the address bar's right end is empty while the page in view is no bookmark and
filled while it is one. A click on it, or Ctrl+D, bookmarks the page or takes its bookmark
away. Bookmarks are offered in the address bar's list with a star in front, whether they
have been visited or not, and one is filled in as it is typed, as an address typed in
before is.

`exodium://bookmarks` (Ctrl+Shift+O) lists them, the one added last first, with a box to
search them and a button on each to remove it. Where Chrome, Edge, Brave or Vivaldi keeps
bookmarks on this PC, the page has a button to bring them in: those that aren't here yet
are added after the ones that are, without their folders, and nothing of the other
browser's is changed. Only its first profile is read (the one it calls *Default*), and
only pages are taken: not its `chrome://` pages or bookmarklets.

They are kept in `bookmarks.txt`, a line for each (address, when it was added, title),
which is written whenever they change.

## Sleeping tabs

A tab that isn't in view still has its page, in a process of its own that keeps everything
the page holds. Measured on 2026-11-09 after a day's use, that was 24 processes and 4.9 GB.
So a tab left out of view for two hours is put to sleep (`sleeping_tabs.cpp`): its webview
is closed, which ends its process and frees all of that at once, and the tab stays in the
strip with its title and icon. Looked at again, it is given a new webview and its Back and
Forward, as a tab is that comes back when Winamp starts, and its page loads afresh. It is
what Chrome's Memory Saver does.

What a sleeping page loses is whatever it held that isn't in its address: where it was
scrolled to, what a script had worked out. A tab is left awake while there is a sign that
it is in use:

- The tab in view of each window, whether the window is on the screen or not.
- One whose page is making sound, is still loading, or has a download going.
- One with something typed into a form that hasn't been sent, or text in an editor. Its
  page is asked just before it would be put to sleep; one that doesn't answer stays awake.
- Claude Code's tabs.

A sleeping tab's webview is closed, not suspended, so it works the same with the debugging
port open: a suspended page would be one that doesn't answer Claude Code's tools. A tab of
yours that is asleep has no page for those tools to list until it is looked at again.

*Put unused tabs to sleep* on Exodium's page in Winamp's Preferences turns it off, and
`sleep_tabs_minutes` under `[Exodium]` in `winamp.ini` says how long a tab is left, from 1
minute to a week; it is read when Winamp starts.

## Private windows

Ctrl+Shift+N opens a private window, for pages that are to leave nothing behind, as
Chrome's Incognito windows and Edge's InPrivate ones are. Its title ends in *(Private)*,
in its title bar, on the taskbar and in Alt+Tab.

- Its tabs' webviews are in the browser profile's private session: apart from the other
  windows' cookies, sign-ins and site data, shared between the private windows, and gone
  once the last private tab is closed.
- Nothing of them goes into the history, into the windows and tabs kept for the next start,
  or among the tabs Ctrl+Shift+T brings back, which does nothing in a private window. A
  zoom set in one lasts until its page changes, and isn't kept for the site.
- Every tab opened in one is private, and so is a window one of its pages opens. Tabs aren't
  dragged between private windows and the others: dropped on the other kind's strip, they
  get a window of their own kind.
- Extensions aren't in them, Bitwarden included, and their tabs aren't put to sleep.

Files downloaded in one are kept, and so are bookmarks made in one, as in Chrome. The
address bar still suggests from the history and the bookmarks, and adds nothing to them; a
row forgotten there by its × is forgotten in every window. What a site sees, and what the
network sees, is the same as from any other window.

## The history on disk, and the log

`history.txt` is written whole as Winamp exits. Until 2026-11-09 that was the only time it
was written, so a Winamp that crashed or was cut off lost the history of its whole run.
Now every visit is added to `history.journal` beside it within two seconds, a line for
each, and a start that finds a journal does what it says to the history it reads
(`history.h`, `history_store.cpp`). The journal is done with whenever the history is
written whole: at exit, at a start that found one, and when it has grown past half a
megabyte.

The history keeps the 20,000 pages visited last (it was 2,000, which a month's browsing
filled). The address bar looks through all of them for every key typed, which now takes
about half as long for each page as it did: measured side by side on a real history, 2,000
pages took 4.0 ms a key before and take 2.3 now, and ten times as many take ten times that.

`exodium.log`, in the same folder, has what goes wrong out of sight, with the time: a
webview that couldn't be made, a page's process that ended and why, an address the webview
wouldn't take, a file that couldn't be written. It is kept under a megabyte (the last half
of it as `exodium.log.old`).

## Picking up where it left off

When Winamp starts, the browser windows that were open when it closed come back, as
Chrome's *Continue where you left off* has it (`session.cpp`):

- Each window where it was, the size it was, maximized or minimized if it was, and the
  one that was in front in front. They come up once Winamp has finished starting, over
  Winamp's own windows, as a link's window does.
- Each with its tabs in their order, the one that was in view in view, and each tab with
  its Back and Forward: the pages behind it and ahead of it, with their titles.
- Only the tabs in view load at once. The others have their page's title until they are
  first looked at, and load then, as in Chrome.
- The first window, the one Winamp's menu shows, comes back out of sight if it was out of
  sight, with its tabs, for when it is next shown.
- Claude Code's windows and tabs don't come back: they belong to its session.
- Started from the Start menu's entry, Winamp brings the windows back and opens no window
  on the home page besides them; the one in front gets the keyboard instead.

Ctrl+Shift+T brings back a closed window whole: the window closed last, if it was closed
after the tab closed last, comes back with all its tabs, each with its Back and Forward,
at the place it had. A tab closed on its own comes back into the window it is pressed
in, as before. Closed windows and tabs are kept from one run of Winamp to the next, the
latest 25 of them, a window counting as one, so Ctrl+Shift+T reaches back past a
restart. The first window, closed and shown again from Winamp's menu, is a window with a
tab on the home page; Ctrl+Shift+T in it puts the tabs it had back in it, rather than in
a window of their own. So does Ctrl+Shift+T in any window with a tab that hasn't been
anywhere but the home page, as Ctrl+N makes.

With *Reopen the windows that were open when Winamp closed* unticked in the Preferences,
Winamp starts with no browser window, as it did before, and the windows open when it
closed are kept as windows closed whole: Ctrl+Shift+T brings them back, the one that was
in front first.

How it is kept: `session.txt` in `Plugins\exodium_data` (`session_file.h` describes the
lines), written every two seconds when anything in it has changed and once more as
Winamp exits, so a Winamp that crashes loses two seconds of it at most. Each tab's pages
are asked of its webview (the DevTools protocol's `Page.getNavigationHistory`) whenever
its history changes, and the latest 50 of a tab are kept. A tab is given its pages back
the way Ctrl+click on Back gives a new tab a copy of a tab's history (`back_forward.cpp`):
its webview goes to each page in turn as a blank stand-in that never reaches the site,
then steps to the page it is to show, which loads for real. It shows a spinner meanwhile,
and takes Back and Forward once that page has started loading. A page that was a form's
answer comes back as its plain address; pages made by script (`blob:`,
`javascript:`) and addresses longer than 8192 characters are left out.

A tab closed on its own keeps its webview for a while, as before, and comes back as it
was, its page from the back/forward cache if it is still there. A tab that had pages
ahead of the one it was on comes back from its pages instead, as Forward needs them: the
webview put away lost them when it left its page. So does a tab whose webview went with
its window, or with the debugging port on, which keeps none. A tab is given its pages
back too when the browser process dies, which takes every webview with it.

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

## Bitwarden

Bitwarden's extension for Chrome runs in Exodium, in the browser's profile, where it fills
sign-in forms from the little menu it puts in a page's fields. WebView2 has no toolbar for
extensions, so Exodium has a button of its own at the toolbar's right end, with Bitwarden's
picture: it opens Bitwarden's popup under it, the page Chrome shows under the extension's
icon. So does Ctrl+Shift+Y. The popup goes as Chrome's does: on a click anywhere else, on
Escape, on the button or Ctrl+Shift+Y again, and when Bitwarden closes it after filling a
page. What Bitwarden opens itself, as when it pops its popup out into a window, opens in a
window of Exodium's, the size of the popup.

It is fetched from the Chrome Web Store the first time Winamp starts with it on, and is
there a few seconds later; the pages open by then get its menu once they are loaded again.
The store is asked once a day after that. A newer version is fetched in the background and
put in place the next time Winamp starts, since an extension's files can't be changed under
it while it runs. Nothing is put in place unless the package is signed with Bitwarden's own
key, the one its id comes from (`extension_package.h`); the store's hash of the package is
checked too. It is kept in `Plugins\exodium_data\extensions\bitwarden`, and its data (the
account, the vault) in the browser profile, from one version to the next.

Two things are changed in Bitwarden's manifest on the way. Its key goes in, so that it has
the id it has in Chrome, which the Bitwarden desktop app knows it by (for unlocking with
Windows Hello through the app). And the side panel's permission comes out:
`chrome.sidePanel.open` takes WebView2's whole browser process down, every tab with it,
and without the permission Bitwarden opens a window instead.

Bitwarden fills the tab its popup is in, and a WebView2 page is a window of its own to it,
so its popup would be in no tab. It is told the tab instead, the way it tells its own popped
out windows, by a number only extensions can find out: a small extension of Exodium's own,
`Plugins\exodium_data\extensions\exodium_tab_helper`, opens in the popup first and turns the
tab's DevTools id into that number (`bitwarden_popup.cpp`).

Bitwarden keeps an unlocked vault's key in the browser process's memory alone, unless told
otherwise in its own settings: its lock is "on browser restart" to begin with. WebView2 ends
that process as soon as its last page goes, which is when every window of Exodium's has been
closed, while Winamp goes on; the vault then locked, and the next login field brought up
"Unlock your account" instead of the logins. So while Bitwarden is on, a page of the user's
profile that is never shown (it stays on `about:blank`) keeps the process up until Winamp
exits (`extensions.cpp`). With every window closed that costs about 280 MB of committed
memory, most of it Bitwarden's own process (measured 2026-11-11, the working sets trimmed
to almost nothing within seconds). The vault still locks when Winamp restarts, as it would
with Chrome's restart; Bitwarden's PIN unlock (with "lock with master password on browser
restart" off) makes that a few keys.

The "Unlock account" in Bitwarden's menu in a page asks for a window of Bitwarden's own to
unlock in. That opens its popup under the toolbar button instead (`OpenRequestedWindow` in
`webview.cpp`), which goes again once Bitwarden moves on from its lock page, and gives the
page the keyboard back: Bitwarden's own window would stay open, as WebView2 doesn't pass on
its closing that window or bringing the page back to the front. The login field is then
clicked again for the logins, where Chrome brings the menu back by itself.

What doesn't work as in Chrome: Bitwarden's own keyboard shortcuts (WebView2 doesn't pass
them on; Ctrl+Shift+Y is Exodium's), the entries it adds to a page's context menu, and the
count of logins it would show on its icon. Its other windows, such as the one for a new
login from its menu in a page, open as windows of Exodium's and don't close themselves when
Bitwarden is done with them, for the same reason as above.

The checkbox *Bitwarden password manager* on Exodium's page in Winamp's Preferences turns
it off and on again, at once: off, it stops running and its button goes, but it stays in
the profile with its data, and isn't updated. Claude Code's windows never have it; see
[Letting Claude Code drive it](#letting-claude-code-drive-it).

## DevTools

WebView2 opens a page's DevTools in a window of their own, and has no way to dock them.
Where the debugging port is on (`remote_debugging_port` in `winamp.ini`, as for
[Claude Code](#letting-claude-code-drive-it)), Exodium docks them in the browser window
instead, beside the page or under it, as Chrome does. Without the port they are the
webview's own, as before.

- F12 and Ctrl+Shift+I open them for the tab in view, and close them again, from the page,
  the address bar or the DevTools themselves. Ctrl+Shift+J opens them on the console, and
  Ctrl+Shift+C to pick an element of the page with the mouse.
- *Inspect* in a page's own menu opens them on the element the menu was opened on.
- A strip along their top has what the DevTools' own menu has in Chrome: buttons that dock
  them on the left, at the bottom or on the right, one that gives them a window of their
  own (the webview's, for that once), and one that closes them. The bar between them and
  the page sizes them when dragged; the page and the DevTools each keep some room.
- Each tab has its own, shown with the tab. They go with the tab when it is dragged to
  another window, and close with it. A tab with its DevTools open isn't put to sleep.
- Ctrl with + and -, or with the wheel, makes them larger and smaller, all of them
  together, and Ctrl+0 normal again.
- The browser's shortcuts for tabs and windows work from inside them, as they do in
  Chrome (Ctrl+T, Ctrl+W, Ctrl+Tab, Ctrl+1 to 9, Ctrl+N and the like). The rest of what
  the browser takes from a page (Ctrl+L, Ctrl+D, Ctrl+H, Ctrl+J, Alt with the arrows) is
  the DevTools' own there, and F5 loads the page they are for again.
- The side they are on, the size they were dragged to and their zoom are kept in
  `winamp.ini`, under `[Exodium]`: `devtools_dock` (`right`, `bottom` or `left`),
  `devtools_width`, `devtools_height` and `devtools_zoom` (50 to 300).

How it works (`devtools_panel.cpp`): the debugging port serves the DevTools themselves as
a page, for any page the browser has, and takes that page's connection back to it. A tab's
docked DevTools are a second webview in its window on that page, at
`http://127.0.0.1:<port>/devtools/devtools_app.html?ws=127.0.0.1:<port>/devtools/page/<id>`,
and the tab's page gives up one side of its place to it. (The address has to say
`127.0.0.1`: the port refuses the connection from `localhost`, or from any other site.)
That webview is in a browser profile of its own, *DevTools*, so that what the DevTools
keep of themselves (their settings, the tool they showed last) stays from one run to the
next, and none of the user's extensions run in them. A private window's are in that
profile's private session and keep nothing: not what was typed into the console, nor where
a script was stopped. If the DevTools can't be loaded from the port, as when another
program has it, they open in the webview's own window instead, and `exodium.log` says why.

What isn't as in Chrome:

- There is no button for emulating a phone or tablet. In Chrome that is the browser's
  doing as much as the DevTools', and the DevTools served by the port come without it. The
  button for a window of their own opens the webview's, which have it.
- The first time, Edge's DevTools open on their *Welcome* tool. They open on the tool
  that was last picked with the mouse from then on.
- A page with no icon of its own gets the browser's plain one on its tab while its
  DevTools are open.
- Tools on the debugging port, Claude Code's among them, see each open DevTools as one
  more page.

Selecting the element for *Inspect*, picking one with Ctrl+Shift+C, and showing the
console in DevTools that are already open are asked of the DevTools' page by script, in
terms of its own modules; everything else is its address. Should a later version of the
DevTools have those modules otherwise, the DevTools still open, on the right tool, and
only those three do no more than that.

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

The plugin keeps its data in `Plugins\exodium_data`: `history.txt` and `history.journal`
(see [The history on disk, and the log](#the-history-on-disk-and-the-log)),
`bookmarks.txt`, `zoom.txt`, `session.txt` (the windows and tabs, see
[Picking up where it left off](#picking-up-where-it-left-off)), `exodium.log` and the
browser profile (cookies, sign-ins, cache). The window's place and the settings are kept in `winamp.ini`.

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
- **Reopen the windows that were open when Winamp closed**: see
  [Picking up where it left off](#picking-up-where-it-left-off). On to begin with; unticked,
  those windows can still be had back with Ctrl+Shift+T.
- **Show close buttons on tabs**: the × at each tab's right end. Without them, a click
  there is a click on the tab, and tabs are closed with Ctrl+W, the toolbar's close
  button or a middle-click.
- **Ctrl+N in Winamp opens a new Exodium window**: see [Keyboard](#keyboard). On to begin with.
- **Bitwarden password manager**: see [Bitwarden](#bitwarden). On to begin with. It sits
  beside the first checkbox, as there is no room under the others.
- **Show Exodium in the Start menu**: makes or removes the Start menu's entry at once (see
  [The Start menu](#the-start-menu)). It shows whether the entry is there, however it got
  there or went, and isn't kept in `winamp.ini`. It needs `exodium_open.exe` in `Plugins`;
  without it, the page says so and the box can only be unticked.
- **Put unused tabs to sleep**: see [Sleeping tabs](#sleeping-tabs). On to begin with. It
  sits beside the Start menu's checkbox, as Bitwarden's does beside the close buttons'.
- **Winamp's visualizers show page sound while Winamp is stopped**: see
  [The pages' sound in Winamp's visualizers](#the-pages-sound-in-winamps-visualizers). On
  to begin with; unticked while Winamp is playing a page's sound, Winamp stops.
- **Corner ornaments**: the skin of the ornaments on the windows' corners: *Iron*, as they
  always were, *Venom*, *Charms* or *Dark Charms*. Every window changes at once. Venom's
  top right pod reaches further in, so the tab strip ends a little earlier with it. The
  Charms skins have their buttons in a thick bar, which lies over the right end of the tab
  strip when the window is maximized or at the top of the screen: the strip ends some 320
  pixels from the right then.
- **Glow round**, in the *Looks* box with the corner ornaments: *All of Winamp*, *Exodium
  only* or *Off*. While a window of Winamp's is in front (the browser's, the library, the
  main window, the Preferences...), a glow goes round the outline of Winamp's windows, or
  of the browser windows only: round the ornaments' ironwork and skins' shaped windows too.
  Windows docked together, edge to edge as Winamp docks them (within 2 pixels), glow as one,
  and their glow never lies over any of them. A window that isn't docked to the others, such
  as a browser window put down apart from them or over them, glows on its own: its glow lies
  over the windows under it, Winamp's too, and theirs over it where it is under them, as
  shadows do. Windows that only overlap, or meet at a corner, aren't docked. Dialogs, menus
  and ornaments go with the window they belong to. The glow fades out over the desktop and
  other programs' windows, and goes as soon as another program comes to the front, unless
  it is always there. A maximized window has none, as its glow would be off the screen.
  Where another window is between docked ones in the Z order, the windows above it and below
  it each have a glow of their own, under them. See `glow_shape.h` for how.
  - **Always, not only while Winamp is in front**: the glow stays with another program in
    front too, under Winamp's windows wherever they are among the others. Off to begin with.
  - **Size**: how far out it goes, 10 to 200 of Winamp's pixels; 60 to begin with.
  - **Opacity**: how solid it is at the windows' edges; 100% to begin with.
  - **Spread**: how far out it stays that solid before it fades; 0% to begin with.
  - **Color...**, or a click on the square beside it: Windows' colour picker. Black to
    begin with.
  - **Default** puts those back as they were, which is the look of the mock-up the glow
    was made from: black at the edge, fading to nothing over some 54 pixels.

  The Preferences are one of Winamp's windows, so the glow is there to be seen while they
  are open: a slider shows what it does as it is dragged, and is written to `winamp.ini`
  once it is let go.

The rest are kept in the `[Exodium]` section of `winamp.ini`, beside `remote_debugging_port`.
A setting is only written there once it has been changed:

```
[Exodium]
home_page=https://example.com
search_engine=custom
custom_search_url=https://kagi.com/search?q=%s
corner_skin=venom
glow=exodium
glow_size=90
glow_opacity=80
glow_spread=20
glow_color=7f00ff
glow_always=1
restore_windows=0
tab_close_buttons=0
ctrl_n_in_winamp=0
sound_in_visualizers=0
bitwarden=0
sleep_tabs=0
```

Two more are read from that section when Winamp starts and aren't on the page:
`remote_debugging_port` (see [Letting Claude Code drive it](#letting-claude-code-drive-it))
and `sleep_tabs_minutes`, how long a tab is left out of view before it is put to sleep
(120 unless it says otherwise). And the browser keeps four there itself, for where the
DevTools are docked and how large: `devtools_dock`, `devtools_width`, `devtools_height`
and `devtools_zoom` (see [DevTools](#devtools)).

`search_engine` is `google`, `bing`, `duckduckgo`, `brave`, `ecosia`, `startpage`, `yahoo`
or `custom`. `corner_skin` is `iron`, `venom`, `charms` or `darkcharms`. `glow` is
`winamp`, `exodium` or `off`. `glow_size` is 10 to 200, `glow_opacity` 0 to 100 and
`glow_spread` 0 to 90; a number past either end is taken as that end. `glow_color` is
`RRGGBB` in hexadecimal, as on the web.

To add a checkbox to the page:

1. In `settings.h`, a `bool` in `Settings` with its default, and a line in `kFlagSettings`
   with its key in `winamp.ini`. Reading and writing it come with that, and the unit test
   `EveryFlagIsKeptUnderItsOwnKey` checks it.
2. An id in `resource.h`, after the last checkbox's, and a `CONTROL` line in
   `res\exodium.rc` under the last checkbox, 12 units further down. The page is full: the
   note at the foot of the *Options* box (`IDC_PREFS_START_MENU_NOTE`) and the *Looks* box
   move down with it, and the boxes above it have to give up the room. A checkbox with
   few words can share a row with another instead, on its right at 140 units, as
   Bitwarden's and the sleeping tabs' do.
3. A line in `kCheckboxes` in `settings_page.cpp`.
4. The browser reads it from `g_app.settings`. If something has to happen the moment it
   changes, that goes in `ApplySettings()` in `settings_page.cpp`.

To add a skin for the corner ornaments:

1. Have an image AI draw the four corners the way they sit on a window, each in its own
   quarter of the picture, on black, with the minimize, maximize and close buttons on the
   top right one. Giving it a picture of Iron's four corners as an example works.
2. Cut it up with `tools\cut-corner-skin.py` (Python with numpy, Pillow and scipy). It
   needs the middles of the three buttons and the row where the top right pod ends, in the
   picture's pixels:
   `python tools\cut-corner-skin.py picture.png name --buttons 1340,81 1405,81 1471,81 --pod-bottom 153 --preview check.png`.
   It writes the four pieces to `exodium\res`, and prints the skin's line. Where a corner
   reaches past the picture's middle line, `--split X,Y` puts the line in the gap instead.
   A picture on a see-through background keeps its own see-through parts. Buttons larger
   than Iron's take `--button-radius` (34 for Charms). Check the preview on white for
   holes or specks.
3. That line goes in `kCornerSkins` in `corner_skins.h`; its four ids in `resource.h`; the
   pictures in `res\exodium.rc` and `exodium.vcxproj`. Keep the picture it was cut from in
   `res\sources`.

## The pages' sound in Winamp's visualizers

With `in_exodium.dll` in `Plugins`, Winamp's visualizers show the sound Exodium's pages
make: the analyzer or oscilloscope in Winamp's main window, and whichever visualization
plugin is running, such as MilkDrop or AVS. It happens by itself, under these rules
(`sound_rule.h`):

- It starts when a page starts making sound and Winamp is stopped. While Winamp plays or
  is paused on something of its own, that is left alone.
- Winamp then shows it as a track, *Exodium:* and the page's title, which follows the page
  (the next video's title, say). Its time counts up from when the sound started.
- It stops as soon as the pages go quiet (WebView2 says so some 0.3 s after the sound
  stops), and Winamp goes back to showing the playlist's track. A gap between two videos
  stops it too, and the next video starts it again.
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
- `in_exodium.dll` plays nothing itself and never uses Winamp's output plugin. It is made
  to show the newest sound there is: the pictures show sound 12 to 23 ms old, half of it
  under 18 (it was 30 to 34 until 2026-11-07). Of that, 12 ms is the 576 frames a
  visualizer takes, whose newest is 0 to 11 ms old: Windows mixes in 10 ms packets, and
  there is nothing newer between them. It records at 48 kHz, which Windows mixes
  at, so nothing is resampled. As each packet of sound comes, every 10 ms, it hands Winamp
  the newest 576 frames three times, under times 1 ms apart (`kVisBlockCopies`), and tells
  Winamp it is playing just before the first (`g_shownFromMs`). Winamp gives a
  visualization plugin, at each picture, the first block it hasn't yet that starts after
  that time, so every picture shows the newest sound there is, and a plugin that draws up
  to three pictures before the next packet still gets it each time; one that drew more
  would get nothing, or sound from long before (at 260 pictures a second, a fifth of them). Plugins that ask for their sound ahead of
  that time (AVS by 14 ms, the classic one by 10) are told a time that much earlier: it
  finds which one runs and reads what it asks for out of its memory (`vis_latency.h`).
  Its recording thread is scheduled as Windows' audio threads are (MMCSS).
- From a page starting to make sound to the visualizers showing it takes some 120 ms,
  nearly all of it Chromium's: starting the sound and noticing the page is making it.
  Exodium takes about 1 ms of that, and the first 10 ms packet of sound makes blocks.
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
isn't running, and sends the link to the plugin as a `WM_COPYDATA` (`link_ipc.h`). `.htm`
and `.html` files open the same way. The plugin turns links down until Winamp has
finished starting, and `exodium_open.exe` keeps asking, because Winamp's main window
takes the windows it owns to the front as it starts, and that would bury a browser
window, which it doesn't own.

A link opens as Chrome opens one: in a new tab at the end of the strip of the browser
window nearest the front, which comes to the front with the tab in view
(`FrontUserWindow` in `browser_window.cpp`). Nearest the front is by the windows' order
on the screen, so with another program in front it is the browser window that was used
last. A minimized window is only taken when no other is open, and is brought back; a
video that has the screen gives it up. Private windows and Claude Code's are passed over,
and with no window open at all the link gets a new one.

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

1. Copy `tools\exodium_mcp.mjs`, `tools\exodium_start.html` and `tools\exodium_hook.mjs`
   into Winamp's `Plugins` folder, beside `exodium_open.exe`.
2. Add this to `winamp.ini`, and restart Winamp:

   ```
   [Exodium]
   remote_debugging_port=9333
   ```

3. Tell Claude Code about it:

   ```
   claude mcp add --scope user exodium -- node "<Winamp>\Plugins\exodium_mcp.mjs"
   ```

4. Add the hooks to Claude Code's `settings.json` (`%USERPROFILE%\.claude`), with forward
   slashes in the path:

   ```json
   "hooks": {
     "PreToolUse": [{ "matcher": "mcp__exodium__.*", "hooks": [{ "type": "command", "command": "node \"<Winamp>/Plugins/exodium_hook.mjs\"", "timeout": 10 }] }],
     "SubagentStop": [{ "hooks": [{ "type": "command", "command": "node \"<Winamp>/Plugins/exodium_hook.mjs\"", "timeout": 10 }] }],
     "Stop": [{ "hooks": [{ "type": "command", "command": "node \"<Winamp>/Plugins/exodium_hook.mjs\"", "timeout": 10 }] }]
   }
   ```

   Without them the tools still work, but every agent of a session shares one window,
   and windows close only when they are left unused or the session ends.

The port is off unless `winamp.ini` names one, because **any program on this PC can use
it while it is open, and with it every site the browser is signed in to**, and Bitwarden's
vault while it is unlocked. It listens on
127.0.0.1 only, and refuses web pages, so sites can't reach it. Remove the setting and
restart Winamp to close it. While it is open, a closed tab is closed outright rather than
kept, so Ctrl+Shift+T brings it back without Back and Forward.

`exodium_mcp.mjs` sits between Claude Code and Playwright's server (see the top of the
file for why). Claude gets a window of its own and keeps out of the tabs in use, each new
page is an Exodium window, and Winamp is started if it isn't running.

Every agent works apart from the others. A session's subagents call the tools through
the same process as the main conversation, and the calls themselves don't say whose they
are, so two agents working at once used to drive the same page and read each other's
results. The `PreToolUse` hook writes down which agent each call is from, in
`%TEMP%\exodium-mcp\agents`, and the wrapper gives every agent a Playwright server of its
own: its own windows, current page, tab list, console and recording. Each agent is shown
only its own tabs, and different agents' calls run side by side. Calls the hook said
nothing about are the main conversation's.

An agent's windows close once it has finished with them, and nothing else's:

- `browser_close` closes the calling agent's windows.
- A subagent's close as soon as its task ends (`SubagentStop`), and its Playwright server
  stops.
- The main conversation's close two minutes after its turn ends (`Stop`), so that a quick
  answer from the user finds them as they were.
- Any agent's close after ten minutes unused.
- None of them close while the user is in one: real mouse or key input in the page since
  the agent's last call, in the last two minutes, puts it off. Each page an agent works in
  notes the time of the last such input. A page can't simply be asked whether it has
  focus, because Playwright tells every page it connects to that it does.

The agent's next call opens a new window and takes it back to the address it was on, and
to the device it had been given, and says so in its answer. `EXODIUM_IDLE_MS` and
`EXODIUM_TURN_END_MS` change the two waits, for testing.

What Claude sees of the rest is kept down:

- Playwright lists every tab in its answers. Claude is shown only its own pages, and the
  pages those opened, with a count of the rest; `browser_tabs` with `all: true` lists the
  user's too, for when they point Claude at one.
- Video is recorded of Claude's page only, where Playwright would record every tab.
- A console message repeated thousands of times is shown once, with its count.
- Playwright keeps a page's file picker from opening, so that `browser_file_upload` can
  answer it, and left to itself it does that to every tab; only the page Claude is
  working in has its picker caught, so upload buttons in the other tabs open the usual one.
- The user's password manager is theirs alone. Claude's own windows don't have it, but a
  tab of the user's that Claude is pointed at does: Bitwarden's menu there is a frame of the
  extension's in the page, and its popup and windows are the extension's pages. The wrapper
  refuses to go to an extension's page or select one (they are left out of the tab lists,
  with a count), and in a tab of the user's it refuses a click, drag, hover, typing or
  evaluation aimed at an element in an extension's frame (by ref or selector), a click or
  drag by coordinates that lands on one, and keys while the keyboard is in one (Bitwarden
  takes it there on ArrowDown). That check costs Playwright's half second, and is only made
  in the user's tabs, for calls that aim at something. Screenshots of such a tab still show
  Bitwarden's menu when it is open. This keeps Claude's browser tools away from it; it is no
  wall against a program on the PC, which the port lets reach everything, Claude Code's own
  shell included.

Exodium knows Claude's windows from the page they open on, and treats them differently:

- They have a browser profile of their own, `Claude`, apart from the user's: none of the
  user's sign-ins, cookies, site data or extensions reach them, and nothing Claude does in
  them reaches the user's. Bitwarden in particular is never in them. A page one of them
  opens, and a tab copied from one with Back or Forward, is in the same profile. A runtime
  too old to have profiles gives Claude no window rather than one in the user's profile.
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
it's missing, registers the browser and puts it in the Start menu, adds the MCP server
and points its hooks at the new Winamp folder, and lists what's left to do.
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

Unit tests cover everything that needs neither a window nor a webview: addresses and what
typed text is taken for, the history and its journal, bookmarks and other browsers' files of
them, suggestions, zoom, the files they are kept in, a tab's Back and Forward and how
they are copied, the session file of windows and tabs, search engines, the settings and how `winamp.ini` keeps them, where a
tab's icon, speaker, close button and title go, where Ctrl+N in Winamp's windows opens a
browser window, what the downloads panel says about a download, where docked DevTools go
on each side and what is kept of them, `exodium_open.exe`'s
command line, and when Winamp plays the pages' sound and how `in_exodium.dll` cuts it
into blocks for the visualizers. And extension packages: the store's answer, a CRX3
package signed by a key the test makes (and changed after signing, or signed by another
key), inflating, zip archives and the names in them, and the manifest's changes. With
`EXODIUM_TEST_CRX` naming a copy of Bitwarden's real package, that is checked and unpacked
too.

```
msbuild tests\exodium_tests.vcxproj /p:Configuration=Release /p:Platform=Win32
bin\Win32\Release\exodium_tests.exe
```

The rest is covered by a smoke test that runs the built plugin in a scratch copy of
Winamp, beside the one in use and without touching it, and works it by window messages:
pages, tabs and their close buttons, selecting several tabs and dragging them between
windows, Back and Forward opened in new tabs, pages that open tabs and windows of their
own, a download and the downloads panel, minimizing and maximizing, the corner ornaments
and their buttons, the glow's window under and round Winamp's windows (and how many ms it
takes to follow a window moved or sized, which it prints), closing, what is on disk after Winamp exits, links from outside with
Winamp running and closed, and Exodium's page in Winamp's Preferences: opening it,
changing what is on it, what that writes to `winamp.ini`, and the browser following it.
Its Start menu checkbox makes and removes a shortcut in a scratch folder, which is then
opened with Winamp running and closed. It types a file's path into the address bar, sends
a tab's menu's commands (as the menu itself does, by `kMsgTabCommand`), presses Ctrl+1,
Ctrl+9, Ctrl+Page Up and Down and Ctrl+D, clicks the star, and opens the bookmarks page.
On a site of its own, served from this computer for as long as it is needed, it types into
the address bar and clicks the × on a page's row and on the site's, and the zoom's − and +
and its percentage, and checks what the history, its journal and `zoom.txt` then hold, how
large the pages are drawn, that the site's other tab follows, and that a narrow window goes
without the zoom's buttons.
In a run of its own, with a tab left for twelve seconds instead of two hours
(`EXODIUM_SLEEP_TABS_MS`), it sees a tab out of view put to sleep and wake up when clicked,
and one with text typed into it stay awake; opens a private window, and checks that its
pages are in a browser context of their own on the debugging port and in none of the
history, its journal, or the windows and tabs kept; then it ends Winamp from outside just
after a page was visited, and checks that the next start has that page in the history
from the journal. Its downloads go to a folder in its scratch copy,
never to the Downloads folder in use. With the debugging port on, it checks that a window
of Claude's is in a browser profile of its own. Bitwarden is off in it, as turning it on
would fetch it from the store, and the test needs no network; Bitwarden and the wrapper's
refusals were tried by hand in a scratch Winamp (see [Bitwarden](#bitwarden)).

It also has a page play a tone too quiet to hear and checks that Winamp plays it for the
visualizers and stops, leaves the playlist alone, and gives way to Stop and Play. What
reaches the visualizers is written down by `vis_probe.dll` (`tests\vis_probe`), a
visualization plugin of the test's own, and `in_exodium.dll` turns the tone up for them
(`EXODIUM_SOUND_TEST_GAIN`). `in_exodium.dll` also marks each block with when it was
heard (`EXODIUM_SOUND_TEST_MARK`, `sound_mark.h`), and the probe reads that back out of
every picture, so the test checks how old the sound each picture shows is. The probe can
be made to draw at another pace, or ask for its sound ahead as other plugins do
(`EXODIUM_VIS_PROBE_TIMING`), for measuring. The scratch Winamp's playlist only ever holds silent WAVs
then, since its settings are a copy of the real ones and Play is pressed. Without
`in_exodium.dll` and `vis_probe.dll` beside the plugin under test, that part is skipped.

```
powershell -ExecutionPolicy Bypass -File tools\smoke-test.ps1
```

It runs on a desktop of its own, which Windows never shows, so it can be left running
while you work: its windows don't come up in front of yours or take the keyboard, and
yours can't get in the way of its drags. `-OnScreen` runs it where it can be watched.

Picking up where it left off has a test of its own, which runs Winamp four times in a
scratch copy of its own, on a desktop of its own as the smoke test does, beside it if need
be: windows and tabs with pages behind and ahead of them, Winamp closed and started again,
their places, titles, Back and Forward, Ctrl+Shift+T on windows closed before and after
the restart, the Start menu's entry starting Winamp, and the setting turned off.

```
powershell -ExecutionPolicy Bypass -File tools\session-test.ps1
```

So do the DevTools docked in the window, in a scratch copy with the debugging port on
(9336, to keep clear of the one in use and the smoke test's): F12 and the other keys from
the window and from inside the DevTools, the strip's buttons and each side's places, the
splitter and its limits, a tab's own DevTools coming and going with the tab, a tab dragged
out with them, *Inspect* in a page's menu on a scrolled page, a private window's, what
`winamp.ini` keeps, and Winamp closing with them open. What the DevTools themselves show
(the page they are connected to, the tool in view, the element selected) is asked of their
page over the port. `-ShotDir` saves pictures of the window with them on each side.

```
powershell -ExecutionPolicy Bypass -File tools\devtools-test.ps1
```

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
exodium\res\sources\      the pictures skins were cut from, which aren't built in
exodium\third_party\      Winamp SDK headers, as they came, and in2.h and vis.h written out to the SDK's layout
exodium_open\             what Windows runs for a link, and the Start menu's entry runs, to get it to the plugin
in_exodium\               the input plugin that gives Winamp's visualizers the pages' sound
gen_llama\                the llama sound plugin
tests\                    unit tests and the small runner they use
tests\vis_probe\          the visualization plugin the smoke test writes down what reaches the visualizers with
tools\smoke-test.ps1      the smoke test
tools\session-test.ps1    the test of the windows and tabs coming back as Winamp starts
tools\devtools-test.ps1   the test of the DevTools docked in the window
tools\register-browser.ps1  registers Exodium with Windows as a browser, and puts it in the Start menu
tools\make-icon.ps1       draws exodium_open.exe's icon
tools\cut-corner-skin.py  cuts a skin for the corner ornaments out of an image AI's picture of the four corners
tools\export-setup.ps1    packs up Winamp, Exodium and their settings for another PC
tools\import-setup.ps1    and sets them up there
tools\exodium_mcp.mjs     what Claude Code runs for its browser tools
tools\exodium_hook.mjs    Claude Code's hook for them: which agent is calling, and when one is done
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
| `library_frame.cpp` | The same ornaments on Winamp's library window: finding its frame, following it, maximizing it |
| `corner_skins.h` | The skins the ornaments can have: their pictures and where the frame's corner, pod and buttons are on them |
| `docked_windows.cpp` | Which of Winamp's windows dock, and bringing the windows docked with one that is activated up with it |
| `glow.cpp` | The black glow round Winamp's windows while it is in front: its own thread, finding the windows, showing the pictures |
| `glow_shape.cpp` | The same as the logic files: which windows glow together, and the glow's picture |
| `tabs.cpp`, `tab_drag.cpp` | Opening, closing, reopening, selecting, moving and dragging tabs, and the close buttons on them |
| `tab_menu.cpp` | The menu a right click on a tab brings up, muting a tab, and what a tab says under the mouse |
| `sleeping_tabs.cpp` | Putting tabs left out of view to sleep, and what keeps one awake |
| `bookmarks_ui.cpp` | The star on the toolbar, Ctrl+D, and what the bookmarks page's buttons do: removing one, bringing in another browser's |
| `history_store.cpp` | The history's file and its journal, and the bookmarks' file |
| `log.cpp` | `exodium.log` |
| `address_bar.cpp` | The address bar, its suggestion list, and the keyboard handling all controls share |
| `webview.cpp`, `webview_events.cpp` | Creating webviews and reacting to what they report |
| `back_forward.cpp` | Back and Forward, and the page they lead to in a new tab or window with a copy of the tab's history; a reopened or restored tab is given its history the same way |
| `session.cpp` | The windows and tabs kept from one run of Winamp to the next: saving them, and bringing them back as Winamp starts |
| `session_file.cpp` | The same as the logic files: the session file, and what of a tab's history it keeps |
| `fullscreen.cpp`, `zoom.cpp` | Fullscreen video; per-site zoom, and what the toolbar says of it |
| `downloads.cpp` | Downloads: following them in place of WebView2's dialog, and what the panel's buttons do to them |
| `downloads_panel.cpp` | The downloads button on the toolbar and the panel it opens |
| `extensions.cpp` | Bitwarden and the helper in the user's profile: fetching Bitwarden from the Chrome Web Store on a thread of its own, putting a new version in place, turning it on and off, keeping the browser process up while it is on |
| `bitwarden_popup.cpp` | Bitwarden's button on the toolbar and the popup it opens |
| `devtools_panel.cpp` | A tab's DevTools docked in its window: their webview on the debugging port's own DevTools page, the keys, *Inspect*, the strip along their top and the splitter |
| `devtools_layout.cpp` | The same as the logic files: where the page, the splitter, the strip and the DevTools go on each side, what `winamp.ini` keeps of them, their address |
| `extension_package.cpp` | The same as the logic files: the store's answer, a CRX3 package's signature, unzipping, the manifest's changes |
| `settings_page.cpp` | Exodium's page in Winamp's Preferences, and what the browser does when a setting changes |
| `start_menu.cpp` | Exodium's entry in the Start menu, which the page's checkbox makes and removes |
| `suggestions.cpp`, `history.cpp`, `history_page.cpp`, `bookmarks.cpp`, `bookmarks_page.cpp`, `session_history.cpp`, `site_zoom.cpp`, `tab_order.cpp`, `tab_parts.cpp`, `url_util.cpp`, `text_util.cpp`, `download_status.cpp` | Logic with no window in it; this is what the unit tests build |
| `json_reader.h` | The same: the JSON reader the DevTools protocol's answers and other browsers' bookmark files are read with |
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
