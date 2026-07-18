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

#include "gen.h"
#include "wa_ipc.h"
#include "embedwnd.h"
#include <gdiplus.h>
#pragma comment(lib, "gdiplus.lib")

using namespace Gdiplus;
using Microsoft::WRL::ComPtr;

// FIX: LNK2001 error (unresolved external symbol "wchar_t * ini_file")
// The embedwnd utility requires this global variable from the Winamp SDK context.
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
static const int  kBtnW = 64, kBtnGap = 6, kMargin = 6;
static const UINT WM_APP_INIT_WEBVIEW = WM_APP + 42;
static const UINT WM_APP_SWITCH_TAB = WM_APP + 43;

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
    bool isLoading = false;

    HWND suggest = nullptr;
    bool suggestVisible = false;

    bool editDirty = false;
    bool settingUrl = false;

    std::vector<Tab> tabs;
    int active = -1;
    int nextTabId = 1;

    embedWindowState ews = { 0 };
    UINT_PTR menuId = (UINT_PTR)-1;
    GUID guid = {};

    bool isFullscreen = false;
    HWND fsWnd = nullptr;      // dedicated borderless top-level window used while a tab is fullscreen
    int fsTabId = -1;          // id of the tab whose controller is parented into fsWnd
};

static std::vector<std::unique_ptr<BrowserWindow>> g_windows;

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
static bool WinAlive(BrowserWindow* win) {
    for (auto& w : g_windows) if (w.get() == win) return true;
    return false;
}
static int TabIndexById(BrowserWindow* win, int id) {
    for (int i = 0; i < (int)win->tabs.size(); ++i) if (win->tabs[i].id == id) return i;
    return -1;
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
static BrowserWindow* CreateBrowserWindow(bool show);
static void EnterVideoFullscreen(BrowserWindow* win);
static void ExitVideoFullscreen(BrowserWindow* win);

static void FocusAddressBar(BrowserWindow* win) { SetFocus(win->editUrl); SendMessageW(win->editUrl, EM_SETSEL, 0, -1); }
static std::wstring GetActiveUrl(BrowserWindow* win) {
    if (win->active < 0 || win->active >= (int)win->tabs.size() || !win->tabs[win->active].web) return L"";
    LPWSTR uri = nullptr; std::wstring out; if (SUCCEEDED(win->tabs[win->active].web->get_Source(&uri))) { if (uri) { out = uri; CoTaskMemFree(uri); } } return out;
}
static void UpdateUrlBox(BrowserWindow* win) {
    if (GetFocus() == win->editUrl && win->editDirty) return;
    if (win->active < 0 || win->active >= (int)win->tabs.size() || !win->tabs[win->active].web) return;
    LPWSTR uri = nullptr;
    if (SUCCEEDED(win->tabs[win->active].web->get_Source(&uri))) {
        win->settingUrl = true; SetWindowTextW(win->editUrl, uri ? uri : L""); win->settingUrl = false;
        if (uri) CoTaskMemFree(uri);
    }
}
static void UpdateReloadButton(BrowserWindow* win)
{
    if (!win->btnReload) return;
    SetWindowTextW(win->btnReload, win->isLoading ? L"⏹" : L"↺");
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

    if (ctrl && !shift && !alt && w == 'N') { CreateBrowserWindow(true); return true; }

    if (ctrl && shift && !alt && w == 'T') { ReopenLastClosedTab(win); return true; }

    if (!win) return false;

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
    wcfs.lpfnWndProc = DefWindowProcW;
    wcfs.hInstance = g_hInst;
    wcfs.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wcfs.lpszClassName = L"ExodiumFullscreen";
    RegisterClassW(&wcfs);

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
static LRESULT CALLBACK SuggestSubproc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR)
{
    BrowserWindow* win = WinFromChild(h);
    switch (m)
    {
    case WM_GETDLGCODE:
        return DLGC_WANTALLKEYS | DLGC_WANTCHARS;

    case WM_CHAR:
    {
        if (GetKeyState(VK_CONTROL) & 0x8000)
            return 0;

        wchar_t ch = static_cast<wchar_t>(w);

        if (ch == L'\r') {
            if (win) CommitSuggestion(win, true);
            return 0;
        }
        if (ch == L'\x1b') {
            if (win) { HideSuggestions(win); if (win->editUrl) SetFocus(win->editUrl); }
            return 0;
        }
        break;
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
            if (win) { HideSuggestions(win); if (win->editUrl) SetFocus(win->editUrl); }
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
    {
        if (GetKeyState(VK_CONTROL) & 0x8000)
            return 0;

        if (w == L'\r') {
            if (win) { win->editDirty = false; NavigateFromUrlBox(win); }
            return 0;
        }
        break;
    }
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

    int x = kMargin; const int tabH = kTabBarH - 2, tabW = 140;
    for (int i = 0; i < (int)win->tabs.size(); ++i) {
        if (!win->tabs[i].btn) {
            win->tabs[i].btn = CreateWindowExW(0, L"BUTTON", win->tabs[i].title.empty() ? L"(new tab)" : win->tabs[i].title.c_str(),
                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 0, 0, 0, 0, win->client, (HMENU)(INT_PTR)(WM_APP_SWITCH_TAB + i), g_hInst, nullptr);
            SendMessageW(win->tabs[i].btn, WM_SETFONT, (WPARAM)(g_font ? g_font : GetStockObject(DEFAULT_GUI_FONT)), TRUE);
            SubclassCtrlCommon(win->tabs[i].btn);
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

    MoveWindow(win->btnBack, tX, y, kBtnW, h, TRUE); tX += kBtnW + kBtnGap;
    MoveWindow(win->btnFwd, tX, y, kBtnW, h, TRUE); tX += kBtnW + kBtnGap;
    MoveWindow(win->btnReload, tX, y, kBtnW, h, TRUE); tX += kBtnW + kBtnGap;
    MoveWindow(win->btnCloseTab, tX, y, kBtnW, h, TRUE); tX += kBtnW + kBtnGap;

    int urlW = rc.right - tX - kMargin;
    if (urlW < 120) urlW = 120;
    MoveWindow(win->editUrl, tX, y, urlW, h, TRUE);
    EnsureSuggestListbox(win); HideSuggestions(win);

    LayoutActiveWebView(win);
}

// ---------- tab ops ----------
static void UpdateTabTitle(BrowserWindow* win, int idx, const wchar_t* t) {
    if (idx < 0 || idx >= (int)win->tabs.size()) return;
    win->tabs[idx].title = t ? t : L"(tab)";
    if (win->tabs[idx].btn) SetWindowTextW(win->tabs[idx].btn, win->tabs[idx].title.c_str());
}
static void SwitchToTab(BrowserWindow* win, int idx) {
    if (win->isFullscreen) return; // the active tab's controller is parented elsewhere right now
    if (idx < 0 || idx >= (int)win->tabs.size()) return;
    if (win->active == idx) return;

    HideAllWebViews(win);
    win->active = idx;
    LayoutActiveWebView(win);
    UpdateUrlBox(win);
    HideSuggestions(win);

    for (int i = 0; i < (int)win->tabs.size(); ++i)
        if (win->tabs[i].btn) SendMessageW(win->tabs[i].btn, BM_SETSTATE, (WPARAM)(i == win->active), 0);

    if (win->tabs[win->active].ctl) {
        win->tabs[win->active].ctl->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
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
    win->tabs[idx].id = win->nextTabId++;
    win->tabs[idx].title = L"(new tab)";
    win->tabs[idx].pendingUri = url ? url : L"";
    LayoutToolbarAndTabs(win);
    SwitchToTab(win, idx);
    PostMessage(win->client, WM_APP_INIT_WEBVIEW, 0, 0);
}
static void CloseTab(BrowserWindow* win, int idx) {
    if (idx < 0 || idx >= (int)win->tabs.size()) return;

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
        win->active = -1;
        NewTab(win, L"https://google.com/");
        return;
    }
    else if (win->active >= idx) {
        win->active = max(0, win->active - 1);
    }

    LayoutToolbarAndTabs(win);
    // Force a full refresh of the active tab's visuals since indices shifted.
    int cur = win->active; win->active = -1; SwitchToTab(win, cur);
}
static void CloseActiveTab(BrowserWindow* win) { CloseTab(win, win->active); }

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

static void WireTabEvents(BrowserWindow* win, int tabId) {
    int idx = TabIndexById(win, tabId);
    if (idx < 0) return;

    ComPtr<ICoreWebView2> web = win->tabs[idx].web;
    if (!web || !win->tabs[idx].ctl) return;

    web->add_DocumentTitleChanged(Microsoft::WRL::Callback<ICoreWebView2DocumentTitleChangedEventHandler>(
        [win, tabId](ICoreWebView2* s, IUnknown*)->HRESULT {
            if (!WinAlive(win)) return S_OK;
            int i = TabIndexById(win, tabId); if (i < 0) return S_OK;
            LPWSTR t = nullptr;
            if (s && SUCCEEDED(s->get_DocumentTitle(&t))) {
                UpdateTabTitle(win, i, t);
                if (t) CoTaskMemFree(t);
            }
            return S_OK;
        }).Get(), nullptr);

    web->add_NavigationStarting(Microsoft::WRL::Callback<ICoreWebView2NavigationStartingEventHandler>(
        [win, tabId](ICoreWebView2* s, ICoreWebView2NavigationStartingEventArgs* a)->HRESULT {
            if (!s || !a || !WinAlive(win)) return S_OK;
            int i = TabIndexById(win, tabId); if (i < 0) return S_OK;

            if (i == win->active) {
                win->isLoading = true;
                UpdateReloadButton(win);
            }

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
        [win, tabId](ICoreWebView2* s, ICoreWebView2SourceChangedEventArgs*)->HRESULT {
            if (!s || !WinAlive(win)) return S_OK;
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
        [win, tabId](ICoreWebView2* s, ICoreWebView2NavigationCompletedEventArgs*)->HRESULT {
            if (!WinAlive(win)) return S_OK;
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

            if (i == win->active) {
                win->isLoading = false;
                UpdateReloadButton(win);
            }
            return S_OK;
        }).Get(), nullptr);

    web->add_NewWindowRequested(Microsoft::WRL::Callback<ICoreWebView2NewWindowRequestedEventHandler>(
        [win](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* a)->HRESULT {
            LPWSTR uri = nullptr;
            a->get_Uri(&uri);
            if (WinAlive(win)) NewTab(win, uri ? uri : L"about:blank");
            if (uri) CoTaskMemFree(uri);
            a->put_Handled(TRUE);
            return S_OK;
        }).Get(), nullptr);

    ComPtr<ICoreWebView2_8> web8;
    if (SUCCEEDED(web.As(&web8))) {
        web8->add_DownloadStarting(Microsoft::WRL::Callback<ICoreWebView2DownloadStartingEventHandler>(
            [](ICoreWebView2* /*s*/, ICoreWebView2DownloadStartingEventArgs* /*args*/)->HRESULT {
                return S_OK;
            }).Get(), nullptr);
    }

    win->tabs[idx].ctl->add_AcceleratorKeyPressed(Microsoft::WRL::Callback<ICoreWebView2AcceleratorKeyPressedEventHandler>(
        [win](ICoreWebView2Controller*, ICoreWebView2AcceleratorKeyPressedEventArgs* args)->HRESULT {
            UINT vk = 0; args->get_VirtualKey(&vk);
            COREWEBVIEW2_KEY_EVENT_KIND kind; args->get_KeyEventKind(&kind);
            if (kind != COREWEBVIEW2_KEY_EVENT_KIND_KEY_DOWN &&
                kind != COREWEBVIEW2_KEY_EVENT_KIND_SYSTEM_KEY_DOWN)
                return S_OK;

            bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            bool alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
            if (WinAlive(win) && HandleGlobalShortcut(win, vk, ctrl, shift, alt)) {
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

static void CreateWebViewForTab(BrowserWindow* win, int tabId) {
    int idx = TabIndexById(win, tabId);
    if (idx < 0 || win->tabs[idx].ctl) return;

    EnsureWebViewEnv([win, tabId]() {
        if (!WinAlive(win) || !g_env) return;
        int i = TabIndexById(win, tabId);
        if (i < 0 || win->tabs[i].ctl) return;

        g_env->CreateCoreWebView2Controller(win->client,
            Microsoft::WRL::Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                [win, tabId](HRESULT hr2, ICoreWebView2Controller* ctl)->HRESULT {
                    if (FAILED(hr2) || !ctl) return hr2 ? hr2 : E_FAIL;
                    if (!WinAlive(win)) { ctl->Close(); return S_OK; }
                    int i = TabIndexById(win, tabId);
                    if (i < 0) { ctl->Close(); return S_OK; }

                    win->tabs[i].ctl = ctl;
                    ctl->get_CoreWebView2(&win->tabs[i].web);

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
            if (code == EN_KILLFOCUS) { self->editDirty = false; return 0; }
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
                    if (self->isLoading) self->tabs[self->active].web->Stop();
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

static LRESULT CALLBACK FrameProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    BrowserWindow* self = WinFromFrame(hwnd);
    WNDPROC prev = self ? self->prevFrameProc : nullptr;
    switch (msg) {
    case WM_SHOWWINDOW: if (wParam && self && self->client) PostMessage(self->client, WM_APP_INIT_WEBVIEW, 0, 0); break;
    case WM_ERASEBKGND: { RECT r; GetClientRect(hwnd, &r); FillRect((HDC)wParam, &r, g_brBlack ? g_brBlack : (HBRUSH)GetStockObject(BLACK_BRUSH)); return 1; }
    case WM_SIZE: if (self && self->client) { RECT fr; GetClientRect(hwnd, &fr); SetWindowPos(self->client, 0, 8, 24, fr.right - 8, fr.bottom - 24, SWP_NOZORDER | SWP_NOACTIVATE); } break;
    case WM_NCDESTROY: if (self && self->prevFrameProc) { SetWindowLongPtr(hwnd, GWLP_WNDPROC, (LONG_PTR)self->prevFrameProc); self->prevFrameProc = nullptr; } break;
    }
    return prev ? CallWindowProc(prev, hwnd, msg, wParam, lParam) : DefWindowProc(hwnd, msg, wParam, lParam);
}

// ---- Winamp plumbing ----
static BrowserWindow* CreateBrowserWindow(bool show)
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

    // Cascade additional windows so they don't stack exactly on the first.
    if (g_windows.size() > 1) {
        RECT wr; GetWindowRect(win->embedFrame, &wr);
        int off = (int)(g_windows.size() - 1) * 30;
        SetWindowPos(win->embedFrame, nullptr, wr.left + off, wr.top + off, 0, 0,
            SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }

    RECT fr; GetClientRect(win->embedFrame, &fr);
    win->client = CreateWindowExW(0, L"ExodiumClient", L"", WS_CHILD | WS_VISIBLE,
        8, 24, fr.right - 8, fr.bottom - 24,
        win->embedFrame, nullptr, g_hInst, win);

    // Initial tab
    win->tabs.push_back(Tab{});
    win->tabs[0].id = win->nextTabId++;
    win->active = 0;
    LayoutToolbarAndTabs(win);

    // Per-window menu entry so each window can be toggled from Winamp.
    win->menuId = (UINT_PTR)SendMessage(plugin.hwndParent, WM_WA_IPC, 0, IPC_REGISTER_LOWORD_COMMAND);
    wchar_t menuTitle[] = L"Exodium";
    AddEmbeddedWindowToMenus(TRUE, (UINT)win->menuId, menuTitle, show ? TRUE : FALSE);

    if (show) ShowWindow(win->embedFrame, SW_SHOWNA);
    PostMessage(win->client, WM_APP_INIT_WEBVIEW, 0, 0);
    return win;
}

static void DestroyBrowserWindow(BrowserWindow* win)
{
    if (win->fsWnd) { DestroyWindow(win->fsWnd); win->fsWnd = nullptr; }

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
