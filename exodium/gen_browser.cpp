// STAGE 14: Multi-window support.
// - All per-window state moved from globals into a BrowserWindow struct.
// - Multiple embedded browser windows, each with its own tabs/toolbar.
// - Ctrl+N opens a new window. History and closed-tab stack stay shared.
// - Tab async events now key off a stable per-tab id (not a shifting index).

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#ifndef _WIN32_IE
#define _WIN32_IE    0x0600
#endif

#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <shlwapi.h>
#include <dwmapi.h>
#include <wrl.h>
#include <vector>
#include <string>
#include <memory>
#include <algorithm>
#include <cstdio>
#include <functional>
#include "WebView2.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "dwmapi.lib")

#include "gen.h"
#include "wa_ipc.h"
#include "embedwnd.h"
#include <gdiplus.h>
#pragma comment(lib, "gdiplus.lib")

using namespace Gdiplus;
using Microsoft::WRL::ComPtr;

// The embedwnd utility keeps the window position and size in this file. It is
// pointed at winamp.ini in init().
wchar_t* ini_file = nullptr;

// ---- IDs / layout ----
enum : int {
    IDC_BACK = 1001, IDC_FWD = 1002, IDC_URL = 1003, IDC_GO = 1004,
    IDC_RELOAD = 1005,
    IDC_CLOSE_TAB = 1006,
    IDC_TABBAR = 1100, IDC_NEW_TAB = 1101,
    IDC_SUGGEST = 1200,
};
static const int  kTabBarH = 26, kToolbarH = 32;
static const int  kTabW = 140, kTabMinW = 24;
static const int  kBtnW = 64, kBtnGap = 6, kMargin = 6;
static const int  kTabDetachSlop = 12; // how far above/below a tab strip a dragged tab still counts as on it
static const UINT WM_APP_INIT_WEBVIEW = WM_APP + 42;
static const UINT WM_APP_SWITCH_TAB = WM_APP + 43;
static const UINT WM_APP_DROP_TAB = WM_APP + 44;
static const UINT WM_APP_CLOSE_TAB = WM_APP + 45;

static HINSTANCE g_hInst = nullptr;

// ---- Winamp parent subclass (single hook, dispatches to all windows) ----
static WNDPROC g_oldWaProc = nullptr;

// ---- shared WebView2 environment ----
static ComPtr<ICoreWebView2Environment> g_env;
static bool g_envCreating = false;
static std::vector<std::function<void()>> g_envPendingCallbacks;

// ---- shared UI resources ----
static HBRUSH g_brBlack = nullptr, g_brPanel = nullptr, g_brEdit = nullptr;
static COLORREF g_colBg = RGB(0, 0, 0), g_colPanel = RGB(16, 16, 16), g_colBtnActive = RGB(32, 32, 32), g_colText = RGB(255, 255, 255);
static HFONT g_font = nullptr;
static ULONG_PTR g_gdiplusToken = 0;
static bool g_globalsReady = false;

// ---- per-window model ----
struct Tab {
    ComPtr<ICoreWebView2Controller> ctl;
    ComPtr<ICoreWebView2> web;
    HWND btn = nullptr;
    std::wstring title;
    std::wstring pendingUri;
    std::wstring lastUri;
    HICON favicon = nullptr;
    int id = 0;                 // stable id for async event routing
    bool creating = false;      // a controller is being created for this tab
    bool loading = false;       // a navigation is in progress
};

struct BrowserWindow {
    HWND embedFrame = nullptr;
    WNDPROC prevFrameProc = nullptr;
    HWND client = nullptr;

    HWND tabBar = nullptr;
    HWND newTabBtn = nullptr;
    HWND btnBack = nullptr, btnFwd = nullptr, btnGo = nullptr, editUrl = nullptr;
    HWND btnReload = nullptr;
    HWND btnCloseTab = nullptr;

    HWND suggest = nullptr;
    bool suggestVisible = false;

    bool editDirty = false;
    bool settingUrl = false;
    bool urlClickSelect = false;   // the click in progress is the one that focused the address bar

    std::vector<Tab> tabs;
    int active = -1;

    embedWindowState ews = { 0 };
    UINT_PTR menuId = (UINT_PTR)-1;
    GUID guid = {};

    bool minimized = false;        // the frame is minimized; its pages are told they are hidden

    bool isFullscreen = false;
    HWND fsWnd = nullptr;      // dedicated borderless top-level window used while a tab is fullscreen
    int fsTabId = -1;          // id of the tab whose controller is parented into fsWnd
};

static std::vector<std::unique_ptr<BrowserWindow>> g_windows;

// Tab ids are unique across all windows, so a tab keeps its id (and its event
// routing) when it is dragged from one window to another.
static int g_nextTabId = 1;

// ---- tab drag (one at a time, shared across windows) ----
struct TabDrag {
    int tabId = 0;             // 0 = no tab is being pressed/dragged
    HWND btn = nullptr;        // the pressed tab button; holds mouse capture
    POINT grab = {};           // where inside the tab it was grabbed
    bool dragging = false;     // moved far enough to count as a drag, not a click
    HWND ghost = nullptr;      // floating copy of the tab that follows the cursor
    HWND marker = nullptr;     // insertion mark shown in the tab strip under the cursor
};
static TabDrag g_drag;
struct TabDrop { int tabId; POINT pt; POINT grab; };

// ---- shared history / closed tabs ----
static std::vector<std::wstring> g_history;
struct ClosedTab { std::wstring url; };
static std::vector<ClosedTab> g_closedTabs;

// ---- window/tab lookup ----
static BrowserWindow* WinFromClient(HWND client) {
    for (auto& w : g_windows) if (w->client == client) return w.get();
    return nullptr;
}
static BrowserWindow* WinFromFrame(HWND frame) {
    for (auto& w : g_windows) if (w->embedFrame == frame) return w.get();
    return nullptr;
}
static BrowserWindow* WinFromChild(HWND child) { return WinFromClient(GetParent(child)); }
static int TabIndexById(BrowserWindow* win, int id) {
    for (int i = 0; i < (int)win->tabs.size(); ++i) if (win->tabs[i].id == id) return i;
    return -1;
}
// The window a tab currently lives in, or null once the tab is gone.
static BrowserWindow* WinFromTabId(int id) {
    for (auto& w : g_windows) if (TabIndexById(w.get(), id) >= 0) return w.get();
    return nullptr;
}

// ---------- persistence ----------
static void ModuleDir(wchar_t* out, size_t cch) { out[0] = 0; if (!GetModuleFileNameW((HINSTANCE)g_hInst, out, (DWORD)cch))return; PathRemoveFileSpecW(out); }

static std::wstring DataDirPath() {
    wchar_t d[MAX_PATH]; ModuleDir(d, MAX_PATH);
    std::wstring p = d; p += L"\\exodium_data";
    CreateDirectoryW(p.c_str(), nullptr);
    return p;
}
static std::wstring WebViewProfilePath() {
    std::wstring p = DataDirPath() + L"\\webview_profile";
    CreateDirectoryW(p.c_str(), nullptr);
    return p;
}
static std::wstring HistoryFilePath() { auto p = DataDirPath(); p += L"\\history.txt"; return p; }

static void TrimCRLF(std::wstring& s) { while (!s.empty() && (s.back() == L'\r' || s.back() == L'\n')) s.pop_back(); }
static bool IsLikelyHttpUrl(const std::wstring& u) {
    if (u.empty()) return false;
    return StrCmpNIW(u.c_str(), L"http://", 7) == 0 || StrCmpNIW(u.c_str(), L"https://", 8) == 0 || StrCmpNIW(u.c_str(), L"file://", 7) == 0;
}
static void AddToHistory(const std::wstring& url) {
    if (!IsLikelyHttpUrl(url)) return;
    auto it = std::remove_if(g_history.begin(), g_history.end(), [&](const std::wstring& s) { return _wcsicmp(s.c_str(), url.c_str()) == 0; });
    if (it != g_history.end()) g_history.erase(it, g_history.end());
    g_history.insert(g_history.begin(), url);
    if (g_history.size() > 200) g_history.resize(200);
}
static void LoadHistory() {
    g_history.clear();
    std::wstring path = HistoryFilePath();
    FILE* f = nullptr; if (_wfopen_s(&f, path.c_str(), L"rt, ccs=UNICODE") != 0 || !f) return;
    wchar_t buf[4096];
    while (fgetws(buf, ARRAYSIZE(buf), f)) {
        std::wstring line = buf; TrimCRLF(line); if (line.empty() || !IsLikelyHttpUrl(line)) continue;
        bool exists = false; for (auto& s : g_history) { if (!_wcsicmp(s.c_str(), line.c_str())) { exists = true; break; } }
        if (!exists) g_history.push_back(line);
    }
    fclose(f);
    std::reverse(g_history.begin(), g_history.end());
    if (g_history.size() > 200) g_history.resize(200);
}
static void SaveHistory() {
    std::wstring path = HistoryFilePath();
    FILE* f = nullptr; if (_wfopen_s(&f, path.c_str(), L"wt, ccs=UNICODE") != 0 || !f) return;
    for (const auto& u : g_history) { fputws(u.c_str(), f); fputwc(L'\n', f); }
    fclose(f);
}

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
static BrowserWindow* CreateBrowserWindow(bool show, const RECT* place = nullptr, bool forDroppedTab = false);
static LRESULT CALLBACK TabButtonSubproc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR);
static LRESULT CALLBACK TabGhostProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
static void EndTabDrag();
static void EnterVideoFullscreen(BrowserWindow* win);
static void ExitVideoFullscreen(BrowserWindow* win);
static void LeaveVideoFullscreen(BrowserWindow* win);
static LRESULT CALLBACK FullscreenProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

static void FocusAddressBar(BrowserWindow* win) { SetFocus(win->editUrl); SendMessageW(win->editUrl, EM_SETSEL, 0, -1); }
static std::wstring GetActiveUrl(BrowserWindow* win) {
    if (win->active < 0 || win->active >= (int)win->tabs.size() || !win->tabs[win->active].web) return L"";
    LPWSTR uri = nullptr; std::wstring out; if (SUCCEEDED(win->tabs[win->active].web->get_Source(&uri))) { if (uri) { out = uri; CoTaskMemFree(uri); } } return out;
}
static void UpdateUrlBox(BrowserWindow* win) {
    if (GetFocus() == win->editUrl && win->editDirty) return;
    if (win->active < 0 || win->active >= (int)win->tabs.size()) return;
    // A tab whose webview isn't up yet shows where it is headed.
    std::wstring url = win->tabs[win->active].web ? GetActiveUrl(win) : win->tabs[win->active].pendingUri;
    win->settingUrl = true; SetWindowTextW(win->editUrl, url.c_str()); win->settingUrl = false;
}
static void UpdateReloadButton(BrowserWindow* win)
{
    if (!win->btnReload) return;
    bool loading = win->active >= 0 && win->active < (int)win->tabs.size() && win->tabs[win->active].loading;
    SetWindowTextW(win->btnReload, loading ? L"⏹" : L"↺");
}

// Give the frame its own taskbar button / Alt+Tab slot. Winamp creates the
// embed frame as a top-level window owned by the main window, so it is
// suppressed from the switcher unless it carries WS_EX_APPWINDOW.
static void EnableTaskbarButton(HWND frame)
{
    if (!frame) return;
    LONG_PTR ex = GetWindowLongPtr(frame, GWL_EXSTYLE);
    ex |= WS_EX_APPWINDOW;
    ex &= ~WS_EX_TOOLWINDOW;
    SetWindowLongPtr(frame, GWL_EXSTYLE, ex);
}

// Windows keeps an owner and everything it owns together in the z-order, so while
// Winamp's main window owned the frame, bringing the browser forward dragged the
// library, playlist and the rest up with it. Without an owner it is raised, and
// Alt+Tabbed to, on its own.
static void DetachFromWinampWindows(HWND frame)
{
    if (!frame || (GetWindowLongPtr(frame, GWL_STYLE) & WS_CHILD)) return; // docked into a skin
    if (GetWindow(frame, GW_OWNER)) SetWindowLongPtr(frame, GWLP_HWNDPARENT, 0);
}

// Windows only maximizes (Win+Up), minimizes (Win+Down) and snaps (Win+Left/Right)
// windows that say they can be, which Winamp's frames don't. Those styles normally bring
// a system border with them. FrameProc hands all of it back to the client area and DWM is
// told to draw none of it, so the frame still looks exactly the way Winamp paints it.
static void EnableSnapAndMaximize(HWND frame)
{
    if (!frame) return;
    SetWindowLongPtr(frame, GWL_STYLE, GetWindowLongPtr(frame, GWL_STYLE) | WS_THICKFRAME | WS_MAXIMIZEBOX | WS_MINIMIZEBOX);
    DWMNCRENDERINGPOLICY policy = DWMNCRP_DISABLED;
    DwmSetWindowAttribute(frame, DWMWA_NCRENDERING_POLICY, &policy, sizeof(policy));
    SetWindowPos(frame, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}

// Turns a maximized frame back into a normal window at rc (screen coordinates).
static void RestoreFrameTo(HWND frame, const RECT& rc)
{
    WINDOWPLACEMENT wp{ sizeof(wp) };
    if (!GetWindowPlacement(frame, &wp)) return;
    wp.rcNormalPosition = rc;
    wp.showCmd = SW_SHOWNORMAL;
    SetWindowPlacement(frame, &wp);
    // A placement is measured from the work area's corner; this settles any difference.
    SetWindowPos(frame, nullptr, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top, SWP_NOZORDER | SWP_NOACTIVATE);
}

// Reflect the active tab's title in the frame caption so each window is a
// distinct, recognizable Alt+Tab entry.
static void UpdateFrameTitle(BrowserWindow* win)
{
    if (!win->embedFrame) return;
    std::wstring t;
    if (win->active >= 0 && win->active < (int)win->tabs.size())
        t = win->tabs[win->active].title;
    if (t.empty() || t == L"(new tab)") t = L"Exodium";
    SetWindowTextW(win->embedFrame, t.c_str());
}

static void ReopenLastClosedTab(BrowserWindow* win)
{
    if (g_closedTabs.empty()) return;
    ClosedTab ct = g_closedTabs.back();
    g_closedTabs.pop_back();
    if (!ct.url.empty())
        NewTab(win, ct.url.c_str());
}

static bool HandleGlobalShortcut(BrowserWindow* win, WPARAM w, bool ctrl, bool shift, bool alt) {
    // Handles shortcuts locally when the plugin or webview is focused.

    if (g_drag.tabId && w == VK_ESCAPE) { EndTabDrag(); return true; }

    if (ctrl && !shift && !alt && w == 'N') {
        // Opens at the size of the window it came from, stepped down and right of it.
        RECT r; bool like = win && GetWindowRect(win->embedFrame, &r);
        if (like) OffsetRect(&r, 30, 30);
        BrowserWindow* nw = CreateBrowserWindow(true, like ? &r : nullptr);
        if (nw) SetForegroundWindow(nw->embedFrame);
        return true;
    }

    if (!win) return false;

    // These need the tab strip and toolbar back, so they end fullscreen video first.
    if (win->isFullscreen && ctrl && !alt && (w == 'L' || w == 'T' || w == 'W' || w == VK_TAB))
        LeaveVideoFullscreen(win);

    if (ctrl && shift && !alt && w == 'T') { ReopenLastClosedTab(win); return true; }

    if (ctrl && !shift && !alt && w == 'L') { FocusAddressBar(win); win->editDirty = true; return true; }
    if (ctrl && !alt && !shift && w == 'T') { NewTab(win); return true; }
    if (ctrl && !alt && !shift && w == 'W') { CloseActiveTab(win); return true; }

    if (ctrl && !alt && w == VK_TAB) {
        if (shift) PrevTab(win); else NextTab(win);
        return true;
    }

    if (!ctrl && !shift && alt && w == VK_LEFT && win->active >= 0 && win->tabs[win->active].web) {
        win->tabs[win->active].web->GoBack();    return true;
    }
    if (!ctrl && !shift && alt && w == VK_RIGHT && win->active >= 0 && win->tabs[win->active].web) {
        win->tabs[win->active].web->GoForward(); return true;
    }

    return false;
}

static void ApplySuggestSelectionToEdit(BrowserWindow* win) {
    if (!win->suggestVisible || !win->suggest || !win->editUrl) return;
    LRESULT sel = SendMessageW(win->suggest, LB_GETCURSEL, 0, 0);
    if (sel == LB_ERR) return;
    wchar_t buf[2048] = L"";
    SendMessageW(win->suggest, LB_GETTEXT, (WPARAM)sel, (LPARAM)buf);
    win->settingUrl = true;
    SetWindowTextW(win->editUrl, buf);
    win->settingUrl = false;
    SendMessageW(win->editUrl, EM_SETSEL, 0, -1);
}

static bool LooksLikeDomain(const std::wstring& s) {
    if (s.empty()) return false;
    if (s.find(L' ') != std::wstring::npos) return false;
    auto dot = s.find(L'.'); if (dot == std::wstring::npos) return false;
    auto lastDot = s.rfind(L'.');
    if (lastDot == std::wstring::npos || lastDot + 1 >= s.size()) return false;
    size_t tldLen = s.size() - (lastDot + 1);
    if (tldLen < 2 || tldLen > 24) return false;
    for (size_t i = lastDot + 1; i < s.size(); ++i) {
        if (!iswalpha(s[i])) return false;
    }
    return true;
}

static std::wstring BuildGoogleQuery(const std::wstring& q) {
    wchar_t enc[4096];
    DWORD cch = ARRAYSIZE(enc);
    if (FAILED(UrlEscapeW(q.c_str(), enc, &cch, URL_ESCAPE_PERCENT | URL_ESCAPE_AS_UTF8))) {
        return L"https://www.google.com/search?q=" + q;
    }
    std::wstring url = L"https://www.google.com/search?q=";
    url += enc;
    return url;
}

static LRESULT CALLBACK WaSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    for (auto& w : g_windows)
        HandleEmbeddedWindowWinampWindowMessages(w->embedFrame, (UINT)w->menuId, &w->ews, TRUE, hwnd, msg, wParam, lParam);
    LRESULT r = CallWindowProc(g_oldWaProc, hwnd, msg, wParam, lParam);
    for (auto& w : g_windows)
        HandleEmbeddedWindowWinampWindowMessages(w->embedFrame, (UINT)w->menuId, &w->ews, FALSE, hwnd, msg, wParam, lParam);
    return r;
}

static void HookWinampParent()
{
    if (!g_oldWaProc)
        g_oldWaProc = (WNDPROC)SetWindowLongPtr(plugin.hwndParent, GWLP_WNDPROC, (LONG_PTR)WaSubclassProc);
}

static void EnsureGlobals()
{
    if (g_globalsReady) return;

    INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_WIN95_CLASSES | ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);

    g_brBlack = CreateSolidBrush(g_colBg);
    g_brPanel = CreateSolidBrush(g_colPanel);
    g_brEdit = CreateSolidBrush(g_colBg);
    g_font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

    WNDCLASSW wc = {};
    wc.lpfnWndProc = ClientProc;
    wc.hInstance = g_hInst;
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = L"ExodiumClient";
    RegisterClassW(&wc);

    // Plain, borderless top-level window used only while a video/page is fullscreen.
    // The WebView2 controller is reparented into this instead of resizing the
    // Winamp-owned embed frame, which avoids fighting Winamp's own window chrome.
    WNDCLASSW wcfs = {};
    wcfs.lpfnWndProc = FullscreenProc;
    wcfs.hInstance = g_hInst;
    wcfs.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wcfs.lpszClassName = L"ExodiumFullscreen";
    RegisterClassW(&wcfs);

    // Floating copy of a tab that follows the cursor while the tab is dragged.
    WNDCLASSW wcgh = {};
    wcgh.lpfnWndProc = TabGhostProc;
    wcgh.hInstance = g_hInst;
    wcgh.lpszClassName = L"ExodiumTabGhost";
    RegisterClassW(&wcgh);

    LoadHistory();

    g_globalsReady = true;
}

// ---------- owner draw ----------
static void DrawOwnerButton(const DRAWITEMSTRUCT* dis, bool isTab, bool isActive, HICON hIcon) {
    HDC dc = dis->hDC;
    RECT r = dis->rcItem;

    COLORREF bg = isTab ? (isActive ? g_colBtnActive : g_colPanel) : g_colPanel;
    HBRUSH br = CreateSolidBrush(bg);
    FillRect(dc, &r, br);
    DeleteObject(br);

    HPEN pen = CreatePen(PS_SOLID, 1, RGB(60, 60, 60));
    HPEN old = (HPEN)SelectObject(dc, pen);
    MoveToEx(dc, r.left, r.top, nullptr);
    LineTo(dc, r.right - 1, r.top);
    LineTo(dc, r.right - 1, r.bottom - 1);
    LineTo(dc, r.left, r.bottom - 1);
    LineTo(dc, r.left, r.top);
    SelectObject(dc, old);
    DeleteObject(pen);

    RECT textRect = r;

    if (isTab && hIcon) {
        const int iconSize = 16;
        const int padding = 4;
        int btnHeight = r.bottom - r.top;
        int iconY = r.top + (btnHeight - iconSize) / 2;

        DrawIconEx(dc, r.left + padding, iconY, hIcon, iconSize, iconSize, 0, nullptr, DI_NORMAL);

        textRect.left += iconSize + padding * 2;
    }

    wchar_t text[256];
    GetWindowTextW(dis->hwndItem, text, ARRAYSIZE(text));

    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, g_colText);

    HFONT oldf = (HFONT)SelectObject(dc, g_font ? g_font : (HFONT)GetStockObject(DEFAULT_GUI_FONT));
    DrawTextW(dc, text, -1, &textRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    SelectObject(dc, oldf);

    if (dis->itemState & ODS_FOCUS) {
        RECT rf = textRect;
        InflateRect(&rf, -3, -3);
        DrawFocusRect(dc, &rf);
    }
}

// ---------- suggestions ----------
static void HideSuggestions(BrowserWindow* win) { if (win->suggest) ShowWindow(win->suggest, SW_HIDE); win->suggestVisible = false; }
static void EnsureSuggestListbox(BrowserWindow* win) {
    if (!win->suggest) {
        win->suggest = CreateWindowExW(
            WS_EX_TOOLWINDOW, L"LISTBOX", L"",
            WS_CHILD | WS_TABSTOP | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT | WS_VSCROLL | WS_BORDER,
            0, 0, 0, 0, win->client, (HMENU)IDC_SUGGEST, g_hInst, nullptr);
        SendMessageW(win->suggest, WM_SETFONT, (WPARAM)g_font, TRUE);
        SetWindowSubclass(win->suggest, SuggestSubproc, 2, 0);
    }
}
static void PositionSuggestBelowEdit(BrowserWindow* win, int items) {
    RECT er; GetWindowRect(win->editUrl, &er);
    POINT tl{ er.left,er.bottom }, br{ er.right,er.bottom };
    ScreenToClient(win->client, &tl); ScreenToClient(win->client, &br);
    int itemH = (int)SendMessageW(win->suggest, LB_GETITEMHEIGHT, 0, 0); if (itemH <= 0) itemH = 18;
    int h = min(items, 8) * itemH + 2, w = (br.x - tl.x);
    SetWindowPos(win->suggest, HWND_TOP, tl.x, tl.y, w, h, SWP_SHOWWINDOW);
}
static void PopulateSuggestions(BrowserWindow* win, const std::wstring& q) {
    EnsureSuggestListbox(win); SendMessageW(win->suggest, LB_RESETCONTENT, 0, 0);
    if (q.size() < 2) { HideSuggestions(win); return; }
    auto wlower = [](std::wstring s) { for (auto& c : s) c = (wchar_t)towlower(c); return s; };
    std::wstring ql = wlower(q); std::vector<std::wstring> matches;
    auto add_if = [&](const std::wstring& s) {
        if (matches.size() >= 64) return;
        if (std::any_of(matches.begin(), matches.end(), [&](const std::wstring& x) {return _wcsicmp(x.c_str(), s.c_str()) == 0; })) return;
        matches.push_back(s);
        };
    for (const auto& u : g_history) { std::wstring ul = wlower(u); if (ul.find(ql) == 0) add_if(u); }
    for (const auto& u : g_history) { std::wstring ul = wlower(u); if (!ul.empty() && ul.find(ql) != 0 && ul.find(ql) != std::wstring::npos) add_if(u); }
    if (matches.empty()) { HideSuggestions(win); return; }
    for (const auto& s : matches) SendMessageW(win->suggest, LB_ADDSTRING, 0, (LPARAM)s.c_str());
    SendMessageW(win->suggest, LB_SETCURSEL, 0, 0); PositionSuggestBelowEdit(win, (int)matches.size()); win->suggestVisible = true;
}
static void CommitSuggestion(BrowserWindow* win, bool navigate) {
    if (!win->suggestVisible) return;
    LRESULT sel = SendMessageW(win->suggest, LB_GETCURSEL, 0, 0); if (sel == LB_ERR) return;
    wchar_t buf[2048] = L""; SendMessageW(win->suggest, LB_GETTEXT, (WPARAM)sel, (LPARAM)buf);
    win->settingUrl = true; SetWindowTextW(win->editUrl, buf); win->settingUrl = false;
    HideSuggestions(win); SetFocus(win->editUrl); SendMessageW(win->editUrl, EM_SETSEL, 0, -1);
    if (navigate) { win->editDirty = false; NavigateFromUrlBox(win); }
}
// Closes the list and hands the keyboard back to the address bar with the picked
// suggestion left in it, caret at the end, ready to be edited or added to.
static void EditPickedSuggestion(BrowserWindow* win) {
    HideSuggestions(win);
    if (!win->editUrl) return;
    SetFocus(win->editUrl);
    int len = GetWindowTextLengthW(win->editUrl);
    SendMessageW(win->editUrl, EM_SETSEL, len, len);
    SendMessageW(win->editUrl, EM_SCROLLCARET, 0, 0);
    win->editDirty = true; // it is the user's text now; page events must not replace it
}

static LRESULT CALLBACK SuggestSubproc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR)
{
    BrowserWindow* win = WinFromChild(h);
    switch (m)
    {
    case WM_GETDLGCODE:
        return DLGC_WANTALLKEYS | DLGC_WANTCHARS;

    case WM_CHAR:
    {
        wchar_t ch = static_cast<wchar_t>(w);

        if (ch == L'\r') {
            if (win) CommitSuggestion(win, true);
            return 0;
        }
        // Escape was handled on WM_KEYDOWN; the rest are leftovers of Ctrl shortcuts.
        if (ch < 0x20 && ch != L'\b')
            return 0;

        // Typing, or Backspace, carries on from the picked suggestion in the address bar.
        if (win && win->editUrl) {
            EditPickedSuggestion(win);
            SendMessageW(win->editUrl, WM_CHAR, w, l);
        }
        return 0;
    }

    case WM_KEYDOWN:
    {
        int count = (int)SendMessageW(h, LB_GETCOUNT, 0, 0);
        int sel = (int)SendMessageW(h, LB_GETCURSEL, 0, 0);
        if (count <= 0)
            break;

        if (w == VK_RETURN) {
            if (win) CommitSuggestion(win, true);
            return 0;
        }
        if (w == VK_ESCAPE) {
            if (win) EditPickedSuggestion(win);
            return 0;
        }
        if (w == VK_UP) {
            if (sel <= 0) {
                if (win && win->editUrl) {
                    SetFocus(win->editUrl);
                    SendMessageW(win->editUrl, EM_SETSEL, 0, -1);
                }
                return 0;
            }
            SendMessageW(h, LB_SETCURSEL, sel - 1, 0);
            if (win) ApplySuggestSelectionToEdit(win);
            return 0;
        }
        if (w == VK_DOWN) {
            int next = (sel == LB_ERR) ? 0 : sel + 1;
            if (next >= count)
                next = count - 1;
            SendMessageW(h, LB_SETCURSEL, next, 0);
            if (win) ApplySuggestSelectionToEdit(win);
            return 0;
        }
        break;
    }
    }

    return DefSubclassProc(h, m, w, l);
}

// ---------- navigation ----------
static void NavigateFromUrlBox(BrowserWindow* win) {
    if (win->active < 0 || win->active >= (int)win->tabs.size()) return;
    wchar_t raw[2048] = L""; GetWindowTextW(win->editUrl, raw, ARRAYSIZE(raw));
    std::wstring input = raw;
    auto trim = [](std::wstring& s) {
        auto notsp = [](wchar_t c) { return c != L' ' && c != L'\t' && c != L'\r' && c != L'\n'; };
        while (!s.empty() && !notsp(s.front())) s.erase(s.begin());
        while (!s.empty() && !notsp(s.back()))  s.pop_back();
        };
    trim(input);
    if (input.empty()) return;
    std::wstring urlToGo;
    if (PathIsURLW(input.c_str())) urlToGo = input;
    else if (LooksLikeDomain(input)) urlToGo = L"https://" + input;
    else urlToGo = BuildGoogleQuery(input);

    win->editDirty = false;
    HideSuggestions(win);
    if (win->tabs[win->active].web) win->tabs[win->active].web->Navigate(urlToGo.c_str());
    else {
        win->tabs[win->active].pendingUri = urlToGo;
        PostMessage(win->client, WM_APP_INIT_WEBVIEW, 0, 0);
    }
}

// ---------- edit subclass ----------
static LRESULT CALLBACK EditSubproc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR)
{
    BrowserWindow* win = WinFromChild(h);
    switch (m) {
    case WM_NCCALCSIZE:
        if (w) {
            NCCALCSIZE_PARAMS* p = reinterpret_cast<NCCALCSIZE_PARAMS*>(l);
            InflateRect(&p->rgrc[0], -1, -1);
            return 0;
        }
        break;

    case WM_NCPAINT: {
        HDC hdc = GetWindowDC(h);
        if (hdc) {
            RECT wr; GetWindowRect(h, &wr); OffsetRect(&wr, -wr.left, -wr.top);
            HPEN pen = CreatePen(PS_SOLID, 1, RGB(60, 60, 60));
            HGDIOBJ oldPen = SelectObject(hdc, pen);
            HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(NULL_BRUSH));
            Rectangle(hdc, 0, 0, wr.right - wr.left, wr.bottom - wr.top);
            SelectObject(hdc, oldBrush);
            SelectObject(hdc, oldPen);
            DeleteObject(pen);
            ReleaseDC(h, hdc);
        }
        return 0;
    }

    case WM_GETDLGCODE:
        return DLGC_WANTALLKEYS | DLGC_WANTCHARS;

    case WM_KEYDOWN: {
        bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        bool alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
        if (HandleGlobalShortcut(win, (WPARAM)w, ctrl, shift, alt)) return 0;
        if (win && ctrl && w == 'L') { FocusAddressBar(win); win->editDirty = true; return 0; }
        if (win && h == win->editUrl && ctrl && !alt && w == 'A') { SendMessageW(h, EM_SETSEL, 0, -1); return 0; }
        if (win && w == VK_DOWN && win->suggestVisible) {
            SetFocus(win->suggest);
            SendMessageW(win->suggest, LB_SETCURSEL, 0, 0);
            ApplySuggestSelectionToEdit(win);
            return 0;
        }
        if (win && w == VK_ESCAPE && win->suggestVisible) { HideSuggestions(win); return 0; }
        if (win && w == VK_RETURN) { win->editDirty = false; NavigateFromUrlBox(win); return 0; }
        break;
    }

    case WM_CHAR:
        // Enter already navigated on WM_KEYDOWN. The other control characters are what
        // shortcuts like Ctrl+L or Ctrl+T leave behind, and the edit would beep at them
        // or insert junk. Only copy, paste, cut, undo and Backspace go through. Testing
        // the character rather than the Ctrl key keeps AltGr-typed characters working.
        if ((w < 0x20 || w == 0x7F) && w != 0x03 && w != 0x16 && w != 0x18 && w != 0x1A && w != L'\b')
            return 0;
        break;

    case WM_LBUTTONDOWN:
        if (win && h == win->editUrl && GetFocus() != h) win->urlClickSelect = true;
        break;

    case WM_LBUTTONUP:
        // Clicking into the address bar selects the whole URL, as browsers do, unless
        // that click was itself a drag-select.
        if (win && h == win->editUrl && win->urlClickSelect) {
            win->urlClickSelect = false;
            LRESULT r = DefSubclassProc(h, m, w, l);
            DWORD from = 0, to = 0;
            SendMessageW(h, EM_GETSEL, (WPARAM)&from, (LPARAM)&to);
            if (from == to) SendMessageW(h, EM_SETSEL, 0, -1);
            return r;
        }
        break;
    }

    return DefSubclassProc(h, m, w, l);
}

static void SubclassCtrlCommon(HWND h) { if (h) SetWindowSubclass(h, EditSubproc, 1, 0); }

// ---------- layout ----------
static void LayoutActiveWebView(BrowserWindow* win) {
    if (win->isFullscreen) return; // bounds/parent are driven by the fullscreen window instead
    if (win->active < 0 || win->active >= (int)win->tabs.size()) return;
    if (!win->tabs[win->active].ctl) return;
    RECT rc; GetClientRect(win->client, &rc);
    int top = kTabBarH + kToolbarH; RECT b = { kMargin,top,rc.right - kMargin,rc.bottom - kMargin };
    win->tabs[win->active].ctl->put_Bounds(b); win->tabs[win->active].ctl->put_IsVisible(TRUE);
}
static void HideAllWebViews(BrowserWindow* win) { for (auto& t : win->tabs) if (t.ctl) t.ctl->put_IsVisible(FALSE); }

static void LayoutToolbarAndTabs(BrowserWindow* win) {
    if (!win->client) return;
    RECT rc; GetClientRect(win->client, &rc);
    if (!win->tabBar) win->tabBar = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, win->client, (HMENU)IDC_TABBAR, g_hInst, nullptr);
    MoveWindow(win->tabBar, 0, 0, rc.right, kTabBarH, TRUE);

    // Tabs are full width while they all fit and squeeze evenly once they don't.
    const int tabH = kTabBarH - 2, tabCount = (int)win->tabs.size();
    int x = kMargin, tabW = kTabW;
    if (tabCount > 0) tabW = max(kTabMinW, min(kTabW, (rc.right - 26 - kMargin) / tabCount - 4));
    for (int i = 0; i < (int)win->tabs.size(); ++i) {
        if (!win->tabs[i].btn) {
            win->tabs[i].btn = CreateWindowExW(0, L"BUTTON", win->tabs[i].title.empty() ? L"(new tab)" : win->tabs[i].title.c_str(),
                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 0, 0, 0, 0, win->client, (HMENU)(INT_PTR)(WM_APP_SWITCH_TAB + i), g_hInst, nullptr);
            SendMessageW(win->tabs[i].btn, WM_SETFONT, (WPARAM)(g_font ? g_font : GetStockObject(DEFAULT_GUI_FONT)), TRUE);
            SubclassCtrlCommon(win->tabs[i].btn);
            SetWindowSubclass(win->tabs[i].btn, TabButtonSubproc, 3, 0);
        }
        else {
            SetWindowLongPtrW(win->tabs[i].btn, GWLP_ID, (LONG_PTR)(WM_APP_SWITCH_TAB + i));
        }
        MoveWindow(win->tabs[i].btn, x, 1, tabW, tabH, TRUE); x += tabW + 4;
    }

    if (!win->newTabBtn) {
        win->newTabBtn = CreateWindowExW(0, L"BUTTON", L"+", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 0, 0, 0, 0, win->client, (HMENU)IDC_NEW_TAB, g_hInst, nullptr);
        SendMessageW(win->newTabBtn, WM_SETFONT, (WPARAM)g_font, TRUE);
        SubclassCtrlCommon(win->newTabBtn);
    }
    MoveWindow(win->newTabBtn, rc.right - 26, 1, 24, tabH, TRUE);

    int y = kTabBarH + kMargin, h = kToolbarH - kMargin * 2, tX = kMargin;

    if (!win->btnBack)
        win->btnBack = CreateWindowExW(0, L"BUTTON", L"←", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
            0, 0, 0, 0, win->client, (HMENU)IDC_BACK, g_hInst, nullptr);
    if (!win->btnFwd)
        win->btnFwd = CreateWindowExW(0, L"BUTTON", L"→", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
            0, 0, 0, 0, win->client, (HMENU)IDC_FWD, g_hInst, nullptr);
    if (!win->btnReload)
        win->btnReload = CreateWindowExW(0, L"BUTTON", L"↺", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
            0, 0, 0, 0, win->client, (HMENU)IDC_RELOAD, g_hInst, nullptr);
    if (!win->btnCloseTab)
        win->btnCloseTab = CreateWindowExW(0, L"BUTTON", L"X", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
            0, 0, 0, 0, win->client, (HMENU)IDC_CLOSE_TAB, g_hInst, nullptr);
    if (!win->editUrl)
        win->editUrl = CreateWindowExW(0, L"EDIT", L"https://google.com/", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
            0, 0, 0, 0, win->client, (HMENU)IDC_URL, g_hInst, nullptr);

    for (HWND hCtl : { win->btnBack, win->btnFwd, win->btnReload, win->btnCloseTab, win->editUrl })
        SendMessageW(hCtl, WM_SETFONT, (WPARAM)g_font, TRUE);

    SubclassCtrlCommon(win->btnBack);
    SubclassCtrlCommon(win->btnFwd);
    SubclassCtrlCommon(win->btnReload);
    SubclassCtrlCommon(win->btnCloseTab);
    SetWindowSubclass(win->editUrl, EditSubproc, 1, 0);

    // In a narrow window the buttons give up width before the address bar does.
    const int btnW = max(28, min(kBtnW, (rc.right - 2 * kMargin - 160) / 4 - kBtnGap));
    MoveWindow(win->btnBack, tX, y, btnW, h, TRUE); tX += btnW + kBtnGap;
    MoveWindow(win->btnFwd, tX, y, btnW, h, TRUE); tX += btnW + kBtnGap;
    MoveWindow(win->btnReload, tX, y, btnW, h, TRUE); tX += btnW + kBtnGap;
    MoveWindow(win->btnCloseTab, tX, y, btnW, h, TRUE); tX += btnW + kBtnGap;

    int urlW = rc.right - tX - kMargin;
    if (urlW < 40) urlW = 40;
    MoveWindow(win->editUrl, tX, y, urlW, h, TRUE);
    EnsureSuggestListbox(win); HideSuggestions(win);

    LayoutActiveWebView(win);
}

// ---------- tab ops ----------
static void UpdateTabTitle(BrowserWindow* win, int idx, const wchar_t* t) {
    if (idx < 0 || idx >= (int)win->tabs.size()) return;
    win->tabs[idx].title = t ? t : L"(tab)";
    if (win->tabs[idx].btn) SetWindowTextW(win->tabs[idx].btn, win->tabs[idx].title.c_str());
    if (idx == win->active) UpdateFrameTitle(win);
}
static void SwitchToTab(BrowserWindow* win, int idx) {
    if (win->isFullscreen) return; // the active tab's controller is parented elsewhere right now
    if (idx < 0 || idx >= (int)win->tabs.size()) return;
    if (win->active == idx) return;

    HideAllWebViews(win);
    win->active = idx;
    LayoutActiveWebView(win);
    UpdateUrlBox(win);
    UpdateReloadButton(win);
    UpdateFrameTitle(win);
    HideSuggestions(win);

    for (int i = 0; i < (int)win->tabs.size(); ++i)
        if (win->tabs[i].btn) SendMessageW(win->tabs[i].btn, BM_SETSTATE, (WPARAM)(i == win->active), 0);

    if (win->tabs[win->active].ctl) {
        win->tabs[win->active].ctl->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
    }
    else {
        // Webviews are only made for tabs that get looked at; this one's turn has come.
        PostMessage(win->client, WM_APP_INIT_WEBVIEW, 0, 0);
    }
}
static void NextTab(BrowserWindow* win) {
    if (win->tabs.empty()) return;
    int idx = (win->active + 1) % (int)win->tabs.size();
    SwitchToTab(win, idx);
}
static void PrevTab(BrowserWindow* win) {
    if (win->tabs.empty()) return;
    int idx = (win->active - 1 + (int)win->tabs.size()) % (int)win->tabs.size();
    SwitchToTab(win, idx);
}
static void NewTab(BrowserWindow* win, const wchar_t* url /*= L"https://google.com/"*/) {
    if (!url) url = L"https://google.com/";
    int idx = (int)win->tabs.size();
    win->tabs.push_back(Tab{});
    win->tabs[idx].id = g_nextTabId++;
    win->tabs[idx].title = L"(new tab)";
    win->tabs[idx].pendingUri = url ? url : L"";
    LayoutToolbarAndTabs(win);
    SwitchToTab(win, idx);
    PostMessage(win->client, WM_APP_INIT_WEBVIEW, 0, 0);
}
// Re-syncs a window after the tab at idx was taken out of it. It must still have tabs.
static void RefreshAfterTabRemoved(BrowserWindow* win, int idx) {
    if (win->active >= idx) win->active = max(0, win->active - 1);

    LayoutToolbarAndTabs(win);
    // Force a full refresh of the active tab's visuals since indices shifted.
    int cur = win->active; win->active = -1; SwitchToTab(win, cur);
}
// A window left without tabs goes away, the same way it does when its close button
// is clicked. It keeps a blank tab for when it is shown again.
static void CloseEmptiedWindow(BrowserWindow* win) {
    win->tabs.push_back(Tab{});
    win->tabs[0].id = g_nextTabId++;
    win->active = 0;
    win->editDirty = false; // whatever was being typed belonged to the tab that is gone
    HideSuggestions(win);
    LayoutToolbarAndTabs(win);
    UpdateFrameTitle(win);
    UpdateUrlBox(win);
    UpdateReloadButton(win);
    SendMessage(win->client, WM_CLOSE, 0, 0);
}
static void CloseTab(BrowserWindow* win, int idx) {
    if (idx < 0 || idx >= (int)win->tabs.size()) return;
    if (win->isFullscreen && win->tabs[idx].id == win->fsTabId) LeaveVideoFullscreen(win);

    std::wstring closingUrl;
    if (!win->tabs[idx].lastUri.empty()) {
        closingUrl = win->tabs[idx].lastUri;
    }
    else if (win->tabs[idx].web) {
        LPWSTR src = nullptr;
        if (SUCCEEDED(win->tabs[idx].web->get_Source(&src)) && src) {
            closingUrl = src;
            CoTaskMemFree(src);
        }
    }
    if (!closingUrl.empty()) {
        g_closedTabs.push_back({ closingUrl });
    }

    if (win->tabs[idx].btn) { DestroyWindow(win->tabs[idx].btn); win->tabs[idx].btn = nullptr; }
    if (win->tabs[idx].ctl) { win->tabs[idx].ctl->Close(); win->tabs[idx].ctl.Reset(); }
    win->tabs[idx].web.Reset();
    if (win->tabs[idx].favicon) { DestroyIcon(win->tabs[idx].favicon); win->tabs[idx].favicon = nullptr; }
    win->tabs.erase(win->tabs.begin() + idx);

    if (win->tabs.empty()) {
        CloseEmptiedWindow(win);
        return;
    }
    RefreshAfterTabRemoved(win, idx);
}
static void CloseActiveTab(BrowserWindow* win) { CloseTab(win, win->active); }

// ---------- tab dragging ----------
// A tab can be dragged along its strip to reorder it, onto another window's strip
// to move it there, or away from every strip to give it a window of its own.

// Moves a tab, live webview included, to position insertAt of dst, counted without
// the moved tab. dst may be the window the tab is already in.
static void MoveTab(BrowserWindow* src, int idx, BrowserWindow* dst, int insertAt)
{
    if (idx < 0 || idx >= (int)src->tabs.size()) return;
    int others = (int)dst->tabs.size() - (dst == src ? 1 : 0);
    insertAt = max(0, min(insertAt, others));

    if (dst == src) {
        if (insertAt == idx) return;
        int activeId = src->active >= 0 ? src->tabs[src->active].id : 0;
        Tab tab = std::move(src->tabs[idx]);
        src->tabs.erase(src->tabs.begin() + idx);
        src->tabs.insert(src->tabs.begin() + insertAt, std::move(tab));
        src->active = TabIndexById(src, activeId);
        LayoutToolbarAndTabs(src); // renumbers and repositions the tab buttons
        for (auto& t : src->tabs) if (t.btn) InvalidateRect(t.btn, nullptr, TRUE);
        return;
    }

    Tab tab = std::move(src->tabs[idx]);
    src->tabs.erase(src->tabs.begin() + idx);

    // Tab buttons are children of their window's client, so dst makes a fresh one
    // on layout. The webview itself just moves to the new parent.
    if (tab.btn) { DestroyWindow(tab.btn); tab.btn = nullptr; }
    if (tab.ctl) {
        tab.ctl->put_IsVisible(FALSE);
        tab.ctl->put_ParentWindow(dst->client);
    }

    if (src->tabs.empty()) {
        CloseEmptiedWindow(src);
    }
    else {
        RefreshAfterTabRemoved(src, idx);
    }

    dst->tabs.insert(dst->tabs.begin() + insertAt, std::move(tab));
    dst->active = -1; // indices shifted; SwitchToTab below re-syncs everything
    LayoutToolbarAndTabs(dst);
    SwitchToTab(dst, insertAt);
    if (dst->tabs[insertAt].ctl) dst->tabs[insertAt].ctl->NotifyParentWindowPositionChanged();
}

// Finds the tab strip under a screen point. insertAt is the position the dragged
// tab would take among that window's other tabs, marker where to draw the mark.
static BrowserWindow* TabDropTarget(POINT pt, int dragTabId, int& insertAt, POINT& marker)
{
    BrowserWindow* dst = nullptr;
    HWND desktop = GetDesktopWindow();
    for (HWND h = WindowFromPoint(pt); h && h != desktop && !dst; h = GetAncestor(h, GA_PARENT))
        for (auto& w : g_windows) if (h == w->client || h == w->embedFrame) { dst = w.get(); break; }
    if (!dst || dst->isFullscreen || !dst->tabBar) return nullptr;

    RECT strip; GetWindowRect(dst->tabBar, &strip);
    RECT hit = strip; InflateRect(&hit, 0, kTabDetachSlop);
    if (!PtInRect(&hit, pt)) return nullptr;

    // The mark sits in the gap in front of the first tab whose middle is right of the cursor.
    insertAt = 0;
    marker = { strip.left + kMargin - 3, strip.top + 1 };
    for (auto& t : dst->tabs) {
        if (t.id == dragTabId || !t.btn) continue;
        RECT br; GetWindowRect(t.btn, &br);
        if (pt.x < (br.left + br.right) / 2) { marker.x = br.left - 3; break; }
        ++insertAt;
        marker.x = br.right + 1;
    }
    return dst;
}

// Moves a window so that its first tab's top-left corner lands on a screen point.
static void MoveFrameTabTo(BrowserWindow* win, POINT at)
{
    if (win->tabs.empty() || !win->tabs[0].btn) return;
    RECT fr, br; GetWindowRect(win->embedFrame, &fr); GetWindowRect(win->tabs[0].btn, &br);
    int x = fr.left + at.x - br.left, y = fr.top + at.y - br.top;
    MONITORINFO mi = { sizeof(MONITORINFO) };
    if (GetMonitorInfo(MonitorFromPoint(at, MONITOR_DEFAULTTONEAREST), &mi)) y = max(y, (int)mi.rcWork.top);
    SetWindowPos(win->embedFrame, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

static void DropDraggedTab(const TabDrop& drop)
{
    BrowserWindow* src = WinFromTabId(drop.tabId);
    if (!src || src->isFullscreen) return;
    POINT tabAt = { drop.pt.x - drop.grab.x, drop.pt.y - drop.grab.y };

    int insertAt = 0; POINT marker = {};
    BrowserWindow* dst = TabDropTarget(drop.pt, drop.tabId, insertAt, marker);
    bool newWindow = false;
    if (!dst) {
        // Dropped away from every tab strip: the tab gets a window of its own, the
        // size of the one it came from. A window's only tab just takes its window along.
        if (src->tabs.size() == 1) { MoveFrameTabTo(src, tabAt); return; }
        RECT sr; GetWindowRect(src->embedFrame, &sr);
        dst = CreateBrowserWindow(true, &sr, true);
        if (!dst) return;
        newWindow = true;
    }

    MoveTab(src, TabIndexById(src, drop.tabId), dst, insertAt);

    if (newWindow) {
        MoveFrameTabTo(dst, tabAt);
        ShowWindow(dst->embedFrame, SW_SHOWNA);
    }
    SetForegroundWindow(dst->embedFrame);
    if (dst->active >= 0 && dst->tabs[dst->active].ctl)
        dst->tabs[dst->active].ctl->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
}

static LRESULT CALLBACK TabGhostProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_PAINT) {
        // Drawn exactly like the active tab; the ghost's window text is the tab title.
        PAINTSTRUCT ps;
        DRAWITEMSTRUCT dis = {};
        dis.hwndItem = hwnd;
        dis.hDC = BeginPaint(hwnd, &ps);
        GetClientRect(hwnd, &dis.rcItem);
        HICON icon = nullptr;
        if (BrowserWindow* win = WinFromTabId(g_drag.tabId)) icon = win->tabs[TabIndexById(win, g_drag.tabId)].favicon;
        DrawOwnerButton(&dis, true, true, icon);
        EndPaint(hwnd, &ps);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static void BeginTabDrag()
{
    g_drag.dragging = true;

    // Layered + transparent keeps both windows click-through, so they never get in
    // the way of finding the window under the cursor.
    const DWORD ex = WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE;
    RECT br; GetWindowRect(g_drag.btn, &br);
    wchar_t title[256] = L""; GetWindowTextW(g_drag.btn, title, ARRAYSIZE(title));
    g_drag.ghost = CreateWindowExW(ex, L"ExodiumTabGhost", title, WS_POPUP,
        br.left, br.top, br.right - br.left, br.bottom - br.top, nullptr, nullptr, g_hInst, nullptr);
    if (g_drag.ghost) SetLayeredWindowAttributes(g_drag.ghost, 0, 200, LWA_ALPHA);
    g_drag.marker = CreateWindowExW(ex, L"STATIC", L"", WS_POPUP | SS_WHITERECT,
        0, 0, 2, kTabBarH - 2, nullptr, nullptr, g_hInst, nullptr);
    if (g_drag.marker) SetLayeredWindowAttributes(g_drag.marker, 0, 255, LWA_ALPHA);
}

static void UpdateTabDrag(POINT pt)
{
    if (g_drag.ghost)
        SetWindowPos(g_drag.ghost, HWND_TOPMOST, pt.x - g_drag.grab.x, pt.y - g_drag.grab.y, 0, 0,
            SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    if (!g_drag.marker) return;
    int insertAt = 0; POINT marker = {};
    if (TabDropTarget(pt, g_drag.tabId, insertAt, marker))
        SetWindowPos(g_drag.marker, HWND_TOPMOST, marker.x, marker.y, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    else
        ShowWindow(g_drag.marker, SW_HIDE);
}

// Ends the press/drag without dropping anything; also used to cancel one.
static void EndTabDrag()
{
    TabDrag d = g_drag;
    g_drag = {}; // cleared first: releasing capture re-enters via WM_CAPTURECHANGED
    if (d.ghost) DestroyWindow(d.ghost);
    if (d.marker) DestroyWindow(d.marker);
    if (d.btn && GetCapture() == d.btn) ReleaseCapture();
}

static LRESULT CALLBACK TabButtonSubproc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR)
{
    switch (m) {
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK: {
        // Handled here rather than by the button so that a press can turn into a
        // drag. As in Chrome, the tab is selected on press, not on release.
        BrowserWindow* win = WinFromChild(h);
        int idx = GetDlgCtrlID(h) - (int)WM_APP_SWITCH_TAB;
        if (!win || idx < 0 || idx >= (int)win->tabs.size()) break;
        EndTabDrag();
        SetFocus(h);
        SwitchToTab(win, idx);
        g_drag.tabId = win->tabs[idx].id;
        g_drag.btn = h;
        g_drag.grab = { GET_X_LPARAM(l), GET_Y_LPARAM(l) };
        SetCapture(h);
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (g_drag.btn != h) break;
        POINT pt = { GET_X_LPARAM(l), GET_Y_LPARAM(l) };
        if (!g_drag.dragging) {
            if (abs(pt.x - g_drag.grab.x) <= GetSystemMetrics(SM_CXDRAG) &&
                abs(pt.y - g_drag.grab.y) <= GetSystemMetrics(SM_CYDRAG))
                return 0;
            BeginTabDrag();
        }
        ClientToScreen(h, &pt);
        UpdateTabDrag(pt);
        return 0;
    }
    case WM_LBUTTONUP: {
        if (g_drag.btn != h) break;
        TabDrop* drop = nullptr;
        if (g_drag.dragging) {
            drop = new TabDrop{ g_drag.tabId, { GET_X_LPARAM(l), GET_Y_LPARAM(l) }, g_drag.grab };
            ClientToScreen(h, &drop->pt);
        }
        EndTabDrag();
        // Posted, so that the drop never destroys this button from inside its own message.
        if (drop && !PostMessage(GetParent(h), WM_APP_DROP_TAB, 0, (LPARAM)drop)) delete drop;
        return 0;
    }
    case WM_MBUTTONUP: {
        // Middle-click closes the tab. Posted, for the same reason as a drop.
        BrowserWindow* win = WinFromChild(h);
        int idx = GetDlgCtrlID(h) - (int)WM_APP_SWITCH_TAB;
        if (win && idx >= 0 && idx < (int)win->tabs.size())
            PostMessage(win->client, WM_APP_CLOSE_TAB, (WPARAM)win->tabs[idx].id, 0);
        return 0;
    }
    case WM_CAPTURECHANGED:
        if (g_drag.btn == h) EndTabDrag();
        break;
    case WM_NCDESTROY:
        if (g_drag.btn == h) EndTabDrag();
        RemoveWindowSubclass(h, TabButtonSubproc, id);
        break;
    }
    return DefSubclassProc(h, m, w, l);
}

// ---------- webview wiring ----------
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

// Standard WebView2 fullscreen pattern (matches Microsoft's own WebView2Browser
// sample): reparent the active tab's controller into a dedicated, plain,
// borderless top-level window sized to the monitor, instead of resizing or
// restyling the Winamp-owned embed frame. This avoids fighting Winamp's own
// window chrome entirely, so there is no leftover title bar/border and no
// frame-recalculation glitches on the first transition.
static void EnterVideoFullscreen(BrowserWindow* win) {
    if (win->isFullscreen) return;
    int i = win->active;
    if (i < 0 || i >= (int)win->tabs.size() || !win->tabs[i].ctl) return;

    HMONITOR mon = MonitorFromWindow(win->embedFrame, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = { sizeof(MONITORINFO) };
    if (!GetMonitorInfo(mon, &mi)) return;
    int w = mi.rcMonitor.right - mi.rcMonitor.left;
    int h = mi.rcMonitor.bottom - mi.rcMonitor.top;

    // WS_EX_TOOLWINDOW keeps it out of the taskbar/Alt-Tab list, matching how
    // browsers don't add a separate taskbar entry for fullscreen video.
    HWND fsWnd = CreateWindowExW(WS_EX_TOOLWINDOW, L"ExodiumFullscreen", L"",
        WS_POPUP, mi.rcMonitor.left, mi.rcMonitor.top, w, h,
        nullptr, nullptr, g_hInst, nullptr);
    if (!fsWnd) return;

    win->isFullscreen = true;
    win->fsWnd = fsWnd;
    win->fsTabId = win->tabs[i].id;
    SetToolbarControlsVisible(win, false);

    ComPtr<ICoreWebView2Controller> ctl = win->tabs[i].ctl;
    ctl->put_ParentWindow(fsWnd);
    RECT b = { 0, 0, w, h };
    ctl->put_Bounds(b);
    ctl->put_IsVisible(TRUE);
    ctl->NotifyParentWindowPositionChanged();

    ShowWindow(fsWnd, SW_SHOW);
    SetForegroundWindow(fsWnd);
}

static void ExitVideoFullscreen(BrowserWindow* win) {
    if (!win->isFullscreen) return;
    win->isFullscreen = false;

    int i = TabIndexById(win, win->fsTabId);
    if (i >= 0 && win->tabs[i].ctl) {
        ComPtr<ICoreWebView2Controller> ctl = win->tabs[i].ctl;
        ctl->put_ParentWindow(win->client);
        ctl->NotifyParentWindowPositionChanged();
    }
    win->fsTabId = -1;

    if (win->fsWnd) { DestroyWindow(win->fsWnd); win->fsWnd = nullptr; }

    SetToolbarControlsVisible(win, true);
    LayoutToolbarAndTabs(win); // recomputes normal bounds via LayoutActiveWebView
}

// Ends fullscreen from our side (a shortcut, a closing tab) rather than the page's.
// The page is told as well, so it doesn't keep its fullscreen layout inside the tab.
static void LeaveVideoFullscreen(BrowserWindow* win) {
    if (!win->isFullscreen) return;
    int i = TabIndexById(win, win->fsTabId);
    if (i >= 0 && win->tabs[i].web) win->tabs[i].web->ExecuteScript(L"document.exitFullscreen()", nullptr);
    ExitVideoFullscreen(win);
}

static LRESULT CALLBACK FullscreenProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    // Alt+F4 must not destroy this window with a tab's webview still inside it.
    if (msg == WM_CLOSE) {
        for (auto& w : g_windows) if (w->fsWnd == hwnd) { LeaveVideoFullscreen(w.get()); break; }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// Handlers look the tab's window up by id on every event instead of capturing it,
// so they keep working after the tab is dragged to another window.
static void WireTabEvents(BrowserWindow* owner, int tabId) {
    int idx = TabIndexById(owner, tabId);
    if (idx < 0) return;

    ComPtr<ICoreWebView2> web = owner->tabs[idx].web;
    if (!web || !owner->tabs[idx].ctl) return;

    web->add_DocumentTitleChanged(Microsoft::WRL::Callback<ICoreWebView2DocumentTitleChangedEventHandler>(
        [tabId](ICoreWebView2* s, IUnknown*)->HRESULT {
            BrowserWindow* win = WinFromTabId(tabId); if (!win) return S_OK;
            int i = TabIndexById(win, tabId); if (i < 0) return S_OK;
            LPWSTR t = nullptr;
            if (s && SUCCEEDED(s->get_DocumentTitle(&t))) {
                UpdateTabTitle(win, i, t);
                if (t) CoTaskMemFree(t);
            }
            return S_OK;
        }).Get(), nullptr);

    web->add_NavigationStarting(Microsoft::WRL::Callback<ICoreWebView2NavigationStartingEventHandler>(
        [tabId](ICoreWebView2* s, ICoreWebView2NavigationStartingEventArgs* a)->HRESULT {
            BrowserWindow* win = WinFromTabId(tabId);
            if (!s || !a || !win) return S_OK;
            int i = TabIndexById(win, tabId); if (i < 0) return S_OK;

            win->tabs[i].loading = true;
            if (i == win->active) UpdateReloadButton(win);

            LPWSTR uri = nullptr;
            if (SUCCEEDED(a->get_Uri(&uri))) {
                if (i == win->active && !(GetFocus() == win->editUrl && win->editDirty)) {
                    win->settingUrl = true;
                    SetWindowTextW(win->editUrl, uri ? uri : L"");
                    win->settingUrl = false;
                }
                if (uri) CoTaskMemFree(uri);
            }
            return S_OK;
        }).Get(), nullptr);

    web->add_SourceChanged(Microsoft::WRL::Callback<ICoreWebView2SourceChangedEventHandler>(
        [tabId](ICoreWebView2* s, ICoreWebView2SourceChangedEventArgs*)->HRESULT {
            BrowserWindow* win = WinFromTabId(tabId);
            if (!s || !win) return S_OK;
            int i = TabIndexById(win, tabId); if (i < 0 || i != win->active) return S_OK;
            if (GetFocus() == win->editUrl && win->editDirty) return S_OK;

            LPWSTR src = nullptr;
            if (SUCCEEDED(s->get_Source(&src))) {
                win->settingUrl = true;
                SetWindowTextW(win->editUrl, src ? src : L"");
                win->settingUrl = false;
                if (src) CoTaskMemFree(src);
            }
            return S_OK;
        }).Get(), nullptr);

    web->add_NavigationCompleted(Microsoft::WRL::Callback<ICoreWebView2NavigationCompletedEventHandler>(
        [tabId](ICoreWebView2* s, ICoreWebView2NavigationCompletedEventArgs*)->HRESULT {
            BrowserWindow* win = WinFromTabId(tabId); if (!win) return S_OK;
            int i = TabIndexById(win, tabId); if (i < 0) return S_OK;
            if (s) {
                LPWSTR src = nullptr;
                if (SUCCEEDED(s->get_Source(&src))) {
                    std::wstring url = src ? src : L"";
                    win->tabs[i].lastUri = url;
                    AddToHistory(url);
                    if (i == win->active && !(GetFocus() == win->editUrl && win->editDirty)) {
                        win->settingUrl = true;
                        SetWindowTextW(win->editUrl, url.c_str());
                        win->settingUrl = false;
                    }
                    if (src) CoTaskMemFree(src);
                }
            }

            win->tabs[i].loading = false;
            if (i == win->active) UpdateReloadButton(win);
            return S_OK;
        }).Get(), nullptr);

    web->add_NewWindowRequested(Microsoft::WRL::Callback<ICoreWebView2NewWindowRequestedEventHandler>(
        [tabId](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* a)->HRESULT {
            // Popup-based OAuth (Google Identity Services, etc.) hands the caller's
            // script a reference via window.open() and talks to it over
            // window.opener/postMessage. That only works if we give WebView2 a real
            // CoreWebView2 via put_NewWindow — a plain new tab leaves window.open()
            // returning null and breaks those sign-in flows.
            BrowserWindow* win = WinFromTabId(tabId);
            if (!win || !g_env) {
                a->put_Handled(TRUE);
                return S_OK;
            }

            Microsoft::WRL::ComPtr<ICoreWebView2NewWindowRequestedEventArgs> args(a);
            Microsoft::WRL::ComPtr<ICoreWebView2Deferral> deferral;
            args->GetDeferral(&deferral);

            int idx = (int)win->tabs.size();
            win->tabs.push_back(Tab{});
            int newId = g_nextTabId++;
            win->tabs[idx].id = newId;
            win->tabs[idx].title = L"(new tab)";
            win->tabs[idx].creating = true;
            LayoutToolbarAndTabs(win);

            // Ctrl+click opens the link in the background, as browsers do; adding Shift
            // goes to it. The event doesn't say how the link was clicked, but it arrives
            // while the keys are still down.
            BOOL byUser = FALSE; args->get_IsUserInitiated(&byUser);
            bool inBackground = byUser && (GetAsyncKeyState(VK_CONTROL) & 0x8000) && !(GetAsyncKeyState(VK_SHIFT) & 0x8000);
            if (!inBackground) SwitchToTab(win, idx);

            HWND parent = win->client;
            g_env->CreateCoreWebView2Controller(parent,
                Microsoft::WRL::Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                    [newId, parent, args, deferral](HRESULT hr2, ICoreWebView2Controller* ctl)->HRESULT {
                        BrowserWindow* win = WinFromTabId(newId);
                        if (win) win->tabs[TabIndexById(win, newId)].creating = false;
                        if (SUCCEEDED(hr2) && ctl && win) {
                            int i = TabIndexById(win, newId);
                            if (i >= 0) {
                                win->tabs[i].ctl = ctl;
                                ctl->get_CoreWebView2(&win->tabs[i].web);
                                // The tab may have been dragged to another window meanwhile.
                                if (win->client != parent) ctl->put_ParentWindow(win->client);

                                RECT rc; GetClientRect(win->client, &rc);
                                RECT b = { kMargin, kTabBarH + kToolbarH, rc.right - kMargin, rc.bottom - kMargin };
                                ctl->put_Bounds(b);
                                ctl->put_IsVisible(FALSE);

                                WireTabEvents(win, newId);

                                // Must not be navigated before this call — WebView2
                                // auto-navigates it to the requested uri once assigned.
                                args->put_NewWindow(win->tabs[i].web.Get());

                                if (i == win->active) {
                                    LayoutActiveWebView(win);
                                    win->tabs[i].ctl->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
                                }
                            }
                            else {
                                ctl->Close();
                            }
                        }
                        else if (ctl) {
                            ctl->Close();
                        }
                        args->put_Handled(TRUE);
                        deferral->Complete();
                        return S_OK;
                    }).Get());
            return S_OK;
        }).Get(), nullptr);

    ComPtr<ICoreWebView2_8> web8;
    if (SUCCEEDED(web.As(&web8))) {
        web8->add_DownloadStarting(Microsoft::WRL::Callback<ICoreWebView2DownloadStartingEventHandler>(
            [](ICoreWebView2* /*s*/, ICoreWebView2DownloadStartingEventArgs* /*args*/)->HRESULT {
                return S_OK;
            }).Get(), nullptr);
    }

    owner->tabs[idx].ctl->add_AcceleratorKeyPressed(Microsoft::WRL::Callback<ICoreWebView2AcceleratorKeyPressedEventHandler>(
        [tabId](ICoreWebView2Controller*, ICoreWebView2AcceleratorKeyPressedEventArgs* args)->HRESULT {
            UINT vk = 0; args->get_VirtualKey(&vk);
            COREWEBVIEW2_KEY_EVENT_KIND kind; args->get_KeyEventKind(&kind);
            if (kind != COREWEBVIEW2_KEY_EVENT_KIND_KEY_DOWN &&
                kind != COREWEBVIEW2_KEY_EVENT_KIND_SYSTEM_KEY_DOWN)
                return S_OK;

            bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            bool alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
            BrowserWindow* win = WinFromTabId(tabId);
            if (win && HandleGlobalShortcut(win, vk, ctrl, shift, alt)) {
                args->put_Handled(TRUE);
                return S_OK;
            }
            return S_OK;
        }).Get(), nullptr);

    // --- Favicon support: ICoreWebView2_15 ---
    ComPtr<ICoreWebView2_15> web15;
    if (SUCCEEDED(web.As(&web15))) {
        web15->add_FaviconChanged(
            Microsoft::WRL::Callback<ICoreWebView2FaviconChangedEventHandler>(
                [tabId](ICoreWebView2* sender, IUnknown*) -> HRESULT {
                    if (!sender) return S_OK;

                    Microsoft::WRL::ComPtr<ICoreWebView2_15> wv15;
                    if (FAILED(sender->QueryInterface(IID_PPV_ARGS(&wv15)))) return S_OK;

                    return wv15->GetFavicon(
                        COREWEBVIEW2_FAVICON_IMAGE_FORMAT_PNG,
                        Microsoft::WRL::Callback<ICoreWebView2GetFaviconCompletedHandler>(
                            [tabId](HRESULT hr, IStream* iconStream) -> HRESULT {
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

                                BrowserWindow* win = WinFromTabId(tabId);
                                int i = win ? TabIndexById(win, tabId) : -1;
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
            [tabId](ICoreWebView2* sender, IUnknown*) -> HRESULT {
                BrowserWindow* win = WinFromTabId(tabId);
                if (!sender || !win) return S_OK;
                int i = TabIndexById(win, tabId);
                if (i < 0 || i != win->active) return S_OK;

                BOOL fs = FALSE;
                sender->get_ContainsFullScreenElement(&fs);
                if (fs) EnterVideoFullscreen(win);
                else ExitVideoFullscreen(win);
                return S_OK;
            }).Get(), nullptr);
}

// Helper to ensure Environment is created once (shared across all windows)
static void EnsureWebViewEnv(std::function<void()> onReady) {
    if (g_env) {
        onReady();
        return;
    }
    g_envPendingCallbacks.push_back(onReady);
    if (g_envCreating) return;

    g_envCreating = true;
    std::wstring userData = WebViewProfilePath();

    CreateCoreWebView2EnvironmentWithOptions(nullptr, userData.c_str(), nullptr,
        Microsoft::WRL::Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [](HRESULT hr, ICoreWebView2Environment* env)->HRESULT {
                g_envCreating = false;
                if (SUCCEEDED(hr) && env) {
                    g_env = env;
                    for (auto& cb : g_envPendingCallbacks) cb();
                }
                g_envPendingCallbacks.clear();
                return S_OK;
            }).Get());
}

static void CreateWebViewForTab(BrowserWindow* owner, int tabId) {
    int idx = TabIndexById(owner, tabId);
    if (idx < 0 || owner->tabs[idx].ctl || owner->tabs[idx].creating) return;

    EnsureWebViewEnv([tabId]() {
        BrowserWindow* win = WinFromTabId(tabId);
        if (!win || !g_env) return;
        int i = TabIndexById(win, tabId);
        if (i < 0 || win->tabs[i].ctl || win->tabs[i].creating) return;
        win->tabs[i].creating = true;

        HWND parent = win->client;
        g_env->CreateCoreWebView2Controller(parent,
            Microsoft::WRL::Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                [tabId, parent](HRESULT hr2, ICoreWebView2Controller* ctl)->HRESULT {
                    BrowserWindow* win = WinFromTabId(tabId);
                    int i = win ? TabIndexById(win, tabId) : -1;
                    if (i >= 0) win->tabs[i].creating = false;
                    if (FAILED(hr2) || !ctl) return hr2 ? hr2 : E_FAIL;
                    if (i < 0) { ctl->Close(); return S_OK; }

                    win->tabs[i].ctl = ctl;
                    ctl->get_CoreWebView2(&win->tabs[i].web);
                    // The tab may have been dragged to another window while this was pending.
                    if (win->client != parent) ctl->put_ParentWindow(win->client);

                    ComPtr<ICoreWebView2> wv = win->tabs[i].web;
                    ComPtr<ICoreWebView2_13> wv13;
                    if (SUCCEEDED(wv.As(&wv13))) {
                        ComPtr<ICoreWebView2Profile> prof;
                        if (SUCCEEDED(wv13->get_Profile(&prof)) && prof) {
                            ComPtr<ICoreWebView2Profile2> prof2;
                            if (SUCCEEDED(prof.As(&prof2)) && prof2) {
                                prof2->put_PreferredColorScheme(COREWEBVIEW2_PREFERRED_COLOR_SCHEME_DARK);
                            }
                        }
                    }

                    RECT rc; GetClientRect(win->client, &rc);
                    RECT b = { kMargin,kTabBarH + kToolbarH,rc.right - kMargin,rc.bottom - kMargin };
                    ctl->put_Bounds(b);
                    ctl->put_IsVisible(FALSE);

                    WireTabEvents(win, tabId);

                    if (!win->tabs[i].pendingUri.empty()) win->tabs[i].web->Navigate(win->tabs[i].pendingUri.c_str());
                    else win->tabs[i].web->Navigate(L"https://google.com/");

                    if (i == win->active) {
                        LayoutActiveWebView(win);
                        if (win->tabs[i].ctl) win->tabs[i].ctl->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
                    }
                    return S_OK;
                }).Get());
        });
}

// ---------- procs ----------
static LRESULT CALLBACK ClientProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_NCCREATE) {
        CREATESTRUCT* cs = (CREATESTRUCT*)lParam;
        SetWindowLongPtr(hwnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
        return DefWindowProc(hwnd, msg, wParam, lParam);
    }

    BrowserWindow* self = (BrowserWindow*)GetWindowLongPtr(hwnd, GWLP_USERDATA);
    if (!self) return DefWindowProc(hwnd, msg, wParam, lParam);

    switch (msg) {
    case WM_ERASEBKGND: {
        RECT r; GetClientRect(hwnd, &r);
        FillRect((HDC)wParam, &r, g_brBlack ? g_brBlack : (HBRUSH)GetStockObject(BLACK_BRUSH));
        return 1;
    }
    case WM_CTLCOLORSTATIC: {
        HDC dc = (HDC)wParam; SetTextColor(dc, g_colText);
        if ((HWND)lParam == self->tabBar) { SetBkColor(dc, g_colBg); return (LRESULT)(g_brBlack ? g_brBlack : GetStockObject(BLACK_BRUSH)); }
        SetBkColor(dc, g_colPanel); return (LRESULT)(g_brPanel ? g_brPanel : GetStockObject(BLACK_BRUSH));
    }
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORBTN:
    case WM_CTLCOLORLISTBOX: {
        HDC dc = (HDC)wParam; SetTextColor(dc, g_colText);
        if (msg == WM_CTLCOLOREDIT) { SetBkColor(dc, g_colBg); return (LRESULT)g_brEdit; }
        SetBkColor(dc, g_colPanel); return (LRESULT)g_brPanel;
    }
    case WM_DRAWITEM: {
        const DRAWITEMSTRUCT* dis = (const DRAWITEMSTRUCT*)lParam;
        if (!dis) break;
        if (dis->CtlID == IDC_SUGGEST) return FALSE;

        bool isTab = false, isActive = false;
        HICON hIcon = nullptr;
        UINT id = (UINT)GetDlgCtrlID(dis->hwndItem);

        if (id >= (UINT)WM_APP_SWITCH_TAB && id < (UINT)WM_APP_SWITCH_TAB + 0x1000) {
            isTab = true;
            int idx = (int)(id - WM_APP_SWITCH_TAB);
            isActive = (idx == self->active);
            if (idx >= 0 && idx < (int)self->tabs.size()) {
                hIcon = self->tabs[idx].favicon;
            }
        }

        DrawOwnerButton(dis, isTab, isActive, hIcon);
        return TRUE;
    }

    case WM_APP_INIT_WEBVIEW:
        if (self->active >= 0 && self->active < (int)self->tabs.size() && !self->tabs[self->active].ctl)
            CreateWebViewForTab(self, self->tabs[self->active].id);
        return 0;
    case WM_APP_DROP_TAB: {
        std::unique_ptr<TabDrop> drop((TabDrop*)lParam);
        DropDraggedTab(*drop);
        return 0;
    }
    case WM_APP_CLOSE_TAB:
        CloseTab(self, TabIndexById(self, (int)wParam));
        return 0;
    case WM_SIZE:
        LayoutToolbarAndTabs(self);
        if (self->suggestVisible) { int cnt = (int)SendMessageW(self->suggest, LB_GETCOUNT, 0, 0); if (cnt > 0) PositionSuggestBelowEdit(self, cnt); }
        return 0;
    case WM_KEYDOWN: {
        bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        bool alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
        if (wParam == VK_RETURN && GetFocus() == self->editUrl) { self->editDirty = false; NavigateFromUrlBox(self); return 0; }
        if (HandleGlobalShortcut(self, wParam, ctrl, shift, alt)) return 0;
        break;
    }
    case WM_COMMAND: {
        int id = LOWORD(wParam), code = HIWORD(wParam);
        if (id == IDC_URL) {
            if (code == EN_CHANGE) {
                if (!self->settingUrl && GetFocus() == self->editUrl) { self->editDirty = true; wchar_t q[2048] = L""; GetWindowTextW(self->editUrl, q, 2048); PopulateSuggestions(self, q); }
                return 0;
            }
            if (code == EN_KILLFOCUS) {
                self->editDirty = false;
                if (GetFocus() != self->suggest) HideSuggestions(self);
                return 0;
            }
        }
        if (id == IDC_SUGGEST) {
            if (code == LBN_SELCHANGE) { ApplySuggestSelectionToEdit(self); return 0; }
            if (code == LBN_DBLCLK) { CommitSuggestion(self, true); return 0; }
            if (code == LBN_KILLFOCUS) { if (GetFocus() != self->editUrl) HideSuggestions(self); return 0; }
        }
        if (code == BN_CLICKED) {
            if (id == IDC_BACK) {
                if (self->active >= 0 && self->tabs[self->active].web) self->tabs[self->active].web->GoBack();
                return 0;
            }
            if (id == IDC_FWD) {
                if (self->active >= 0 && self->tabs[self->active].web) self->tabs[self->active].web->GoForward();
                return 0;
            }
            if (id == IDC_RELOAD) {
                if (self->active >= 0 && self->tabs[self->active].web) {
                    if (self->tabs[self->active].loading) self->tabs[self->active].web->Stop();
                    else self->tabs[self->active].web->Reload();
                }
                return 0;
            }
            if (id == IDC_CLOSE_TAB) {
                CloseActiveTab(self);
                return 0;
            }
            if (id == IDC_NEW_TAB) {
                NewTab(self);
                return 0;
            }
        }
        if (id >= (int)WM_APP_SWITCH_TAB && id < (int)WM_APP_SWITCH_TAB + 1024) { int idx = id - WM_APP_SWITCH_TAB; if (idx < (int)self->tabs.size()) SwitchToTab(self, idx); return 0; }
        break;
    }
    case WM_CLOSE: ShowWindow(GetParent(hwnd), SW_HIDE); return 0;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

// The Jump to File Extra plugin (gen_jumpex) hooks every embedded frame and, from the
// moment one is activated until it is next clicked, strips the move out of any request
// to reposition it while it is the active window. Tools such as AltSnap move windows
// from outside, so after an Alt+Tab the frame could only be resized down and right:
// growing it up or left needs it to move. The plugin lets the next two position changes
// through once it has seen this message, so sending it ahead of each one keeps the
// frame movable. Nothing else in Winamp acts on it.
static void AllowOutsideMoves(HWND frame) { SendMessageW(frame, WM_USER, 0, 2003); }

static LRESULT CALLBACK FrameProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    BrowserWindow* self = WinFromFrame(hwnd);
    WNDPROC prev = self ? self->prevFrameProc : nullptr;
    switch (msg) {
    case WM_ACTIVATE:
    case WM_ACTIVATEAPP:
    case WM_WINDOWPOSCHANGING:
        AllowOutsideMoves(hwnd);
        break;
    case WM_WINDOWPOSCHANGED:
        if (self && prev) {
            // A minimized frame is a sliver parked far off screen. Winamp would lay the
            // frame's contents out to fit it and remember that as the window's place, so
            // it isn't told. The pages just hear that they are out of sight meanwhile.
            const WINDOWPOS* wp = (const WINDOWPOS*)lParam;
            bool was = self->minimized;
            self->minimized = IsIconic(hwnd) || (!(wp->flags & SWP_NOMOVE) && wp->x <= -32000 && wp->y <= -32000);
            if (self->minimized) {
                if (!was && !self->isFullscreen) HideAllWebViews(self);
                return 0;
            }
            LRESULT r = CallWindowProc(prev, hwnd, msg, wParam, lParam);
            // Winamp only follows the frame's position when it does the moving itself.
            // Without this a move from outside is forgotten when the position is saved.
            if (!(GetWindowLongPtr(hwnd, GWL_STYLE) & WS_CHILD)) GetWindowRect(hwnd, &self->ews.r);
            if (was) LayoutActiveWebView(self);
            return r;
        }
        break;
    case WM_NCCALCSIZE:
        // No system border: the whole window stays client area (see EnableSnapAndMaximize).
        if (!(GetWindowLongPtr(hwnd, GWL_STYLE) & WS_CHILD)) return 0;
        break;
    case WM_GETMINMAXINFO: {
        LRESULT r = prev ? CallWindowProc(prev, hwnd, msg, wParam, lParam) : 0;
        MINMAXINFO* mm = (MINMAXINFO*)lParam;
        MONITORINFO mi{ sizeof(mi) };
        if (mm && GetMonitorInfo(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi)) {
            // Maximized means the work area exactly. Left to itself Windows adds the
            // width of the border the frame doesn't have and hangs its edges off screen.
            mm->ptMaxPosition = { mi.rcWork.left - mi.rcMonitor.left, mi.rcWork.top - mi.rcMonitor.top };
            mm->ptMaxSize = { mi.rcWork.right - mi.rcWork.left, mi.rcWork.bottom - mi.rcWork.top };
            mm->ptMinTrackSize = { 275, 116 }; // the smallest frame Winamp can draw
        }
        return r;
    }
    case WM_LBUTTONDOWN:
        // Winamp moves and sizes the frame by hand, which would leave a maximized one
        // still counted as maximized. It becomes a normal window first: under the cursor
        // for a title bar drag, as Windows does it, and where it is for the resize grip.
        if (IsZoomed(hwnd)) {
            RECT zr; GetWindowRect(hwnd, &zr);
            int x = GET_X_LPARAM(lParam), y = GET_Y_LPARAM(lParam);
            int w = zr.right - zr.left, h = zr.bottom - zr.top;
            WINDOWPLACEMENT wp{ sizeof(wp) };
            if (y < 14 && x < w - 12 && GetWindowPlacement(hwnd, &wp)) {
                int nw = wp.rcNormalPosition.right - wp.rcNormalPosition.left;
                int nh = wp.rcNormalPosition.bottom - wp.rcNormalPosition.top;
                int nx = MulDiv(x, nw, max(1, w)); // the same way along the title bar
                RECT to = { zr.left + x - nx, zr.top, zr.left + x - nx + nw, zr.top + nh };
                RestoreFrameTo(hwnd, to);
                lParam = MAKELPARAM(nx, y);
            }
            else if (x > w - 20 && y > h - 20) RestoreFrameTo(hwnd, zr);
        }
        break;
    case WM_SHOWWINDOW:
        // lParam is zero for a show/hide of this window itself, as opposed to one that
        // follows Winamp being minimized or restored.
        if (self && !lParam && self->menuId != (UINT_PTR)-1) CheckEmbeddedWindowMenus((UINT)self->menuId, wParam ? TRUE : FALSE);
        if (wParam && self && self->client) PostMessage(self->client, WM_APP_INIT_WEBVIEW, 0, 0);
        if (wParam) DetachFromWinampWindows(hwnd); // in case Winamp gave it its owner back
        // Shown from Winamp's menu after being hidden while minimized: come all the way back.
        if (wParam && !lParam && IsIconic(hwnd)) PostMessage(hwnd, WM_SYSCOMMAND, SC_RESTORE, 0);
        break;
    case WM_ERASEBKGND: { RECT r; GetClientRect(hwnd, &r); FillRect((HDC)wParam, &r, g_brBlack ? g_brBlack : (HBRUSH)GetStockObject(BLACK_BRUSH)); return 1; }
    case WM_SIZE: if (self && self->client) { RECT fr; GetClientRect(hwnd, &fr); SetWindowPos(self->client, 0, 8, 24, fr.right - 8, fr.bottom - 24, SWP_NOZORDER | SWP_NOACTIVATE); } break;
    case WM_NCDESTROY: if (self && self->prevFrameProc) { SetWindowLongPtr(hwnd, GWLP_WNDPROC, (LONG_PTR)self->prevFrameProc); self->prevFrameProc = nullptr; } break;
    }
    return prev ? CallWindowProc(prev, hwnd, msg, wParam, lParam) : DefWindowProc(hwnd, msg, wParam, lParam);
}

// ---- Winamp plumbing ----
static BrowserWindow* CreateBrowserWindow(bool show, const RECT* place /*= nullptr*/, bool forDroppedTab /*= false*/)
{
    EnsureGlobals();

    auto up = std::make_unique<BrowserWindow>();
    BrowserWindow* win = up.get();
    g_windows.push_back(std::move(up));

    // Each window gets its own GUID so Winamp/skin engine can tell frames apart.
    if (FAILED(CoCreateGuid(&win->guid))) win->guid = {};
    win->ews = {};
    win->embedFrame = CreateEmbeddedWindow(&win->ews, win->guid);
    if (!win->embedFrame) {
        g_windows.pop_back();
        return nullptr;
    }
    SetWindowTextW(win->embedFrame, L"Exodium");
    win->prevFrameProc = (WNDPROC)SetWindowLongPtr(win->embedFrame, GWLP_WNDPROC, (LONG_PTR)FrameProc);
    EnableTaskbarButton(win->embedFrame);
    DetachFromWinampWindows(win->embedFrame);
    EnableSnapAndMaximize(win->embedFrame);

    if (place) {
        SetWindowPos(win->embedFrame, nullptr, place->left, place->top,
            place->right - place->left, place->bottom - place->top, SWP_NOZORDER | SWP_NOACTIVATE);
    }
    // Cascade additional windows so they don't stack exactly on the first.
    else if (g_windows.size() > 1) {
        RECT wr; GetWindowRect(win->embedFrame, &wr);
        int off = (int)(g_windows.size() - 1) * 30;
        SetWindowPos(win->embedFrame, nullptr, wr.left + off, wr.top + off, 0, 0,
            SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }

    RECT fr; GetClientRect(win->embedFrame, &fr);
    win->client = CreateWindowExW(0, L"ExodiumClient", L"", WS_CHILD | WS_VISIBLE,
        8, 24, fr.right - 8, fr.bottom - 24,
        win->embedFrame, nullptr, g_hInst, win);

    // Initial tab. A window made to receive a dragged-out tab starts empty instead;
    // the caller places and shows it once that tab is in.
    if (!forDroppedTab) {
        win->tabs.push_back(Tab{});
        win->tabs[0].id = g_nextTabId++;
        win->active = 0;
    }
    LayoutToolbarAndTabs(win);

    // Per-window menu entry so each window can be toggled from Winamp.
    win->menuId = (UINT_PTR)SendMessage(plugin.hwndParent, WM_WA_IPC, 0, IPC_REGISTER_LOWORD_COMMAND);
    wchar_t menuTitle[] = L"Exodium";
    AddEmbeddedWindowToMenus(TRUE, (UINT)win->menuId, menuTitle, show ? TRUE : FALSE);
    if (forDroppedTab) return win;

    if (show) ShowWindow(win->embedFrame, SW_SHOWNA);
    PostMessage(win->client, WM_APP_INIT_WEBVIEW, 0, 0);
    return win;
}

static void DestroyBrowserWindow(BrowserWindow* win)
{
    if (win->fsWnd) { DestroyWindow(win->fsWnd); win->fsWnd = nullptr; }

    // A maximized or minimized window is remembered at the size it goes back to.
    WINDOWPLACEMENT wp{ sizeof(wp) };
    if (win->embedFrame && (IsZoomed(win->embedFrame) || IsIconic(win->embedFrame)) && GetWindowPlacement(win->embedFrame, &wp))
        win->ews.r = wp.rcNormalPosition;

    for (auto& t : win->tabs) {
        if (t.ctl) t.ctl->Close();
        t.web.Reset();
        if (t.btn) DestroyWindow(t.btn);
        if (t.favicon) { DestroyIcon(t.favicon); t.favicon = nullptr; }
    }
    win->tabs.clear();

    if (win->suggest) { DestroyWindow(win->suggest); win->suggest = nullptr; }
    if (win->client) { DestroyWindow(win->client); win->client = nullptr; }

    AddEmbeddedWindowToMenus(FALSE, (UINT)win->menuId, nullptr, FALSE);

    if (win->embedFrame) {
        if (win->prevFrameProc) SetWindowLongPtr(win->embedFrame, GWLP_WNDPROC, (LONG_PTR)win->prevFrameProc);
        DestroyWindow(win->embedFrame); win->embedFrame = nullptr;
    }
    DestroyEmbeddedWindow(&win->ews);
}

static int  __cdecl init();
static void __cdecl quit();
static void __cdecl config();

winampGeneralPurposePlugin plugin = {
  GPPHDR_VER, (char*)"Exodium", init, config, quit, 0, 0
};
extern "C" __declspec(dllexport) winampGeneralPurposePlugin* winampGetGeneralPurposePlugin() { return &plugin; }

static int __cdecl init()
{
    g_hInst = (HINSTANCE)plugin.hDllInstance;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    Gdiplus::GdiplusStartupInput gdiplusStartupInput;
    Gdiplus::GdiplusStartup(&g_gdiplusToken, &gdiplusStartupInput, nullptr);

    ini_file = (wchar_t*)SendMessage(plugin.hwndParent, WM_WA_IPC, 0, IPC_GETINIFILEW);

    EnsureGlobals();
    HookWinampParent();
    CreateBrowserWindow(false);  // first window, hidden until shown via menu/config

    return 0;
}

static void __cdecl config()
{
    // Show (or create) the first window.
    if (g_windows.empty()) { CreateBrowserWindow(true); return; }
    BrowserWindow* win = g_windows.front().get();
    if (win->embedFrame) ShowWindow(win->embedFrame, SW_SHOWNA);
    if (win->client) PostMessage(win->client, WM_APP_INIT_WEBVIEW, 0, 0);
}

static void __cdecl quit()
{
    SaveHistory();

    for (auto& w : g_windows) DestroyBrowserWindow(w.get());
    g_windows.clear();

    if (g_env) { g_env.Reset(); }

    if (g_oldWaProc) {
        SetWindowLongPtr(plugin.hwndParent, GWLP_WNDPROC, (LONG_PTR)g_oldWaProc); g_oldWaProc = nullptr;
    }

    if (g_brBlack) { DeleteObject(g_brBlack); g_brBlack = nullptr; }
    if (g_brPanel) { DeleteObject(g_brPanel); g_brPanel = nullptr; }
    if (g_brEdit) { DeleteObject(g_brEdit);  g_brEdit = nullptr; }
    if (g_gdiplusToken) {
        Gdiplus::GdiplusShutdown(g_gdiplusToken);
        g_gdiplusToken = 0;
    }
    CoUninitialize();
}
