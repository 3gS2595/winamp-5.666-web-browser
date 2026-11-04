// Back and Forward. A plain click goes back or forward in the tab. Ctrl+click, or the
// middle button, opens the page it leads to in a new tab behind the one in view;
// Ctrl+Shift+click opens it in front, and Shift+click in a new window, as Chrome does.
// The new tab is given a copy of the tab's history, so that its own Back and Forward lead
// where the tab's would from that page. The tab itself stays where it is.
//
// WebView2 has no way to copy a webview's history, so the new tab makes its own, an entry
// at a time: it goes to each address in turn, and the page it asks for on the way is
// answered here (WebResourceRequested) with a blank stand-in instead of being fetched, so
// that no site hears of it and none of their pages run. Then it steps to the entry it is
// to show, which, with nothing answered any more, loads for real, as the others do when
// Back or Forward lead to them. Every entry has its real address all along, so the
// history file, the address bar, the zoom and tools on the debugging port never see an
// address of the plugin's own. While the stand-ins are made, the tab's events show
// nothing of them (Tab::copyingHistory); it has the shown page's title and address from
// the start.
//
// What decided it, measured on runtime 156:
// - Stand-ins answer file addresses as well as web ones. A step to one, once the copy is
//   made, loads the real page as a back/forward load would, from the cache if it is there.
//   The back/forward cache kept no page at all; a stand-in reloads itself should one ever
//   come back from it.
// - A fragment entry is made in the document of the entry before it, and raises only
//   SourceChanged and HistoryChanged. Stepping to the shown entry within the stand-in's
//   document loads nothing, so then it is reloaded.
// - A site's service worker answers its pages before WebResourceRequested hears of them,
//   and would run the site, unless the DevTools protocol bypasses it, which takes
//   Network.enable first.
// - The other way, stand-ins at an address of the plugin's own that replace themselves
//   with the real page by script, can't lead to file addresses at all, and the page comes
//   as a cross-site navigation from them, without the site's SameSite=Strict cookies.
#include "browser.h"

#include "text_util.h"

#include <shlwapi.h>
#include <wrl/event.h>

#include <string>
#include <utility>

namespace exodium {

// How far a tab's copy has got.
struct HistoryCopyJob {
    enum class Step {
        kNone,    // between entries, or waiting to hear what was made
        kAsked,   // the webview was sent to the entry `next`, and hasn't started loading a document for it
        kLoading, // it is loading that entry's stand-in, as the navigation `navigationId`
        kGoing,   // every entry is made, and it is on its way to the shown one
    };

    HistoryCopy copy;
    std::size_t next = 0; // the entry being made
    Step step = Step::kNone;
    UINT64 navigationId = 0;
    bool answering = false; // stand-ins are answered, and service workers bypassed
};

namespace {

using Microsoft::WRL::Callback;
using DevToolsHandler = ICoreWebView2CallDevToolsProtocolMethodCompletedHandler;
using Step = HistoryCopyJob::Step;

constexpr wchar_t kEveryDocument[] = L"*";
constexpr wchar_t kStandInHeaders[] = L"Content-Type: text/html; charset=utf-8\n"
                                      L"Cache-Control: no-store";
// How far a window opened by Shift+click is stepped down and right of the one it came
// from, as one opened with Ctrl+N is.
constexpr int kCascadeOffset = 30;

void MakeNextEntry(int tabId);

// The tab, while its history is being copied.
[[nodiscard]] Tab* CopyingTab(int tabId) noexcept {
    const TabLocation at = FindTab(tabId);
    return at && at.tab().historyCopy && at.tab().webview ? &at.tab() : nullptr;
}

[[nodiscard]] const SessionEntry& ShownEntry(const HistoryCopyJob& job) noexcept {
    return job.copy.entries[job.copy.shown];
}

// Stops answering for the tab's pages, and lets its sites' service workers answer them again.
void StopStandIns(Tab& tab) {
    if (!std::exchange(tab.historyCopy->answering, false) || !tab.webview) return;
    tab.webview->RemoveWebResourceRequestedFilter(kEveryDocument, COREWEBVIEW2_WEB_RESOURCE_CONTEXT_DOCUMENT);
    tab.webview->CallDevToolsProtocolMethod(L"Network.setBypassServiceWorker", L"{\"bypass\":false}", nullptr);
    tab.webview->CallDevToolsProtocolMethod(L"Network.disable", L"{}", nullptr);
}

// The copy can't be finished: the tab goes to the page, without the history.
void GiveUpOnHistory(Tab& tab) {
    tab.copyingHistory = false;
    tab.webview->Navigate(ShownEntry(*tab.historyCopy).url.c_str());
    EndHistoryCopy(tab);
}

// The entries the tab made, as the webview has them: the tab steps to the shown one.
void OnCopyMade(int tabId, const std::optional<SessionHistory>& made) {
    Tab* const tab = CopyingTab(tabId);
    if (!tab) return;
    HistoryCopyJob& job = *tab->historyCopy;
    const std::optional<std::size_t> shown = made ? FindShownEntry(*made, job.copy) : std::nullopt;
    if (!shown) {
        GiveUpOnHistory(*tab);
        return;
    }
    // From here on its events are the shown page's own.
    tab->copyingHistory = false;
    if (static_cast<int>(*shown) == made->current) {
        // It is the entry made last, which is still its stand-in.
        tab->webview->Reload();
        EndHistoryCopy(*tab);
        return;
    }
    job.step = Step::kGoing;
    const std::wstring params = L"{\"entryId\":" + std::to_wstring(made->entries[*shown].id) + L"}";
    tab->webview->CallDevToolsProtocolMethod(L"Page.navigateToHistoryEntry", params.c_str(),
                                             Callback<DevToolsHandler>([tabId](HRESULT result, LPCWSTR) -> HRESULT {
                                                 Tab* const tab = CopyingTab(tabId);
                                                 if (FAILED(result) && tab && tab->historyCopy->step == Step::kGoing)
                                                     GiveUpOnHistory(*tab);
                                                 return S_OK;
                                             }).Get());
}

// Every entry is made. The stand-ins are stopped first, so that the step to the shown
// entry, which is asked for after them, finds the real page.
void GoToShownEntry(int tabId) {
    Tab* const tab = CopyingTab(tabId);
    if (!tab) return;
    tab->historyCopy->step = Step::kNone;
    StopStandIns(*tab);
    tab->webview->CallDevToolsProtocolMethod(
        L"Page.getNavigationHistory", L"{}",
        Callback<DevToolsHandler>([tabId](HRESULT result, LPCWSTR json) -> HRESULT {
            OnCopyMade(tabId, SUCCEEDED(result) && json ? ParseSessionHistory(json) : std::nullopt);
            return S_OK;
        }).Get());
}

// Sends the tab to the next entry to be made or, once they all are, to the shown one.
void MakeNextEntry(int tabId) {
    Tab* const tab = CopyingTab(tabId);
    if (!tab) return;
    HistoryCopyJob& job = *tab->historyCopy;
    for (; job.next < job.copy.entries.size(); ++job.next) {
        job.step = Step::kAsked;
        if (SUCCEEDED(tab->webview->Navigate(job.copy.entries[job.next].url.c_str()))) return;
        // An address the webview won't take is left out.
    }
    GoToShownEntry(tabId);
}

void EntryMade(int tabId) {
    if (Tab* const tab = CopyingTab(tabId)) {
        ++tab->historyCopy->next;
        MakeNextEntry(tabId);
    }
}

void OnCopyNavigationStarting(int tabId, ICoreWebView2NavigationStartingEventArgs* args) {
    Tab* const tab = CopyingTab(tabId);
    if (!tab || !args) return;
    HistoryCopyJob& job = *tab->historyCopy;
    UINT64 navigationId = 0;
    args->get_NavigationId(&navigationId);
    switch (job.step) {
    case Step::kAsked:
        job.step = Step::kLoading;
        job.navigationId = navigationId;
        return;
    case Step::kLoading:
        if (navigationId == job.navigationId) return; // a redirect of the entry's own
        break;
    case Step::kGoing:
        // The shown entry is loading, for real: the copy is done.
        EndHistoryCopy(*tab);
        return;
    case Step::kNone:
        break;
    }
    // Somebody else's navigation, the user's or a page's. The copy stops where it has got
    // to; the entries made so far become their pages when Back or Forward lead to them.
    EndHistoryCopy(*tab);
}

void OnCopyNavigationCompleted(int tabId, ICoreWebView2NavigationCompletedEventArgs* args) {
    Tab* const tab = CopyingTab(tabId);
    if (!tab || !args) return;
    const HistoryCopyJob& job = *tab->historyCopy;
    UINT64 navigationId = 0;
    args->get_NavigationId(&navigationId);
    // Made or not, it is on to the next: an entry the webview wouldn't make is left out,
    // and one turned down before it started loading never got as far as kLoading.
    if (job.step == Step::kAsked || (job.step == Step::kLoading && navigationId == job.navigationId)) {
        EntryMade(tabId);
    }
}

void OnCopyHistoryChanged(int tabId) {
    Tab* const tab = CopyingTab(tabId);
    if (!tab) return;
    if (tab->historyCopy->step == Step::kAsked) {
        // Made without loading a document: a fragment of the entry before, in its document.
        EntryMade(tabId);
    } else if (tab->historyCopy->step == Step::kGoing) {
        // At the shown entry without loading a document: it shares the stand-in's, and is
        // blank still.
        tab->webview->Reload();
        EndHistoryCopy(*tab);
    }
}

void OnStandInRequested(int tabId, ICoreWebView2WebResourceRequestedEventArgs* args) {
    Tab* const tab = CopyingTab(tabId);
    if (!tab || !args || !tab->historyCopy->answering || !g_app.environment) return;
    ComPtr<ICoreWebView2WebResourceRequest> request;
    if (FAILED(args->get_Request(&request)) || !request) return;
    // The plugin's own pages are made by handlers of their own, and are no site's.
    const auto uri = ComString(request.Get(), &ICoreWebView2WebResourceRequest::get_Uri);
    if (!uri || IsPluginPage(*uri)) return;

    // The page's own title, which the entry keeps: going to it later within the document of
    // another entry, the tab shows the title the entry has.
    const HistoryCopyJob& job = *tab->historyCopy;
    std::wstring_view title;
    if (job.next < job.copy.entries.size()) title = job.copy.entries[job.next].title;
    const std::string html = ToUtf8(StandInPageHtml(title));
    ComPtr<IStream> body;
    body.Attach(::SHCreateMemStream(reinterpret_cast<const BYTE*>(html.data()), static_cast<UINT>(html.size())));
    ComPtr<ICoreWebView2WebResourceResponse> response;
    if (body &&
        SUCCEEDED(g_app.environment->CreateWebResourceResponse(body.Get(), 200, L"OK", kStandInHeaders, &response))) {
        args->put_Response(response.Get());
    }
}

// A window for Shift+click, the size of the one it came from and stepped down and right
// of it. Like one made for a dragged-out tab it starts empty and out of sight.
[[nodiscard]] BrowserWindow* CreateWindowBeside(const BrowserWindow& from) {
    // A minimized window is a sliver parked off screen. The place it goes back to stands in for it.
    RECT place = ::IsIconic(from.frame) ? from.embedState.r : WindowRectOf(from.frame);
    ::OffsetRect(&place, kCascadeOffset, kCascadeOffset);
    return CreateBrowserWindow(true, &place, true);
}

// Opens the copy in a new tab, placed the way a tab a link on the page opens is, and
// counted as opened from the tab in view; or in a new window.
void OpenHistoryCopy(int sourceTabId, HistoryCopy copy, OpenIn where) {
    const TabLocation source = FindTab(sourceTabId);
    if (!source || copy.shown >= copy.entries.size()) return;
    BrowserWindow& win = *source.window;
    // Taken before the tab is added, which may move the tabs in memory. A copy of one of
    // Claude's tabs is Claude's, as a page its page opens is.
    const bool forClaude = source.tab().forClaude;
    const std::uint32_t claudeSession = source.tab().claudeSession;
    const SessionEntry& shown = copy.entries[copy.shown];
    const std::wstring url = shown.url;
    const std::wstring title = shown.title.empty() ? shown.url : shown.title;

    BrowserWindow* host = &win;
    int index = 0;
    int openerId = 0;
    if (where == OpenIn::kNewWindow) {
        host = CreateWindowBeside(win);
        if (!host) return;
    } else {
        const bool foreground = where == OpenIn::kForegroundTab;
        index = LinkTabIndex(win, foreground);
        openerId = ActiveTabId(win);
        if (foreground) ForgetOpeners(win);
    }

    Tab& added = AddTab(*host, title, index);
    added.pendingUrl = url;
    // Where it is said to be until the shown page has loaded, should it be closed first.
    added.lastUrl = url;
    added.openerId = openerId;
    added.forClaude = forClaude;
    added.claudeSession = claudeSession;
    added.historyCopy = std::make_shared<HistoryCopyJob>(HistoryCopyJob{.copy = std::move(copy)});
    const int tabId = added.id;
    LayoutToolbarAndTabs(*host);

    if (where == OpenIn::kNewWindow) {
        SwitchToTab(*host, index);
        if (forClaude) {
            ShowWithoutFocus(*host);
        } else {
            ::ShowWindow(host->frame, SW_SHOWNA);
            ::SetForegroundWindow(host->frame);
        }
    } else if (where == OpenIn::kForegroundTab) {
        SwitchToTab(*host, index);
    }
    // Made at once, in view or not, as a tab a link opens is.
    CreateWebViewForTab(*host, tabId);
}

} // namespace

void GoBackOrForward(BrowserWindow& win, bool forward, OpenIn where) {
    Tab* const tab = ActiveTab(win);
    if (!tab || !tab->webview) return;
    // Its own history is still being made, and there is no telling yet where Back and
    // Forward lead.
    if (tab->historyCopy) return;
    if (where == OpenIn::kSameTab) {
        if (forward) tab->webview->GoForward();
        else tab->webview->GoBack();
        return;
    }
    // Asked for first: with nowhere to go, no tab is opened.
    const int sourceTabId = tab->id;
    tab->webview->CallDevToolsProtocolMethod(
        L"Page.getNavigationHistory", L"{}",
        Callback<DevToolsHandler>([sourceTabId, forward, where](HRESULT result, LPCWSTR json) -> HRESULT {
            if (FAILED(result) || !json) return S_OK;
            const std::optional<SessionHistory> history = ParseSessionHistory(json);
            if (std::optional<HistoryCopy> copy = history ? PlanHistoryCopy(*history, forward) : std::nullopt) {
                OpenHistoryCopy(sourceTabId, std::move(*copy), where);
            }
            return S_OK;
        }).Get());
}

void StartHistoryCopy(int tabId) {
    const TabLocation at = FindTab(tabId);
    if (!at || !at.tab().historyCopy || !at.tab().webview) return;
    Tab& tab = at.tab();
    ICoreWebView2* const webview = tab.webview.Get();
    tab.copyingHistory = true;
    tab.historyCopy->answering = true;
    tab.loading = true;
    StartTabSpinners(*at.window);
    if (at.isActive()) UpdateReloadButton(*at.window);

    // Handlers besides the tab's own. Like those, they live as long as the webview does,
    // and once the copy is done they find nothing to do.
    webview->AddWebResourceRequestedFilter(kEveryDocument, COREWEBVIEW2_WEB_RESOURCE_CONTEXT_DOCUMENT);
    webview->add_WebResourceRequested(
        Callback<ICoreWebView2WebResourceRequestedEventHandler>(
            [tabId](ICoreWebView2*, ICoreWebView2WebResourceRequestedEventArgs* args) -> HRESULT {
                OnStandInRequested(tabId, args);
                return S_OK;
            })
            .Get(),
        nullptr);
    webview->add_NavigationStarting(
        Callback<ICoreWebView2NavigationStartingEventHandler>([tabId](ICoreWebView2*,
                                                                      ICoreWebView2NavigationStartingEventArgs* args)
                                                                  -> HRESULT {
            OnCopyNavigationStarting(tabId, args);
            return S_OK;
        }).Get(),
        nullptr);
    webview->add_NavigationCompleted(
        Callback<ICoreWebView2NavigationCompletedEventHandler>([tabId](ICoreWebView2*,
                                                                       ICoreWebView2NavigationCompletedEventArgs* args)
                                                                   -> HRESULT {
            OnCopyNavigationCompleted(tabId, args);
            return S_OK;
        }).Get(),
        nullptr);
    webview->add_HistoryChanged(
        Callback<ICoreWebView2HistoryChangedEventHandler>([tabId](ICoreWebView2*, IUnknown*) -> HRESULT {
            OnCopyHistoryChanged(tabId);
            return S_OK;
        }).Get(),
        nullptr);

    // Service workers are bypassed before the first entry is made.
    webview->CallDevToolsProtocolMethod(L"Network.enable", L"{}",
                                        Callback<DevToolsHandler>([tabId](HRESULT, LPCWSTR) -> HRESULT {
                                            Tab* const tab = CopyingTab(tabId);
                                            if (!tab || !tab->historyCopy->answering) return S_OK;
                                            tab->webview->CallDevToolsProtocolMethod(
                                                L"Network.setBypassServiceWorker", L"{\"bypass\":true}",
                                                Callback<DevToolsHandler>([tabId](HRESULT, LPCWSTR) -> HRESULT {
                                                    MakeNextEntry(tabId);
                                                    return S_OK;
                                                }).Get());
                                            return S_OK;
                                        }).Get());
}

void EndHistoryCopy(Tab& tab) {
    if (!tab.historyCopy) return;
    StopStandIns(tab);
    tab.historyCopy.reset();
    tab.copyingHistory = false;
}

} // namespace exodium
