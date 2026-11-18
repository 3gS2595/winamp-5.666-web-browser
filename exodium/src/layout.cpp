// Where a browser window's controls go: the tab strip along the top, the toolbar
// under it, and the active tab's webview in what is left.
#include "browser.h"

#include <algorithm>
#include <initializer_list>

namespace exodium {

namespace {

constexpr int kTabTop = 1;
constexpr int kTabHeight = kTabBarHeight - 2;
// The address bar keeps this much of a narrow window before the buttons stop shrinking.
constexpr int kUrlReservedWidth = 160;
constexpr int kToolbarButtonCount = 4;
constexpr int kBookmarkButtonWidth = 28; // the star's; as wide as the downloads button

[[nodiscard]] int TabCount(const BrowserWindow& win) noexcept {
    return static_cast<int>(win.tabs.size());
}

void SetControlFont(HWND control) noexcept {
    ::SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(g_app.theme.font), TRUE);
}

HWND CreateControl(const BrowserWindow& win, const wchar_t* className, const wchar_t* text, DWORD style, int id) {
    const HWND control = ::CreateWindowExW(0, className, text, WS_CHILD | style, 0, 0, 0, 0, win.client,
                                           reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), g_app.instance, nullptr);
    SetControlFont(control);
    return control;
}

HWND CreateButton(const BrowserWindow& win, const wchar_t* text, int id, DWORD style = WS_VISIBLE) {
    const HWND button = CreateControl(win, L"BUTTON", text, BS_OWNERDRAW | style, id);
    SubclassControl(button);
    return button;
}

void ShowControl(HWND control, bool show) noexcept {
    if (control) ::ShowWindow(control, show ? SW_SHOWNA : SW_HIDE);
}

// Lays out the buttons at the toolbar's right end. Two of them come and go: Bitwarden's,
// while it is there, at the very end, and the downloads button, while there are downloads,
// left of it. The star that bookmarks the page in view is always there, left of both and
// right up against the address bar. Says where the address bar has to end.
int LayoutToolbarEnd(BrowserWindow& win, int top, int height) {
    int right = LayoutBitwardenButton(win, ClientRectOf(win.client).right - kMargin, top, height);
    right = LayoutDownloads(win, right, top, height);
    ::MoveWindow(win.bookmarkButton, right - kBookmarkButtonWidth, top, kBookmarkButtonWidth, height, TRUE);
    return right - kBookmarkButtonWidth - kButtonGap;
}

// Where a window's tab strip begins and ends: clear of the ornaments on its corners.
[[nodiscard]] int TabStripLeft(const BrowserWindow& win) noexcept {
    return OrnamentStripCover(win).left;
}
[[nodiscard]] int TabStripRight(const BrowserWindow& win) noexcept {
    return ClientRectOf(win.client).right - OrnamentStripCover(win).right;
}

} // namespace

void CreateControls(BrowserWindow& win) {
    // The strip's background goes first, so that it stays behind the tabs laid over it.
    win.tabBar = CreateControl(win, L"STATIC", L"", WS_VISIBLE, kIdTabBar);
    // The buttons that scroll the strip are only shown while it has tabs out of sight.
    win.tabsLeftButton = CreateButton(win, L"<", kIdTabsLeft, 0);
    win.tabsRightButton = CreateButton(win, L">", kIdTabsRight, 0);
    win.newTabButton = CreateButton(win, L"+", kIdNewTab);

    win.backButton = CreateButton(win, kGlyphBack, kIdBack);
    win.forwardButton = CreateButton(win, kGlyphForward, kIdForward);
    win.reloadButton = CreateButton(win, kGlyphReload, kIdReload);
    win.closeTabButton = CreateButton(win, L"X", kIdCloseTab);
    CreateAddressBar(win);
    win.bookmarkButton = CreateButton(win, kGlyphBookmark, kIdBookmark);
    CreateDownloadsControls(win);
    CreateBitwardenButton(win);
    CreateTabTips(win);
    CreateDevToolsControls(win); // after the tips, which its buttons have too
}

RECT WebViewBounds(const BrowserWindow& win) {
    const RECT client = ClientRectOf(win.client);
    // Maximized, the page goes right to the screen's edges, as the window does.
    const int margin = IsMaximized(win) ? 0 : kMargin;
    return {margin, kTabBarHeight + kToolbarHeight, client.right - margin, client.bottom - margin};
}

void LayoutActiveWebView(BrowserWindow& win) {
    if (win.isFullscreen) return; // bounds/parent are driven by the fullscreen window instead
    Tab* const tab = ActiveTab(win);
    // The tab's DevTools, while they are docked, have their side of the page's place.
    const RECT page = LayoutDevTools(win, tab, WebViewBounds(win));
    if (!tab || !tab->controller) return;
    tab->controller->put_Bounds(page);
    tab->controller->put_IsVisible(TRUE);
    // A webview made or brought into the window since the panel opened is in front of it.
    RaiseDownloadsPanel(win);
}

void HideAllWebViews(BrowserWindow& win) {
    for (Tab& tab : win.tabs) {
        if (tab.controller) tab.controller->put_IsVisible(FALSE);
        if (tab.devtools.controller) tab.devtools.controller->put_IsVisible(FALSE);
    }
}

void StartTabSpinners(BrowserWindow& win) {
    if (win.client) ::SetTimer(win.client, kSpinTimer, kSpinTickMs, nullptr);
}

void OnSpinTimer(BrowserWindow& win) {
    // Turns the spinners of the busy tabs, and gives a tab that has stopped being
    // busy one more repaint to take its spinner away.
    bool anyBusy = false;
    for (Tab& tab : win.tabs) {
        const bool busy = tab.busy();
        if ((busy || tab.spinning) && tab.button) ::InvalidateRect(tab.button.get(), nullptr, FALSE);
        tab.spinning = busy;
        anyBusy = anyBusy || busy;
    }
    if (!anyBusy) ::KillTimer(win.client, kSpinTimer);
}

// Tabs are full width while they all fit and squeeze evenly once they don't. At
// their narrowest the strip scrolls instead: it shows as many as it has room for,
// from win.firstTab on, and the rest are hidden.
TabStripFit FitTabStrip(const BrowserWindow& win) {
    const int count = TabCount(win);
    int room = TabStripRight(win) - kNewTabSlot - kMargin - TabStripLeft(win); // up to the new tab button
    TabStripFit fit{.tabWidth = kTabWidth, .shown = count, .scrolls = false};
    if (count > 0) fit.tabWidth = std::max(kTabMinWidth, std::min(kTabWidth, room / count - kTabGap));
    // They don't grow into the room a tab closed with the mouse leaves: see tab_drag.cpp.
    if (win.keptTabWidth > 0) fit.tabWidth = std::min(fit.tabWidth, win.keptTabWidth);
    if (count * (kTabMinWidth + kTabGap) > room) {
        room -= 2 * (kTabArrowWidth + kTabArrowGap); // the scroll buttons
        fit.scrolls = true;
        fit.shown = std::max(1, std::min(count, room / (kTabMinWidth + kTabGap)));
    }
    return fit;
}

void LayoutTabStrip(BrowserWindow& win) {
    const TabStripFit fit = FitTabStrip(win);
    const int count = TabCount(win);
    win.firstTab = std::max(0, std::min(win.firstTab, count - fit.shown));

    int x = TabStripLeft(win) + kMargin;
    for (int i = 0; i < count; ++i) {
        Tab& tab = win.tabs[i];
        const int id = kTabButtonIdBase + i;
        if (!tab.button) {
            tab.button.reset(
                CreateControl(win, L"BUTTON", tab.title.empty() ? kNewTabTitle : tab.title.c_str(), BS_OWNERDRAW, id));
            SubclassControl(tab.button.get());
            SubclassTabButton(tab.button.get());
            AddTabTip(win, tab.button.get());
        } else {
            ::SetWindowLongPtrW(tab.button.get(), GWLP_ID, id);
        }
        const bool shown = i >= win.firstTab && i < win.firstTab + fit.shown;
        if (shown) {
            ::MoveWindow(tab.button.get(), x, kTabTop, fit.tabWidth, kTabHeight, TRUE);
            x += fit.tabWidth + kTabGap;
        }
        ShowControl(tab.button.get(), shown && !win.isFullscreen);
    }

    const int arrowX = TabStripRight(win) - kNewTabSlot - 2 * (kTabArrowWidth + kTabArrowGap);
    ::MoveWindow(win.tabsLeftButton, arrowX, kTabTop, kTabArrowWidth, kTabHeight, TRUE);
    ::MoveWindow(win.tabsRightButton, arrowX + kTabArrowWidth + kTabArrowGap, kTabTop, kTabArrowWidth, kTabHeight,
                 TRUE);
    for (const HWND arrow : {win.tabsLeftButton, win.tabsRightButton}) {
        ShowControl(arrow, fit.scrolls && !win.isFullscreen);
    }

    // A tab may have come in busy, dragged over from another window.
    if (std::ranges::any_of(win.tabs, &Tab::busy)) StartTabSpinners(win);
}

void ScrollTabStrip(BrowserWindow& win, int by) {
    win.firstTab += by; // LayoutTabStrip keeps it in range
    LayoutTabStrip(win);
}

void ShowTabInStrip(BrowserWindow& win, int index) {
    const TabStripFit fit = FitTabStrip(win);
    int first = win.firstTab;
    if (index < first) first = index;
    else if (index >= first + fit.shown) first = index - fit.shown + 1;
    if (first == win.firstTab) return;
    win.firstTab = first;
    LayoutTabStrip(win);
}

void LayoutToolbarAndTabs(BrowserWindow& win) {
    if (!win.client) return; // still being created
    const RECT client = ClientRectOf(win.client);

    ::MoveWindow(win.tabBar, 0, 0, client.right, kTabBarHeight, TRUE);
    LayoutTabStrip(win);
    ::MoveWindow(win.newTabButton, TabStripRight(win) - kNewTabSlot, kTabTop, kNewTabWidth, kTabHeight, TRUE);

    // In a narrow window the buttons give up width before the address bar does.
    const int y = kTabBarHeight + kMargin;
    const int height = kToolbarHeight - kMargin * 2;
    const int available = (client.right - 2 * kMargin - kUrlReservedWidth) / kToolbarButtonCount - kButtonGap;
    const int buttonWidth = std::max(kButtonMinWidth, std::min(kButtonWidth, available));
    int x = kMargin;
    for (const HWND button : {win.backButton, win.forwardButton, win.reloadButton, win.closeTabButton}) {
        ::MoveWindow(button, x, y, buttonWidth, height, TRUE);
        x += buttonWidth + kButtonGap;
    }
    const int urlRight = LayoutToolbarEnd(win, y, height);
    ::MoveWindow(win.urlEdit, x, y, std::max(kUrlMinWidth, urlRight - x), height, TRUE);

    HideSuggestions(win);
    LayoutActiveWebView(win);
}

void FitAddressBar(BrowserWindow& win) {
    RECT edit = WindowRectOf(win.urlEdit);
    ::MapWindowPoints(nullptr, win.client, reinterpret_cast<POINT*>(&edit), 2);
    const int right = LayoutToolbarEnd(win, edit.top, Height(edit));
    ::MoveWindow(win.urlEdit, edit.left, edit.top, std::max(kUrlMinWidth, right - static_cast<int>(edit.left)),
                 Height(edit), TRUE);
}

void SetToolbarControlsVisible(BrowserWindow& win, bool visible) {
    ShowControl(win.tabBar, visible);
    ShowControl(win.newTabButton, visible);
    // Which tabs and scroll buttons a strip shows is up to LayoutTabStrip; here they only go.
    if (!visible) {
        ShowControl(win.tabsLeftButton, false);
        ShowControl(win.tabsRightButton, false);
    }
    for (const Tab& tab : win.tabs) ShowControl(tab.button.get(), visible);
    for (const HWND control :
         {win.backButton, win.forwardButton, win.reloadButton, win.closeTabButton, win.urlEdit, win.bookmarkButton}) {
        ShowControl(control, visible);
    }
    ShowDownloadsButton(win, visible);
    ShowBitwardenButton(win, visible);
    if (!visible) {
        HideSuggestions(win);
        HideDevTools(win); // they come back with the page, when the window is laid out again
    }
}

} // namespace exodium
