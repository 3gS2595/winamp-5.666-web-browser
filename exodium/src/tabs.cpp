// Opening, switching, selecting, closing, reopening and moving tabs.
#include "browser.h"

#include <algorithm>
#include <iterator>
#include <ranges>
#include <utility>

namespace exodium {

namespace {

[[nodiscard]] int TabCount(const BrowserWindow& win) noexcept {
    return static_cast<int>(win.tabs.size());
}

[[nodiscard]] int SelectedCount(const BrowserWindow& win) noexcept {
    return static_cast<int>(std::ranges::count(win.tabs, true, &Tab::selected));
}

[[nodiscard]] TabSelection CurrentSelection(const BrowserWindow& win) {
    TabSelection selected;
    for (const Tab& tab : win.tabs) selected.push_back(tab.selected);
    return selected;
}

// Selects the tabs `selected` says, and repaints those whose look that changes. A tab
// only looks selected while others are too; on its own it is just the active tab.
void ApplySelection(BrowserWindow& win, const TabSelection& selected) {
    const bool severalBefore = SelectedCount(win) > 1;
    const bool severalAfter = std::ranges::count(selected, true) > 1;
    for (int i = 0; i < TabCount(win) && i < static_cast<int>(selected.size()); ++i) {
        Tab& tab = win.tabs[i];
        const bool lookedSelected = tab.selected && severalBefore;
        tab.selected = selected[i];
        if (lookedSelected != (tab.selected && severalAfter) && tab.button) {
            ::InvalidateRect(tab.button.get(), nullptr, FALSE);
        }
    }
}

[[nodiscard]] bool IsOneOf(const std::vector<int>& tabIds, int tabId) noexcept {
    return std::ranges::find(tabIds, tabId) != tabIds.end();
}

// A tab leaves its window, or its place in it. As in Chrome, the tabs opened from it
// count from then on as opened from the tab it was opened from.
void PassOnOpener(BrowserWindow& win, const Tab& leaving) {
    for (Tab& tab : win.tabs) {
        if (tab.openerId == leaving.id) tab.openerId = leaving.openerId == tab.id ? 0 : leaving.openerId;
    }
}

// The id of the tab that is to be the active one once the tabs in tabIds have gone:
// the active tab if it stays, or else the nearest one that does, left of it first.
[[nodiscard]] int ActiveAfterRemoving(const BrowserWindow& win, const std::vector<int>& tabIds) {
    std::vector<bool> staying;
    for (const Tab& tab : win.tabs) staying.push_back(!IsOneOf(tabIds, tab.id));
    const int index = NearestWanted(staying, win.active);
    return index >= 0 ? win.tabs[index].id : 0;
}

// Re-syncs a window after tabs were taken out of it. It must still have tabs, among
// them activeId, which becomes the active one.
void RefreshAfterTabsRemoved(BrowserWindow& win, int activeId) {
    win.active = -1; // indices shifted; SwitchToTab below re-syncs everything
    LayoutToolbarAndTabs(win);
    SwitchToTab(win, std::max(0, TabIndexById(win, activeId)));
}

// Puts a closing tab's webview away instead of closing it. The page is left for
// kParkedUrl, which unloads it. A page that asks whether to leave stays where it is
// and is only kept quiet.
void ParkClosedTab(Tab&& tab, ClosedTab&& closed) {
    tab.controller->put_IsVisible(FALSE);
    SetTabMuted(tab, true);
    tab.parked = true;
    tab.webview->Navigate(kParkedUrl);
    closed.tab = std::move(tab);
    g_app.closedTabs.push_back(std::move(closed));

    std::size_t kept = 0;
    for (ClosedTab& earlier : g_app.closedTabs | std::views::reverse) {
        if (earlier.tab.controller && ++kept > kClosedTabsKept) DiscardClosedWebView(earlier);
    }
    TrimClosedTabs();
}

// The pages a closing tab is reopened with: those it had, or else the page it was on.
[[nodiscard]] HistoryCopy PagesToReopen(const Tab& tab, const std::wstring& url) {
    if (!tab.pages.entries.empty()) return tab.pages;
    const std::wstring title = tab.title == kNewTabTitle ? std::wstring() : tab.title;
    return {.entries = {{.url = url, .title = title}}};
}

// Puts a closed tab back at the end of a window's strip, out of view.
void PutBackClosedTab(BrowserWindow& win, ClosedTab&& closed) {
    // Put away on kParkedUrl, its webview lost the pages that were ahead of the one it was
    // on, as any page does that goes somewhere new. Its pages have them, and it is made
    // again from them, Forward and all.
    if (closed.tab.controller && closed.pages.shown + 1 < closed.pages.entries.size()) DiscardClosedWebView(closed);
    if (!closed.tab.controller) {
        Tab& tab = AddTab(win, closed.title.empty() ? closed.url : closed.title);
        tab.pendingUrl = closed.url;
        GiveTabPages(tab, std::move(closed.pages));
        return;
    }
    // Its webview is still there: the tab comes back as it was, and a step back from
    // the page it was put away on is the page it was closed on.
    closed.tab.controller->put_ParentWindow(win.client);
    Tab& tab = win.tabs.emplace_back(std::move(closed.tab));
    tab.controller->NotifyParentWindowPositionChanged();
    tab.seenAt = ::GetTickCount64(); // not left out of view all the while it was closed
    tab.askingToSleep = false;       // whatever its page was asked then, the answer found no tab
    SetTabMuted(tab, tab.muted);     // heard again, unless the user had muted it
    // Whether it is making sound may have changed while it was closed, when its
    // events couldn't find it in any window.
    UpdateTabAudio(tab);
    // Anywhere else and it never left its page, or is still on the way out, which
    // the tab's navigation handlers see to.
    if (SourceOf(tab.webview.Get()) == kParkedUrl) {
        tab.parked = false;
        tab.webview->GoBack();
    }
}

// Puts closed tabs back at the end of a window's strip, side by side, and brings the one
// at `shown` among them into view.
void PutBackClosedTabs(BrowserWindow& win, std::vector<ClosedTab>&& closed, std::size_t shown) {
    if (closed.empty()) return;
    shown = std::min(shown, closed.size() - 1);
    const int first = TabCount(win);
    const std::wstring shownUrl = closed[shown].url;
    for (ClosedTab& tab : closed) PutBackClosedTab(win, std::move(tab));
    LayoutToolbarAndTabs(win);
    const int index = first + static_cast<int>(shown);
    SwitchToTab(win, index);
    // Its webview may be on its way back from the page it was put away on, and say that is where it is.
    if (const Tab* tab = ActiveTab(win); tab && tab->webview && SourceOf(tab->webview.Get()) == kParkedUrl) {
        ShowPageUrl(win, shownUrl);
    }
}

// A window closed whole comes back whole: in a window of its own at the place it had, or
// in `win` if all it has is a tab that hasn't been anywhere, which goes.
void ReopenClosedWindow(BrowserWindow& win, const ClosedWindow& closedWindow, std::vector<ClosedTab>&& closed) {
    std::ranges::sort(closed, {}, &ClosedTab::indexInWindow);
    // The tab that was in view, or the nearest one that came back after it.
    std::size_t shown = 0;
    while (shown + 1 < closed.size() && closed[shown].indexInWindow < closedWindow.activeIndex) ++shown;

    if (IsBlankWindow(win)) {
        Tab blank = std::move(win.tabs.front());
        win.tabs.clear();
        win.active = -1;
        KeepWebViewForDownloads(blank);
        PutBackClosedTabs(win, std::move(closed), shown);
        return;
    }
    const RECT place = KeptOnScreen(closedWindow.place);
    BrowserWindow* const host = CreateBrowserWindow(true, &place, true);
    if (!host) return;
    PutBackClosedTabs(*host, std::move(closed), shown);
    ::ShowWindow(host->frame, closedWindow.maximized ? SW_SHOWMAXIMIZED : SW_SHOWNA);
    ::SetForegroundWindow(host->frame);
}

} // namespace

void TrimClosedTabs() {
    std::vector<ClosedTab>& closed = g_app.closedTabs;
    std::size_t items = 0;
    for (std::size_t i = closed.size(); i-- > 0;) {
        // The tabs of a window closed whole are one item.
        const bool sameAsNext = i + 1 < closed.size() && closed[i].window && closed[i].window == closed[i + 1].window;
        if (sameAsNext || ++items <= kClosedItemsKept) continue;
        for (std::size_t j = 0; j <= i; ++j) DiscardClosedWebView(closed[j]);
        closed.erase(closed.begin(), closed.begin() + static_cast<std::ptrdiff_t>(i + 1));
        return;
    }
}

bool IsBlankTab(const Tab& tab) {
    // Opened on the home page, which may have sent it on, and gone nowhere else since.
    return (tab.pendingUrl.empty() || tab.pendingUrl == HomePage()) && tab.pages.entries.size() <= 1 &&
           !tab.historyCopy && !tab.forClaude;
}

bool IsBlankWindow(const BrowserWindow& win) {
    return win.tabs.size() == 1 && !win.isFullscreen && IsBlankTab(win.tabs.front());
}

void CloseTabs(BrowserWindow& win, const std::vector<int>& tabIds) {
    if (std::ranges::all_of(win.tabs, [&](const Tab& tab) { return IsOneOf(tabIds, tab.id); })) {
        CloseBrowserWindow(win);
        return;
    }
    const int activeId = ActiveAfterRemoving(win, tabIds);
    for (const int tabId : tabIds) {
        if (const int index = TabIndexById(win, tabId); index >= 0) RemoveTab(win, index);
    }
    RefreshAfterTabsRemoved(win, activeId);
}

void DiscardClosedWebView(ClosedTab& closed) {
    KeepWebViewForDownloads(closed.tab);
    closed.tab.CloseWebView();
    closed.tab.favicon.reset();
}

void UpdateTabTitle(BrowserWindow& win, int index, std::wstring_view title) {
    if (!IsTabIndex(win, index)) return;
    Tab& tab = win.tabs[index];
    tab.title = title;
    if (tab.button) ::SetWindowTextW(tab.button.get(), tab.title.c_str());
    if (index == win.active) UpdateFrameTitle(win);
    if (tab.playingAudio) FollowPageSound(); // Winamp may be showing it
}

void SwitchToTab(BrowserWindow& win, int index) {
    if (win.isFullscreen || !IsTabIndex(win, index)) return;
    TabSelection only(win.tabs.size(), false);
    only[index] = true;
    ApplySelection(win, only);
    win.anchorTabId = win.tabs[index].id;
    ActivateTab(win, index);
}

bool ToggleTabSelected(BrowserWindow& win, int index) {
    if (win.isFullscreen || !IsTabIndex(win, index)) return false;
    win.anchorTabId = win.tabs[index].id;
    TabSelection selected = CurrentSelection(win);
    if (!selected[index]) {
        selected[index] = true;
        ApplySelection(win, selected);
        ActivateTab(win, index);
        return true;
    }
    // The last of them stays: the active tab is always selected.
    if (SelectedCount(win) <= 1) return true;
    selected[index] = false;
    if (index == win.active) ActivateTab(win, NearestWanted(selected, index));
    ApplySelection(win, selected);
    return false;
}

void SelectTabRange(BrowserWindow& win, int index, bool add) {
    if (win.isFullscreen || !IsTabIndex(win, index)) return;
    int anchor = TabIndexById(win, win.anchorTabId);
    if (anchor < 0) {
        anchor = IsTabIndex(win, win.active) ? win.active : index;
        win.anchorTabId = win.tabs[anchor].id;
    }
    ApplySelection(win, SelectRange(CurrentSelection(win), anchor, index, add));
    ActivateTab(win, index);
}

std::vector<int> SelectedTabIds(const BrowserWindow& win) {
    std::vector<int> tabIds;
    for (const Tab& tab : win.tabs) {
        if (tab.selected) tabIds.push_back(tab.id);
    }
    if (tabIds.empty() && IsTabIndex(win, win.active)) tabIds.push_back(win.tabs[win.active].id);
    return tabIds;
}

bool ShowsAsSelected(const BrowserWindow& win, int index) noexcept {
    return IsTabIndex(win, index) && win.tabs[index].selected && SelectedCount(win) > 1;
}

void ActivateTab(BrowserWindow& win, int index) {
    if (win.isFullscreen) return; // the active tab's controller is parented elsewhere right now
    if (!IsTabIndex(win, index) || win.active == index) return;

    // The tab that goes out of view was in view until now, and so is the one that comes in.
    const ULONGLONG now = ::GetTickCount64();
    if (Tab* const leaving = ActiveTab(win)) leaving->seenAt = now;
    win.tabs[index].seenAt = now;

    HideAllWebViews(win);
    win.active = index;
    ShowTabInStrip(win, index);
    LayoutActiveWebView(win);
    UpdateUrlBox(win);
    UpdateReloadButton(win);
    UpdateZoomButtons(win);
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

int ActiveTabId(const BrowserWindow& win) noexcept {
    return IsTabIndex(win, win.active) ? win.tabs[win.active].id : 0;
}

int LinkTabIndex(const BrowserWindow& win, bool foreground) {
    std::vector<int> openers;
    for (const Tab& tab : win.tabs) openers.push_back(tab.openerId ? TabIndexById(win, tab.openerId) : -1);
    return LinkTabPlace(openers, win.active, foreground);
}

void ForgetOpeners(BrowserWindow& win) {
    for (Tab& tab : win.tabs) tab.openerId = 0;
}

void NoteUserSwitch(BrowserWindow& win, int fromTabId) {
    const Tab* const to = ActiveTab(win);
    const int from = TabIndexById(win, fromTabId);
    if (!to || to->id == fromTabId) return;
    if (SwitchForgetsOpeners(fromTabId, from >= 0 ? win.tabs[from].openerId : 0, to->id, to->openerId)) {
        ForgetOpeners(win);
    }
}

void NoteTypedNavigation(BrowserWindow& win) {
    const Tab* const tab = ActiveTab(win);
    if (!tab) return;
    BOOL canGoBack = FALSE;
    if (tab->webview) tab->webview->get_CanGoBack(&canGoBack);
    const bool quickLook = win.active == TabCount(win) - 1 && !canGoBack;
    if (!quickLook) ForgetOpeners(win);
}

void NextTab(BrowserWindow& win) {
    if (win.tabs.empty()) return;
    const int fromTabId = ActiveTabId(win);
    SwitchToTab(win, (win.active + 1) % TabCount(win));
    NoteUserSwitch(win, fromTabId);
}

void PrevTab(BrowserWindow& win) {
    if (win.tabs.empty()) return;
    const int fromTabId = ActiveTabId(win);
    SwitchToTab(win, (win.active - 1 + TabCount(win)) % TabCount(win));
    NoteUserSwitch(win, fromTabId);
}

void SwitchToTabByNumber(BrowserWindow& win, int number) {
    if (win.tabs.empty() || number < 1) return;
    // Nine is the last tab, however many there are; a number past the end is no tab's.
    const int index = number >= 9 ? TabCount(win) - 1 : number - 1;
    if (!IsTabIndex(win, index) || index == win.active) return;
    const int fromTabId = ActiveTabId(win);
    SwitchToTab(win, index);
    NoteUserSwitch(win, fromTabId);
}

std::wstring TabUrl(const Tab& tab) {
    // A tab still making the history it was given is on a stand-in, and says where it is headed.
    if (tab.webview && !tab.copyingHistory) {
        std::wstring source = SourceOf(tab.webview.Get());
        // A webview that has shown nothing yet is on a blank page of its own.
        if (!source.empty() && source != L"about:blank") return source;
    }
    return !tab.lastUrl.empty() ? tab.lastUrl : tab.pendingUrl;
}

void NewTab(BrowserWindow& win, std::wstring_view url) {
    const int index = TabCount(win);
    AddTab(win, kNewTabTitle).pendingUrl = url;
    LayoutToolbarAndTabs(win);
    SwitchToTab(win, index);
    ::PostMessageW(win.client, kMsgInitWebView, 0, 0);
}

void RemoveTab(BrowserWindow& win, int index, const ClosedWith& with) {
    if (win.isFullscreen && win.tabs[index].id == win.fullscreenTabId) LeaveVideoFullscreen(win);

    Tab tab = std::move(win.tabs[index]);
    win.tabs.erase(win.tabs.begin() + index);
    tab.button.reset();
    tab.devtools = {}; // its DevTools, if they were docked beside it, close with it
    EndHistoryCopy(tab);
    PassOnOpener(win, tab);
    tab.openerId = 0; // reopened, it starts afresh

    // The page it was on; for a tab never looked at, where it was headed.
    std::wstring closingUrl = !tab.lastUrl.empty() ? tab.lastUrl
                              : tab.webview        ? SourceOf(tab.webview.Get())
                                                   : tab.pendingUrl;

    ClosedTab closed{.url = closingUrl,
                     .title = tab.title,
                     .pages = PagesToReopen(tab, closingUrl),
                     .window = with.window,
                     .indexInWindow = with.index};

    // Tools on the debugging port see every webview there is. One put away would show up
    // to them as a page, and one closed by a tool would never be seen to go. A private
    // window's tab isn't kept in any way: closed, it is gone.
    if (!closingUrl.empty() && tab.controller && tab.webview && !win.closing && g_app.debuggingPort == 0 &&
        !tab.isPrivate) {
        // A tab that got as far as showing a page can be reopened, so it is put away
        // rather than closed.
        ParkClosedTab(std::move(tab), std::move(closed));
        return;
    }
    // Reopening it makes its pages again. Claude's tabs aren't the user's to reopen:
    // they close when its session ends, and Ctrl+Shift+T would bring them back instead
    // of the tab the user closed.
    if (!closingUrl.empty() && !tab.forClaude && !tab.isPrivate) {
        g_app.closedTabs.push_back(std::move(closed));
        TrimClosedTabs();
    }
    // A webview can't be kept past the window it is in: WebView2 closes it when that
    // window is destroyed. The webviews of a window closed for good go with it, which
    // frees all they held at once. Any other webview goes with `tab`.
    if (win.closing) win.discardedTabs.push_back(std::move(tab));
    else KeepWebViewForDownloads(tab);
}

void CloseTab(BrowserWindow& win, int index) {
    if (IsTabIndex(win, index)) CloseTabs(win, {win.tabs[index].id});
}

// As in Chrome, Ctrl+W and the close button take every selected tab.
void CloseActiveTab(BrowserWindow& win) {
    const Tab* const active = ActiveTab(win);
    if (!active) return;
    CloseTabs(win, active->selected ? SelectedTabIds(win) : std::vector{active->id});
}

void ReopenLastClosedTab(BrowserWindow& win) {
    // The tabs kept to be brought back are the other windows', and stay out of a private one.
    if (g_app.closedTabs.empty() || win.isPrivate) return;
    // A window closed whole has its tabs side by side at the end.
    const std::shared_ptr<const ClosedWindow> window = g_app.closedTabs.back().window;
    std::vector<ClosedTab> closed;
    do {
        closed.push_back(std::move(g_app.closedTabs.back()));
        g_app.closedTabs.pop_back();
    } while (window && !g_app.closedTabs.empty() && g_app.closedTabs.back().window == window);

    if (window) ReopenClosedWindow(win, *window, std::move(closed));
    else PutBackClosedTabs(win, std::move(closed), 0);
}

namespace {

// One of the plugin's own pages, in the tab that already shows it if there is one.
void OpenPluginPage(BrowserWindow& win, std::wstring_view url) {
    const auto showsIt = [url](const Tab& tab) { return tab.lastUrl == url && tab.webview; };
    if (const auto found = std::ranges::find_if(win.tabs, showsIt); found != win.tabs.end()) {
        SwitchToTab(win, static_cast<int>(found - win.tabs.begin()));
        found->webview->Reload(); // it may have been made a while ago
        return;
    }
    // An address bar that is being typed in would otherwise keep the keyboard and its text.
    if (::GetFocus() == win.urlEdit) ::SetFocus(win.client);
    NewTab(win, url);
}

} // namespace

void OpenHistory(BrowserWindow& win) {
    OpenPluginPage(win, kHistoryUrl);
}

void OpenBookmarks(BrowserWindow& win) {
    OpenPluginPage(win, kBookmarksUrl);
}

// Tabs can be dragged along their strip to reorder them, onto another window's strip
// to move them there, or away from every strip to give them a window of their own.
// Several selected tabs go together, and land side by side.
void MoveTabs(BrowserWindow& src, const std::vector<int>& tabIds, BrowserWindow& dst, int insertAt, int activeTabId) {
    std::vector<bool> moving;
    for (const Tab& tab : src.tabs) moving.push_back(IsOneOf(tabIds, tab.id));
    if (std::ranges::none_of(moving, std::identity{})) return;

    for (int i = 0; i < TabCount(src); ++i) {
        if (moving[i]) PassOnOpener(src, src.tabs[i]);
    }

    if (&dst == &src) {
        const int activeId = IsTabIndex(src, src.active) ? src.tabs[src.active].id : 0;
        std::vector<Tab> reordered;
        reordered.reserve(src.tabs.size());
        for (const int from : GatheredOrder(moving, insertAt)) reordered.push_back(std::move(src.tabs[from]));
        src.tabs = std::move(reordered);
        src.active = TabIndexById(src, activeId);
        LayoutToolbarAndTabs(src); // renumbers and repositions the tab buttons
        ShowTabInStrip(src, std::max(0, TabIndexById(src, activeTabId)));
        for (const Tab& tab : src.tabs) {
            if (tab.button) ::InvalidateRect(tab.button.get(), nullptr, TRUE);
        }
        return;
    }

    const int srcActiveId = ActiveAfterRemoving(src, tabIds);
    std::vector<Tab> moved, staying;
    for (int i = 0; i < TabCount(src); ++i) (moving[i] ? moved : staying).push_back(std::move(src.tabs[i]));
    src.tabs = std::move(staying);

    // Tab buttons are children of their window's client, so dst makes fresh ones on
    // layout. The webviews themselves just move to the new parent.
    for (Tab& tab : moved) {
        tab.button.reset();
        tab.openerId = 0; // the tab it was opened from stays behind
        if (tab.controller) {
            tab.controller->put_IsVisible(FALSE);
            tab.controller->put_ParentWindow(dst.client);
        }
        MoveDevToolsTo(tab, dst.client); // docked beside its page, they go where it goes
    }

    if (src.tabs.empty()) CloseBrowserWindow(src); // its last tabs were dragged away
    else RefreshAfterTabsRemoved(src, srcActiveId);

    insertAt = std::clamp(insertAt, 0, TabCount(dst));
    const int count = static_cast<int>(moved.size());
    dst.tabs.insert(dst.tabs.begin() + insertAt, std::make_move_iterator(moved.begin()),
                    std::make_move_iterator(moved.end()));
    dst.active = -1; // indices shifted; SwitchToTab below re-syncs everything
    LayoutToolbarAndTabs(dst);
    const int activeIndex = TabIndexById(dst, activeTabId);
    SwitchToTab(dst, activeIndex >= 0 ? activeIndex : insertAt);

    TabSelection selected(dst.tabs.size(), false);
    for (int i = insertAt; i < insertAt + count; ++i) {
        selected[i] = true;
        if (dst.tabs[i].controller) dst.tabs[i].controller->NotifyParentWindowPositionChanged();
        DevToolsMoved(dst.tabs[i]);
    }
    ApplySelection(dst, selected);
}

} // namespace exodium
