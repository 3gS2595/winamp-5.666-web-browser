// Making webviews: the environment they all share, a tab's own, the one a page
// that opens a window is given, and all of them again after the browser process dies.
#include "browser.h"

#include <wrl/event.h>

#include <utility>

namespace exodium {

namespace {

using Microsoft::WRL::Callback;

using ControllerCompletedHandler = ICoreWebView2CreateCoreWebView2ControllerCompletedHandler;
using EnvironmentCompletedHandler = ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler;

// Makes the environment if there is none yet, then calls `onReady`. Callers that
// arrive while it is being made wait for the same one.
void WithEnvironment(std::move_only_function<void()> onReady) {
    if (g_app.environment) {
        onReady();
        return;
    }
    g_app.environmentWaiters.push_back(std::move(onReady));
    if (g_app.environmentPending) return;
    g_app.environmentPending = true;

    const auto failed = [](HRESULT result) {
        TraceFailure(L"Creating the WebView2 environment", result);
        g_app.environmentPending = false;
        g_app.environmentWaiters.clear(); // whoever asks next tries again
    };
    const HRESULT started = ::CreateCoreWebView2EnvironmentWithOptions(
        nullptr, WebViewProfileDir().c_str(), nullptr,
        Callback<EnvironmentCompletedHandler>([failed](HRESULT result,
                                                       ICoreWebView2Environment* environment) -> HRESULT {
            if (FAILED(result) || !environment) {
                failed(FAILED(result) ? result : E_FAIL);
                return S_OK;
            }
            g_app.environmentPending = false;
            auto waiters = std::exchange(g_app.environmentWaiters, {});
            if (g_app.shutDown) return S_OK; // it came too late to be of use
            g_app.environment = environment;
            for (auto& waiter : waiters) waiter();
            return S_OK;
        }).Get());
    if (FAILED(started)) failed(started);
}

// Takes a newly made controller into its tab, out of sight until the tab is shown.
// `createdIn` is the window it was made for.
void AdoptController(BrowserWindow& win, Tab& tab, ICoreWebView2Controller* controller, HWND createdIn) {
    tab.controller = WebViewController(controller);
    controller->get_CoreWebView2(&tab.webview);
    // The tab may have been dragged to another window while this was pending.
    if (win.client != createdIn) controller->put_ParentWindow(win.client);
    controller->put_Bounds(WebViewBounds(win));
    controller->put_IsVisible(FALSE);
}

// Pages are asked for their dark version, to go with the rest of the window.
void PreferDarkColorScheme(ICoreWebView2* webview) {
    ComPtr<ICoreWebView2_13> webview13;
    ComPtr<ICoreWebView2Profile> profile;
    ComPtr<ICoreWebView2Profile2> profile2;
    if (FAILED(webview->QueryInterface(IID_PPV_ARGS(&webview13))) || FAILED(webview13->get_Profile(&profile)) ||
        !profile || FAILED(profile.As(&profile2))) {
        return;
    }
    profile2->put_PreferredColorScheme(COREWEBVIEW2_PREFERRED_COLOR_SCHEME_DARK);
}

void OnTabControllerCreated(int tabId, HWND createdIn, HRESULT result, ICoreWebView2Controller* controller) {
    const TabLocation at = FindTab(tabId);
    if (at) at.tab().creating = false;
    if (FAILED(result) || !controller) {
        TraceFailure(L"Creating a WebView2 controller", FAILED(result) ? result : E_FAIL);
        return;
    }
    if (!at) { // the tab was closed while this was pending
        controller->Close();
        return;
    }

    BrowserWindow& win = *at.window;
    Tab& tab = at.tab();
    AdoptController(win, tab, controller, createdIn);
    PreferDarkColorScheme(tab.webview.Get());
    WireTabEvents(tabId);
    tab.webview->Navigate(tab.pendingUrl.empty() ? kDefaultUrl : tab.pendingUrl.c_str());

    if (at.isActive()) {
        LayoutActiveWebView(win);
        // A tab opened to type an address into keeps the keyboard there.
        if (::GetFocus() != win.urlEdit) MoveFocusToWebView(tab);
    }
}

// A page that sizes or places the window it opens, or leaves the browser's bars off
// it, means a window of its own, and is given one as it is in Chrome. A link that
// opens elsewhere asks for none of that and gets a tab.
[[nodiscard]] std::optional<PopupRequest> RequestedPopup(ICoreWebView2NewWindowRequestedEventArgs* args) {
    ComPtr<ICoreWebView2WindowFeatures> features;
    if (FAILED(args->get_WindowFeatures(&features)) || !features) return std::nullopt;

    BOOL hasPosition = FALSE, hasSize = FALSE;
    BOOL menuBar = TRUE, statusBar = TRUE, toolbar = TRUE, scrollBars = TRUE;
    features->get_HasPosition(&hasPosition);
    features->get_HasSize(&hasSize);
    features->get_ShouldDisplayMenuBar(&menuBar);
    features->get_ShouldDisplayStatus(&statusBar);
    features->get_ShouldDisplayToolbar(&toolbar);
    features->get_ShouldDisplayScrollBars(&scrollBars);
    if (!hasPosition && !hasSize && menuBar && statusBar && toolbar && scrollBars) return std::nullopt;

    PopupRequest popup;
    UINT32 left = 0, top = 0, width = 0, height = 0;
    if (hasPosition && SUCCEEDED(features->get_Left(&left)) && SUCCEEDED(features->get_Top(&top))) {
        // A place left of or above the main screen comes through as a negative number.
        popup.position = POINT{static_cast<LONG>(left), static_cast<LONG>(top)};
    }
    if (hasSize && SUCCEEDED(features->get_Width(&width)) && SUCCEEDED(features->get_Height(&height))) {
        popup.size = SIZE{static_cast<LONG>(width), static_cast<LONG>(height)};
    }
    return popup;
}

} // namespace

std::wstring SourceOf(ICoreWebView2* webview) {
    return ComString(webview, &ICoreWebView2::get_Source).value_or(std::wstring());
}

void SetTabMuted(Tab& tab, bool muted) {
    ComPtr<ICoreWebView2_8> webview8;
    if (tab.webview && SUCCEEDED(tab.webview.As(&webview8))) webview8->put_IsMuted(muted ? TRUE : FALSE);
}

void UpdateTabAudio(Tab& tab) {
    BOOL playing = FALSE;
    ComPtr<ICoreWebView2_8> webview8;
    if (tab.webview && SUCCEEDED(tab.webview.As(&webview8))) webview8->get_IsDocumentPlayingAudio(&playing);
    if (std::exchange(tab.playingAudio, playing != FALSE) == (playing != FALSE)) return;
    if (tab.button) ::InvalidateRect(tab.button.get(), nullptr, FALSE);
}

void MoveFocusToWebView(Tab& tab) {
    if (tab.controller) tab.controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
}

void CreateWebViewForTab(BrowserWindow& owner, int tabId) {
    const int index = TabIndexById(owner, tabId);
    if (index < 0 || owner.tabs[index].controller || owner.tabs[index].creating) return;

    WithEnvironment([tabId] {
        const TabLocation at = FindTab(tabId);
        if (!at || !g_app.environment) return;
        Tab& tab = at.tab();
        if (tab.controller || tab.creating) return;
        tab.creating = true;
        StartTabSpinners(*at.window);

        const HWND parent = at.window->client;
        const HRESULT started = g_app.environment->CreateCoreWebView2Controller(
            parent,
            Callback<ControllerCompletedHandler>([tabId, parent](HRESULT result,
                                                                 ICoreWebView2Controller* controller) -> HRESULT {
                OnTabControllerCreated(tabId, parent, result, controller);
                return S_OK;
            }).Get());
        if (FAILED(started)) OnTabControllerCreated(tabId, parent, started, nullptr);
    });
}

// Popup-based OAuth (Google Identity Services, etc.) hands the caller's script a
// reference via window.open() and talks to it over window.opener/postMessage. That
// only works if we give WebView2 a real CoreWebView2 via put_NewWindow — a plain new
// tab leaves window.open() returning null and breaks those sign-in flows. The same
// holds whether the webview ends up in a tab of this window or in a window of its own.
void OpenRequestedWindow(BrowserWindow& win, ICoreWebView2NewWindowRequestedEventArgs* request) {
    ComPtr<ICoreWebView2NewWindowRequestedEventArgs> args(request);
    ComPtr<ICoreWebView2Deferral> deferral;
    args->GetDeferral(&deferral);

    // The tab goes into the opener's window, unless the page asked for a window.
    const std::optional<PopupRequest> popup = RequestedPopup(request);
    BrowserWindow* const popupWindow = popup ? CreatePopupWindow(win, *popup) : nullptr;
    const bool isPopup = popupWindow != nullptr;
    BrowserWindow& host = isPopup ? *popupWindow : win;

    const int index = static_cast<int>(host.tabs.size());
    Tab& added = AddTab(host, kNewTabTitle);
    added.creating = true;
    const int tabId = added.id;
    LayoutToolbarAndTabs(host);

    if (isPopup) {
        SwitchToTab(host, index);
        ::ShowWindow(host.frame, SW_SHOWNA);
        ::SetForegroundWindow(host.frame);
    } else {
        // Ctrl+click opens the link in the background, as browsers do; adding Shift
        // goes to it. The event doesn't say how the link was clicked, but it arrives
        // while the keys are still down.
        BOOL byUser = FALSE;
        args->get_IsUserInitiated(&byUser);
        const bool inBackground =
            byUser && (::GetAsyncKeyState(VK_CONTROL) & 0x8000) && !(::GetAsyncKeyState(VK_SHIFT) & 0x8000);
        if (!inBackground) SwitchToTab(host, index);
    }

    const HWND parent = host.client;
    const auto finish = [tabId, parent, isPopup, args, deferral](HRESULT result, ICoreWebView2Controller* controller) {
        const TabLocation at = FindTab(tabId);
        if (at) at.tab().creating = false;
        if (SUCCEEDED(result) && controller && at) {
            BrowserWindow& owner = *at.window;
            Tab& tab = at.tab();
            AdoptController(owner, tab, controller, parent);
            WireTabEvents(tabId);
            // Must not be navigated before this call — WebView2 auto-navigates it to
            // the requested uri once assigned.
            args->put_NewWindow(tab.webview.Get());
            if (at.isActive()) {
                LayoutActiveWebView(owner);
                MoveFocusToWebView(tab);
            }
        } else {
            if (controller) controller->Close();
            // A window opened for the page has nothing to show without it.
            if (isPopup && at) ::PostMessageW(at.window->client, kMsgCloseTab, static_cast<WPARAM>(tabId), 0);
        }
        args->put_Handled(TRUE);
        if (deferral) deferral->Complete();
    };
    const HRESULT started = g_app.environment->CreateCoreWebView2Controller(
        parent,
        Callback<ControllerCompletedHandler>([finish](HRESULT result, ICoreWebView2Controller* controller) -> HRESULT {
            finish(result, controller);
            return S_OK;
        }).Get());
    if (FAILED(started)) finish(started, nullptr);
}

// Each tab gives up its dead webview and is left as a tab is before it is first
// looked at, headed for the page it was on. The tabs in view get a new one at once
// and the rest when they are switched to. Where Back and Forward led went with the
// process.
void ReviveWebViews() {
    g_app.reviving = false;
    for (const auto& win : g_app.windows) {
        ExitVideoFullscreen(*win);
        for (Tab& tab : win->tabs) {
            tab.CloseWebView();
            if (!tab.lastUrl.empty()) tab.pendingUrl = tab.lastUrl;
            tab.loading = tab.typedNav = tab.parked = tab.crashed = false;
            tab.typedNavId = 0;
            tab.zoom = SiteZoom::kNormal; // a new webview starts out at the normal size
        }
        UpdateReloadButton(*win);
    }
    for (ClosedTab& closed : g_app.closedTabs) DiscardClosedWebView(closed);
    g_app.environment.Reset(); // it belonged to the dead process; the next webview asked for makes a new one

    for (const auto& win : g_app.windows) {
        if (::IsWindowVisible(win->frame)) ::PostMessageW(win->client, kMsgInitWebView, 0, 0);
    }
}

} // namespace exodium
