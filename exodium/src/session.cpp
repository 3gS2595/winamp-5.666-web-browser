// The windows and tabs kept from one run of Winamp to the next, so that the browser picks
// up where it left off. The session file (session_file.h) has every window that is open,
// where it is and how it is shown, its tabs, and for each tab the pages its Back and
// Forward lead to; and the tabs and windows closed that Ctrl+Shift+T can still bring back.
//
// It is written every two seconds, when anything in it has changed, and once more as
// Winamp exits, before the browser is taken down; a Winamp that crashes loses two seconds
// of it at most. A tab's pages are asked of its webview whenever its history changes
// (NoteTabHistory), so they are there to be written at any time without waiting on one.
//
// At start-up the windows come back where they were, each with its tabs, the one in view
// in view. A tab is given its pages (GiveTabPages): its webview, once it is made, makes
// them its history the way a tab opened by Ctrl+click on Back does (back_forward.cpp), and
// shows the page it was on. As at any time, only the tabs in view get a webview at once;
// the rest are made when they are first looked at. The windows are made out of sight and
// come up once Winamp has finished starting, which would bury them otherwise, as a link's
// window would be. With Settings::restoreWindows off, the windows aren't brought back, and
// are kept as windows closed whole instead, for Ctrl+Shift+T.
#include "browser.h"

#include "session_file.h"
#include "text_util.h"

#include <wrl/event.h>

#include <algorithm>
#include <functional>
#include <map>
#include <utility>

namespace exodium {

namespace {

using Microsoft::WRL::Callback;
using DevToolsHandler = ICoreWebView2CallDevToolsProtocolMethodCompletedHandler;

constexpr UINT kSaveEveryMs = 2000;
// How often to look whether Winamp has finished starting, while windows wait for it.
constexpr UINT kShowLookMs = 100;
// How long after the windows brought back came up a request for a window on the home page
// is taken to be the one that started Winamp.
constexpr ULONGLONG kStartRequestMs = 10000;

// What the session file was last written with.
std::wstring g_written;

// The windows brought back, while they wait for Winamp to finish starting, and how each is
// to come up.
struct WaitingWindow {
    HWND frame = nullptr;
    WindowShow show = WindowShow::kNormal;
    int stacking = 0;
};
std::vector<WaitingWindow> g_waiting;
bool g_showPending = false;
// When they came up, and the one in front; 0 once a request from outside has had them.
ULONGLONG g_shownAt = 0;
HWND g_shownFront = nullptr;

[[nodiscard]] HWND FirstClient() noexcept {
    return g_app.windows.empty() ? nullptr : g_app.windows.front()->client;
}

[[nodiscard]] SavedPlace ToSaved(const RECT& place) noexcept {
    return {place.left, place.top, place.right, place.bottom};
}

[[nodiscard]] RECT ToRect(const SavedPlace& place) noexcept {
    return {place.left, place.top, place.right, place.bottom};
}

// ---- the browser as the session file has it ----

[[nodiscard]] WindowShow ShowOf(const BrowserWindow& win) noexcept {
    if (!win.frame || !::IsWindowVisible(win.frame)) return WindowShow::kHidden;
    if (win.minimized || ::IsIconic(win.frame)) return WindowShow::kMinimized;
    return IsMaximized(win) ? WindowShow::kMaximized : WindowShow::kNormal;
}

// A tab's pages, or the page it is on or headed for alone. Nothing for a tab that hasn't
// been anywhere but the home page, which comes back as a new tab.
[[nodiscard]] HistoryCopy PagesOf(const Tab& tab) {
    if (IsBlankTab(tab)) return {};
    if (!tab.pages.entries.empty()) return tab.pages;
    const std::wstring& url = !tab.lastUrl.empty() ? tab.lastUrl : tab.pendingUrl;
    if (url.empty()) return {};
    return {.entries = {{.url = url, .title = tab.title == kNewTabTitle ? std::wstring() : tab.title}}};
}

[[nodiscard]] HistoryCopy PagesOf(const ClosedTab& closed) {
    if (!closed.pages.entries.empty()) return closed.pages;
    return {.entries = {{.url = closed.url, .title = closed.title == kNewTabTitle ? std::wstring() : closed.title}}};
}

// Each window's place among the others from the front, by its frame.
[[nodiscard]] std::map<HWND, int> Stacking() {
    std::map<HWND, int> stacking;
    for (const auto& win : g_app.windows) {
        if (win->frame) stacking[win->frame] = 0;
    }
    int next = 0;
    for (HWND window = ::GetTopWindow(nullptr); window; window = ::GetWindow(window, GW_HWNDNEXT)) {
        if (const auto found = stacking.find(window); found != stacking.end()) found->second = next++;
    }
    return stacking;
}

[[nodiscard]] SavedWindow SavedWindowOf(const BrowserWindow& win, int stacking) {
    SavedWindow saved{.show = ShowOf(win), .place = ToSaved(NormalPlace(win)), .stacking = stacking};
    for (int i = 0; i < static_cast<int>(win.tabs.size()); ++i) {
        const Tab& tab = win.tabs[i];
        // Claude's tabs go with its session, and aren't the user's to have back; and nothing
        // of a private window is kept.
        if (tab.forClaude || tab.isPrivate) continue;
        if (i == win.active) saved.active = saved.tabs.size();
        saved.tabs.push_back(PagesOf(tab));
    }
    return saved;
}

[[nodiscard]] SavedSession CurrentSession() {
    SavedSession session;
    const std::map<HWND, int> stacking = Stacking();
    for (const auto& win : g_app.windows) {
        if (win->closing || win->isPrivate) continue;
        const auto found = stacking.find(win->frame);
        SavedWindow saved = SavedWindowOf(*win, found == stacking.end() ? 0 : found->second);
        // Only the first window stays when it is out of sight, or has nothing of the user's.
        const bool first = win.get() == g_app.windows.front().get();
        if (!first && (saved.show == WindowShow::kHidden || saved.tabs.empty())) continue;
        session.windows.push_back(std::move(saved));
    }

    const std::vector<ClosedTab>& closed = g_app.closedTabs;
    for (std::size_t i = 0; i < closed.size();) {
        const std::shared_ptr<const ClosedWindow> window = closed[i].window;
        SavedClosed& item = session.closed.emplace_back(SavedClosed{.wholeWindow = window != nullptr});
        if (!window) {
            item.window.tabs.push_back(PagesOf(closed[i++]));
            continue;
        }
        std::vector<const ClosedTab*> tabs;
        for (; i < closed.size() && closed[i].window == window; ++i) tabs.push_back(&closed[i]);
        std::ranges::sort(tabs, {}, &ClosedTab::indexInWindow);
        item.window.show = window->maximized ? WindowShow::kMaximized : WindowShow::kNormal;
        item.window.place = ToSaved(window->place);
        for (const ClosedTab* tab : tabs) item.window.tabs.push_back(PagesOf(*tab));
        // The tab that was in view, or the nearest one kept after it, as ReopenLastClosedTab has it.
        while (item.window.active + 1 < tabs.size() && tabs[item.window.active]->indexInWindow < window->activeIndex) {
            ++item.window.active;
        }
    }
    return session;
}

// ---- the browser from the session file ----

// Gives a window the tabs saved of one, in place of those it has.
void FillWindow(BrowserWindow& win, const SavedWindow& saved) {
    for (Tab& tab : win.tabs) KeepWebViewForDownloads(tab);
    win.tabs.clear();
    win.active = -1;
    for (const HistoryCopy& pages : saved.tabs) {
        Tab& tab = AddTab(win, kNewTabTitle);
        tab.pendingUrl = HomePage(); // a new tab, unless it has pages
        GiveTabPages(tab, pages);
    }
    if (win.tabs.empty()) AddTab(win, kNewTabTitle).pendingUrl = HomePage();
    LayoutToolbarAndTabs(win);
    SwitchToTab(win, static_cast<int>(std::min(saved.active, win.tabs.size() - 1)));
}

// Keeps a tab or window closed in an earlier session for Ctrl+Shift+T.
void KeepClosed(const SavedClosed& item) {
    std::shared_ptr<const ClosedWindow> window;
    if (item.wholeWindow) {
        window =
            std::make_shared<const ClosedWindow>(ClosedWindow{.place = ToRect(item.window.place),
                                                              .maximized = item.window.show == WindowShow::kMaximized,
                                                              .activeIndex = static_cast<int>(item.window.active)});
    }
    for (std::size_t i = 0; i < item.window.tabs.size(); ++i) {
        const HistoryCopy& pages = item.window.tabs[i];
        if (pages.entries.empty()) continue;
        const SessionEntry& shown = pages.entries[pages.shown];
        g_app.closedTabs.push_back({.url = shown.url,
                                    .title = shown.title,
                                    .pages = pages,
                                    .window = window,
                                    .indexInWindow = static_cast<int>(i)});
    }
}

void RestoreWindows(const std::vector<SavedWindow>& windows) {
    for (std::size_t i = 0; i < windows.size(); ++i) {
        const SavedWindow& saved = windows[i];
        const RECT place = KeptOnScreen(ToRect(saved.place));
        BrowserWindow* win = nullptr;
        if (i == 0) {
            // The first window is there already, hidden, and has its place from winamp.ini;
            // the session's is as new, or newer if Winamp didn't get to save that.
            win = g_app.windows.front().get();
            ::SetWindowPos(win->frame, nullptr, place.left, place.top, Width(place), Height(place),
                           SWP_NOZORDER | SWP_NOACTIVATE);
        } else if (!saved.tabs.empty()) {
            win = CreateBrowserWindow(false, &place, true);
        }
        if (!win) continue;
        FillWindow(*win, saved);
        if (saved.show != WindowShow::kHidden) {
            g_waiting.push_back({.frame = win->frame, .show = saved.show, .stacking = saved.stacking});
        }
    }
}

void StartSaving() {
    if (const HWND client = FirstClient()) ::SetTimer(client, kSessionSaveTimer, kSaveEveryMs, nullptr);
}

} // namespace

void RestoreSession() {
    if (g_app.windows.empty()) return;
    const std::optional<std::wstring> text = ReadTextFile(SessionFile());
    SavedSession session = text ? ParseSession(*text) : SavedSession();

    for (const SavedClosed& item : session.closed) KeepClosed(item);
    if (g_app.settings.restoreWindows) {
        RestoreWindows(session.windows);
    } else {
        // They were closed last, as Winamp closed; the one in front comes back first.
        std::ranges::sort(session.windows, std::ranges::greater{}, &SavedWindow::stacking);
        for (SavedWindow& window : session.windows) {
            std::erase_if(window.tabs, [](const HistoryCopy& tab) { return tab.entries.empty(); });
            if (window.active >= window.tabs.size()) window.active = 0;
            if (!window.tabs.empty()) KeepClosed({.wholeWindow = true, .window = std::move(window)});
        }
    }
    TrimClosedTabs();

    if (g_waiting.empty()) {
        StartSaving();
        return;
    }
    g_showPending = true;
    ::SetTimer(FirstClient(), kSessionShowTimer, kShowLookMs, nullptr);
}

void ShowRestoredWindows() {
    if (!std::exchange(g_showPending, false)) return;
    if (const HWND client = FirstClient()) ::KillTimer(client, kSessionShowTimer);

    // The one at the back first, so that each comes up over those behind it.
    std::vector<WaitingWindow> waiting = std::exchange(g_waiting, {});
    std::ranges::sort(waiting, std::ranges::greater{}, &WaitingWindow::stacking);
    HWND front = nullptr;
    for (const WaitingWindow& window : waiting) {
        // One shown from Winamp's menu in the meantime is left as it is.
        if (!::IsWindow(window.frame) || ::IsWindowVisible(window.frame)) continue;
        switch (window.show) {
        case WindowShow::kMinimized:
            ::ShowWindow(window.frame, SW_SHOWMINNOACTIVE);
            break;
        case WindowShow::kMaximized:
            ::ShowWindow(window.frame, SW_SHOWMAXIMIZED);
            front = window.frame;
            break;
        default:
            ::ShowWindow(window.frame, SW_SHOWNA);
            ::SetWindowPos(window.frame, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            front = window.frame;
            break;
        }
    }
    if (front) {
        ::SetForegroundWindow(front);
        g_shownFront = front;
        g_shownAt = ::GetTickCount64();
    }
    StartSaving();
}

bool OnSessionTimer(UINT_PTR timer) {
    if (timer == kSessionSaveTimer) {
        SaveSession();
        return true;
    }
    if (timer == kSessionShowTimer) {
        if (!WinampIsStarting()) ShowRestoredWindows();
        return true;
    }
    return false;
}

void SaveSession() {
    // While the windows brought back are still to come up, they would be written as hidden;
    // the file has them as they are to be.
    if (g_app.shutDown || g_app.windows.empty() || g_showPending) return;
    std::wstring text = SerializeSession(CurrentSession());
    if (text == g_written) return;
    if (WriteTextFile(SessionFile(), text)) g_written = std::move(text);
}

bool WindowsRestoredForStart() {
    const ULONGLONG shownAt = std::exchange(g_shownAt, 0);
    if (!shownAt || ::GetTickCount64() - shownAt > kStartRequestMs) return false;
    if (!::IsWindow(g_shownFront) || !::IsWindowVisible(g_shownFront)) return false;
    ::SetForegroundWindow(g_shownFront);
    return true;
}

void NoteTabHistory(int tabId) {
    const TabLocation at = FindTab(tabId);
    // A tab still making the history it was given has that as its pages already.
    if (!at || !at.tab().webview || at.tab().historyCopy || at.tab().forClaude) return;
    at.tab().webview->CallDevToolsProtocolMethod(
        L"Page.getNavigationHistory", L"{}",
        Callback<DevToolsHandler>([tabId](HRESULT result, LPCWSTR json) -> HRESULT {
            const TabLocation at = FindTab(tabId);
            if (FAILED(result) || !json || !at || at.tab().historyCopy) return S_OK;
            if (const std::optional<SessionHistory> history = ParseSessionHistory(json)) {
                at.tab().pages = KeptHistory(*history);
            }
            return S_OK;
        }).Get());
}

void NoteTabTitle(Tab& tab, std::wstring_view url, std::wstring_view title) {
    HistoryCopy& pages = tab.pages;
    if (pages.shown < pages.entries.size() && pages.entries[pages.shown].url == url) {
        pages.entries[pages.shown].title = title;
    } else {
        // Its pages aren't in from the webview yet, and may come with the title it had before.
        NoteTabHistory(tab.id);
    }
}

} // namespace exodium
