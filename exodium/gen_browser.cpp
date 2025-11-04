// STAGE 9c: Dark mode + tabs + SPA URL sync + suggestions + PERSISTENT history
// + Ctrl+L hotkey + rock-solid Enter-to-navigate from URL bar
// If your project uses PCH, keep first:
// #include "pch.h"

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
#include <algorithm>
#include <cstdio>
#include "WebView2.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shlwapi.lib")

#include "gen.h"
#include "wa_ipc.h"
#include "embedwnd.h"

using Microsoft::WRL::ComPtr;

// ---- IDs / layout ----
enum : int {
    IDC_BACK = 1001, IDC_FWD = 1002, IDC_URL = 1003, IDC_GO = 1004,
    IDC_TABBAR = 1100, IDC_NEW_TAB = 1101,
    IDC_SUGGEST = 1200,
};
static const int  kTabBarH = 26, kToolbarH = 32;
static const int  kBtnW = 64, kBtnGap = 6, kMargin = 6;
static const UINT WM_APP_INIT_WEBVIEW = WM_APP + 42;
static const UINT WM_APP_SWITCH_TAB = WM_APP + 43;

// Hotkey id for Ctrl+L
static const int HK_CTRLL = 0xC001;

static HINSTANCE g_hInst = nullptr;
static HWND g_embedFrame = nullptr;
static WNDPROC g_prevFrameProc = nullptr;
static HWND g_client = nullptr;

static HWND g_tabBar = nullptr;
static HWND g_newTabBtn = nullptr;
static HWND g_btnBack = nullptr, g_btnFwd = nullptr, g_btnGo = nullptr, g_editUrl = nullptr;

// suggestions
static HWND g_suggest = nullptr;
static bool g_suggestVisible = false;

// edit state
static bool g_editDirty = false;   // user is typing
static bool g_settingUrl = false;  // programmatic SetWindowText guard

struct Tab {
    ComPtr<ICoreWebView2Controller> ctl;
    ComPtr<ICoreWebView2> web;
    HWND btn = nullptr;
    std::wstring title;
    std::wstring pendingUri;
    std::wstring lastUri;
};
static std::vector<Tab> g_tabs;
static int g_active = -1;

// persisted MRU history
static std::vector<std::wstring> g_history;

static embedWindowState g_ews = { 0 };
static const GUID kEmbedGuid = { 0x3f8b13b9,0x2bd9,0x4b3c,{0x94,0x5a,0x2a,0x8d,0xc7,0x60,0x3e,0x11} };

// ---- Dark resources ----
static HBRUSH g_brBlack = nullptr, g_brPanel = nullptr, g_brEdit = nullptr;
static COLORREF g_colBg = RGB(0, 0, 0), g_colPanel = RGB(16, 16, 16), g_colBtnActive = RGB(32, 32, 32), g_colText = RGB(255, 255, 255);
static HFONT g_font = nullptr;

// ---------- persistence ----------
static void ModuleDir(wchar_t* out, size_t cch) { out[0] = 0; if (!GetModuleFileNameW((HINSTANCE)g_hInst, out, (DWORD)cch))return; PathRemoveFileSpecW(out); }
static std::wstring DataDirPath() { wchar_t d[MAX_PATH]; ModuleDir(d, MAX_PATH); std::wstring p = d; p += L"\\exodium_data"; CreateDirectoryW(p.c_str(), nullptr); return p; }
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
static void FocusAddressBar() { SetFocus(g_editUrl); SendMessageW(g_editUrl, EM_SETSEL, 0, -1); }
static std::wstring GetActiveUrl() {
    if (g_active < 0 || g_active >= (int)g_tabs.size() || !g_tabs[g_active].web) return L"";
    LPWSTR uri = nullptr; std::wstring out; if (SUCCEEDED(g_tabs[g_active].web->get_Source(&uri))) { if (uri) { out = uri; CoTaskMemFree(uri); } } return out;
}
static void UpdateUrlBox() {
    if (GetFocus() == g_editUrl && g_editDirty) return;
    if (g_active < 0 || g_active >= (int)g_tabs.size() || !g_tabs[g_active].web) return;
    LPWSTR uri = nullptr;
    if (SUCCEEDED(g_tabs[g_active].web->get_Source(&uri))) {
        g_settingUrl = true; SetWindowTextW(g_editUrl, uri ? uri : L""); g_settingUrl = false;
        if (uri) CoTaskMemFree(uri);
    }
}

// Forward decls
static void LayoutToolbarAndTabs();
static void SwitchToTab(int idx);
static void NavigateFromUrlBox();
static void NextTab();
static void PrevTab();
static void CloseActiveTab();
static void SubclassCtrlCommon(HWND h);
static void NewTab(const wchar_t* url = L"https://google.com/");


static bool HandleGlobalShortcut(WPARAM w, bool ctrl, bool shift, bool alt) {
    // Ctrl+L: focus URL bar
    if (ctrl && !shift && !alt && w == 'L') { FocusAddressBar(); g_editDirty = true; return true; }

    // Ctrl+T / Ctrl+W: new/close tab
    if (ctrl && !alt && !shift && w == 'T') { NewTab(); return true; }
    if (ctrl && !alt && !shift && w == 'W') { CloseActiveTab(); return true; }

    // Ctrl+Tab / Ctrl+Shift+Tab: next/prev tab
    if (ctrl && !alt && w == VK_TAB) { if (shift) PrevTab(); else NextTab(); return true; }

    // Alt+Left/Right: browser nav
    if (!ctrl && !shift && alt && w == VK_LEFT && g_active >= 0 && g_tabs[g_active].web) { g_tabs[g_active].web->GoBack();    return true; }
    if (!ctrl && !shift && alt && w == VK_RIGHT && g_active >= 0 && g_tabs[g_active].web) { g_tabs[g_active].web->GoForward(); return true; }

    return false;
}
// ---------- owner draw ----------
static void DrawOwnerButton(const DRAWITEMSTRUCT* dis, bool isTab, bool isActive) {
    HDC dc = dis->hDC; RECT r = dis->rcItem;
    COLORREF bg = isTab ? (isActive ? g_colBtnActive : g_colPanel) : g_colPanel;
    HBRUSH br = CreateSolidBrush(bg); FillRect(dc, &r, br); DeleteObject(br);
    HPEN pen = CreatePen(PS_SOLID, 1, RGB(60, 60, 60)); HPEN old = (HPEN)SelectObject(dc, pen);
    MoveToEx(dc, r.left, r.top, nullptr); LineTo(dc, r.right - 1, r.top); LineTo(dc, r.right - 1, r.bottom - 1);
    LineTo(dc, r.left, r.bottom - 1); LineTo(dc, r.left, r.top); SelectObject(dc, old); DeleteObject(pen);
    wchar_t text[256]; GetWindowTextW(dis->hwndItem, text, ARRAYSIZE(text));
    SetBkMode(dc, TRANSPARENT); SetTextColor(dc, g_colText);
    HFONT oldf = (HFONT)SelectObject(dc, g_font ? g_font : (HFONT)GetStockObject(DEFAULT_GUI_FONT));
    DrawTextW(dc, text, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    SelectObject(dc, oldf);
    if (dis->itemState & ODS_FOCUS) { RECT rf = r; InflateRect(&rf, -3, -3); DrawFocusRect(dc, &rf); }
}

// ---------- suggestions ----------
static void HideSuggestions() { if (g_suggest) ShowWindow(g_suggest, SW_HIDE); g_suggestVisible = false; }
static void EnsureSuggestListbox() {
    if (!g_suggest) {
        g_suggest = CreateWindowExW(WS_EX_TOOLWINDOW, L"LISTBOX", L"", WS_CHILD | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT | WS_VSCROLL | WS_BORDER,
            0, 0, 0, 0, g_client, (HMENU)IDC_SUGGEST, g_hInst, nullptr);
        SendMessageW(g_suggest, WM_SETFONT, (WPARAM)g_font, TRUE);
        SubclassCtrlCommon(g_suggest); // <-- important
    }
}
static void PositionSuggestBelowEdit(int items) {
    RECT er; GetWindowRect(g_editUrl, &er);
    POINT tl{ er.left,er.bottom }, br{ er.right,er.bottom };
    ScreenToClient(g_client, &tl); ScreenToClient(g_client, &br);
    int itemH = (int)SendMessageW(g_suggest, LB_GETITEMHEIGHT, 0, 0); if (itemH <= 0) itemH = 18;
    int h = min(items, 8) * itemH + 2, w = (br.x - tl.x);
    SetWindowPos(g_suggest, HWND_TOP, tl.x, tl.y, w, h, SWP_SHOWWINDOW);
}
static void PopulateSuggestions(const std::wstring& q) {
    EnsureSuggestListbox(); SendMessageW(g_suggest, LB_RESETCONTENT, 0, 0);
    if (q.size() < 2) { HideSuggestions(); return; }
    auto wlower = [](std::wstring s) { for (auto& c : s) c = (wchar_t)towlower(c); return s; };
    std::wstring ql = wlower(q); std::vector<std::wstring> matches;
    auto add_if = [&](const std::wstring& s) {
        if (matches.size() >= 64) return;
        if (std::any_of(matches.begin(), matches.end(), [&](const std::wstring& x) {return _wcsicmp(x.c_str(), s.c_str()) == 0; })) return;
        matches.push_back(s);
        };
    for (const auto& u : g_history) { std::wstring ul = wlower(u); if (ul.find(ql) == 0) add_if(u); }
    for (const auto& u : g_history) { std::wstring ul = wlower(u); if (!ul.empty() && ul.find(ql) != 0 && ul.find(ql) != std::wstring::npos) add_if(u); }
    if (matches.empty()) { HideSuggestions(); return; }
    for (const auto& s : matches) SendMessageW(g_suggest, LB_ADDSTRING, 0, (LPARAM)s.c_str());
    SendMessageW(g_suggest, LB_SETCURSEL, 0, 0); PositionSuggestBelowEdit((int)matches.size()); g_suggestVisible = true;
}
static void CommitSuggestion(bool navigate) {
    if (!g_suggestVisible) return;
    LRESULT sel = SendMessageW(g_suggest, LB_GETCURSEL, 0, 0); if (sel == LB_ERR) return;
    wchar_t buf[2048] = L""; SendMessageW(g_suggest, LB_GETTEXT, (WPARAM)sel, (LPARAM)buf);
    g_settingUrl = true; SetWindowTextW(g_editUrl, buf); g_settingUrl = false;
    HideSuggestions(); SetFocus(g_editUrl); SendMessageW(g_editUrl, EM_SETSEL, 0, -1);
    if (navigate) { g_editDirty = false; NavigateFromUrlBox(); }
}

// ---------- navigation ----------
static void NavigateFromUrlBox() {
    if (g_active < 0 || g_active >= (int)g_tabs.size()) return;
    wchar_t url[2048] = L""; GetWindowTextW(g_editUrl, url, 2048); if (!url[0]) return;
    if (!PathIsURLW(url)) { wchar_t buf[2048]; wsprintfW(buf, L"https://%s", url); lstrcpynW(url, buf, 2048); }
    g_editDirty = false; HideSuggestions();
    if (g_tabs[g_active].web) g_tabs[g_active].web->Navigate(url);
    else { g_tabs[g_active].pendingUri = url; PostMessage(g_client, WM_APP_INIT_WEBVIEW, 0, 0); }
}

// ---------- edit subclass ----------
static LRESULT CALLBACK EditSubproc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR) {
    switch (m) {
    case WM_GETDLGCODE:
        // Tell Windows we want Enter/keys inside the edit (prevents default button from eating Enter)
        return DLGC_WANTALLKEYS | DLGC_WANTCHARS;
    case WM_KEYDOWN: {
        bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        bool alt = (GetKeyState(VK_MENU) & 0x8000) != 0;

        if (HandleGlobalShortcut((WPARAM)w, ctrl, shift, alt)) return 0;

        // (then your control-specific stuff)
        if (ctrl && w == 'L') { FocusAddressBar(); g_editDirty = true; return 0; }
        if (w == VK_DOWN && g_suggestVisible) { SetFocus(g_suggest); SendMessageW(g_suggest, LB_SETCURSEL, 0, 0); return 0; }
        if (w == VK_ESCAPE && g_suggestVisible) { HideSuggestions(); return 0; }
        if (w == VK_RETURN) { g_editDirty = false; NavigateFromUrlBox(); return 0; }
        break;
    }
    case WM_CHAR:
        if (w == L'\r') { g_editDirty = false; NavigateFromUrlBox(); return 0; }
        break;
    }
    return DefSubclassProc(h, m, w, l);
}
static void SubclassCtrlCommon(HWND h) { if (h) SetWindowSubclass(h, EditSubproc, 1, 0); }

// ---------- layout ----------
static void LayoutActiveWebView() {
    if (g_active < 0 || g_active >= (int)g_tabs.size()) return;
    if (!g_tabs[g_active].ctl) return;
    RECT rc; GetClientRect(g_client, &rc);
    int top = kTabBarH + kToolbarH; RECT b = { kMargin,top,rc.right - kMargin,rc.bottom - kMargin };
    g_tabs[g_active].ctl->put_Bounds(b); g_tabs[g_active].ctl->put_IsVisible(TRUE);
}
static void HideAllWebViews() { for (auto& t : g_tabs) if (t.ctl) t.ctl->put_IsVisible(FALSE); }

static void LayoutToolbarAndTabs() {
    if (!g_client) return;
    RECT rc; GetClientRect(g_client, &rc);
    if (!g_tabBar) g_tabBar = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, g_client, (HMENU)IDC_TABBAR, g_hInst, nullptr);
    MoveWindow(g_tabBar, 0, 0, rc.right, kTabBarH, TRUE);

    int x = kMargin; const int tabH = kTabBarH - 2, tabW = 140;
    for (int i = 0; i < (int)g_tabs.size(); ++i) {
        if (!g_tabs[i].btn) {
            g_tabs[i].btn = CreateWindowExW(0, L"BUTTON", g_tabs[i].title.empty() ? L"(new tab)" : g_tabs[i].title.c_str(),
                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 0, 0, 0, 0, g_client, (HMENU)(INT_PTR)(WM_APP_SWITCH_TAB + i), g_hInst, nullptr);
            SendMessageW(g_tabs[i].btn, WM_SETFONT, (WPARAM)(g_font ? g_font : GetStockObject(DEFAULT_GUI_FONT)), TRUE);
            SubclassCtrlCommon(g_tabs[i].btn);
        }
        else {
            SetWindowLongPtrW(g_tabs[i].btn, GWLP_ID, (LONG_PTR)(WM_APP_SWITCH_TAB + i));
        }
        MoveWindow(g_tabs[i].btn, x, 1, tabW, tabH, TRUE); x += tabW + 4;
    }

    if (!g_newTabBtn) {
        g_newTabBtn = CreateWindowExW(0, L"BUTTON", L"+", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 0, 0, 0, 0, g_client, (HMENU)IDC_NEW_TAB, g_hInst, nullptr);
        SendMessageW(g_newTabBtn, WM_SETFONT, (WPARAM)g_font, TRUE);
        SubclassCtrlCommon(g_newTabBtn);
    }
    MoveWindow(g_newTabBtn, rc.right - 26, 1, 24, tabH, TRUE);

    int y = kTabBarH + kMargin, h = kToolbarH - kMargin * 2, tX = kMargin;

    if (!g_btnBack) g_btnBack = CreateWindowExW(0, L"BUTTON", L"Back", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 0, 0, 0, 0, g_client, (HMENU)IDC_BACK, g_hInst, nullptr);
    if (!g_btnFwd)  g_btnFwd = CreateWindowExW(0, L"BUTTON", L"Forward", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 0, 0, 0, 0, g_client, (HMENU)IDC_FWD, g_hInst, nullptr);
    if (!g_btnGo)   g_btnGo = CreateWindowExW(0, L"BUTTON", L"Go", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, 0, 0, 0, 0, g_client, (HMENU)IDC_GO, g_hInst, nullptr);
    if (!g_editUrl) g_editUrl = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"https://google.com/", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 0, 0, 0, 0, g_client, (HMENU)IDC_URL, g_hInst, nullptr);

    for (HWND hCtl : {g_btnBack, g_btnFwd, g_btnGo, g_editUrl}) SendMessageW(hCtl, WM_SETFONT, (WPARAM)g_font, TRUE);
    SubclassCtrlCommon(g_btnBack); SubclassCtrlCommon(g_btnFwd); SubclassCtrlCommon(g_btnGo);
    SetWindowSubclass(g_editUrl, EditSubproc, 1, 0);

    MoveWindow(g_btnBack, tX, y, kBtnW, h, TRUE); tX += kBtnW + kBtnGap;
    MoveWindow(g_btnFwd, tX, y, kBtnW, h, TRUE); tX += kBtnW + kBtnGap;

    int urlW = (rc.right - tX) - (kBtnW + kMargin + kBtnGap); if (urlW < 120) urlW = 120;
    MoveWindow(g_editUrl, tX, y, urlW, h, TRUE); tX += urlW + kBtnGap;
    MoveWindow(g_btnGo, tX, y, kBtnW, h, TRUE);

    EnsureSuggestListbox(); HideSuggestions();

    if (!g_brPanel) g_brPanel = CreateSolidBrush(g_colPanel);
    if (!g_brEdit)  g_brEdit = CreateSolidBrush(g_colBg);

    LayoutActiveWebView();
}

// ---------- tab ops ----------
static void UpdateTabTitle(int idx, const wchar_t* t) {
    if (idx < 0 || idx >= (int)g_tabs.size()) return;
    g_tabs[idx].title = t ? t : L"(tab)";
    if (g_tabs[idx].btn) SetWindowTextW(g_tabs[idx].btn, g_tabs[idx].title.c_str());
}
static void SwitchToTab(int idx) {
    if (idx < 0 || idx >= (int)g_tabs.size()) return;
    if (g_active == idx) return;

    HideAllWebViews();
    g_active = idx;
    LayoutActiveWebView();
    UpdateUrlBox();
    HideSuggestions();

    // Visually mark active tab
    for (int i = 0; i < (int)g_tabs.size(); ++i)
        if (g_tabs[i].btn) SendMessageW(g_tabs[i].btn, BM_SETSTATE, (WPARAM)(i == g_active), 0);

    // NEW: ensure the new WebView receives keyboard focus
    if (g_tabs[g_active].ctl) {
        g_tabs[g_active].ctl->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
    }
}
static void NextTab() {
    if (g_tabs.empty()) return;
    int idx = (g_active + 1) % (int)g_tabs.size();
    SwitchToTab(idx);
}
static void PrevTab() {
    if (g_tabs.empty()) return;
    int idx = (g_active - 1 + (int)g_tabs.size()) % (int)g_tabs.size();
    SwitchToTab(idx);
}
static void NewTab(const wchar_t* url /*= L"https://google.com/"*/) {
    if (!url) url = L"https://google.com/";
    int idx = (int)g_tabs.size();
    g_tabs.push_back(Tab{});
    g_tabs[idx].title = L"(new tab)";
    g_tabs[idx].pendingUri = url ? url : L"";
    LayoutToolbarAndTabs();
    SwitchToTab(idx);
    PostMessage(g_client, WM_APP_INIT_WEBVIEW, 0, 0);
}
static void CloseTab(int idx) {
    if (idx < 0 || idx >= (int)g_tabs.size()) return;
    if (g_tabs[idx].btn) { DestroyWindow(g_tabs[idx].btn); g_tabs[idx].btn = nullptr; }
    if (g_tabs[idx].ctl) { g_tabs[idx].ctl->Close(); g_tabs[idx].ctl.Reset(); }
    g_tabs[idx].web.Reset(); g_tabs.erase(g_tabs.begin() + idx);
    if (g_tabs.empty()) { g_active = -1; NewTab(L"https://google.com/"); return; }
    else if (g_active >= idx) g_active = max(0, g_active - 1);
    LayoutToolbarAndTabs(); SwitchToTab(g_active);
}
static void CloseActiveTab() { CloseTab(g_active); }

// ---------- webview wiring ----------
static void WireTabEvents(int idx) {
    if (idx < 0 || idx >= (int)g_tabs.size()) return;
    auto* web = g_tabs[idx].web.Get(); auto* ctl = g_tabs[idx].ctl.Get(); if (!web || !ctl) return;

    web->add_DocumentTitleChanged(Microsoft::WRL::Callback<ICoreWebView2DocumentTitleChangedEventHandler>(
        [idx](ICoreWebView2* s, IUnknown*)->HRESULT {
            LPWSTR t = nullptr; if (s && SUCCEEDED(s->get_DocumentTitle(&t))) { UpdateTabTitle(idx, t); if (t) CoTaskMemFree(t); }
            return S_OK;
        }).Get(), nullptr);

    web->add_NavigationStarting(Microsoft::WRL::Callback<ICoreWebView2NavigationStartingEventHandler>(
        [idx](ICoreWebView2* s, ICoreWebView2NavigationStartingEventArgs* a)->HRESULT {
            if (!s || !a) return S_OK; LPWSTR uri = nullptr;
            if (SUCCEEDED(a->get_Uri(&uri))) { if (idx == g_active && !(GetFocus() == g_editUrl && g_editDirty)) { g_settingUrl = true; SetWindowTextW(g_editUrl, uri ? uri : L""); g_settingUrl = false; } if (uri) CoTaskMemFree(uri); }
            return S_OK;
        }).Get(), nullptr);

    web->add_SourceChanged(Microsoft::WRL::Callback<ICoreWebView2SourceChangedEventHandler>(
        [idx](ICoreWebView2* s, ICoreWebView2SourceChangedEventArgs*)->HRESULT {
            if (!s || idx != g_active) return S_OK; if (GetFocus() == g_editUrl && g_editDirty) return S_OK;
            LPWSTR src = nullptr; if (SUCCEEDED(s->get_Source(&src))) { g_settingUrl = true; SetWindowTextW(g_editUrl, src ? src : L""); g_settingUrl = false; if (src) CoTaskMemFree(src); }
            return S_OK;
        }).Get(), nullptr);

    web->add_NavigationCompleted(Microsoft::WRL::Callback<ICoreWebView2NavigationCompletedEventHandler>(
        [idx](ICoreWebView2* s, ICoreWebView2NavigationCompletedEventArgs*)->HRESULT {
            if (s) {
                s->ExecuteScript(L"(function(){try{document.documentElement.style.background='#000'}catch(e){};try{if(document.body)document.body.style.background='#000'}catch(e){}})();", nullptr);
                LPWSTR src = nullptr; if (SUCCEEDED(s->get_Source(&src))) {
                    std::wstring url = src ? src : L""; g_tabs[idx].lastUri = url; AddToHistory(url);
                    if (idx == g_active && !(GetFocus() == g_editUrl && g_editDirty)) { g_settingUrl = true; SetWindowTextW(g_editUrl, url.c_str()); g_settingUrl = false; }
                    if (src) CoTaskMemFree(src);
                }
            }
            return S_OK;
        }).Get(), nullptr);

    web->add_NewWindowRequested(Microsoft::WRL::Callback<ICoreWebView2NewWindowRequestedEventHandler>(
        [](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* a)->HRESULT {
            LPWSTR uri = nullptr; a->get_Uri(&uri); NewTab(uri ? uri : L"about:blank"); if (uri) CoTaskMemFree(uri); a->put_Handled(TRUE); return S_OK;
        }).Get(), nullptr);

    ctl->add_AcceleratorKeyPressed(Microsoft::WRL::Callback<ICoreWebView2AcceleratorKeyPressedEventHandler>(
        [](ICoreWebView2Controller*, ICoreWebView2AcceleratorKeyPressedEventArgs* args)->HRESULT {
            UINT vk = 0; args->get_VirtualKey(&vk);
            COREWEBVIEW2_KEY_EVENT_KIND kind; args->get_KeyEventKind(&kind);
            if (kind != COREWEBVIEW2_KEY_EVENT_KIND_KEY_DOWN && kind != COREWEBVIEW2_KEY_EVENT_KIND_SYSTEM_KEY_DOWN) return S_OK;

            bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            bool alt = (GetKeyState(VK_MENU) & 0x8000) != 0;

            if (HandleGlobalShortcut(vk, ctrl, shift, alt)) {
                args->put_Handled(TRUE);
                return S_OK;
            }
            return S_OK;
        }).Get(), nullptr);
}

static void CreateWebViewForTab(int idx) {
    if (idx < 0 || idx >= (int)g_tabs.size() || g_tabs[idx].ctl) return;
    CreateCoreWebView2EnvironmentWithOptions(nullptr, L".\\wv2_userdata", nullptr,
        Microsoft::WRL::Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [idx](HRESULT hr, ICoreWebView2Environment* env)->HRESULT {
                if (FAILED(hr) || !env) return hr ? hr : E_FAIL;
                return env->CreateCoreWebView2Controller(g_client,
                    Microsoft::WRL::Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                        [idx](HRESULT hr2, ICoreWebView2Controller* ctl)->HRESULT {
                            if (FAILED(hr2) || !ctl) return hr2 ? hr2 : E_FAIL;
                            g_tabs[idx].ctl = ctl; ctl->get_CoreWebView2(&g_tabs[idx].web);
                            RECT rc; GetClientRect(g_client, &rc);
                            RECT b = { kMargin,kTabBarH + kToolbarH,rc.right - kMargin,rc.bottom - kMargin };
                            ctl->put_Bounds(b); ctl->put_IsVisible(FALSE);
                            WireTabEvents(idx);
                            if (!g_tabs[idx].pendingUri.empty()) g_tabs[idx].web->Navigate(g_tabs[idx].pendingUri.c_str());
                            else g_tabs[idx].web->Navigate(L"https://google.com/");
                            if (idx == g_active) {
                                LayoutActiveWebView();
                                if (g_tabs[idx].ctl) {
                                    g_tabs[idx].ctl->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
                                }
                            }
                            return S_OK;
                        }).Get());
            }).Get());
}

// ---------- procs ----------
static LRESULT CALLBACK ClientProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_WIN95_CLASSES | ICC_STANDARD_CLASSES }; InitCommonControlsEx(&icc);
        if (!g_brBlack) g_brBlack = CreateSolidBrush(g_colBg);
        if (!g_font)    g_font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        if (!g_brPanel) g_brPanel = CreateSolidBrush(g_colPanel);
        if (!g_brEdit)  g_brEdit = CreateSolidBrush(g_colBg);

        g_tabBar = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd, (HMENU)IDC_TABBAR, g_hInst, nullptr);

        LayoutToolbarAndTabs();

        g_tabs.clear(); g_tabs.push_back(Tab{}); g_active = 0;

        LoadHistory();

        SetWindowSubclass(g_editUrl, EditSubproc, 1, 0);

        PostMessage(hwnd, WM_APP_INIT_WEBVIEW, 0, 0);
        return 0;
    }

    case WM_ERASEBKGND: {
        RECT r; GetClientRect(hwnd, &r);
        FillRect((HDC)wParam, &r, g_brBlack ? g_brBlack : (HBRUSH)GetStockObject(BLACK_BRUSH));
        return 1;
    }

    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORBTN:
    case WM_CTLCOLORLISTBOX: {
        HDC dc = (HDC)wParam; SetTextColor(dc, g_colText);
        if (msg == WM_CTLCOLOREDIT) { SetBkColor(dc, g_colBg); return (LRESULT)g_brEdit; }
        SetBkColor(dc, g_colPanel); return (LRESULT)g_brPanel;
    }

    case WM_DRAWITEM: {
        const DRAWITEMSTRUCT* dis = (const DRAWITEMSTRUCT*)lParam; if (!dis) break;
        if (dis->CtlID == IDC_SUGGEST) return FALSE;
        bool isTab = false, isActive = false; UINT id = (UINT)GetDlgCtrlID(dis->hwndItem);
        if (id >= (UINT)WM_APP_SWITCH_TAB && id < (UINT)WM_APP_SWITCH_TAB + 0x1000) { isTab = true; int idx = (int)(id - WM_APP_SWITCH_TAB); isActive = (idx == g_active); }
        DrawOwnerButton(dis, isTab, isActive); return TRUE;
    }

    case WM_APP_INIT_WEBVIEW:
        if (g_active >= 0 && g_active < (int)g_tabs.size() && !g_tabs[g_active].ctl) CreateWebViewForTab(g_active);
        return 0;

    case WM_SIZE:
        LayoutToolbarAndTabs();
        if (g_suggestVisible) { int cnt = (int)SendMessageW(g_suggest, LB_GETCOUNT, 0, 0); if (cnt > 0) PositionSuggestBelowEdit(cnt); }
        return 0;

        // Global hotkey (Ctrl+L)
    case WM_HOTKEY:
        if (wParam == HK_CTRLL) {
            if (g_editUrl) { SetFocus(g_editUrl); SendMessageW(g_editUrl, EM_SETSEL, 0, -1); g_editDirty = true; HideSuggestions(); }
            return 0;
        }
        break;

    case WM_KEYDOWN: {
        bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        bool alt = (GetKeyState(VK_MENU) & 0x8000) != 0;

        // Enter in URL box (existing)
        if (wParam == VK_RETURN && GetFocus() == g_editUrl) { g_editDirty = false; NavigateFromUrlBox(); return 0; }

        // Global shortcuts (backstop)
        if (HandleGlobalShortcut(wParam, ctrl, shift, alt)) return 0;

        break;
    }

    case WM_COMMAND: {
        int id = LOWORD(wParam), code = HIWORD(wParam);

        if (id == IDC_URL) {
            if (code == EN_CHANGE) {
                if (!g_settingUrl && GetFocus() == g_editUrl) {
                    g_editDirty = true;
                    wchar_t q[2048] = L""; GetWindowTextW(g_editUrl, q, 2048);
                    PopulateSuggestions(q);
                }
                return 0;
            }
            if (code == EN_KILLFOCUS) { g_editDirty = false; return 0; }
        }

        if (id == IDC_SUGGEST) {
            if (code == LBN_DBLCLK) { CommitSuggestion(true); return 0; }
            if (code == LBN_KILLFOCUS) { if (GetFocus() != g_editUrl) HideSuggestions(); return 0; }
        }

        if (id == IDC_GO && code == BN_CLICKED) { g_editDirty = false; NavigateFromUrlBox(); return 0; }
        if (id == IDC_BACK && code == BN_CLICKED) { if (g_active >= 0 && g_tabs[g_active].web) g_tabs[g_active].web->GoBack(); return 0; }
        if (id == IDC_FWD && code == BN_CLICKED) { if (g_active >= 0 && g_tabs[g_active].web) g_tabs[g_active].web->GoForward(); return 0; }

        if (id == IDC_NEW_TAB && code == BN_CLICKED) { NewTab(); return 0; }

        if (id >= (int)WM_APP_SWITCH_TAB && id < (int)WM_APP_SWITCH_TAB + 1024) {
            int idx = id - WM_APP_SWITCH_TAB; if (idx < (int)g_tabs.size()) SwitchToTab(idx); return 0;
        }
        break;
    }

    case WM_CLOSE: ShowWindow(GetParent(hwnd), SW_HIDE); return 0;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

static LRESULT CALLBACK FrameProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_ERASEBKGND: { RECT r; GetClientRect(hwnd, &r); FillRect((HDC)wParam, &r, g_brBlack ? g_brBlack : (HBRUSH)GetStockObject(BLACK_BRUSH)); return 1; }
    case WM_SIZE:
        if (g_client) { RECT fr; GetClientRect(hwnd, &fr); SetWindowPos(g_client, 0, 8, 24, fr.right - 8, fr.bottom - 24, SWP_NOZORDER | SWP_NOACTIVATE); }
        break;
    case WM_NCDESTROY:
        if (g_prevFrameProc) { SetWindowLongPtr(hwnd, GWLP_WNDPROC, (LONG_PTR)g_prevFrameProc); g_prevFrameProc = nullptr; }
        break;
    }
    return g_prevFrameProc ? CallWindowProc(g_prevFrameProc, hwnd, msg, wParam, lParam) : DefWindowProc(hwnd, msg, wParam, lParam);
}

// ---- Winamp plumbing ----
static HWND CreateEmbeddedFrame(HWND wa) {
    g_ews.flags |= EMBED_FLAGS_NOWINDOWMENU;
    g_ews.r.left = 200; g_ews.r.top = 200; g_ews.r.right = 1200; g_ews.r.bottom = 900;
    SET_EMBED_GUID((&g_ews), kEmbedGuid);
    return (HWND)SendMessage(wa, WM_WA_IPC, (WPARAM)&g_ews, IPC_GET_EMBEDIF);
}

static int  __cdecl init();
static void __cdecl quit();
static void __cdecl config();

static winampGeneralPurposePlugin plugin = {
  GPPHDR_VER, (char*)"exodium (Stage 9c)", init, config, quit, 0, 0
};
extern "C" __declspec(dllexport) winampGeneralPurposePlugin* winampGetGeneralPurposePlugin() { return &plugin; }

static int __cdecl init() {
    g_hInst = (HINSTANCE)plugin.hDllInstance;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    if (!g_brBlack) g_brBlack = CreateSolidBrush(g_colBg);
    if (!g_font)    g_font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    if (!g_brPanel) g_brPanel = CreateSolidBrush(g_colPanel);
    if (!g_brEdit)  g_brEdit = CreateSolidBrush(g_colBg);

    g_embedFrame = CreateEmbeddedFrame(plugin.hwndParent);
    if (!g_embedFrame) return 0;

    g_prevFrameProc = (WNDPROC)SetWindowLongPtr(g_embedFrame, GWLP_WNDPROC, (LONG_PTR)FrameProc);

    WNDCLASSW wc = {}; wc.lpfnWndProc = ClientProc; wc.hInstance = g_hInst; wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH); wc.lpszClassName = L"ExodiumClient";
    RegisterClassW(&wc);

    RECT fr; GetClientRect(g_embedFrame, &fr);
    g_client = CreateWindowExW(0, L"ExodiumClient", L"", WS_CHILD | WS_VISIBLE, 8, 24, fr.right - 8, fr.bottom - 24, g_embedFrame, nullptr, g_hInst, nullptr);

    // Ctrl+L hotkey
    RegisterHotKey(g_client, HK_CTRLL, MOD_CONTROL, 'L');

    LoadHistory();

    ShowWindow(g_embedFrame, SW_SHOW);
    return 0;
}
static void __cdecl config() { if (g_embedFrame) ShowWindow(g_embedFrame, SW_SHOWNA); }

static void __cdecl quit() {
    SaveHistory();
    if (g_client) UnregisterHotKey(g_client, HK_CTRLL);

    for (auto& t : g_tabs) { if (t.ctl) t.ctl->Close(); t.web.Reset(); if (t.btn) DestroyWindow(t.btn); }
    g_tabs.clear();
    if (g_suggest) { DestroyWindow(g_suggest); g_suggest = nullptr; }
    if (g_client) { DestroyWindow(g_client); g_client = nullptr; }
    if (g_embedFrame) {
        if (g_prevFrameProc) SetWindowLongPtr(g_embedFrame, GWLP_WNDPROC, (LONG_PTR)g_prevFrameProc);
        DestroyWindow(g_embedFrame); g_embedFrame = nullptr;
    }
    if (g_brBlack) { DeleteObject(g_brBlack); g_brBlack = nullptr; }
    if (g_brPanel) { DeleteObject(g_brPanel); g_brPanel = nullptr; }
    if (g_brEdit) { DeleteObject(g_brEdit);  g_brEdit = nullptr; }
    CoUninitialize();
}
