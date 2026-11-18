// Making webviews: the environment they all share, a tab's own, the one a page
// that opens a window is given, and all of them again after the browser process dies.
#include "browser.h"

#include <WebView2EnvironmentOptions.h>
#include <wrl/event.h>

#include <string>
#include <utility>

namespace exodium {

namespace {

using Microsoft::WRL::Callback;

using ControllerCompletedHandler = ICoreWebView2CreateCoreWebView2ControllerCompletedHandler;
using EnvironmentCompletedHandler = ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler;

// Extensions are on, for Bitwarden (extensions.cpp).
//
// Tools outside the browser, such as Claude Code, drive it over the DevTools protocol on
// the port winamp.ini names. There is none unless it does: anything running on this
// machine can use that port, and with it every site the browser is signed in to.
[[nodiscard]] ComPtr<ICoreWebView2EnvironmentOptions> EnvironmentOptions() {
    auto options = Microsoft::WRL::Make<CoreWebView2EnvironmentOptions>();
    if (ComPtr<ICoreWebView2EnvironmentOptions6> options6; SUCCEEDED(options.As(&options6))) {
        options6->put_AreBrowserExtensionsEnabled(TRUE);
    }
    if (g_app.debuggingPort != 0) {
        const std::wstring arguments = L"--remote-debugging-port=" + std::to_wstring(g_app.debuggingPort);
        options->put_AdditionalBrowserArguments(arguments.c_str());
    }
    return options;
}

// The profile a tab's webview is made in.
struct TabProfile {
    bool forClaude = false; // Claude's own, which has none of the user's sign-ins, site data or extensions
    bool isPrivate = false; // the private session of the profile, which keeps nothing
};
[[nodiscard]] TabProfile ProfileOf(const Tab& tab) noexcept {
    return {.forClaude = tab.forClaude, .isPrivate = tab.isPrivate};
}

// Makes a webview for a tab: in Claude's own profile for one of Claude's, in the private
// session for a tab of a private window, and in the user's profile for the rest. A runtime
// too old to have profiles gives Claude or a private window no webview at all, rather than
// one in the user's profile.
HRESULT CreateController(HWND parent, TabProfile profile,
                         ICoreWebView2CreateCoreWebView2ControllerCompletedHandler* done) {
    if (!profile.forClaude && !profile.isPrivate) return g_app.environment->CreateCoreWebView2Controller(parent, done);
    ComPtr<ICoreWebView2Environment10> environment10;
    ComPtr<ICoreWebView2ControllerOptions> options;
    if (FAILED(g_app.environment.As(&environment10)) ||
        FAILED(environment10->CreateCoreWebView2ControllerOptions(&options)) ||
        (profile.forClaude && FAILED(options->put_ProfileName(kClaudeProfile))) ||
        (profile.isPrivate && FAILED(options->put_IsInPrivateModeEnabled(TRUE)))) {
        return E_NOINTERFACE;
    }
    return environment10->CreateCoreWebView2ControllerWithOptions(parent, options.Get(), done);
}

// Whether a page asked for one of an extension's own windows: Bitwarden's are "popouts".
// Extensions can't say what size they asked for, so they get Bitwarden's popup's.
[[nodiscard]] bool IsExtensionPopout(std::wstring_view url) noexcept {
    return url.starts_with(L"chrome-extension://") && url.contains(L"uilocation=popout");
}
constexpr SIZE kPopoutSize{480, 630};

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
        LogFailure(L"Creating the WebView2 environment", result);
        g_app.environmentPending = false;
        g_app.environmentWaiters.clear(); // whoever asks next tries again
    };
    const HRESULT started = ::CreateCoreWebView2EnvironmentWithOptions(
        nullptr, WebViewProfileDir().c_str(), EnvironmentOptions().Get(),
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

void OnTabControllerCreated(int tabId, HWND createdIn, HRESULT result, ICoreWebView2Controller* controller) {
    const TabLocation at = FindTab(tabId);
    if (at) at.tab().creating = false;
    if (FAILED(result) || !controller) {
        LogFailure(L"Creating a WebView2 controller", FAILED(result) ? result : E_FAIL);
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
    // The extensions are the user's profile's: not Claude's, and not a private window's.
    if (!tab.forClaude && !tab.isPrivate) LoadExtensions(tab.webview.Get());
    // A tab muted before it had this webview, as one that was asleep, is muted still.
    if (tab.muted) SetTabMuted(tab, true);
    WireTabEvents(tabId);
    // A tab opened from Back or Forward makes its history first; see back_forward.cpp.
    if (tab.historyCopy) {
        StartHistoryCopy(tabId);
    } else {
        const std::wstring& url = tab.pendingUrl.empty() ? HomePage() : tab.pendingUrl;
        if (const HRESULT asked = tab.webview->Navigate(url.c_str()); FAILED(asked)) {
            LogFailure(tab.isPrivate ? std::wstring(L"Going to an address in a private window")
                                     : L"Going to " + url.substr(0, 200),
                       asked);
        }
    }

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

// The window a tab a page opened goes into: the opener's, unless that is out of sight.
// WebView2 hands what an extension opens to a webview of the extension's profile, which
// may be the first window's, hidden until it is shown from Winamp's menu. Then it goes to
// the window of the user's nearest the front, or else the opener's is shown.
[[nodiscard]] BrowserWindow& WindowInSight(BrowserWindow& opener) {
    if (::IsWindowVisible(opener.frame)) return opener;
    if (BrowserWindow* const front = FrontUserWindow()) return *front;
    ::ShowWindow(opener.frame, SW_SHOWNA);
    return opener;
}

// How long after the page's context menu was opened on a link a request for that link
// is still taken to come from the menu.
constexpr ULONGLONG kMenuLinkMs = 30'000;

// The webview's own context menu has "Open link in new window", which opens a tab here,
// as Chrome's "Open link in new tab" does. Chrome opens that one behind the tab in view.
// Which entry was picked isn't told, but a request for the very link the menu was opened
// on, made soon after, is that one.
[[nodiscard]] bool OpenedFromLinkMenu(int sourceTabId, ICoreWebView2NewWindowRequestedEventArgs* args) {
    const TabLocation source = FindTab(sourceTabId);
    if (!source) return false;
    Tab& tab = source.tab();
    const std::wstring link = std::exchange(tab.menuLink, {});
    if (link.empty() || ::GetTickCount64() - tab.menuLinkAt > kMenuLinkMs) return false;
    return ComString(args, &ICoreWebView2NewWindowRequestedEventArgs::get_Uri) == link;
}

// Whether a tab a page opened is left behind the tab in view, as in Chrome: one opened
// with Ctrl held, or from the page's context menu. Ctrl+Shift brings it into view. The
// event doesn't say how the link was clicked, but it arrives while the keys are still down.
[[nodiscard]] bool OpensBehind(int sourceTabId, ICoreWebView2NewWindowRequestedEventArgs* args) {
    BOOL byUser = FALSE;
    args->get_IsUserInitiated(&byUser);
    if (!byUser) return false;
    const bool ctrl = (::GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    const bool shift = (::GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    // Asked first, so that the menu's link is let go of whatever the keys say.
    const bool fromMenu = OpenedFromLinkMenu(sourceTabId, args);
    return (ctrl && !shift) || (fromMenu && !ctrl);
}

} // namespace

std::wstring SourceOf(ICoreWebView2* webview) {
    return ComString(webview, &ICoreWebView2::get_Source).value_or(std::wstring());
}

HRESULT CreateWebViewInProfile(HWND parent, const wchar_t* profileName, bool isPrivate,
                               ICoreWebView2CreateCoreWebView2ControllerCompletedHandler* done) {
    ComPtr<ICoreWebView2Environment10> environment10;
    ComPtr<ICoreWebView2ControllerOptions> options;
    if (!g_app.environment || FAILED(g_app.environment.As(&environment10)) ||
        FAILED(environment10->CreateCoreWebView2ControllerOptions(&options)) ||
        FAILED(options->put_ProfileName(profileName)) ||
        (isPrivate && FAILED(options->put_IsInPrivateModeEnabled(TRUE)))) {
        return E_NOINTERFACE;
    }
    return environment10->CreateCoreWebView2ControllerWithOptions(parent, options.Get(), done);
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
    FollowPageSound();
}

void TabIntoPage(BrowserWindow& win, bool backward) {
    Tab* const tab = ActiveTab(win);
    if (tab && tab->controller) {
        // The webview takes it as a Tab that reached it from outside: forward to the first
        // thing on its page that takes the keyboard, backward to the last.
        tab->controller->MoveFocus(backward ? COREWEBVIEW2_MOVE_FOCUS_REASON_PREVIOUS
                                            : COREWEBVIEW2_MOVE_FOCUS_REASON_NEXT);
        return;
    }
    // Its webview is still being made, as just after Ctrl+T. The keyboard waits in the
    // window, out of the address bar, and the page is given it as it comes
    // (OnTabControllerCreated).
    ::SetFocus(win.client);
}

void ReloadTab(Tab& tab, bool pastCache) {
    // One with no webview, not looked at yet or asleep, loads when it is looked at.
    if (!tab.webview) return;
    if (pastCache) tab.webview->CallDevToolsProtocolMethod(L"Page.reload", L"{\"ignoreCache\":true}", nullptr);
    else tab.webview->Reload();
}

void MoveFocusToWebView(Tab& tab) {
    if (!tab.controller) return;
    // A tab of Claude's doesn't take the keyboard into a window the user isn't in: giving
    // it to a page in a window that isn't active would make that window the active one.
    if (tab.forClaude) {
        HWND parent = nullptr;
        if (FAILED(tab.controller->get_ParentWindow(&parent)) ||
            ::GetAncestor(parent, GA_ROOT) != ::GetForegroundWindow()) {
            return;
        }
    }
    tab.controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
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
        const HRESULT started = CreateController(
            parent, ProfileOf(tab),
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
void OpenRequestedWindow(BrowserWindow& opener, int sourceTabId, ICoreWebView2NewWindowRequestedEventArgs* request) {
    // Bitwarden's window for unlocking, from the "Unlock account" in its menu in a page, is
    // its popup under the toolbar button instead, in the window the page is in: that one
    // goes again once the vault is unlocked, and gives the page the keyboard back, which
    // Bitwarden's own window can't be made to do in WebView2.
    const TabLocation asker = FindTab(sourceTabId);
    if (IsBitwardenUnlockPopout(ComString(request, &ICoreWebView2NewWindowRequestedEventArgs::get_Uri).value_or(L"")) &&
        !(asker && (asker.tab().forClaude || asker.tab().isPrivate)) &&
        OpenBitwardenPopupToUnlock(WindowInSight(opener))) {
        request->put_Handled(TRUE);
        return;
    }

    ComPtr<ICoreWebView2NewWindowRequestedEventArgs> args(request);
    ComPtr<ICoreWebView2Deferral> deferral;
    args->GetDeferral(&deferral);

    // The tab goes into the opener's window, unless the page asked for a window. An
    // extension's own window, such as one Bitwarden pops out, is one it asked for.
    std::optional<PopupRequest> popup = RequestedPopup(request);
    if (!popup &&
        IsExtensionPopout(ComString(request, &ICoreWebView2NewWindowRequestedEventArgs::get_Uri).value_or(L""))) {
        popup = PopupRequest{.size = kPopoutSize};
    }
    BrowserWindow& win = popup ? opener : WindowInSight(opener);
    BrowserWindow* const popupWindow = popup ? CreatePopupWindow(win, *popup) : nullptr;
    const bool isPopup = popupWindow != nullptr;
    BrowserWindow& host = isPopup ? *popupWindow : win;

    // A tab in the opener's window goes beside the tab in view, as in Chrome, and counts
    // as opened from it. One brought into view starts afresh: the tabs opened before it
    // no longer count as opened from anywhere.
    const bool behind = !isPopup && OpensBehind(sourceTabId, request);
    const int index = isPopup ? 0 : LinkTabIndex(host, !behind);
    const int openerId = isPopup ? 0 : ActiveTabId(host);
    if (!isPopup && !behind) ForgetOpeners(host);
    const TabLocation source = FindTab(sourceTabId);
    const bool forClaude = source && source.tab().forClaude;
    const std::uint32_t claudeSession = source ? source.tab().claudeSession : 0;
    // The page that asked for it says which profile it is in, and it is as private as that page.
    const TabProfile profile = source ? ProfileOf(source.tab()) : TabProfile{};
    Tab& added = AddTab(host, kNewTabTitle, index);
    added.creating = true;
    added.openerId = openerId;
    added.forClaude = forClaude;
    added.claudeSession = claudeSession;
    added.isPrivate = profile.isPrivate;
    const int tabId = added.id;
    LayoutToolbarAndTabs(host);

    if (isPopup) {
        SwitchToTab(host, index);
        if (forClaude) {
            ShowWithoutFocus(host);
        } else {
            ::ShowWindow(host.frame, SW_SHOWNA);
            ::SetForegroundWindow(host.frame);
        }
    } else if (!behind) {
        SwitchToTab(host, index);
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
    // In the opener's profile, which the new page has to be in to be given to it.
    const HRESULT started = CreateController(
        parent, profile,
        Callback<ControllerCompletedHandler>([finish](HRESULT result, ICoreWebView2Controller* controller) -> HRESULT {
            finish(result, controller);
            return S_OK;
        }).Get());
    if (FAILED(started)) finish(started, nullptr);
}

// Each tab gives up its dead webview and is left as a tab is before it is first
// looked at, headed for the page it was on. The tabs in view get a new one at once
// and the rest when they are switched to. Where Back and Forward led went with the
// process, so each is given its pages again, which its new webview makes its history.
void ReviveWebViews() {
    g_app.reviving = false;
    for (const auto& win : g_app.windows) {
        ExitVideoFullscreen(*win);
        for (Tab& tab : win->tabs) PutTabToSleep(tab);
        UpdateReloadButton(*win);
        LayoutActiveWebView(*win); // the DevTools docked in it went with the page they were for
    }
    for (ClosedTab& closed : g_app.closedTabs) DiscardClosedWebView(closed);
    AbandonDownloads();
    ForgetExtensions();        // the next webview of the user's profile looks at them afresh
    g_app.environment.Reset(); // it belonged to the dead process; the next webview asked for makes a new one

    for (const auto& win : g_app.windows) {
        if (::IsWindowVisible(win->frame)) ::PostMessageW(win->client, kMsgInitWebView, 0, 0);
    }
}

// A page a tool makes through the debugging port is a window of the browser process's
// own, which no tab of ours knows about. It would keep that process going after Winamp
// has gone, and the port open with it. Any webview will do to pass the word on.
void CloseBrowserProcess() {
    for (const auto& win : g_app.windows) {
        for (const Tab& tab : win->tabs) {
            if (!tab.webview) continue;
            tab.webview->CallDevToolsProtocolMethod(L"Browser.close", L"{}", nullptr);
            return;
        }
    }
}

} // namespace exodium
