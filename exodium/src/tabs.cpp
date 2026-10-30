// Opening, switching, closing, reopening and moving tabs.
#include "browser.h"

#include <algorithm>
#include <ranges>
#include <utility>

namespace exodium {

namespace {

[[nodiscard]] int TabCount(const BrowserWindow& win) noexcept {
    return static_cast<int>(win.tabs.size());
}

// Re-syncs a window after the tab at index was taken out of it. It must still have tabs.
void RefreshAfterTabRemoved(BrowserWindow& win, int index) {
    if (win.active >= index) win.active = std::max(0, win.active - 1);

    LayoutToolbarAndTabs(win);
    // Force a full refresh of the active tab's visuals since indices shifted.
    const int current = std::exchange(win.active, -1);
    SwitchToTab(win, current);
}

// Puts a closing tab's webview away instead of closing it. The page is left for
// kParkedUrl, which unloads it. A page that asks whether to leave stays where it is
// and is only kept quiet.
void ParkClosedTab(Tab&& tab, std::wstring url) {
    tab.controller->put_IsVisible(FALSE);
    SetTabMuted(tab, true);
    tab.parked = true;
    tab.webview->Navigate(kParkedUrl);
    g_app.closedTabs.push_back({.url = std::move(url), .tab = std::move(tab)});

    std::size_t kept = 0;
    for (ClosedTab& closed : g_app.closedTabs | std::views::reverse) {
        if (closed.tab.controller && ++kept > kClosedTabsKept) DiscardClosedWebView(closed);
    }
}

} // namespace

void DiscardClosedWebView(ClosedTab& closed) {
    closed.tab.CloseWebView();
    closed.tab.favicon.reset();
}

void UpdateTabTitle(BrowserWindow& win, int index, std::wstring_view title) {
    if (!IsTabIndex(win, index)) return;
    Tab& tab = win.tabs[index];
    tab.title = title;
    if (tab.button) ::SetWindowTextW(tab.button.get(), tab.title.c_str());
    if (index == win.active) UpdateFrameTitle(win);
}

void SwitchToTab(BrowserWindow& win, int index) {
    if (win.isFullscreen) return; // the active tab's controller is parented elsewhere right now
    if (!IsTabIndex(win, index) || win.active == index) return;

    HideAllWebViews(win);
    win.active = index;
    ShowTabInStrip(win, index);
    LayoutActiveWebView(win);
    UpdateUrlBox(win);
    UpdateReloadButton(win);
    UpdateFrameTitle(win);
    HideSuggestions(win);

    for (int i = 0; i < TabCount(win); ++i) {
        if (win.tabs[i].button) ::SendMessageW(win.tabs[i].button.get(), BM_SETSTATE, i == win.active, 0);
    }

    Tab& tab = win.tabs[index];
    if (tab.controller) {
        MoveFocusToWebView(tab);
    } else {
        // Webviews are only made for tabs that get looked at; this one's turn has come.
        ::PostMessageW(win.client, kMsgInitWebView, 0, 0);
    }
}

void NextTab(BrowserWindow& win) {
    if (win.tabs.empty()) return;
    SwitchToTab(win, (win.active + 1) % TabCount(win));
}

void PrevTab(BrowserWindow& win) {
    if (win.tabs.empty()) return;
    SwitchToTab(win, (win.active - 1 + TabCount(win)) % TabCount(win));
}

void NewTab(BrowserWindow& win, std::wstring_view url) {
    const int index = TabCount(win);
    AddTab(win, kNewTabTitle).pendingUrl = url;
    LayoutToolbarAndTabs(win);
    SwitchToTab(win, index);
    ::PostMessageW(win.client, kMsgInitWebView, 0, 0);
}

void RemoveTab(BrowserWindow& win, int index) {
    if (win.isFullscreen && win.tabs[index].id == win.fullscreenTabId) LeaveVideoFullscreen(win);

    Tab tab = std::move(win.tabs[index]);
    win.tabs.erase(win.tabs.begin() + index);
    tab.button.reset();

    // The page it was on; for a tab never looked at, where it was headed.
    std::wstring closingUrl = !tab.lastUrl.empty() ? tab.lastUrl
                              : tab.webview        ? SourceOf(tab.webview.Get())
                                                   : tab.pendingUrl;

    if (!closingUrl.empty() && tab.controller && tab.webview && !win.closing) {
        // A tab that got as far as showing a page can be reopened, so it is put away
        // rather than closed.
        ParkClosedTab(std::move(tab), std::move(closingUrl));
        return;
    }
    // Reopening it starts over at its address.
    if (!closingUrl.empty()) g_app.closedTabs.push_back({.url = std::move(closingUrl)});
    // A webview can't be kept past the window it is in: WebView2 closes it when that
    // window is destroyed. The webviews of a window closed for good go with it, which
    // frees all they held at once. Any other webview goes with `tab`.
    if (win.closing) win.discardedTabs.push_back(std::move(tab));
}

void CloseTab(BrowserWindow& win, int index) {
    if (!IsTabIndex(win, index)) return;
    // A window's last tab takes the window with it.
    if (win.tabs.size() == 1) {
        CloseBrowserWindow(win);
        return;
    }
    RemoveTab(win, index);
    RefreshAfterTabRemoved(win, index);
}

void CloseActiveTab(BrowserWindow& win) {
    CloseTab(win, win.active);
}

void ReopenLastClosedTab(BrowserWindow& win) {
    if (g_app.closedTabs.empty()) return;
    ClosedTab closed = std::move(g_app.closedTabs.back());
    g_app.closedTabs.pop_back();
    if (!closed.tab.controller) {
        NewTab(win, closed.url);
        return;
    }

    // Its webview is still there: the tab comes back as it was, and a step back from
    // the page it was put away on is the page it was closed on.
    closed.tab.controller->put_ParentWindow(win.client);
    const int index = TabCount(win);
    win.tabs.push_back(std::move(closed.tab));
    LayoutToolbarAndTabs(win);
    SwitchToTab(win, index);

    Tab& tab = win.tabs[index];
    tab.controller->NotifyParentWindowPositionChanged();
    SetTabMuted(tab, false);
    // Whether it is making sound may have changed while it was closed, when its
    // events couldn't find it in any window.
    UpdateTabAudio(tab);
    // Anywhere else and it never left its page, or is still on the way out, which
    // the tab's navigation handlers see to.
    if (SourceOf(tab.webview.Get()) == kParkedUrl) {
        tab.parked = false;
        tab.webview->GoBack();
        ShowPageUrl(win, closed.url);
    }
}

// Ctrl+H: the history page, in the tab that already shows it if there is one.
void OpenHistory(BrowserWindow& win) {
    const auto showsHistory = [](const Tab& tab) { return tab.lastUrl == kHistoryUrl && tab.webview; };
    if (const auto found = std::ranges::find_if(win.tabs, showsHistory); found != win.tabs.end()) {
        SwitchToTab(win, static_cast<int>(found - win.tabs.begin()));
        found->webview->Reload(); // it may have been made a while ago
        return;
    }
    // An address bar that is being typed in would otherwise keep the keyboard and its text.
    if (::GetFocus() == win.urlEdit) ::SetFocus(win.client);
    NewTab(win, kHistoryUrl);
}

// A tab can be dragged along its strip to reorder it, onto another window's strip
// to move it there, or away from every strip to give it a window of its own.
void MoveTab(BrowserWindow& src, int index, BrowserWindow& dst, int insertAt) {
    if (!IsTabIndex(src, index)) return;
    const bool sameWindow = &dst == &src;
    insertAt = std::clamp(insertAt, 0, TabCount(dst) - (sameWindow ? 1 : 0));

    if (sameWindow) {
        if (insertAt == index) return;
        const int activeId = src.active >= 0 ? src.tabs[src.active].id : 0;
        const auto first = src.tabs.begin();
        if (insertAt < index) std::rotate(first + insertAt, first + index, first + index + 1);
        else std::rotate(first + index, first + index + 1, first + insertAt + 1);
        src.active = TabIndexById(src, activeId);
        LayoutToolbarAndTabs(src); // renumbers and repositions the tab buttons
        ShowTabInStrip(src, insertAt);
        for (const Tab& tab : src.tabs) {
            if (tab.button) ::InvalidateRect(tab.button.get(), nullptr, TRUE);
        }
        return;
    }

    Tab tab = std::move(src.tabs[index]);
    src.tabs.erase(src.tabs.begin() + index);

    // Tab buttons are children of their window's client, so dst makes a fresh one
    // on layout. The webview itself just moves to the new parent.
    tab.button.reset();
    if (tab.controller) {
        tab.controller->put_IsVisible(FALSE);
        tab.controller->put_ParentWindow(dst.client);
    }

    if (src.tabs.empty()) CloseBrowserWindow(src); // its last tab was dragged away
    else RefreshAfterTabRemoved(src, index);

    dst.tabs.insert(dst.tabs.begin() + insertAt, std::move(tab));
    dst.active = -1; // indices shifted; SwitchToTab below re-syncs everything
    LayoutToolbarAndTabs(dst);
    SwitchToTab(dst, insertAt);
    if (Tab& moved = dst.tabs[insertAt]; moved.controller) moved.controller->NotifyParentWindowPositionChanged();
}

} // namespace exodium
