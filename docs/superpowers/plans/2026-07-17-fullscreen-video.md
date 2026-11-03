# Fullscreen Video Support Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make HTML5 fullscreen (e.g. a video's fullscreen button) actually work in the Exodium Winamp plugin by resizing the plugin's embed window to cover the monitor and hiding the tab bar/toolbar while a page is fullscreen.

**Architecture:** Subscribe to WebView2's `ContainsFullScreenElementChanged` event per-tab in `WireTabEvents`. When it fires true/false for the currently-active tab, call new `EnterVideoFullscreen`/`ExitVideoFullscreen` functions that resize `BrowserWindow::embedFrame` to/from the monitor rect and toggle visibility of the toolbar controls. Two existing layout code paths (`LayoutActiveWebView` and `FrameProc`'s `WM_SIZE` handler) get an `isFullscreen` branch so the WebView2 bounds and client-window offset fill the whole frame instead of leaving room for the toolbar/Winamp chrome.

**Tech Stack:** C++17, Win32 API, WebView2 SDK (`ICoreWebView2`, `ICoreWebView2ContainsFullScreenElementChangedEventHandler`), single source file `exodium/gen_browser.cpp`.

## Global Constraints

- Real build target is Win32 (not x64) — see `docs`/memory `build-setup`. Build with:
  `& "C:\Program Files\Microsoft Visual Studio\18\Insiders\MSBuild\Current\Bin\MSBuild.exe" exodium\exodium.vcxproj /p:Configuration=Release /p:Platform=Win32`
  Output: `exodium\Release\gen_browser.dll`.
- This is a native GUI plugin with no unit test framework. "Tests" in this plan mean: the project compiles cleanly, and a final manual verification pass in a running Winamp confirms the behavior described in the spec's Testing section.
- Only react to fullscreen requests from the currently active tab (`i == win->active`); background-tab requests are ignored, no auto tab-switch.
- No manual Esc handling — Chromium exits fullscreen itself and reports it back through the same event.

---

### Task 1: Add fullscreen state fields and forward declarations

**Files:**
- Modify: `exodium/gen_browser.cpp:85-110` (`BrowserWindow` struct)
- Modify: `exodium/gen_browser.cpp:187-202` (forward declaration block)

**Interfaces:**
- Produces: `BrowserWindow::isFullscreen` (`bool`), `BrowserWindow::savedFrameRect` (`RECT`) — consumed by every later task.
- Produces: forward declarations for `static void EnterVideoFullscreen(BrowserWindow* win);` and `static void ExitVideoFullscreen(BrowserWindow* win);` — consumed by Task 4 (event wiring), which is defined earlier in the file than Task 2's implementations.

- [ ] **Step 1: Add the two fields to `BrowserWindow`**

In `exodium/gen_browser.cpp`, the `BrowserWindow` struct currently ends like this (around line 107-110):

```cpp
    embedWindowState ews = { 0 };
    UINT_PTR menuId = (UINT_PTR)-1;
    GUID guid = {};
};
```

Change it to:

```cpp
    embedWindowState ews = { 0 };
    UINT_PTR menuId = (UINT_PTR)-1;
    GUID guid = {};

    bool isFullscreen = false;
    RECT savedFrameRect{};
};
```

- [ ] **Step 2: Add forward declarations**

The forward declaration block (around line 187-202) currently reads:

```cpp
// ---------- helpers ----------
static void LayoutToolbarAndTabs(BrowserWindow* win);
static void SwitchToTab(BrowserWindow* win, int idx);
static void NavigateFromUrlBox(BrowserWindow* win);
static void NextTab(BrowserWindow* win);
static void PrevTab(BrowserWindow* win);
static void CloseActiveTab(BrowserWindow* win);
static void SubclassCtrlCommon(HWND h);
static void ApplySuggestSelectionToEdit(BrowserWindow* win);
static LRESULT CALLBACK SuggestSubproc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR);
static LRESULT CALLBACK FrameProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
static LRESULT CALLBACK ClientProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
static LRESULT CALLBACK WaSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
static void NewTab(BrowserWindow* win, const wchar_t* url = L"https://google.com/");
static void CreateWebViewForTab(BrowserWindow* win, int tabId);
static BrowserWindow* CreateBrowserWindow(bool show);
```

Add two new lines to it:

```cpp
// ---------- helpers ----------
static void LayoutToolbarAndTabs(BrowserWindow* win);
static void SwitchToTab(BrowserWindow* win, int idx);
static void NavigateFromUrlBox(BrowserWindow* win);
static void NextTab(BrowserWindow* win);
static void PrevTab(BrowserWindow* win);
static void CloseActiveTab(BrowserWindow* win);
static void SubclassCtrlCommon(HWND h);
static void ApplySuggestSelectionToEdit(BrowserWindow* win);
static LRESULT CALLBACK SuggestSubproc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR);
static LRESULT CALLBACK FrameProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
static LRESULT CALLBACK ClientProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
static LRESULT CALLBACK WaSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
static void NewTab(BrowserWindow* win, const wchar_t* url = L"https://google.com/");
static void CreateWebViewForTab(BrowserWindow* win, int tabId);
static BrowserWindow* CreateBrowserWindow(bool show);
static void EnterVideoFullscreen(BrowserWindow* win);
static void ExitVideoFullscreen(BrowserWindow* win);
```

- [ ] **Step 3: Verify it compiles**

Run:
```
& "C:\Program Files\Microsoft Visual Studio\18\Insiders\MSBuild\Current\Bin\MSBuild.exe" exodium\exodium.vcxproj /p:Configuration=Release /p:Platform=Win32
```
Expected: `Build succeeded.` (adding unused struct fields and forward decls cannot break compilation).

- [ ] **Step 4: Commit**

```bash
git add exodium/gen_browser.cpp
git commit -m "Add fullscreen state fields and forward decls to gen_browser"
```

---

### Task 2: Implement `LayoutActiveWebView` fullscreen branch

**Files:**
- Modify: `exodium/gen_browser.cpp:593-600` (`LayoutActiveWebView`)

**Interfaces:**
- Consumes: `BrowserWindow::isFullscreen` (from Task 1).
- Produces: `LayoutActiveWebView` now fills the entire client rect when `isFullscreen` is true — consumed visually by Task 3's `EnterVideoFullscreen`/`ExitVideoFullscreen` (they call the existing `LayoutToolbarAndTabs`/this function to re-layout).

- [ ] **Step 1: Change `LayoutActiveWebView`**

Current code:

```cpp
static void LayoutActiveWebView(BrowserWindow* win) {
    if (win->active < 0 || win->active >= (int)win->tabs.size()) return;
    if (!win->tabs[win->active].ctl) return;
    RECT rc; GetClientRect(win->client, &rc);
    int top = kTabBarH + kToolbarH; RECT b = { kMargin,top,rc.right - kMargin,rc.bottom - kMargin };
    win->tabs[win->active].ctl->put_Bounds(b); win->tabs[win->active].ctl->put_IsVisible(TRUE);
}
```

Change to:

```cpp
static void LayoutActiveWebView(BrowserWindow* win) {
    if (win->active < 0 || win->active >= (int)win->tabs.size()) return;
    if (!win->tabs[win->active].ctl) return;
    RECT rc; GetClientRect(win->client, &rc);
    RECT b;
    if (win->isFullscreen) {
        b = { 0, 0, rc.right, rc.bottom };
    } else {
        int top = kTabBarH + kToolbarH;
        b = { kMargin, top, rc.right - kMargin, rc.bottom - kMargin };
    }
    win->tabs[win->active].ctl->put_Bounds(b); win->tabs[win->active].ctl->put_IsVisible(TRUE);
}
```

- [ ] **Step 2: Verify it compiles**

Run:
```
& "C:\Program Files\Microsoft Visual Studio\18\Insiders\MSBuild\Current\Bin\MSBuild.exe" exodium\exodium.vcxproj /p:Configuration=Release /p:Platform=Win32
```
Expected: `Build succeeded.`

- [ ] **Step 3: Commit**

```bash
git add exodium/gen_browser.cpp
git commit -m "Branch LayoutActiveWebView on isFullscreen to fill the client rect"
```

---

### Task 3: Implement `EnterVideoFullscreen` / `ExitVideoFullscreen` and the `FrameProc` `WM_SIZE` branch

**Files:**
- Modify: `exodium/gen_browser.cpp` — add the two new functions just above `WireTabEvents` (currently at line 754), i.e. right after `LayoutToolbarAndTabs` / tab-op helpers and before `WireTabEvents`.
- Modify: `exodium/gen_browser.cpp:1115` (`FrameProc`'s `WM_SIZE` case)

**Interfaces:**
- Consumes: `BrowserWindow::isFullscreen`, `BrowserWindow::savedFrameRect` (Task 1); `LayoutActiveWebView` (Task 2); `LayoutToolbarAndTabs`, `HideSuggestions` (existing).
- Produces: `EnterVideoFullscreen(BrowserWindow*)`, `ExitVideoFullscreen(BrowserWindow*)` — consumed by Task 4's event handler.

- [ ] **Step 1: Add the two functions**

Insert this immediately before `static void WireTabEvents(BrowserWindow* win, int tabId) {` (currently line 754):

```cpp
static void SetToolbarControlsVisible(BrowserWindow* win, bool visible) {
    int show = visible ? SW_SHOWNA : SW_HIDE;
    if (win->tabBar) ShowWindow(win->tabBar, show);
    if (win->newTabBtn) ShowWindow(win->newTabBtn, show);
    for (auto& t : win->tabs) if (t.btn) ShowWindow(t.btn, show);
    if (win->btnBack) ShowWindow(win->btnBack, show);
    if (win->btnFwd) ShowWindow(win->btnFwd, show);
    if (win->btnReload) ShowWindow(win->btnReload, show);
    if (win->btnCloseTab) ShowWindow(win->btnCloseTab, show);
    if (win->editUrl) ShowWindow(win->editUrl, show);
    if (!visible) HideSuggestions(win);
}

static void EnterVideoFullscreen(BrowserWindow* win) {
    if (win->isFullscreen || !win->embedFrame) return;

    GetWindowRect(win->embedFrame, &win->savedFrameRect);

    HMONITOR mon = MonitorFromWindow(win->embedFrame, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = { sizeof(MONITORINFO) };
    if (!GetMonitorInfo(mon, &mi)) return;

    win->isFullscreen = true;
    SetToolbarControlsVisible(win, false);

    SetWindowPos(win->embedFrame, HWND_TOPMOST,
        mi.rcMonitor.left, mi.rcMonitor.top,
        mi.rcMonitor.right - mi.rcMonitor.left,
        mi.rcMonitor.bottom - mi.rcMonitor.top,
        SWP_SHOWWINDOW);
}

static void ExitVideoFullscreen(BrowserWindow* win) {
    if (!win->isFullscreen || !win->embedFrame) return;

    win->isFullscreen = false;

    SetWindowPos(win->embedFrame, HWND_NOTOPMOST,
        win->savedFrameRect.left, win->savedFrameRect.top,
        win->savedFrameRect.right - win->savedFrameRect.left,
        win->savedFrameRect.bottom - win->savedFrameRect.top,
        SWP_SHOWWINDOW);

    SetToolbarControlsVisible(win, true);
    LayoutToolbarAndTabs(win);
}
```

`SetToolbarControlsVisible` is a small helper used by both functions so the show/hide control list is written once. It is file-local (not in the design doc's function list, but required to avoid duplicating the control list — matches the codebase's existing style of small static helpers like `HideAllWebViews`).

- [ ] **Step 2: Change the `FrameProc` `WM_SIZE` handler**

Current code (line 1115):

```cpp
    case WM_SIZE: if (self && self->client) { RECT fr; GetClientRect(hwnd, &fr); SetWindowPos(self->client, 0, 8, 24, fr.right - 8, fr.bottom - 24, SWP_NOZORDER | SWP_NOACTIVATE); } break;
```

Change to:

```cpp
    case WM_SIZE: if (self && self->client) {
        RECT fr; GetClientRect(hwnd, &fr);
        if (self->isFullscreen) {
            SetWindowPos(self->client, 0, 0, 0, fr.right, fr.bottom, SWP_NOZORDER | SWP_NOACTIVATE);
        } else {
            SetWindowPos(self->client, 0, 8, 24, fr.right - 8, fr.bottom - 24, SWP_NOZORDER | SWP_NOACTIVATE);
        }
    } break;
```

- [ ] **Step 3: Verify it compiles**

Run:
```
& "C:\Program Files\Microsoft Visual Studio\18\Insiders\MSBuild\Current\Bin\MSBuild.exe" exodium\exodium.vcxproj /p:Configuration=Release /p:Platform=Win32
```
Expected: `Build succeeded.`

- [ ] **Step 4: Commit**

```bash
git add exodium/gen_browser.cpp
git commit -m "Add EnterVideoFullscreen/ExitVideoFullscreen and fullscreen WM_SIZE branch"
```

---

### Task 4: Wire the `ContainsFullScreenElementChanged` event

**Files:**
- Modify: `exodium/gen_browser.cpp:916` (end of `WireTabEvents`, just before its closing `}`)

**Interfaces:**
- Consumes: `EnterVideoFullscreen`, `ExitVideoFullscreen` (Task 3); `WinAlive`, `TabIndexById` (existing).
- Produces: nothing further consumed — this is the last task.

- [ ] **Step 1: Add the event handler inside `WireTabEvents`**

`WireTabEvents` currently ends like this (lines 873-917):

```cpp
    // --- Favicon support: ICoreWebView2_15 ---
    ComPtr<ICoreWebView2_15> web15;
    if (SUCCEEDED(web.As(&web15))) {
        web15->add_FaviconChanged(
            Microsoft::WRL::Callback<ICoreWebView2FaviconChangedEventHandler>(
                [win, tabId](ICoreWebView2* sender, IUnknown*) -> HRESULT {
                    if (!sender || !WinAlive(win)) return S_OK;

                    Microsoft::WRL::ComPtr<ICoreWebView2_15> wv15;
                    if (FAILED(sender->QueryInterface(IID_PPV_ARGS(&wv15)))) return S_OK;

                    return wv15->GetFavicon(
                        COREWEBVIEW2_FAVICON_IMAGE_FORMAT_PNG,
                        Microsoft::WRL::Callback<ICoreWebView2GetFaviconCompletedHandler>(
                            [win, tabId](HRESULT hr, IStream* iconStream) -> HRESULT {
                                if (FAILED(hr)) return S_OK;

                                HICON newIcon = nullptr;

                                if (iconStream) {
                                    Gdiplus::Bitmap bmp(iconStream);
                                    if (bmp.GetLastStatus() == Gdiplus::Ok) {
                                        HICON hc = nullptr;
                                        if (bmp.GetHICON(&hc) == Gdiplus::Status::Ok) {
                                            newIcon = hc;
                                        }
                                    }
                                }

                                int i = WinAlive(win) ? TabIndexById(win, tabId) : -1;
                                if (i >= 0) {
                                    Tab& t = win->tabs[i];
                                    if (t.favicon) { DestroyIcon(t.favicon); t.favicon = nullptr; }
                                    t.favicon = newIcon;
                                    if (t.btn) InvalidateRect(t.btn, nullptr, TRUE);
                                }
                                else if (newIcon) {
                                    DestroyIcon(newIcon);
                                }
                                return S_OK;
                            }).Get());
                }).Get(),
                    nullptr);
    }
}
```

Change the final `}` (the one closing `WireTabEvents`, right after the favicon `if` block) so the function ends with the new handler added right before it:

```cpp
    // --- Favicon support: ICoreWebView2_15 ---
    ComPtr<ICoreWebView2_15> web15;
    if (SUCCEEDED(web.As(&web15))) {
        web15->add_FaviconChanged(
            Microsoft::WRL::Callback<ICoreWebView2FaviconChangedEventHandler>(
                [win, tabId](ICoreWebView2* sender, IUnknown*) -> HRESULT {
                    if (!sender || !WinAlive(win)) return S_OK;

                    Microsoft::WRL::ComPtr<ICoreWebView2_15> wv15;
                    if (FAILED(sender->QueryInterface(IID_PPV_ARGS(&wv15)))) return S_OK;

                    return wv15->GetFavicon(
                        COREWEBVIEW2_FAVICON_IMAGE_FORMAT_PNG,
                        Microsoft::WRL::Callback<ICoreWebView2GetFaviconCompletedHandler>(
                            [win, tabId](HRESULT hr, IStream* iconStream) -> HRESULT {
                                if (FAILED(hr)) return S_OK;

                                HICON newIcon = nullptr;

                                if (iconStream) {
                                    Gdiplus::Bitmap bmp(iconStream);
                                    if (bmp.GetLastStatus() == Gdiplus::Ok) {
                                        HICON hc = nullptr;
                                        if (bmp.GetHICON(&hc) == Gdiplus::Status::Ok) {
                                            newIcon = hc;
                                        }
                                    }
                                }

                                int i = WinAlive(win) ? TabIndexById(win, tabId) : -1;
                                if (i >= 0) {
                                    Tab& t = win->tabs[i];
                                    if (t.favicon) { DestroyIcon(t.favicon); t.favicon = nullptr; }
                                    t.favicon = newIcon;
                                    if (t.btn) InvalidateRect(t.btn, nullptr, TRUE);
                                }
                                else if (newIcon) {
                                    DestroyIcon(newIcon);
                                }
                                return S_OK;
                            }).Get());
                }).Get(),
                    nullptr);
    }

    // --- Fullscreen video support ---
    web->add_ContainsFullScreenElementChanged(
        Microsoft::WRL::Callback<ICoreWebView2ContainsFullScreenElementChangedEventHandler>(
            [win, tabId](ICoreWebView2* sender, IUnknown*) -> HRESULT {
                if (!sender || !WinAlive(win)) return S_OK;
                int i = TabIndexById(win, tabId);
                if (i < 0 || i != win->active) return S_OK;

                BOOL fs = FALSE;
                sender->get_ContainsFullScreenElement(&fs);
                if (fs) EnterVideoFullscreen(win);
                else ExitVideoFullscreen(win);
                return S_OK;
            }).Get(), nullptr);
}
```

- [ ] **Step 2: Verify it compiles**

Run:
```
& "C:\Program Files\Microsoft Visual Studio\18\Insiders\MSBuild\Current\Bin\MSBuild.exe" exodium\exodium.vcxproj /p:Configuration=Release /p:Platform=Win32
```
Expected: `Build succeeded.`

- [ ] **Step 3: Commit**

```bash
git add exodium/gen_browser.cpp
git commit -m "Wire ContainsFullScreenElementChanged to enter/exit video fullscreen"
```

---

### Task 5: Manual verification and deploy

**Files:** none (verification + deployment only)

**Interfaces:** none — terminal task.

- [ ] **Step 1: Build Release Win32**

```
& "C:\Program Files\Microsoft Visual Studio\18\Insiders\MSBuild\Current\Bin\MSBuild.exe" exodium\exodium.vcxproj /p:Configuration=Release /p:Platform=Win32
```
Expected: `Build succeeded.`, producing `exodium\Release\gen_browser.dll`.

- [ ] **Step 2: Locate the deployed plugin and back it up**

Find the existing `gen_browser.dll` under the user's Winamp installation (Documents folder per user statement) and copy it aside (e.g. `gen_browser.dll.bak`) before overwriting, in case a rollback is needed.

- [ ] **Step 3: Copy the freshly built DLL over the deployed one**

Copy `exodium\Release\gen_browser.dll` to the Winamp plugins directory found in Step 2, overwriting the existing file. Confirm Winamp is closed first (locked DLLs can't be overwritten while Winamp is running).

- [ ] **Step 4: Manual functional verification**

Launch Winamp, open Exodium, navigate to a page with a fullscreen-capable video (e.g. YouTube), and confirm:
- Clicking the video's fullscreen control expands the plugin window to cover the whole monitor, tab bar/toolbar hidden, video filling the window.
- Pressing Esc restores the window to its original position/size with the toolbar visible and functional again.
- With a second window open (Ctrl+N), starting fullscreen in one window does not affect the other.

- [ ] **Step 5: Commit any final fixups discovered during manual verification**

If Step 4 surfaces a bug, fix it in `exodium/gen_browser.cpp`, rebuild, redeploy (repeat Steps 1-4), then:

```bash
git add exodium/gen_browser.cpp
git commit -m "Fix issue found during fullscreen video manual verification"
```
