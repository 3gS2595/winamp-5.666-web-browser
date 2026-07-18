# Fullscreen video support — design

## Problem

`exodium/gen_browser.cpp` embeds a WebView2 browser inside a Winamp gen_ plugin
window (`BrowserWindow::embedFrame` / `client`). When a web page requests
fullscreen for a `<video>` element (or any element) via the HTML5 Fullscreen
API, WebView2/Chromium fires `ICoreWebView2::add_ContainsFullScreenElementChanged`
expecting the host application to resize the view to fill the screen. Nothing
in this codebase subscribes to that event, so clicking a video's fullscreen
button currently does nothing visible.

## Goal

Clicking fullscreen on a video (or any page content) should make the
Exodium plugin window take over the whole monitor — tab bar and toolbar
hidden, WebView2 filling the entire window — the way a normal browser's
fullscreen mode behaves. Exiting fullscreen (Esc, which Chromium handles
itself and reports back via the same event) restores the window to its
previous position, size, and UI.

## Scope decisions

- **True OS-level fullscreen**, not just "fill the existing panel": the
  `embedFrame` window itself is resized/repositioned to cover the target
  monitor and raised above the taskbar, rather than only resizing the
  WebView2 control within the existing window bounds.
- **Only the active tab's fullscreen request is honored.** If a background
  tab (not currently displayed) fires the event, it is ignored — matches
  normal browser behavior, since a hidden tab has no visible content to
  show fullscreen anyway. No auto-switching to that tab.
- **No manual Esc handling.** Chromium already exits fullscreen on Esc and
  fires `ContainsFullScreenElementChanged` with `false` on its own; we only
  need to react to the event in both directions.
- **Known accepted limitation:** if Winamp is closed while a video is
  fullscreen, `DestroyEmbeddedWindow` may persist the fullscreen window
  geometry into `winamp.ini` instead of the pre-fullscreen geometry. This is
  an edge case (quitting mid-fullscreen) and is not being solved in this
  pass.

## Design

### New per-window state

Add to `BrowserWindow`:

```cpp
bool isFullscreen = false;
RECT savedFrameRect{};   // embedFrame's window rect before entering fullscreen
```

### Event wiring

In `WireTabEvents(win, tabId)`, register on the tab's `ICoreWebView2`:

```cpp
web->add_ContainsFullScreenElementChanged(
    Callback<ICoreWebView2ContainsFullScreenElementChangedEventHandler>(
        [win, tabId](ICoreWebView2* sender, IUnknown*) -> HRESULT {
            if (!WinAlive(win)) return S_OK;
            int i = TabIndexById(win, tabId);
            if (i < 0 || i != win->active) return S_OK; // background tab: ignore

            BOOL fs = FALSE;
            sender->get_ContainsFullScreenElement(&fs);
            if (fs) EnterVideoFullscreen(win);
            else ExitVideoFullscreen(win);
            return S_OK;
        }).Get(), nullptr);
```

### `EnterVideoFullscreen(BrowserWindow* win)`

1. No-op if `win->isFullscreen` already true.
2. `GetWindowRect(win->embedFrame, &win->savedFrameRect)`.
3. Resolve the target monitor: `MonitorFromWindow(win->embedFrame,
   MONITOR_DEFAULTTONEAREST)` + `GetMonitorInfo` → use `rcMonitor` (the full
   monitor rect, not `rcWork`, so the window covers the taskbar too).
4. `SetWindowPos(win->embedFrame, HWND_TOPMOST, monitor.left, monitor.top,
   monitor.right - monitor.left, monitor.bottom - monitor.top,
   SWP_SHOWWINDOW)`.
5. Set `win->isFullscreen = true`.
6. Hide the toolbar/tab-bar controls: `ShowWindow(SW_HIDE)` on `tabBar`,
   `newTabBtn`, all tab buttons, `btnBack`, `btnFwd`, `btnReload`,
   `btnCloseTab`, `editUrl` (and hide suggestions if visible).
7. Re-layout the client area and active WebView2 bounds to the full client
   rect (see "Layout changes" below).

### `ExitVideoFullscreen(BrowserWindow* win)`

1. No-op if `!win->isFullscreen`.
2. `SetWindowPos(win->embedFrame, HWND_NOTOPMOST, savedFrameRect.left,
   savedFrameRect.top, width, height, SWP_SHOWWINDOW)` using the saved rect.
3. Set `win->isFullscreen = false`.
4. Re-show the toolbar/tab-bar controls and call `LayoutToolbarAndTabs(win)`
   to restore normal positions and WebView2 bounds.

### Layout changes

Two existing places hard-code the "normal" layout and need an
`isFullscreen` branch:

- **`LayoutActiveWebView`**: currently always insets bounds by `kMargin` and
  `kTabBarH + kToolbarH`. When `win->isFullscreen`, use the full client rect
  `{0, 0, rc.right, rc.bottom}` instead.
- **`FrameProc`'s `WM_SIZE` handler**: currently always positions `self->client`
  at a fixed `(8, 24)` offset within the frame (room for Winamp's
  skin-drawn chrome). When `win->isFullscreen`, position it at `(0, 0)`
  filling the whole frame instead, so no Winamp chrome shows through.

`LayoutToolbarAndTabs` is only called on the way back to normal layout (from
`ExitVideoFullscreen`), so it doesn't need its own fullscreen branch — it
already ends by calling `LayoutActiveWebView`, which does.

## Testing

- Build Win32 Release per the existing build docs.
- Load a page with a fullscreen-capable video (e.g. YouTube), click its
  fullscreen control, confirm the plugin window expands to cover the whole
  monitor with tab bar/toolbar hidden and the video filling the window.
- Press Esc, confirm the window snaps back to its original position/size
  with the toolbar restored and still functional.
- Open a second window (Ctrl+N), start a video fullscreen in one, confirm
  the other window is unaffected.
- Switch tabs while a background tab's page is in a fullscreen-eligible
  state (not necessarily fullscreen) to confirm no unexpected fullscreen
  triggers.
