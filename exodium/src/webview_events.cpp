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
    g_app.history.SetTitle(SourceOf(sender), *title);
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
            g_app.history.RecordVisit(*url, title, typed, UnixNow());
            if (std::exchange(tab.crashed, false)) InvalidateTabButton(tab);
        }
        if (at.isActive() && !IsEditingUrl(win)) ShowPageUrl(win, *url);
    }

    tab.loading = false;
    if (at.isActive()) UpdateReloadButton(win);
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

// The webview shows its context menu itself. All that is wanted of it here is the link
// it was opened on, if any: see OpensBehind in webview.cpp.
void OnContextMenuRequested(int tabId, ICoreWebView2ContextMenuRequestedEventArgs* args) {
    const TabLocation at = FindTab(tabId);
    if (!at || !args) return;
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

// Requests for kHistoryUrl never reach the network; the page is made here, afresh
// for every visit, and no other page may show it in a frame. A tab copying another's
// history hears of every page it asks for (see back_forward.cpp), and the history is
// no other address's to be given.
void OnHistoryPageRequested(ICoreWebView2WebResourceRequestedEventArgs* args) {
    if (!args || !g_app.environment) return;
    ComPtr<ICoreWebView2WebResourceRequest> request;
    if (FAILED(args->get_Request(&request)) ||
        ComString(request.Get(), &ICoreWebView2WebResourceRequest::get_Uri) != kHistoryUrl) {
        return;
    }
    const std::string html = ToUtf8(BuildHistoryPageHtml(g_app.history.entries()));

    ComPtr<IStream> body;
    body.Attach(::SHCreateMemStream(reinterpret_cast<const BYTE*>(html.data()), static_cast<UINT>(html.size())));
    ComPtr<ICoreWebView2WebResourceResponse> response;
    if (body && SUCCEEDED(g_app.environment->CreateWebResourceResponse(body.Get(), 200, L"OK", kHistoryPageHeaders,
                                                                       &response))) {
        args->put_Response(response.Get());
    }
}

// The history page names an entry to remove by sending its address. No other page
// gets a say in what the history holds.
void OnWebMessage(ICoreWebView2WebMessageReceivedEventArgs* args) {
    if (ComString(args, &ICoreWebView2WebMessageReceivedEventArgs::get_Source) != kHistoryUrl) return;
    const auto url = ComString(args, &ICoreWebView2WebMessageReceivedEventArgs::TryGetWebMessageAsString);
    if (url && g_app.history.Remove(*url)) g_app.history.Save(HistoryFile());
}

void OnProcessFailed(int tabId, ICoreWebView2ProcessFailedEventArgs* args) {
    COREWEBVIEW2_PROCESS_FAILED_KIND kind{};
    if (!args || FAILED(args->get_ProcessFailedKind(&kind))) return;

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

    webview->add_WindowCloseRequested(
        Callback<ICoreWebView2WindowCloseRequestedEventHandler>([tabId](ICoreWebView2*, IUnknown*) -> HRESULT {
            OnWindowCloseRequested(tabId);
            return S_OK;
        }).Get(),
        nullptr);

    webview->AddWebResourceRequestedFilter(kHistoryUrl, COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL);
    webview->add_WebResourceRequested(
        Callback<ICoreWebView2WebResourceRequestedEventHandler>([](ICoreWebView2*,
                                                                   ICoreWebView2WebResourceRequestedEventArgs* args)
                                                                    -> HRESULT {
            OnHistoryPageRequested(args);
            return S_OK;
        }).Get(),
        nullptr);

    webview->add_WebMessageReceived(Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                                        [](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                                            OnWebMessage(args);
                                            return S_OK;
                                        })
                                        .Get(),
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
