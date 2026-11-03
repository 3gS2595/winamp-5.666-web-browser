# Window/Tab Session Persistence — Design

## Problem

`gen_browser.cpp` supports multiple browser windows (`g_windows`), each with its own
position, size, and tabs. None of that is currently persisted: every Winamp restart
starts from a single hidden default window with one blank tab. The user wants their
window layout (position/size), the set of open windows, and each window's tabs
(including which tab was active) restored automatically the next time Winamp starts.

## Scope

Persisted per window:
- Position and size (x, y, width, height)
- Ordered list of open tab URLs
- Which tab index was active

Only **visible** windows are persisted. Clicking a window's X button currently just
calls `ShowWindow(..., SW_HIDE)` — it does not destroy the `BrowserWindow` (see
`gen_browser.cpp` `FrameProc`, `WM_CLOSE` case) — so hidden windows are the user's way
of "closing" a window and are treated as closed for session purposes: they're excluded
from what gets saved, and won't reappear on restore.

Out of scope: browser history (already handled separately by `LoadHistory`/
`SaveHistory`), closed-tab stack (`g_closedTabs`), scroll position / form state within
a page, favicons.

## File format

New file `exodium_data/session.txt`, written with the same plain-text, line-based
style already used for `exodium_data/history.txt` (see `HistoryFilePath`,
`LoadHistory`, `SaveHistory`).

```
WIN <x> <y> <w> <h> <activeIdx>
TAB <url>
TAB <url>
WIN <x> <y> <w> <h> <activeIdx>
TAB <url>
```

- A `WIN` line starts a new window record: `x y w h` are the saved `RECT` fields from
  `GetWindowRect` on that window's `embedFrame`, in screen coordinates; `activeIdx` is
  the 0-based index into the `TAB` lines that follow, up to the next `WIN` line or EOF.
- Each `TAB` line holds one tab's URL, in tab order.
- A window with no `TAB` lines under it is not expected in practice (every window has
  at least one tab) but is handled by falling back to a single default tab (see Load
  behavior below).

This mirrors the existing `history.txt` convention rather than introducing an INI-based
format: `history.txt` is a flat list with no natural key/value structure, and the only
existing INI usage in this codebase (`embedwnd.cpp`'s single-window position
persistence via `WritePrivateProfileString`/`GetPrivateProfileInt` and the shared
`ini_file` global) is currently non-functional (`ini_file` is hardcoded to `nullptr`,
so it silently reads/writes `win.ini` instead of a plugin-owned file). Session
persistence does not build on or fix that mechanism — it's a separate, self-contained
save/load path.

## Save behavior

New `SaveSession()`, called from `quit()` alongside the existing `SaveHistory()` call.

For each `BrowserWindow* win` in `g_windows`:
1. Skip it unless `win->embedFrame && IsWindowVisible(win->embedFrame)`.
2. `GetWindowRect(win->embedFrame, &r)` for position/size.
3. For each `Tab` in `win->tabs`, resolve its URL using the same fallback chain
   `CloseTab` already uses when recording a closed tab: prefer `lastUri`; if empty and
   `web` exists, call `get_Source`; if still empty, fall back to `pendingUri`. Skip a
   tab entirely if no URL can be resolved (e.g. a brand new tab that hasn't started
   navigating and has no pending URL).
4. If the window has at least one resolvable tab, write its `WIN` line and `TAB` lines.

Write the whole file in one pass, overwriting any existing `session.txt` (same pattern
as `SaveHistory`).

## Load behavior

New `LoadSession()`, called from `init()` in place of the current unconditional
`CreateBrowserWindow(false)`.

1. Read `exodium_data/session.txt`. Parse tolerantly, matching `LoadHistory`'s style:
   a line that doesn't start with a recognized tag, or a `WIN` line with non-numeric
   fields, is skipped rather than aborting the whole parse. This means a partially
   corrupt file degrades to "restore what parsed" instead of failing entirely.
2. If parsing yields zero window records (file missing, empty, or nothing parsed),
   fall back to today's behavior exactly: `CreateBrowserWindow(false)` (one hidden
   default window with one blank tab).
3. Otherwise, for each parsed window record, in file order:
   - Call `CreateBrowserWindow(true)`. This already creates the frame, one default tab,
     registers the menu entry, and shows the window.
   - `SetWindowPos` the new `win->embedFrame` to the saved `x, y, w, h` (no
     `SWP_NOMOVE`/`SWP_NOSIZE`), overriding whatever position `CreateEmbeddedWindow`'s
     own (currently-broken) persistence or default cascade logic picked.
   - Replace `win->tabs` with one `Tab{}` per saved URL: `id = g_nextTabId++`,
     `pendingUri = <url>`, `title = L"(tab)"` (will be updated once the page loads, same
     as any normal navigation).
   - Set `win->active` to the saved `activeIdx`, clamped to a valid range in case the
     file is inconsistent (e.g. `activeIdx >= tabs.size()` clamps to `tabs.size() - 1`).
   - Call `LayoutToolbarAndTabs(win)` to rebuild the tab bar buttons for the restored
     tab count.
   - `PostMessage(win->client, WM_APP_INIT_WEBVIEW, 0, 0)` — per existing behavior, this
     only creates a WebView2 controller for `win->active`. Restored non-active tabs stay
     controller-less (just a title/URL) until clicked, exactly like a tab opened via
     `NewTab` behaves before its `WM_APP_INIT_WEBVIEW` message is processed.

No change to `config()` — it continues to just show/init the first window on demand
when there are zero windows.

## Required fix: lazy tab activation gap

`SwitchToTab` currently does nothing to create a WebView2 controller for the tab being
switched to — it only calls `MoveFocus` if `win->tabs[win->active].ctl` already exists.
This has never mattered before because the only way a tab is added is via `NewTab`,
which immediately follows up with `WM_APP_INIT_WEBVIEW` for the tab it just made
active. Restored non-active tabs break that assumption: they can be switched to (via
tab-bar click, Ctrl+Tab, etc.) while still controller-less.

Fix: in `SwitchToTab`, after setting `win->active = idx`, if
`!win->tabs[idx].ctl`, call `CreateWebViewForTab(win, win->tabs[idx].id)` (the same
function `WM_APP_INIT_WEBVIEW` already calls). This makes clicking into any
lazy/controller-less tab — restored or otherwise — create its controller on demand.

## Error handling

- Missing/unreadable `session.txt`: treated as "no session", falls back to the current
  single-hidden-window startup.
- Malformed individual lines: skipped, per the tolerant-parse behavior above.
- A saved window whose saved position is now off-screen (e.g. monitor unplugged): not
  specially handled — `SetWindowPos` will place it at the saved (now possibly invalid)
  coordinates, matching how most Windows apps behave in this situation. Out of scope
  for this change.

## Testing

Manual, since this is Win32/WebView2 UI code with no existing automated test harness
in this file:

1. Open Winamp, open Exodium, create a second window (Ctrl+N), open 2-3 tabs in each
   with distinct URLs, make one tab active in each window that isn't the first tab,
   move/resize one window.
2. Close one window via its X button.
3. Quit Winamp.
4. Relaunch Winamp and confirm: the two remaining windows reappear at their saved
   positions/sizes with their saved tabs in order and the correct tab active; the
   window closed via X does not reappear.
5. Confirm inactive restored tabs did not eagerly create a WebView2 controller (e.g.
   temporarily add a debug print in `CreateWebViewForTab`, or watch WebView2 subprocess
   count in Task Manager before vs. after clicking an inactive restored tab).
6. Click an inactive restored tab and confirm it now loads (verifies the `SwitchToTab`
   fix).
7. Delete `exodium_data/session.txt` and relaunch; confirm fallback to today's
   single-hidden-window behavior still works.
