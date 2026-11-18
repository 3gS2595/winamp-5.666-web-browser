// What can be done to a tab from the strip besides clicking and dragging it (tab_drag.cpp):
//
// - A right click brings up a menu, as in Chrome: a new tab beside it, loading its page
//   again, a second tab on the same page, muting it, closing it, the others or those to
//   its right, and bringing back what was closed last. A tab that is one of several
//   selected takes the others along where that makes sense, as dragging it does.
// - A muted tab stays muted whatever becomes of its webview, and its speaker is crossed
//   out in the strip (drawing.cpp).
// - The mouse resting on a tab is told its page's whole title and its site, which a tab
//   squeezed among many has no room for.
#include "browser.h"

#include <commctrl.h>

#include <algorithm>
#include <utility>

namespace exodium {

namespace {

constexpr int kTipMaxWidth = 420;         // of a tab's tip, in pixels, before its text wraps
constexpr std::size_t kTipMaxTitle = 300; // and in characters

// The tabs a command is for: the tab itself, or, where it is one of several selected,
// all of those, in strip order.
[[nodiscard]] std::vector<int> TabsMeant(const BrowserWindow& win, int tabId) {
    const int index = TabIndexById(win, tabId);
    if (index >= 0 && ShowsAsSelected(win, index)) return SelectedTabIds(win);
    return {tabId};
}

// A new tab right after the one at `index`, in view, with the keyboard in the address bar.
void NewTabAfter(BrowserWindow& win, int index) {
    AddTab(win, kNewTabTitle, index + 1).pendingUrl = HomePage();
    LayoutToolbarAndTabs(win);
    SwitchToTab(win, index + 1);
    ::PostMessageW(win.client, kMsgInitWebView, 0, 0);
    FocusAddressBar(win);
}

void CloseAllBut(BrowserWindow& win, int from, int keptId) {
    std::vector<int> going;
    for (int i = from; i < static_cast<int>(win.tabs.size()); ++i) {
        if (win.tabs[i].id != keptId) going.push_back(win.tabs[i].id);
    }
    if (!going.empty()) CloseTabs(win, going);
}

} // namespace

void SetTabUserMuted(Tab& tab, bool muted) {
    if (std::exchange(tab.muted, muted) == muted) return;
    SetTabMuted(tab, muted);
    if (tab.button) ::InvalidateRect(tab.button.get(), nullptr, FALSE);
    // Winamp's visualizers have nothing to show of a page that can't be heard.
    FollowPageSound();
}

void RunTabCommand(BrowserWindow& win, int tabId, TabCommand command) {
    const int index = TabIndexById(win, tabId);
    if (index < 0 || win.closing || win.isFullscreen) return;
    switch (command) {
    case TabCommand::kNewTabAfter:
        NewTabAfter(win, index);
        break;
    case TabCommand::kReload:
        for (const int id : TabsMeant(win, tabId)) {
            if (const TabLocation at = FindTab(id)) ReloadTab(at.tab());
        }
        break;
    case TabCommand::kDuplicate:
        DuplicateTab(win, tabId);
        break;
    case TabCommand::kToggleMute: {
        // They all go the way the tab that was clicked does.
        const bool mute = !win.tabs[index].muted;
        for (const int id : TabsMeant(win, tabId)) {
            if (const TabLocation at = FindTab(id)) SetTabUserMuted(at.tab(), mute);
        }
        break;
    }
    case TabCommand::kClose:
        CloseTabs(win, TabsMeant(win, tabId));
        break;
    case TabCommand::kCloseOthers:
        CloseAllBut(win, 0, tabId);
        break;
    case TabCommand::kCloseToRight:
        CloseAllBut(win, index + 1, tabId);
        break;
    case TabCommand::kReopenClosed:
        ReopenLastClosedTab(win);
        break;
    }
}

void ShowTabMenu(BrowserWindow& win, int tabId, POINT screenPoint) {
    const int index = TabIndexById(win, tabId);
    if (index < 0 || win.closing || win.isFullscreen) return;
    const Tab& tab = win.tabs[index];
    const bool several = TabsMeant(win, tabId).size() > 1;
    const int count = static_cast<int>(win.tabs.size());

    const HMENU menu = ::CreatePopupMenu();
    if (!menu) return;
    const auto add = [menu](TabCommand command, const wchar_t* text, bool enabled = true) {
        ::AppendMenuW(menu, MF_STRING | (enabled ? MF_ENABLED : MF_GRAYED), static_cast<UINT_PTR>(command), text);
    };
    const auto addLine = [menu] { ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr); };
    add(TabCommand::kNewTabAfter, L"New tab to the right");
    addLine();
    // A tab that hasn't been looked at since it was brought back, or is asleep, has no
    // page to load again: it loads when it is looked at.
    add(TabCommand::kReload, several ? L"Reload tabs" : L"Reload", several || tab.webview);
    add(TabCommand::kDuplicate, L"Duplicate");
    add(TabCommand::kToggleMute,
        tab.muted ? (several ? L"Unmute tabs" : L"Unmute tab") : (several ? L"Mute tabs" : L"Mute tab"));
    addLine();
    add(TabCommand::kClose, several ? L"Close tabs" : L"Close\tCtrl+W");
    add(TabCommand::kCloseOthers, L"Close other tabs", count > 1);
    add(TabCommand::kCloseToRight, L"Close tabs to the right", index + 1 < count);
    addLine();
    add(TabCommand::kReopenClosed, L"Reopen closed tab\tCtrl+Shift+T", !g_app.closedTabs.empty() && !win.isPrivate);

    // A menu only goes away on a click elsewhere if its window is the one in front.
    const HWND client = win.client;
    const HWND button = tab.button.get();
    ::SetForegroundWindow(win.frame);
    const int picked = ::TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON, screenPoint.x,
                                        screenPoint.y, 0, client, nullptr);
    ::DestroyMenu(menu);
    // The menu took messages while it was up, and anything may have happened to the window
    // and its tabs meanwhile: nothing of them is touched from here on. The command is done
    // from the message loop, where the tab is found by its button, if it still has that
    // one; and never from inside the click on the button itself.
    if (!::IsWindow(client)) return;
    ::PostMessageW(client, WM_NULL, 0, 0);
    if (picked > 0)
        ::PostMessageW(client, kMsgTabCommand, static_cast<WPARAM>(picked), reinterpret_cast<LPARAM>(button));
}

// ---- what a tab says under the mouse ----

void CreateTabTips(BrowserWindow& win) {
    // The frame owns it, which keeps it in front of the frame, and that is all it needs. It
    // is not made a topmost window, as such controls often are: a window that owns a
    // topmost one can't be put right under another window any more. Windows takes it to
    // the top of the others instead, and the windows docked with one that is activated
    // came up over it, not under it (docked_windows.cpp; the smoke test caught it).
    win.tabTips = ::CreateWindowExW(0, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_NOPREFIX, CW_USEDEFAULT, CW_USEDEFAULT,
                                    CW_USEDEFAULT, CW_USEDEFAULT, win.client, nullptr, g_app.instance, nullptr);
    // A width for its text to wrap at, which also lets it have more lines than one.
    if (win.tabTips) ::SendMessageW(win.tabTips, TTM_SETMAXTIPWIDTH, 0, kTipMaxWidth);
}

namespace {

// A tab's button as the control knows it. The size is that of the first version of the
// structure, which every version of the common controls takes.
[[nodiscard]] TOOLINFOW TipFor(HWND client, HWND button) noexcept {
    TOOLINFOW tool{};
    tool.cbSize = TTTOOLINFOW_V1_SIZE;
    tool.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
    tool.hwnd = client;
    tool.uId = reinterpret_cast<UINT_PTR>(button);
    tool.lpszText = LPSTR_TEXTCALLBACKW; // asked for each time, as a tab's title changes
    return tool;
}

} // namespace

void AddTabTip(BrowserWindow& win, HWND button) {
    if (!win.tabTips || !button) return;
    TOOLINFOW tool = TipFor(win.client, button);
    ::SendMessageW(win.tabTips, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tool));
}

void RemoveTabTip(HWND button) {
    const BrowserWindow* const win = WindowFromChild(button);
    if (!win || !win->tabTips) return;
    TOOLINFOW tool = TipFor(win->client, button);
    ::SendMessageW(win->tabTips, TTM_DELTOOLW, 0, reinterpret_cast<LPARAM>(&tool));
}

bool OnTabTipNeeded(BrowserWindow& win, NMHDR* header) {
    if (!header || !win.tabTips || header->hwndFrom != win.tabTips || header->code != TTN_GETDISPINFOW) return false;
    auto* const info = reinterpret_cast<NMTTDISPINFOW*>(header);
    win.tabTipText.clear();
    const HWND button = reinterpret_cast<HWND>(header->idFrom);
    const int index = ::IsWindow(button) ? ::GetDlgCtrlID(button) - kTabButtonIdBase : -1;
    if (IsTabIndex(win, index) && win.tabs[index].button.get() == button) {
        const Tab& tab = win.tabs[index];
        win.tabTipText = tab.title.empty() ? std::wstring(kNewTabTitle) : tab.title.substr(0, kTipMaxTitle);
        // Under it, where the page is: one of the browser's own by its name, a site by its
        // host, and anything else by its address.
        const std::wstring url = TabUrl(tab);
        std::wstring where(PluginPageName(url));
        if (where.empty()) where = HostOf(url);
        if (where.empty()) where = url.substr(0, kTipMaxTitle);
        if (!where.empty() && where != win.tabTipText) win.tabTipText.append(L"\r\n").append(where);
        if (tab.muted) win.tabTipText.append(L"\r\nMuted");
    }
    info->lpszText = win.tabTipText.data();
    return true;
}

} // namespace exodium
