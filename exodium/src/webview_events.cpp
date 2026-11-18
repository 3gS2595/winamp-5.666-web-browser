// What a tab's webview tells the browser. Every handler is given the tab's id and
// finds the tab afresh, wherever it is by then: it may have been dragged to another
// window, or closed, since the event was asked for.
#include "browser.h"

#include "gdiplus_headers.h"
#include "history_page.h"
#include "text_util.h"

#include <shlwapi.h>
#include <wrl/event.h>

#include <utility>

namespace exodium {

namespace {

using Microsoft::WRL::Callback;

constexpr wchar_t kHistoryPageHeaders[] = L"Content-Type: text/html; charset=utf-8\n"
                                          L"Cache-Control: no-store\n"
                                          L"Content-Security-Policy: frame-ancestors 'none'";

void InvalidateTabButton(const Tab& tab, bool erase = false) noexcept {
    if (tab.button) ::InvalidateRect(tab.button.get(), nullptr, erase);
}

void OnTitleChanged(int tabId, ICoreWebView2* sender) {
    const TabLocation at = FindTab(tabId);
    if (!at || at.tab().copyingHistory) return;
    const auto title = ComString(sender, &ICoreWebView2::get_DocumentTitle);
    if (!title) return;
    UpdateTabTitle(*at.window, at.index, *title);
    const std::wstring source = SourceOf(sender);
    // Nothing a private window shows goes into the history, a title no more than a visit.
    if (!at.tab().isPrivate) g_app.history.SetTitle(source, *title);
    NoteTabTitle(at.tab(), source, *title);
}

void OnNavigationStarting(int tabId, ICoreWebView2NavigationStartingEventArgs* args) {
    const TabLocation at = FindTab(tabId);
    if (!at || !args) return;
    BrowserWindow& win = *at.window;

    const auto uri = ComString(args, &ICoreWebView2NavigationStartingEventArgs::get_Uri);
    // A reopened tab has the page it was put away on ahead of it. Forward doesn't
    // lead there.
    if (uri == kParkedUrl) {
        args->put_Cancel(TRUE);
        return;
    }

    // A navigation the address bar asked for is known by its id from here on. The
    // page it interrupts ends too, and that end must not be taken for this one's.
    if (std::exchange(at.tab().typedNav, false)) args->get_NavigationId(&at.tab().typedNavId);

    at.tab().loading = true;
    StartTabSpinners(win);
    if (at.isActive()) UpdateReloadButton(win);
    if (uri && at.isActive() && !IsEditingUrl(win) && !at.tab().copyingHistory) ShowPageUrl(win, *uri);
}

void OnSourceChanged(int tabId, ICoreWebView2* sender) {
    const TabLocation at = FindTab(tabId);
    if (!at || !sender || at.tab().copyingHistory) return;
    ApplySiteZoom(at.tab());
    if (at.isActive()) UpdateBookmarkButton(*at.window); // the star is for the page in view
    if (!at.isActive() || IsEditingUrl(*at.window)) return;
    if (const auto source = ComString(sender, &ICoreWebView2::get_Source)) ShowPageUrl(*at.window, *source);
}

void OnNavigationCompleted(int tabId, ICoreWebView2* sender, ICoreWebView2NavigationCompletedEventArgs* args) {
    const TabLocation at = FindTab(tabId);
    // A stand-in is no visit, and the tab stays busy until the page it shows has loaded.
    if (!at || at.tab().copyingHistory) return;
    BrowserWindow& win = *at.window;
    Tab& tab = at.tab();
    UINT64 navigationId = 0;
    if (args) args->get_NavigationId(&navigationId);
    const bool typed = navigationId != 0 && navigationId == tab.typedNavId;
    if (typed) tab.typedNavId = 0;

    if (const auto url = ComString(sender, &ICoreWebView2::get_Source)) {
        // Reopened while it was still on its way to the page closed tabs are put
        // away on: it turns round.
        if (*url == kParkedUrl) {
            if (std::exchange(tab.parked, false)) sender->GoBack();
            return;
        }
        tab.lastUrl = *url;

        // A load that failed is left out, or a mistyped address would be suggested
        // from then on.
        BOOL succeeded = TRUE;
        if (args) args->get_IsSuccess(&succeeded);
        if (succeeded) {
            const std::wstring title = ComString(sender, &ICoreWebView2::get_DocumentTitle).value_or(std::wstring());
            if (!tab.isPrivate) g_app.history.RecordVisit(*url, title, typed, UnixNow());
            if (std::exchange(tab.crashed, false)) InvalidateTabButton(tab);
        }
        if (at.isActive() && !IsEditingUrl(win)) ShowPageUrl(win, *url);
    }

    tab.loading = false;
    if (at.isActive()) {
        UpdateReloadButton(win);
        UpdateBookmarkButton(win);
    }
}

void OnNewWindowRequested(int tabId, ICoreWebView2NewWindowRequestedEventArgs* args) {
    if (!args) return;
    const TabLocation at = FindTab(tabId);
    if (!at || !g_app.environment) {
        args->put_Handled(TRUE);
        return;
    }
    OpenRequestedWindow(*at.window, tabId, args);
}

// The webview shows its context menu itself. What is wanted of it here is the link it
// was opened on, if any: see OpensBehind in webview.cpp. And its "Inspect" opens the
// DevTools docked in the window, where they can be: see devtools_panel.cpp.
void OnContextMenuRequested(int tabId, ICoreWebView2ContextMenuRequestedEventArgs* args) {
    const TabLocation at = FindTab(tabId);
    if (!at || !args) return;
    OfferDockedInspect(tabId, args);
    Tab& tab = at.tab();
    tab.menuLink.clear();
    ComPtr<ICoreWebView2ContextMenuTarget> target;
    BOOL hasLink = FALSE;
    if (FAILED(args->get_ContextMenuTarget(&target)) || !target || FAILED(target->get_HasLinkUri(&hasLink)) ||
        !hasLink) {
        return;
    }
    tab.menuLink = ComString(target.Get(), &ICoreWebView2ContextMenuTarget::get_LinkUri).value_or(std::wstring());
    tab.menuLinkAt = ::GetTickCount64();
}

// A page that closes itself, as a sign-in popup does when it is done, takes its tab
// with it. Posted, so that the webview isn't closed from inside its own event.
void OnWindowCloseRequested(int tabId) {
    if (const TabLocation at = FindTab(tabId)) {
        ::PostMessageW(at.window->client, kMsgCloseTab, static_cast<WPARAM>(tabId), 0);
    }
}

// Requests for kHistoryUrl and kBookmarksUrl never reach the network; the page is made
// here, afresh for every visit, and no other page may show it in a frame. A tab copying
// another's history hears of every page it asks for (see back_forward.cpp), and the
// history and the bookmarks are no other address's to be given.
void OnPluginPageRequested(ICoreWebView2WebResourceRequestedEventArgs* args) {
    if (!args || !g_app.environment) return;
    ComPtr<ICoreWebView2WebResourceRequest> request;
    if (FAILED(args->get_Request(&request))) return;
    const auto uri = ComString(request.Get(), &ICoreWebView2WebResourceRequest::get_Uri);
    std::string html;
    if (uri == kHistoryUrl) html = ToUtf8(BuildHistoryPageHtml(g_app.history.entries()));
    else if (uri == kBookmarksUrl) html = ToUtf8(BookmarksPageHtml());
    else return;

    ComPtr<IStream> body;
    body.Attach(::SHCreateMemStream(reinterpret_cast<const BYTE*>(html.data()), static_cast<UINT>(html.size())));
    ComPtr<ICoreWebView2WebResourceResponse> response;
    if (body && SUCCEEDED(g_app.environment->CreateWebResourceResponse(body.Get(), 200, L"OK", kHistoryPageHeaders,
                                                                       &response))) {
        args->put_Response(response.Get());
    }
}

// The history page names an entry to remove by sending its address, and the bookmarks
// page says what was clicked on it (bookmarks.h). No other page gets a say in what the
// history or the bookmarks hold.
void OnWebMessage(ICoreWebView2* sender, ICoreWebView2WebMessageReceivedEventArgs* args) {
    const auto source = ComString(args, &ICoreWebView2WebMessageReceivedEventArgs::get_Source);
    if (source != kHistoryUrl && source != kBookmarksUrl) return;
    const auto message = ComString(args, &ICoreWebView2WebMessageReceivedEventArgs::TryGetWebMessageAsString);
    if (!message) return;
    if (source == kBookmarksUrl) {
        OnBookmarksPageMessage(sender, *message);
    } else if (g_app.history.Remove(*message)) {
        SaveHistorySoon(true); // on disk at once: it was taken out to be gone
    }
}

// What ended a process, for the log.
[[nodiscard]] std::wstring_view NameOf(COREWEBVIEW2_PROCESS_FAILED_REASON reason) noexcept {
    switch (reason) {
    case COREWEBVIEW2_PROCESS_FAILED_REASON_UNEXPECTED:
        return L"it ended unexpectedly";
    case COREWEBVIEW2_PROCESS_FAILED_REASON_UNRESPONSIVE:
        return L"it stopped answering";
    case COREWEBVIEW2_PROCESS_FAILED_REASON_TERMINATED:
        return L"it was ended from outside";
    case COREWEBVIEW2_PROCESS_FAILED_REASON_CRASHED:
        return L"it crashed";
    case COREWEBVIEW2_PROCESS_FAILED_REASON_LAUNCH_FAILED:
        return L"it couldn't be started";
    case COREWEBVIEW2_PROCESS_FAILED_REASON_OUT_OF_MEMORY:
        return L"it ran out of memory";
    default:
        return L"for a reason not known";
    }
}

// Writes down that a process of the browser's ended, which process and why. The pages'
// own are frequent enough in a long day to be worth a look when one page keeps doing it.
void LogProcessFailure(int tabId, COREWEBVIEW2_PROCESS_FAILED_KIND kind, ICoreWebView2ProcessFailedEventArgs* args) {
    std::wstring what;
    switch (kind) {
    case COREWEBVIEW2_PROCESS_FAILED_KIND_BROWSER_PROCESS_EXITED:
        // Every webview says so; once is enough.
        if (g_app.reviving) return;
        what = L"The browser process ended, and every page with it";
        break;
    case COREWEBVIEW2_PROCESS_FAILED_KIND_RENDER_PROCESS_EXITED:
    case COREWEBVIEW2_PROCESS_FAILED_KIND_RENDER_PROCESS_UNRESPONSIVE: {
        what = kind == COREWEBVIEW2_PROCESS_FAILED_KIND_RENDER_PROCESS_EXITED ? L"A page's process ended"
                                                                              : L"A page stopped answering";
        // With the site it was on, which tells one page that keeps doing it from another;
        // but not a private window's, of which nothing is written down.
        if (const TabLocation at = FindTab(tabId)) {
            what.append(L" (")
                .append(at.tab().isPrivate ? L"in a private window" : HostOf(TabUrl(at.tab())))
                .append(L")");
        }
        break;
    }
    case COREWEBVIEW2_PROCESS_FAILED_KIND_GPU_PROCESS_EXITED:
        what = L"The graphics process ended";
        break;
    default:
        what = L"A helper process of the browser's ended";
        break;
    }
    COREWEBVIEW2_PROCESS_FAILED_REASON reason{};
    int exitCode = 0;
    if (ComPtr<ICoreWebView2ProcessFailedEventArgs2> more; SUCCEEDED(args->QueryInterface(IID_PPV_ARGS(&more)))) {
        if (SUCCEEDED(more->get_Reason(&reason))) what.append(L": ").append(NameOf(reason));
        if (SUCCEEDED(more->get_ExitCode(&exitCode))) {
            what.append(L", exit code 0x").append(ToHex(static_cast<unsigned long>(exitCode), 8));
        }
    }
    LogLine(what + L".");
}

void OnProcessFailed(int tabId, ICoreWebView2ProcessFailedEventArgs* args) {
    COREWEBVIEW2_PROCESS_FAILED_KIND kind{};
    if (!args || FAILED(args->get_ProcessFailedKind(&kind))) return;
    LogProcessFailure(tabId, kind, args);

    if (kind == COREWEBVIEW2_PROCESS_FAILED_KIND_BROWSER_PROCESS_EXITED) {
        // The browser process took every webview with it, and each of them says so.
        // The first to say it has them all made again, once this event is over.
        if (!g_app.reviving && !g_app.windows.empty()) {
            g_app.reviving = true;
            ::PostMessageW(g_app.windows.front()->client, kMsgRevive, 0, 0);
        }
    } else if (kind == COREWEBVIEW2_PROCESS_FAILED_KIND_RENDER_PROCESS_EXITED) {
        // Only this tab's page is gone. The webview puts up a page of its own to say
        // so, with a way to load the lost one again; what is left to do here is to
        // stop the tab looking busy and to mark it in the strip.
        const TabLocation at = FindTab(tabId);
        if (!at) return;
        Tab& tab = at.tab();
        tab.loading = false;
        tab.crashed = true;
        tab.playingAudio = false; // its sound went with it
        InvalidateTabButton(tab);
        if (at.isActive()) UpdateReloadButton(*at.window);
    }
}

void OnZoomFactorChanged(int tabId, ICoreWebView2Controller* controller) {
    const TabLocation at = FindTab(tabId);
    double factor = SiteZoom::kNormal;
    if (!at || !controller || FAILED(controller->get_ZoomFactor(&factor))) return;
    // Any factor but the one this tab was given is the user's doing.
    if (!SameZoom(factor, at.tab().zoom)) SetSiteZoom(at.tab(), factor);
}

// The webview has the keyboard: the browser's shortcuts are picked out of what is
// typed into the page.
void OnAcceleratorKey(int tabId, ICoreWebView2AcceleratorKeyPressedEventArgs* args) {
    if (!args) return;
    UINT key = 0;
    COREWEBVIEW2_KEY_EVENT_KIND kind{};
    args->get_VirtualKey(&key);
    args->get_KeyEventKind(&kind);
    if (kind != COREWEBVIEW2_KEY_EVENT_KIND_KEY_DOWN && kind != COREWEBVIEW2_KEY_EVENT_KIND_SYSTEM_KEY_DOWN) return;

    const TabLocation at = FindTab(tabId);
    if (at && HandleGlobalShortcut(at.window, key, CurrentModifiers())) args->put_Handled(TRUE);
}

[[nodiscard]] UniqueIcon IconFromPng(IStream* png) {
    if (!png) return {};
    Gdiplus::Bitmap bitmap(png);
    HICON icon = nullptr;
    if (bitmap.GetLastStatus() != Gdiplus::Ok || bitmap.GetHICON(&icon) != Gdiplus::Ok) return {};
    return UniqueIcon(icon);
}

void OnFaviconChanged(int tabId, ICoreWebView2* sender) {
    ComPtr<ICoreWebView2_15> webview15;
    if (!sender || FAILED(sender->QueryInterface(IID_PPV_ARGS(&webview15)))) return;
    webview15->GetFavicon(
        COREWEBVIEW2_FAVICON_IMAGE_FORMAT_PNG,
        Callback<ICoreWebView2GetFaviconCompletedHandler>([tabId](HRESULT result, IStream* png) -> HRESULT {
            if (FAILED(result)) return S_OK;
            UniqueIcon icon = IconFromPng(png);
            if (const TabLocation at = FindTab(tabId)) {
                at.tab().favicon = std::move(icon);
                InvalidateTabButton(at.tab(), true);
            }
            return S_OK;
        }).Get());
}

void OnPlayingAudioChanged(int tabId) {
    if (const TabLocation at = FindTab(tabId)) UpdateTabAudio(at.tab());
}

void OnFullscreenChanged(int tabId, ICoreWebView2* sender) {
    const TabLocation at = FindTab(tabId);
    if (!at || !sender || !at.isActive()) return;
    BOOL fullscreen = FALSE;
    sender->get_ContainsFullScreenElement(&fullscreen);
    if (fullscreen) EnterVideoFullscreen(*at.window);
    else ExitVideoFullscreen(*at.window);
}

} // namespace

void WireTabEvents(int tabId) {
    const TabLocation at = FindTab(tabId);
    if (!at || !at.tab().webview || !at.tab().controller) return;
    ICoreWebView2* const webview = at.tab().webview.Get();
    ICoreWebView2Controller* const controller = at.tab().controller.get();

    // The handlers live as long as the webview does, so none is ever unregistered.
    webview->add_DocumentTitleChanged(
        Callback<ICoreWebView2DocumentTitleChangedEventHandler>([tabId](ICoreWebView2* sender, IUnknown*) -> HRESULT {
            OnTitleChanged(tabId, sender);
            return S_OK;
        }).Get(),
        nullptr);

    webview->add_NavigationStarting(
        Callback<ICoreWebView2NavigationStartingEventHandler>([tabId](ICoreWebView2*,
                                                                      ICoreWebView2NavigationStartingEventArgs* args)
                                                                  -> HRESULT {
            OnNavigationStarting(tabId, args);
            return S_OK;
        }).Get(),
        nullptr);

    webview->add_SourceChanged(
        Callback<ICoreWebView2SourceChangedEventHandler>([tabId](ICoreWebView2* sender,
                                                                 ICoreWebView2SourceChangedEventArgs*) -> HRESULT {
            OnSourceChanged(tabId, sender);
            return S_OK;
        }).Get(),
        nullptr);

    webview->add_NavigationCompleted(
        Callback<ICoreWebView2NavigationCompletedEventHandler>([tabId](ICoreWebView2* sender,
                                                                       ICoreWebView2NavigationCompletedEventArgs* args)
                                                                   -> HRESULT {
            OnNavigationCompleted(tabId, sender, args);
            return S_OK;
        }).Get(),
        nullptr);

    webview->add_NewWindowRequested(
        Callback<ICoreWebView2NewWindowRequestedEventHandler>([tabId](ICoreWebView2*,
                                                                      ICoreWebView2NewWindowRequestedEventArgs* args)
                                                                  -> HRESULT {
            OnNewWindowRequested(tabId, args);
            return S_OK;
        }).Get(),
        nullptr);

    // Whatever changes where Back and Forward lead: a page loaded, a fragment, pushState,
    // a step back or forward. The tab's pages are asked for again.
    webview->add_HistoryChanged(
        Callback<ICoreWebView2HistoryChangedEventHandler>([tabId](ICoreWebView2*, IUnknown*) -> HRESULT {
            NoteTabHistory(tabId);
            return S_OK;
        }).Get(),
        nullptr);

    webview->add_WindowCloseRequested(
        Callback<ICoreWebView2WindowCloseRequestedEventHandler>([tabId](ICoreWebView2*, IUnknown*) -> HRESULT {
            OnWindowCloseRequested(tabId);
            return S_OK;
        }).Get(),
        nullptr);

    webview->AddWebResourceRequestedFilter(kHistoryUrl, COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL);
    webview->AddWebResourceRequestedFilter(kBookmarksUrl, COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL);
    webview->add_WebResourceRequested(
        Callback<ICoreWebView2WebResourceRequestedEventHandler>([](ICoreWebView2*,
                                                                   ICoreWebView2WebResourceRequestedEventArgs* args)
                                                                    -> HRESULT {
            OnPluginPageRequested(args);
            return S_OK;
        }).Get(),
        nullptr);

    webview->add_WebMessageReceived(
        Callback<ICoreWebView2WebMessageReceivedEventHandler>([](ICoreWebView2* sender,
                                                                 ICoreWebView2WebMessageReceivedEventArgs* args)
                                                                  -> HRESULT {
            OnWebMessage(sender, args);
            return S_OK;
        }).Get(),
        nullptr);

    webview->add_ProcessFailed(
        Callback<ICoreWebView2ProcessFailedEventHandler>([tabId](ICoreWebView2*,
                                                                 ICoreWebView2ProcessFailedEventArgs* args) -> HRESULT {
            OnProcessFailed(tabId, args);
            return S_OK;
        }).Get(),
        nullptr);

    webview->add_ContainsFullScreenElementChanged(
        Callback<ICoreWebView2ContainsFullScreenElementChangedEventHandler>([tabId](ICoreWebView2* sender,
                                                                                    IUnknown*) -> HRESULT {
            OnFullscreenChanged(tabId, sender);
            return S_OK;
        }).Get(),
        nullptr);

    // Favicons came with a later version of the runtime than the rest.
    if (ComPtr<ICoreWebView2_15> webview15; SUCCEEDED(webview->QueryInterface(IID_PPV_ARGS(&webview15)))) {
        webview15->add_FaviconChanged(
            Callback<ICoreWebView2FaviconChangedEventHandler>([tabId](ICoreWebView2* sender, IUnknown*) -> HRESULT {
                OnFaviconChanged(tabId, sender);
                return S_OK;
            }).Get(),
            nullptr);
    }

    // And the context menu.
    if (ComPtr<ICoreWebView2_11> webview11; SUCCEEDED(webview->QueryInterface(IID_PPV_ARGS(&webview11)))) {
        webview11->add_ContextMenuRequested(
            Callback<ICoreWebView2ContextMenuRequestedEventHandler>(
                [tabId](ICoreWebView2*, ICoreWebView2ContextMenuRequestedEventArgs* args) -> HRESULT {
                    OnContextMenuRequested(tabId, args);
                    return S_OK;
                })
                .Get(),
            nullptr);
    }

    // As did being told when a page starts or stops making sound.
    if (ComPtr<ICoreWebView2_8> webview8; SUCCEEDED(webview->QueryInterface(IID_PPV_ARGS(&webview8)))) {
        webview8->add_IsDocumentPlayingAudioChanged(
            Callback<ICoreWebView2IsDocumentPlayingAudioChangedEventHandler>([tabId](ICoreWebView2*,
                                                                                     IUnknown*) -> HRESULT {
                OnPlayingAudioChanged(tabId);
                return S_OK;
            }).Get(),
            nullptr);
    }

    controller->add_ZoomFactorChanged(
        Callback<ICoreWebView2ZoomFactorChangedEventHandler>([tabId](ICoreWebView2Controller* sender,
                                                                     IUnknown*) -> HRESULT {
            OnZoomFactorChanged(tabId, sender);
            return S_OK;
        }).Get(),
        nullptr);

    controller->add_AcceleratorKeyPressed(
        Callback<ICoreWebView2AcceleratorKeyPressedEventHandler>(
            [tabId](ICoreWebView2Controller*, ICoreWebView2AcceleratorKeyPressedEventArgs* args) -> HRESULT {
                OnAcceleratorKey(tabId, args);
                return S_OK;
            })
            .Get(),
        nullptr);

    WireTabDownloads(tabId);
}

} // namespace exodium
