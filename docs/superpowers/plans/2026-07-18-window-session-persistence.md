# Window/Tab Session Persistence Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Persist each visible browser window's position/size and tabs (URLs + active tab) to disk on Winamp shutdown, and restore them on the next Winamp startup.

**Architecture:** Two new functions in `exodium/gen_browser.cpp` — `SaveSession()` (called from `quit()`) and `LoadSession()` (called from `init()`, replacing the current unconditional default-window creation) — read/write a new plain-text file `exodium_data/session.txt`, following the exact style of the existing `LoadHistory()`/`SaveHistory()` pair in the same file. A small fix to `SwitchToTab` makes clicking into a tab that has no WebView2 controller yet (true for restored non-active tabs) create one on demand.

**Tech Stack:** C++17, Win32 API, WebView2 COM API (existing plugin stack — no new dependencies).

## Global Constraints

- Session file: `exodium_data/session.txt` (same directory as `exodium_data/history.txt`, via the existing `DataDirPath()` helper).
- Format: `WIN <x> <y> <w> <h> <activeIdx>` line per window, followed by `TAB <url>` lines (one per tab, in order) until the next `WIN` line or EOF.
- Only windows where `IsWindowVisible(win->embedFrame)` is true at quit time are saved. Hidden windows (closed via the X button) are excluded.
- Malformed lines are skipped during parsing (tolerant parse, matching `LoadHistory`'s style) — never abort the whole load on one bad line.
- If the session file is missing, empty, or yields zero valid window records, fall back exactly to today's behavior: one hidden default window (`CreateBrowserWindow(false)`).
- Restored windows show immediately (`CreateBrowserWindow(true)`); only the active tab of each restored window gets a WebView2 controller at restore time — other tabs stay controller-less (just `pendingUri` + title) until clicked, per the existing `WM_APP_INIT_WEBVIEW` behavior.
- This plugin has no automated test suite (Win32 GUI + WebView2 COM code, no headless test harness in the repo). Verification in this plan is build success (0 compiler errors/warnings introduced) plus scripted manual checks against the running plugin in Winamp, matching the spec's own Testing section.
- Build command: `& "C:\Program Files\Microsoft Visual Studio\18\Insiders\MSBuild\Current\Bin\MSBuild.exe" exodium\exodium.vcxproj /p:Configuration=Release /p:Platform=Win32`. Output: `exodium\Release\gen_browser.dll`.

---

### Task 1: Save session on quit

**Files:**
- Modify: `exodium/gen_browser.cpp` (add near `SaveHistory` at line 184; add forward declaration near line 192; call from `quit()` at line 1412)

**Interfaces:**
- Consumes: `BrowserWindow` (line 85), `Tab` (line 74), `g_windows` (line 116), `DataDirPath()` (line 145), `TrimCRLF` (line 158, unused here but shows file style)
- Produces: `SessionFilePath() -> std::wstring`, `SaveSession() -> void` — both used by Task 2.

- [ ] **Step 1: Add `SessionFilePath()` next to `HistoryFilePath()`**

In `exodium/gen_browser.cpp`, right after line 156 (`static std::wstring HistoryFilePath() { auto p = DataDirPath(); p += L"\\history.txt"; return p; }`), add:

```cpp
static std::wstring SessionFilePath() { auto p = DataDirPath(); p += L"\\session.txt"; return p; }
```

- [ ] **Step 2: Add `ResolveTabUrl` helper**

Directly below the `SessionFilePath` line added in Step 1, add:

```cpp
static std::wstring ResolveTabUrl(Tab& t) {
    if (!t.lastUri.empty()) return t.lastUri;
    if (t.web) {
        LPWSTR src = nullptr;
        if (SUCCEEDED(t.web->get_Source(&src)) && src) {
            std::wstring url = src;
            CoTaskMemFree(src);
            return url;
        }
    }
    return t.pendingUri;
}
```

This mirrors the fallback chain `CloseTab` already uses (line 761-771: prefer `lastUri`, else live `get_Source`), with `pendingUri` added as a final fallback for a tab that was never navigated.

- [ ] **Step 3: Add `SaveSession()`**

Directly below `ResolveTabUrl`, add:

```cpp
static void SaveSession() {
    std::wstring path = SessionFilePath();
    FILE* f = nullptr; if (_wfopen_s(&f, path.c_str(), L"wt, ccs=UNICODE") != 0 || !f) return;
    for (auto& w : g_windows) {
        if (!w->embedFrame || !IsWindowVisible(w->embedFrame)) continue;

        std::vector<std::wstring> urls;
        int activeIdx = -1;
        for (int i = 0; i < (int)w->tabs.size(); ++i) {
            std::wstring u = ResolveTabUrl(w->tabs[i]);
            if (u.empty()) continue;
            if (i == w->active) activeIdx = (int)urls.size();
            urls.push_back(u);
        }
        if (urls.empty()) continue;
        if (activeIdx < 0) activeIdx = 0;

        RECT r; GetWindowRect(w->embedFrame, &r);
        fwprintf(f, L"WIN %d %d %d %d %d\n", r.left, r.top, r.right - r.left, r.bottom - r.top, activeIdx);
        for (auto& u : urls) { fputws(L"TAB ", f); fputws(u.c_str(), f); fputwc(L'\n', f); }
    }
    fclose(f);
}
```

- [ ] **Step 4: Call `SaveSession()` from `quit()`**

No forward declaration is needed: `SaveSession` is defined (Step 3) well before its only call site in `quit()` (line ~1412), the same way `SaveHistory` already works with no forward declaration.

In `quit()` (around line 1410-1412), change:

```cpp
static void __cdecl quit()
{
    SaveHistory();
```

to:

```cpp
static void __cdecl quit()
{
    SaveHistory();
    SaveSession();
```

- [ ] **Step 5: Build**

Run:
```
& "C:\Program Files\Microsoft Visual Studio\18\Insiders\MSBuild\Current\Bin\MSBuild.exe" exodium\exodium.vcxproj /p:Configuration=Release /p:Platform=Win32
```
Expected: `0 Error(s)` in the build summary.

- [ ] **Step 6: Manual verification**

1. Copy `exodium\Release\gen_browser.dll` into Winamp's `Plugins` folder (same location as before this change — if unsure of the path, check where the previously-built DLL was already deployed).
2. Launch Winamp, open Exodium (from the menu), open a second window with Ctrl+N, open 2 tabs in each window with distinct real URLs (e.g. `https://example.com/` and `https://www.wikipedia.org/`), click the second tab in each window so it's active, move one window to a different screen position.
3. Quit Winamp.
4. Open `exodium_data\session.txt` next to `gen_browser.dll` in a text editor. Confirm it contains two `WIN` lines with plausible x/y/w/h and an `activeIdx` of `1` (second tab), each followed by 2 `TAB` lines with the URLs you opened, in order.

- [ ] **Step 7: Commit**

```bash
git add exodium/gen_browser.cpp
git commit -m "Save visible window positions and tabs to session.txt on quit"
```

---

### Task 2: Restore session on init

**Files:**
- Modify: `exodium/gen_browser.cpp` (add near `LoadHistory` at line 170; call from `init()` at line 1396, replacing the current `CreateBrowserWindow(false)` call)

**Interfaces:**
- Consumes: `SessionFilePath()` and `TrimCRLF()` (from Task 1 / existing code), `CreateBrowserWindow(bool show) -> BrowserWindow*` (line 206 forward decl, defined line 1330), `LayoutToolbarAndTabs(BrowserWindow*)` (line 192 forward decl), `WM_APP_INIT_WEBVIEW` (line 53), `Tab` fields `id`, `pendingUri`, `title` (line 74-83), `g_nextTabId` (global tab id counter), `BrowserWindow::tabs`, `::active`, `::client`, `::embedFrame` (line 85-114).
- Produces: `LoadSession() -> void`, called once from `init()`.

- [ ] **Step 1: Add a session-record struct and parser**

In `exodium/gen_browser.cpp`, right after the `LoadHistory()` function (ends around line 183, just before `static void SaveHistory()` at line 184), add:

```cpp
struct SessionWindowRecord {
    RECT rect = { 0, 0, 0, 0 };
    int activeIdx = 0;
    std::vector<std::wstring> urls;
};

static std::vector<SessionWindowRecord> LoadSessionRecords() {
    std::vector<SessionWindowRecord> records;
    std::wstring path = SessionFilePath();
    FILE* f = nullptr; if (_wfopen_s(&f, path.c_str(), L"rt, ccs=UNICODE") != 0 || !f) return records;
    wchar_t buf[4096];
    while (fgetws(buf, ARRAYSIZE(buf), f)) {
        std::wstring line = buf; TrimCRLF(line);
        if (line.size() > 4 && wcsncmp(line.c_str(), L"WIN ", 4) == 0) {
            int x, y, w, h, activeIdx;
            if (swscanf_s(line.c_str() + 4, L"%d %d %d %d %d", &x, &y, &w, &h, &activeIdx) == 5) {
                SessionWindowRecord rec;
                rec.rect = { x, y, x + w, y + h };
                rec.activeIdx = activeIdx;
                records.push_back(rec);
            }
        } else if (line.size() > 4 && wcsncmp(line.c_str(), L"TAB ", 4) == 0) {
            if (!records.empty()) records.back().urls.push_back(line.substr(4));
        }
    }
    fclose(f);
    for (auto& rec : records) {
        if (rec.urls.empty()) continue;
        if (rec.activeIdx < 0 || rec.activeIdx >= (int)rec.urls.size()) rec.activeIdx = 0;
    }
    return records;
}
```

Note: `SessionFilePath()` must exist before this point in the file — Task 1 places it at line ~157, which is above `LoadHistory` (line 170), so it's already in scope here.

- [ ] **Step 2: Add `LoadSession()`**

Directly below `LoadSessionRecords`, add:

```cpp
static void LoadSession() {
    auto records = LoadSessionRecords();
    bool anyRestored = false;
    for (auto& rec : records) {
        if (rec.urls.empty()) continue;

        BrowserWindow* win = CreateBrowserWindow(true);
        if (!win) continue;
        anyRestored = true;

        SetWindowPos(win->embedFrame, nullptr, rec.rect.left, rec.rect.top,
            rec.rect.right - rec.rect.left, rec.rect.bottom - rec.rect.top,
            SWP_NOZORDER | SWP_NOACTIVATE);

        win->tabs.clear();
        for (auto& url : rec.urls) {
            Tab t;
            t.id = g_nextTabId++;
            t.pendingUri = url;
            t.title = L"(tab)";
            win->tabs.push_back(std::move(t));
        }
        win->active = rec.activeIdx;
        LayoutToolbarAndTabs(win);
        PostMessage(win->client, WM_APP_INIT_WEBVIEW, 0, 0);
    }
    if (!anyRestored) CreateBrowserWindow(false);
}
```

`CreateBrowserWindow(true)` already creates one placeholder tab and shows the window; this replaces that placeholder tab list with the restored tabs before the queued `WM_APP_INIT_WEBVIEW` message is processed, so only the correct restored active tab gets a WebView2 controller (see Task 3 for the companion fix needed when a *non*-active restored tab is later clicked).

- [ ] **Step 3: Call `LoadSession()` from `init()`**

In `init()` (around line 1387-1399), change:

```cpp
    EnsureGlobals();
    HookWinampParent();
    CreateBrowserWindow(false);  // first window, hidden until shown via menu/config

    return 0;
```

to:

```cpp
    EnsureGlobals();
    HookWinampParent();
    LoadSession();  // recreates saved visible windows/tabs, or one hidden default window if no session

    return 0;
```

- [ ] **Step 4: Build**

Run:
```
& "C:\Program Files\Microsoft Visual Studio\18\Insiders\MSBuild\Current\Bin\MSBuild.exe" exodium\exodium.vcxproj /p:Configuration=Release /p:Platform=Win32
```
Expected: `0 Error(s)`.

- [ ] **Step 5: Manual verification — restore path**

1. Deploy the built `gen_browser.dll`, reusing the `session.txt` created during Task 1's manual verification (two windows, 2 tabs each, second tab active in each, one window moved).
2. Launch Winamp.
3. Confirm both windows appear automatically (no need to open the menu), at the positions saved previously, each showing 2 tabs with the second tab active in each (check the URL box shows the second tab's URL, not the first's).

- [ ] **Step 6: Manual verification — fallback path**

1. Quit Winamp. Delete `exodium_data\session.txt`.
2. Launch Winamp. Confirm no Exodium window appears automatically (today's existing behavior: first window created hidden). Open Exodium from the Winamp menu and confirm it opens normally with one blank tab.

- [ ] **Step 7: Commit**

```bash
git add exodium/gen_browser.cpp
git commit -m "Restore saved windows and tabs on startup, falling back to default window"
```

---

### Task 3: Lazy-create WebView2 controller when switching into a controller-less tab

**Files:**
- Modify: `exodium/gen_browser.cpp:718-736` (`SwitchToTab`)

**Interfaces:**
- Consumes: `CreateWebViewForTab(BrowserWindow* win, int tabId)` (forward-declared line 205, defined line 1127 area) — already used elsewhere for the exact same purpose.
- Produces: no new interface; fixes a gap so any controller-less tab (restored non-active tabs from Task 2, or any other future lazily-added tab) gets a controller when the user switches to it.

- [ ] **Step 1: Locate the current tail of `SwitchToTab`**

Current code (lines ~732-736):

```cpp
    if (win->tabs[win->active].ctl) {
        win->tabs[win->active].ctl->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
    }
}
```

- [ ] **Step 2: Replace it to create the controller on demand**

Change that block to:

```cpp
    if (win->tabs[win->active].ctl) {
        win->tabs[win->active].ctl->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
    } else {
        CreateWebViewForTab(win, win->tabs[win->active].id);
    }
}
```

- [ ] **Step 3: Build**

Run:
```
& "C:\Program Files\Microsoft Visual Studio\18\Insiders\MSBuild\Current\Bin\MSBuild.exe" exodium\exodium.vcxproj /p:Configuration=Release /p:Platform=Win32
```
Expected: `0 Error(s)`.

- [ ] **Step 4: Manual verification**

1. Using the same restored two-window/2-tab-each session from Task 2, click the *first* tab (the inactive, controller-less one) in one of the windows.
2. Confirm it now loads its page (was previously just a title with no content until this fix). Confirm switching back and forth between the two tabs keeps both working normally, and that closing/reopening a tab via the existing `+`/`X` buttons still behaves as before (regression check — this code path is shared with normal tab creation flows once a controller already exists).

- [ ] **Step 5: Commit**

```bash
git add exodium/gen_browser.cpp
git commit -m "Create WebView2 controller on demand when switching into a controller-less tab"
```

---

### Task 4: End-to-end verification against the full spec test plan

**Files:** none (verification only).

**Interfaces:** none.

- [ ] **Step 1: Run the full scenario from the design spec**

1. Launch Winamp. Open Exodium, create a second window (Ctrl+N). Open 3 tabs in the first window and 2 in the second, using distinct real URLs. In each window, make active a tab that is *not* the first one. Move/resize one window to an easily-recognizable position/size.
2. Open a third window (Ctrl+N) and close it via its X button (leaving it hidden, not destroyed).
3. Quit Winamp.
4. Relaunch Winamp.

- [ ] **Step 2: Verify outcomes**

1. Exactly two windows reappear automatically (not three) — the one closed via X does not come back.
2. Each reappeared window is at its saved position/size, with its saved tabs in the saved order and the correct tab active.
3. Confirm inactive restored tabs did not eagerly create a WebView2 controller: before clicking any inactive tab, open Task Manager → Details tab, note the number of `msedgewebview2.exe` processes; it should correspond only to the two active tabs' controllers (plus the shared browser/GPU processes WebView2 always spawns), not one per restored tab.
4. Click an inactive restored tab in either window and confirm it now loads (Task 3's fix).

- [ ] **Step 3: No commit for this task** (verification only, no code changes).
