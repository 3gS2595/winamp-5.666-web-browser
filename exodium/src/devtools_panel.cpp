// A tab's DevTools, docked in the tab's window: beside its page or under it, as Chrome has
// them, in place of the window of their own the webview opens them in.
//
// WebView2 has no way to dock its DevTools. But the debugging port serves the DevTools
// themselves as a page (measured 2026-11-13 on runtime 157), and takes that page's
// connection back to it. So a tab's docked DevTools are a second webview in the tab's
// window, on that page for the tab's own page; the tab's page gives up one side of its
// place to it, with a splitter between the two that sizes them. That is only there to be
// had while winamp.ini names a debugging port (see webview.cpp): without one, F12 and
// "Inspect" are the webview's own, as before.
//
// - F12 and Ctrl+Shift+I open them for the tab in view, and close them. Ctrl+Shift+J opens
//   them on the console, and Ctrl+Shift+C to pick an element of the page with the mouse.
//   "Inspect" in a page's own menu opens them on the element the menu was opened on.
// - Each tab has its own, shown with the tab and gone with its page: when the tab closes,
//   and when it is put to sleep, which one with its DevTools open isn't.
// - A strip along their top has what the DevTools' own menu has in Chrome: the side they
//   are docked on, a window of their own (the webview's, for this once), and closing them.
//   The side, the size they were dragged to and their zoom are kept in winamp.ini.
// - Their webview is in a profile of its own, kDevToolsProfile, so that what they keep of
//   themselves lasts and no extension of the user's runs in them. A private window's are
//   in that profile's private session, and keep nothing.
//
// The page served there is the DevTools as a remote tool has them, with no browser round
// them: they have no button for emulating a device, which in Chrome is the browser's doing.
// Three things reach into that page from here, and are no more than a convenience if a
// later version of it should have them otherwise: selecting an element, picking one with
// the mouse, and showing the console once they are open. Everything else is the address.
#include "browser.h"

#include "gdiplus_headers.h"
#include "json_reader.h"
#include "text_util.h"

#include <commctrl.h>
#include <windowsx.h>
#include <wrl/event.h>

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace exodium {

namespace {

using Microsoft::WRL::Callback;

using ControllerHandler = ICoreWebView2CreateCoreWebView2ControllerCompletedHandler;
using ProtocolHandler = ICoreWebView2CallDevToolsProtocolMethodCompletedHandler;
using ScriptHandler = ICoreWebView2ExecuteScriptCompletedHandler;

constexpr int kBarPadding = 8; // left of the strip's title
constexpr wchar_t kBarTitle[] = L"DevTools";
// What the webview shows until the DevTools' page has painted, which is dark as well.
constexpr COREWEBVIEW2_COLOR kPanelBackground{255, 32, 32, 32};

int g_serial = 0;      // the last opening's; see DockedDevTools::serial
DevToolsPrefs g_saved; // as winamp.ini has them

// ---- what is kept of them ----

[[nodiscard]] std::optional<std::wstring> ReadIniValue(std::wstring_view key) {
    if (!g_app.winampIni) return std::nullopt;
    wchar_t value[64] = {};
    const DWORD length = ::GetPrivateProfileStringW(kSettingsSection, std::wstring(key).c_str(), L"", value,
                                                    static_cast<DWORD>(std::size(value)), g_app.winampIni);
    if (length == 0) return std::nullopt;
    return std::wstring(value, length);
}

// Writes what has changed of them since they were last read or written.
void SavePrefs() {
    if (!g_app.winampIni || g_app.devtoolsPrefs == g_saved) return;
    const auto before = DevToolsPrefEntries(g_saved), after = DevToolsPrefEntries(g_app.devtoolsPrefs);
    for (std::size_t i = 0; i < after.size() && i < before.size(); ++i) {
        if (after[i].value == before[i].value) continue;
        ::WritePrivateProfileStringW(kSettingsSection, std::wstring(after[i].key).c_str(), after[i].value.c_str(),
                                     g_app.winampIni);
    }
    g_saved = g_app.devtoolsPrefs;
}

// ---- what their page is told ----

// The start of a script for the DevTools' page that waits for the page they are for to be
// known to them, which it is a moment after they load: `target` is that page, as they
// have it. Their own modules are had by importing them again, which gives the ones in use.
constexpr wchar_t kWithTarget[] = LR"js((async () => {
  const at = name => new URL(name, location.href).href;
  const SDK = await import(at('core/sdk/sdk.js'));
  let target = null;
  for (let tries = 0; tries < 150 && !target; ++tries) {
    target = SDK.TargetManager.TargetManager.instance().primaryPageTarget();
    if (!target) await new Promise(done => setTimeout(done, 100));
  }
  if (!target) return;
)js";
constexpr wchar_t kScriptEnd[] = L"\n})().catch(() => {});";

// Selects a node of the page in the Elements tool, which is shown: the node the page's
// own DevTools session knows by this number.
[[nodiscard]] std::wstring RevealNodeScript(int backendNodeId) {
    std::wstring script = kWithTarget;
    script
        .append(L"  const Common = await import(at('core/common/common.js'));\n"
                L"  const node = await new SDK.DOMModel.DeferredDOMNode(target, ")
        .append(std::to_wstring(backendNodeId))
        .append(L").resolvePromise();\n"
                L"  if (node) await Common.Revealer.reveal(node);")
        .append(kScriptEnd);
    return script;
}

// Has the mouse pick an element of the page, or with `toggle` stop picking one if it is.
[[nodiscard]] std::wstring PickElementScript(bool toggle) {
    std::wstring script = kWithTarget;
    script
        .append(L"  const UI = await import(at('ui/legacy/legacy.js'));\n"
                L"  const action = UI.ActionRegistry.ActionRegistry.instance()"
                L".getAction('elements.toggle-element-search');\n"
                L"  if (")
        .append(toggle ? L"true" : L"false")
        .append(L" || !action.toggled()) await action.execute();")
        .append(kScriptEnd);
    return script;
}

// Shows one of their tools, by the name the address has for it.
[[nodiscard]] std::wstring ShowToolScript(std::wstring_view tool) {
    return L"try { InspectorFrontendAPI.showPanel(" + JsStringLiteral(tool) + L"); } catch (e) {}";
}

// Runs a script in a tab's DevTools: now, or once their page has loaded.
void Tell(Tab& tab, std::wstring script) {
    DockedDevTools& devtools = tab.devtools;
    if (!devtools.open) return;
    if (devtools.loaded && devtools.webview) devtools.webview->ExecuteScript(script.c_str(), nullptr);
    else devtools.waiting.push_back(std::move(script));
}

// ---- opening and closing ----

// The tab, if its DevTools are still the ones opened as `serial`.
[[nodiscard]] TabLocation StillOpen(int tabId, int serial) noexcept {
    const TabLocation at = FindTab(tabId);
    if (!at || !at.tab().devtools.open || at.tab().devtools.serial != serial) return {};
    return at;
}

// Closes a tab's DevTools from the message loop: from inside an event of their own webview
// they can't be closed there and then.
void PostClose(int tabId, int serial, DevToolsClose how) {
    const TabLocation at = StillOpen(tabId, serial);
    if (!at || !at.window->client) return;
    ::PostMessageW(at.window->client, kMsgCloseDevTools, static_cast<WPARAM>(tabId),
                   static_cast<LPARAM>(serial) << 2 | static_cast<LPARAM>(how));
}

// They can't be docked after all: the webview's own, in a window of theirs, stand in.
void GiveUpDocking(int tabId, int serial, std::wstring_view what, HRESULT result) {
    LogFailure(what, result);
    PostClose(tabId, serial, DevToolsClose::kOwnWindow);
}

// Whether a window is the one the user is in, so that giving a webview of it the keyboard
// makes no other window the active one.
[[nodiscard]] bool IsInFront(const BrowserWindow& win) noexcept {
    return win.frame && ::GetAncestor(win.frame, GA_ROOT) == ::GetForegroundWindow();
}

void FocusDevTools(const TabLocation& at) {
    const DockedDevTools& devtools = at.tab().devtools;
    if (devtools.controller && at.isActive() && !at.window->isFullscreen && IsInFront(*at.window)) {
        devtools.controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
    }
}

// Shows every open DevTools at this size, which is kept. Each of their webviews says so in
// its turn (OnZoomed), and by then that is the size there is.
void SetZoom(int percent) {
    percent = std::clamp(percent, kDevToolsZoomLeast, kDevToolsZoomMost);
    if (percent == g_app.devtoolsPrefs.zoomPercent) return;
    g_app.devtoolsPrefs.zoomPercent = percent;
    SavePrefs();
    for (const auto& win : g_app.windows) {
        for (const Tab& tab : win->tabs) {
            if (tab.devtools.controller) tab.devtools.controller->put_ZoomFactor(percent / 100.0);
        }
    }
}

// Ctrl and the wheel in the DevTools, which the webview has zoomed for: the others follow.
void OnZoomed(ICoreWebView2Controller* controller) {
    double factor = 1.0;
    if (!controller || FAILED(controller->get_ZoomFactor(&factor))) return;
    SetZoom(static_cast<int>(std::lround(factor * 100.0)));
}

// The browser's shortcuts that are its own from inside the DevTools too, as they are in
// Chrome: the ones for tabs and windows. The rest of what the browser takes from a page
// (Ctrl+L, Ctrl+D, Ctrl+H, Ctrl+J, Alt with the arrows) the DevTools have uses of their own for.
[[nodiscard]] bool IsBrowsersInDevTools(UINT key, Modifiers mods) noexcept {
    const bool digit = (key >= '1' && key <= '9') || (key >= VK_NUMPAD1 && key <= VK_NUMPAD9);
    if (mods == kCtrl) {
        return key == 'T' || key == 'W' || key == 'N' || key == VK_TAB || key == VK_NEXT || key == VK_PRIOR || digit;
    }
    if (mods == kCtrlShift) return key == 'T' || key == 'N' || key == 'W' || key == VK_TAB;
    if (mods == kAlt) return key == VK_F4 || key == 'D';
    return mods == kNoModifiers && key == VK_F6;
}

// A key pressed with the keyboard in the DevTools.
void OnKey(int tabId, int serial, ICoreWebView2AcceleratorKeyPressedEventArgs* args) {
    if (!args) return;
    UINT key = 0;
    COREWEBVIEW2_KEY_EVENT_KIND kind{};
    args->get_VirtualKey(&key);
    args->get_KeyEventKind(&kind);
    if (kind != COREWEBVIEW2_KEY_EVENT_KIND_KEY_DOWN && kind != COREWEBVIEW2_KEY_EVENT_KIND_SYSTEM_KEY_DOWN) return;
    const TabLocation at = StillOpen(tabId, serial);
    if (!at) return;
    const Modifiers mods = CurrentModifiers();

    // The keys that opened them close them from inside.
    if ((mods == kNoModifiers && key == VK_F12) || (mods == kCtrlShift && key == 'I')) {
        args->put_Handled(TRUE);
        PostClose(tabId, serial, DevToolsClose::kRefocus);
        return;
    }
    if (mods == kCtrlShift && key == 'J') {
        args->put_Handled(TRUE);
        Tell(at.tab(), ShowToolScript(L"console"));
        return;
    }
    // Their size is the webview's zoom, which the keys for it in the DevTools' own page
    // would ask of a browser that isn't round them here.
    if (mods.ctrl && !mods.alt) {
        const bool larger = key == VK_OEM_PLUS || key == VK_ADD, smaller = key == VK_OEM_MINUS || key == VK_SUBTRACT;
        const bool normal = !mods.shift && (key == '0' || key == VK_NUMPAD0);
        if (larger || smaller || normal) {
            args->put_Handled(TRUE);
            SetZoom(normal ? 100 : NextDevToolsZoom(g_app.devtoolsPrefs.zoomPercent, larger));
            return;
        }
    }
    if (IsBrowsersInDevTools(key, mods) && HandleGlobalShortcut(at.window, key, mods)) args->put_Handled(TRUE);
}

// A link in the DevTools, to their documentation or to something of the page's, opens in
// a tab of the window they are in.
void OnLink(int tabId, ICoreWebView2NewWindowRequestedEventArgs* args) {
    if (!args) return;
    args->put_Handled(TRUE);
    const TabLocation at = FindTab(tabId);
    const auto url = ComString(args, &ICoreWebView2NewWindowRequestedEventArgs::get_Uri);
    if (!at || !url || at.window->isFullscreen || at.window->closing) return;
    if (!url->starts_with(L"https://") && !url->starts_with(L"http://")) return;
    NewTab(*at.window, *url);
}

// Their page has loaded, or hasn't.
void OnLoaded(int tabId, int serial, ICoreWebView2NavigationCompletedEventArgs* args) {
    const TabLocation at = StillOpen(tabId, serial);
    if (!at || at.tab().devtools.loaded) return;
    BOOL succeeded = FALSE;
    int status = 0;
    if (args) args->get_IsSuccess(&succeeded);
    if (ComPtr<ICoreWebView2NavigationCompletedEventArgs2> more;
        args && SUCCEEDED(args->QueryInterface(IID_PPV_ARGS(&more)))) {
        more->get_HttpStatusCode(&status);
    }
    if (!succeeded || status >= 400) {
        // Nothing serves them: the port is another program's, or this runtime has no page for them.
        GiveUpDocking(tabId, serial, L"Loading the DevTools from the debugging port", E_FAIL);
        return;
    }
    DockedDevTools& devtools = at.tab().devtools;
    devtools.loaded = true;
    for (const std::wstring& script : std::exchange(devtools.waiting, {})) {
        devtools.webview->ExecuteScript(script.c_str(), nullptr);
    }
}

void Wire(int tabId, int serial, ICoreWebView2* webview, ICoreWebView2Controller* controller) {
    // The DevTools are all their webview shows, whatever their page would go to.
    webview->add_NavigationStarting(Callback<ICoreWebView2NavigationStartingEventHandler>(
                                        [](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* args) -> HRESULT {
                                            const auto url =
                                                ComString(args, &ICoreWebView2NavigationStartingEventArgs::get_Uri);
                                            if (!url || !IsDevToolsUrl(g_app.debuggingPort, *url))
                                                args->put_Cancel(TRUE);
                                            return S_OK;
                                        })
                                        .Get(),
                                    nullptr);
    webview->add_NavigationCompleted(
        Callback<ICoreWebView2NavigationCompletedEventHandler>(
            [tabId, serial](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs* args) -> HRESULT {
                OnLoaded(tabId, serial, args);
                return S_OK;
            })
            .Get(),
        nullptr);
    webview->add_NewWindowRequested(
        Callback<ICoreWebView2NewWindowRequestedEventHandler>([tabId](ICoreWebView2*,
                                                                      ICoreWebView2NewWindowRequestedEventArgs* args)
                                                                  -> HRESULT {
            OnLink(tabId, args);
            return S_OK;
        }).Get(),
        nullptr);
    webview->add_WindowCloseRequested(
        Callback<ICoreWebView2WindowCloseRequestedEventHandler>([tabId, serial](ICoreWebView2*, IUnknown*) -> HRESULT {
            PostClose(tabId, serial, DevToolsClose::kRefocus);
            return S_OK;
        }).Get(),
        nullptr);
    // Their own page's process ending takes only them. The browser's takes every page, and
    // the tab's webview says so (OnProcessFailed in webview_events.cpp).
    webview->add_ProcessFailed(
        Callback<ICoreWebView2ProcessFailedEventHandler>([tabId, serial](
                                                             ICoreWebView2*,
                                                             ICoreWebView2ProcessFailedEventArgs* args) -> HRESULT {
            COREWEBVIEW2_PROCESS_FAILED_KIND kind{};
            if (args && SUCCEEDED(args->get_ProcessFailedKind(&kind)) &&
                kind == COREWEBVIEW2_PROCESS_FAILED_KIND_RENDER_PROCESS_EXITED) {
                PostClose(tabId, serial, DevToolsClose::kPlain);
            }
            return S_OK;
        }).Get(),
        nullptr);
    controller->add_AcceleratorKeyPressed(
        Callback<ICoreWebView2AcceleratorKeyPressedEventHandler>(
            [tabId, serial](ICoreWebView2Controller*, ICoreWebView2AcceleratorKeyPressedEventArgs* args) -> HRESULT {
                OnKey(tabId, serial, args);
                return S_OK;
            })
            .Get(),
        nullptr);
    controller->add_ZoomFactorChanged(
        Callback<ICoreWebView2ZoomFactorChangedEventHandler>([](ICoreWebView2Controller* sender, IUnknown*) -> HRESULT {
            OnZoomed(sender);
            return S_OK;
        }).Get(),
        nullptr);
}

// Their webview is no browser: nothing of one answers its keys, and it has no DevTools of
// its own to open on the DevTools.
void Configure(ICoreWebView2* webview, ICoreWebView2Controller* controller) {
    ComPtr<ICoreWebView2Settings> settings;
    if (SUCCEEDED(webview->get_Settings(&settings)) && settings) {
        settings->put_AreDevToolsEnabled(FALSE);
        settings->put_IsStatusBarEnabled(FALSE);
        if (ComPtr<ICoreWebView2Settings3> settings3; SUCCEEDED(settings.As(&settings3))) {
            settings3->put_AreBrowserAcceleratorKeysEnabled(FALSE);
        }
        if (ComPtr<ICoreWebView2Settings4> settings4; SUCCEEDED(settings.As(&settings4))) {
            settings4->put_IsGeneralAutofillEnabled(FALSE);
            settings4->put_IsPasswordAutosaveEnabled(FALSE);
        }
    }
    if (ComPtr<ICoreWebView2Controller2> controller2;
        SUCCEEDED(controller->QueryInterface(IID_PPV_ARGS(&controller2)))) {
        controller2->put_DefaultBackgroundColor(kPanelBackground);
    }
    PreferDarkColorScheme(webview);
}

void OnControllerCreated(int tabId, int serial, HWND createdIn, const std::wstring& url, HRESULT result,
                         ICoreWebView2Controller* controller) {
    const TabLocation at = StillOpen(tabId, serial);
    if (FAILED(result) || !controller || !at) {
        if (controller) controller->Close();
        if (at) GiveUpDocking(tabId, serial, L"Making a webview for the DevTools", FAILED(result) ? result : E_FAIL);
        return;
    }
    BrowserWindow& win = *at.window;
    DockedDevTools& devtools = at.tab().devtools;
    devtools.controller = WebViewController(controller);
    controller->get_CoreWebView2(&devtools.webview);
    if (!devtools.webview) {
        GiveUpDocking(tabId, serial, L"Making a webview for the DevTools", E_FAIL);
        return;
    }
    // The tab may have been dragged to another window while this was pending.
    if (win.client != createdIn) controller->put_ParentWindow(win.client);
    controller->put_IsVisible(FALSE);
    Configure(devtools.webview.Get(), controller);
    controller->put_ZoomFactor(g_app.devtoolsPrefs.zoomPercent / 100.0);
    Wire(tabId, serial, devtools.webview.Get(), controller);
    if (const HRESULT asked = devtools.webview->Navigate(url.c_str()); FAILED(asked)) {
        GiveUpDocking(tabId, serial, L"Going to the DevTools on the debugging port", asked);
        return;
    }
    if (at.isActive()) {
        LayoutActiveWebView(win);
        FocusDevTools(at);
    }
}

// The id tools on the debugging port know a page by, out of its answer to Target.getTargetInfo.
[[nodiscard]] std::optional<std::wstring> TargetIdIn(std::wstring_view json) {
    std::optional<std::wstring> id;
    JsonReader reader(json);
    const bool read = reader.Object([&](const std::wstring& key) {
        if (key != L"targetInfo") return reader.Skip();
        return reader.Object([&](const std::wstring& name) {
            if (name != L"targetId") return reader.Skip();
            id = reader.String();
            return id.has_value();
        });
    });
    return read ? id : std::nullopt;
}

// The number the page's DevTools session knows a node by, out of its answer to
// DOM.getNodeForLocation.
[[nodiscard]] std::optional<int> BackendNodeIdIn(std::wstring_view json) {
    std::optional<int> id;
    JsonReader reader(json);
    const bool read = reader.Object([&](const std::wstring& key) {
        if (key != L"backendNodeId") return reader.Skip();
        id = reader.Integer();
        return id.has_value();
    });
    return read ? id : std::nullopt;
}

void OnTargetKnown(int tabId, int serial, const std::optional<std::wstring>& targetId, const std::wstring& tool) {
    const TabLocation at = StillOpen(tabId, serial);
    if (!at) return;
    const auto url = targetId ? DevToolsUrl(g_app.debuggingPort, *targetId, tool) : std::nullopt;
    if (!url || !g_app.environment) {
        GiveUpDocking(tabId, serial, L"Asking a page what the debugging port knows it by", E_FAIL);
        return;
    }
    const HWND parent = at.window->client;
    // A private window's are in the private session of their profile: what they would keep
    // of the page (what was typed into the console, where its scripts were stopped) isn't kept.
    const HRESULT started = CreateWebViewInProfile(
        parent, kDevToolsProfile, at.tab().isPrivate,
        Callback<ControllerHandler>([tabId, serial, parent,
                                     url = *url](HRESULT result, ICoreWebView2Controller* controller) -> HRESULT {
            OnControllerCreated(tabId, serial, parent, url, result, controller);
            return S_OK;
        }).Get());
    if (FAILED(started)) OnControllerCreated(tabId, serial, parent, *url, started, nullptr);
}

// Opens a tab's DevTools, docked, on one of their tools ("console", "elements"), or on
// the one they showed last. The tab has a webview.
void Open(BrowserWindow& win, Tab& tab, std::wstring_view tool) {
    tab.devtools = {};
    tab.devtools.open = true;
    const int serial = tab.devtools.serial = ++g_serial;
    const int tabId = tab.id;
    // Their side of the window is set aside at once; they come into it in a moment.
    if (ActiveTab(win) == &tab) LayoutActiveWebView(win);
    const HRESULT asked = tab.webview->CallDevToolsProtocolMethod(
        L"Target.getTargetInfo", L"{}",
        Callback<ProtocolHandler>([tabId, serial, tool = std::wstring(tool)](HRESULT result, LPCWSTR json) -> HRESULT {
            OnTargetKnown(tabId, serial, SUCCEEDED(result) && json ? TargetIdIn(json) : std::nullopt, tool);
            return S_OK;
        }).Get());
    if (FAILED(asked)) GiveUpDocking(tabId, serial, L"Asking a page what the debugging port knows it by", asked);
}

// ---- "Inspect" in a page's menu ----

// Opens a tab's DevTools on a node of its page, or just on the Elements tool if the node
// isn't known.
void RevealNode(int tabId, std::optional<int> backendNodeId) {
    const TabLocation found = FindTab(tabId);
    if (!found || !found.tab().webview || found.window->closing) return;
    BrowserWindow& win = *found.window;
    LeaveVideoFullscreen(win); // lays the window out again, but leaves its tabs where they are
    Tab& tab = found.tab();
    const bool wasOpen = tab.devtools.open;
    if (!wasOpen) Open(win, tab, L"elements");
    if (backendNodeId) Tell(tab, RevealNodeScript(*backendNodeId));
    else if (wasOpen) Tell(tab, ShowToolScript(L"elements"));
    FocusDevTools(found);
}

// The element at a point of a tab's webview, in the pixels its bounds are in. The node is
// found before the DevTools open, which makes the page narrower and may move what is in it.
void InspectElementAt(int tabId, POINT inView) {
    const TabLocation at = FindTab(tabId);
    if (!at || !at.tab().webview || !at.tab().controller) return;
    RECT bounds{};
    at.tab().controller->get_Bounds(&bounds);
    const int width = Width(bounds), height = Height(bounds);

    const auto findNode = [tabId](POINT inDocument) {
        const TabLocation found = FindTab(tabId);
        if (!found || !found.tab().webview) return false;
        const std::wstring where = L"{\"x\":" + std::to_wstring(inDocument.x) + L",\"y\":" +
                                   std::to_wstring(inDocument.y) + L",\"includeUserAgentShadowDOM\":false}";
        return SUCCEEDED(found.tab().webview->CallDevToolsProtocolMethod(
            L"DOM.getNodeForLocation", where.c_str(),
            Callback<ProtocolHandler>([tabId](HRESULT result, LPCWSTR json) -> HRESULT {
                RevealNode(tabId, SUCCEEDED(result) && json ? BackendNodeIdIn(json) : std::nullopt);
                return S_OK;
            }).Get()));
    };
    // The page says how large it is in its own pixels and how far it is scrolled, which
    // turns the point into the place in its document that the DevTools know nodes by.
    const HRESULT asked = at.tab().webview->ExecuteScript(
        kPageMetricsScript,
        Callback<ScriptHandler>([tabId, inView, width, height, findNode](HRESULT result, LPCWSTR json) -> HRESULT {
            const auto page = SUCCEEDED(result) && json ? ParsePageMetrics(json) : std::nullopt;
            const auto inDocument = page ? DocumentPointOf(inView, width, height, *page) : std::nullopt;
            if (!inDocument || !findNode(*inDocument)) RevealNode(tabId, std::nullopt);
            return S_OK;
        }).Get());
    if (FAILED(asked)) RevealNode(tabId, std::nullopt);
}

// ---- the strip along their top, and the splitter ----

[[nodiscard]] Gdiplus::Color GdiplusColor(COLORREF color) {
    Gdiplus::Color result;
    result.SetFromCOLORREF(color);
    return result;
}

[[nodiscard]] std::optional<DockSide> SideOf(DevToolsButton button) noexcept {
    switch (button) {
    case DevToolsButton::DockRight:
        return DockSide::Right;
    case DevToolsButton::DockBottom:
        return DockSide::Bottom;
    case DevToolsButton::DockLeft:
        return DockSide::Left;
    default:
        return std::nullopt;
    }
}

// A button's picture, in the middle of `box`: a window with the side the DevTools go on
// filled in, two windows for one of their own, a cross.
void DrawButtonPicture(HDC dc, DevToolsButton button, const RECT& box, COLORREF ink) {
    const UniqueBrush brush(::CreateSolidBrush(ink));
    const int x = (box.left + box.right) / 2, y = (box.top + box.bottom) / 2;
    const auto fill = [&](int left, int top, int right, int bottom) {
        const RECT rect{x + left, y + top, x + right, y + bottom};
        ::FillRect(dc, &rect, brush.get());
    };
    const auto frame = [&](int left, int top, int right, int bottom) {
        const RECT rect{x + left, y + top, x + right, y + bottom};
        ::FrameRect(dc, &rect, brush.get());
    };
    switch (button) {
    case DevToolsButton::DockRight:
        frame(-6, -5, 7, 5);
        fill(2, -5, 7, 5);
        break;
    case DevToolsButton::DockLeft:
        frame(-6, -5, 7, 5);
        fill(-6, -5, -1, 5);
        break;
    case DevToolsButton::DockBottom:
        frame(-6, -5, 7, 5);
        fill(-6, 1, 7, 5);
        break;
    case DevToolsButton::OwnWindow:
        frame(-6, -2, 4, 6);
        fill(-3, -6, 7, -5); // the top and the right of the one behind
        fill(6, -6, 7, 3);
        fill(-3, -6, -2, -2);
        fill(4, 2, 7, 3);
        break;
    case DevToolsButton::Close: {
        Gdiplus::Graphics graphics(dc);
        graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        const Gdiplus::Pen pen(GdiplusColor(ink), 1.5f);
        const float cx = (box.left + box.right) / 2.0f, cy = (box.top + box.bottom) / 2.0f;
        graphics.DrawLine(&pen, cx - 4.0f, cy - 4.0f, cx + 4.0f, cy + 4.0f);
        graphics.DrawLine(&pen, cx - 4.0f, cy + 4.0f, cx + 4.0f, cy - 4.0f);
        break;
    }
    case DevToolsButton::None:
        break;
    }
}

void PaintBar(HWND bar, const BrowserWindow* win) {
    PAINTSTRUCT paint{};
    const HDC target = ::BeginPaint(bar, &paint);
    if (!target) return;
    const RECT rect = ClientRectOf(bar);
    // Drawn off screen and copied over in one go, so that it never shows half drawn.
    const UniqueDc buffer(::CreateCompatibleDC(target));
    const UniqueBitmap bitmap(::CreateCompatibleBitmap(target, std::max(1L, rect.right), std::max(1L, rect.bottom)));
    const HDC dc = buffer.get();
    {
        const SelectedObject selectedBitmap(dc, bitmap.get());
        const SelectedObject selectedFont(dc, g_app.theme.font);
        const Theme& theme = g_app.theme;
        ::FillRect(dc, &rect, theme.panel.get());
        const RECT line{0, rect.bottom - 1, rect.right, rect.bottom};
        ::FillRect(dc, &line, theme.border.get());

        const DevToolsUi none;
        const DevToolsUi& ui = win ? win->devtools : none;
        int titleRight = rect.right - kBarPadding;
        for (const DevToolsButton button : kDevToolsButtons) {
            const RECT box = DevToolsButtonRect(rect.right, button);
            if (::IsRectEmpty(&box)) continue;
            titleRight = std::min(titleRight, static_cast<int>(box.left) - kDevToolsButtonGap);
            const bool hot = ui.hot == button, pressed = ui.pressed == button;
            if (hot || pressed) {
                const UniqueBrush back(::CreateSolidBrush(pressed && hot ? color::kClosePressed : color::kCloseHot));
                ::FillRect(dc, &box, back.get());
            }
            // The side they are on now is picked out.
            const bool current = SideOf(button) == g_app.devtoolsPrefs.side;
            DrawButtonPicture(dc, button, box, current ? color::kSpinner : hot ? color::kText : color::kSuggestDim);
        }
        if (titleRight > kBarPadding) {
            RECT title{kBarPadding, 0, titleRight, rect.bottom - 1};
            ::SetBkMode(dc, TRANSPARENT);
            ::SetTextColor(dc, color::kText);
            ::DrawTextW(dc, kBarTitle, -1, &title,
                        DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        }
        ::BitBlt(target, 0, 0, rect.right, rect.bottom, dc, 0, 0, SRCCOPY);
    }
    ::EndPaint(bar, &paint);
}

// A button of the strip as the tips' control knows it: a part of the strip.
[[nodiscard]] TOOLINFOW TipFor(HWND bar, DevToolsButton button) noexcept {
    TOOLINFOW tool{};
    tool.cbSize = TTTOOLINFOW_V1_SIZE;
    tool.uFlags = TTF_SUBCLASS;
    tool.hwnd = bar;
    tool.uId = static_cast<UINT_PTR>(button);
    tool.rect = DevToolsButtonRect(ClientRectOf(bar).right, button);
    return tool;
}

// Docks every window's DevTools on another side.
void DockOn(DockSide side) {
    if (g_app.devtoolsPrefs.side == side) return;
    g_app.devtoolsPrefs.side = side;
    SavePrefs();
    for (const auto& win : g_app.windows) {
        if (!win->client || win->closing) continue;
        LayoutActiveWebView(*win);
        if (win->devtools.bar) ::InvalidateRect(win->devtools.bar, nullptr, FALSE);
    }
}

void OnBarButton(BrowserWindow& win, DevToolsButton button) {
    const int tabId = ActiveTabId(win);
    if (const auto side = SideOf(button)) {
        DockOn(*side);
    } else if (button == DevToolsButton::Close) {
        CloseDevTools(tabId, DevToolsClose::kRefocus);
    } else if (button == DevToolsButton::OwnWindow) {
        CloseDevTools(tabId, DevToolsClose::kOwnWindow);
    }
}

void SetHot(BrowserWindow& win, DevToolsButton button) {
    if (std::exchange(win.devtools.hot, button) != button) ::InvalidateRect(win.devtools.bar, nullptr, FALSE);
}

// How far along the side the DevTools are sized by a point of the splitter is, in the
// coordinates of the window's client.
[[nodiscard]] int AlongSide(const BrowserWindow& win, HWND splitter, LPARAM lParam) noexcept {
    POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
    ::MapWindowPoints(splitter, win.client, &point, 1);
    return IsBesidePage(g_app.devtoolsPrefs.side) ? point.x : point.y;
}

void StopSizing(BrowserWindow& win) {
    if (!std::exchange(win.devtools.sizing, false)) return;
    if (::GetCapture() == win.devtools.splitter) ::ReleaseCapture();
    SavePrefs();
    // The other windows' DevTools are the same size.
    for (const auto& other : g_app.windows) {
        if (other.get() != &win && other->client && !other->closing) LayoutActiveWebView(*other);
    }
}

void ShowControl(HWND control, const RECT* place) noexcept {
    if (!control) return;
    if (!place) {
        ::ShowWindow(control, SW_HIDE);
        return;
    }
    ::MoveWindow(control, place->left, place->top, Width(*place), Height(*place), TRUE);
    ::ShowWindow(control, SW_SHOWNA);
}

} // namespace

bool CanDockDevTools() noexcept {
    return g_app.debuggingPort != 0 && !g_app.shutDown;
}

void LoadDevToolsPrefs() {
    g_app.devtoolsPrefs = g_saved = ReadDevToolsPrefs(ReadIniValue);
}

void CreateDevToolsControls(BrowserWindow& win) {
    const auto create = [&win](const wchar_t* className, const wchar_t* text, int id) {
        return ::CreateWindowExW(0, className, text, WS_CHILD | WS_CLIPSIBLINGS, 0, 0, 0, 0, win.client,
                                 reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), g_app.instance, nullptr);
    };
    win.devtools.bar = create(kDevToolsBarClass, kBarTitle, kIdDevToolsBar);
    win.devtools.splitter = create(kDevToolsSplitterClass, L"", kIdDevToolsSplitter);
    if (!win.devtools.bar || !win.tabTips) return;
    // Its buttons say what they do under the mouse, from the control the tabs' tips are in.
    for (const DevToolsButton button : kDevToolsButtons) {
        TOOLINFOW tool = TipFor(win.devtools.bar, button);
        // Text out of a literal, which ends where a string has to.
        tool.lpszText = const_cast<wchar_t*>(DevToolsButtonTip(button).data());
        ::SendMessageW(win.tabTips, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tool));
    }
}

RECT LayoutDevTools(BrowserWindow& win, Tab* tab, const RECT& area) {
    DevToolsUi& ui = win.devtools;
    if (!tab || !tab->devtools.open || win.isFullscreen) {
        StopSizing(win);
        ShowControl(ui.bar, nullptr);
        ShowControl(ui.splitter, nullptr);
        return area;
    }
    const DevToolsSplit split = SplitForDevTools(area, g_app.devtoolsPrefs);
    ShowControl(ui.splitter, &split.splitter);
    ShowControl(ui.bar, &split.bar);
    // Their webview comes a moment after they are opened; its place is kept for it meanwhile.
    if (tab->devtools.controller) {
        tab->devtools.controller->put_Bounds(split.panel);
        tab->devtools.controller->put_IsVisible(TRUE);
    }
    return split.page;
}

void HideDevTools(BrowserWindow& win) {
    StopSizing(win);
    ShowControl(win.devtools.bar, nullptr);
    ShowControl(win.devtools.splitter, nullptr);
    for (const Tab& tab : win.tabs) {
        if (tab.devtools.controller) tab.devtools.controller->put_IsVisible(FALSE);
    }
}

bool HandleDevToolsShortcut(BrowserWindow& win, WPARAM key, Modifiers mods) {
    const bool toggle = (mods == kNoModifiers && key == VK_F12) || (mods == kCtrlShift && key == 'I');
    const bool console = mods == kCtrlShift && key == 'J';
    const bool pick = mods == kCtrlShift && key == 'C';
    if ((!toggle && !console && !pick) || !CanDockDevTools() || win.closing) return false;

    // They are in the window, which a page that has the screen has out of sight.
    LeaveVideoFullscreen(win);
    const TabLocation at = FindTab(ActiveTabId(win));
    // A tab whose page isn't up yet has nothing to show them for; the key is theirs all the same.
    if (!at || !at.tab().webview) return true;
    Tab& tab = at.tab();
    const bool wasOpen = tab.devtools.open;
    if (toggle) {
        if (wasOpen) CloseDevTools(tab.id, DevToolsClose::kRefocus);
        else Open(win, tab, {});
        return true;
    }
    if (console) {
        if (wasOpen) Tell(tab, ShowToolScript(L"console"));
        else Open(win, tab, L"console");
        FocusDevTools(at);
        return true;
    }
    // Picking an element: on, as they open for it; and on or off, as the key is pressed
    // again, while the keyboard stays in the page the element is picked in.
    if (!wasOpen) Open(win, tab, L"elements");
    Tell(tab, PickElementScript(wasOpen));
    return true;
}

void CloseDevTools(int tabId, DevToolsClose how) {
    const TabLocation at = FindTab(tabId);
    if (!at || !at.tab().devtools.open) return;
    BrowserWindow& win = *at.window;
    Tab& tab = at.tab();
    tab.devtools = {}; // closes their webview
    if (at.isActive() && !win.closing) {
        LayoutActiveWebView(win);
        if (how != DevToolsClose::kPlain && IsInFront(win)) MoveFocusToWebView(tab);
    }
    if (how == DevToolsClose::kOwnWindow && tab.webview) tab.webview->OpenDevToolsWindow();
}

void OnDevToolsCloseMessage(WPARAM wParam, LPARAM lParam) {
    const int tabId = static_cast<int>(wParam);
    if (!StillOpen(tabId, static_cast<int>(lParam >> 2))) return; // closed since, and perhaps opened again
    CloseDevTools(tabId, static_cast<DevToolsClose>(lParam & 3));
}

void OfferDockedInspect(int tabId, ICoreWebView2ContextMenuRequestedEventArgs* args) {
    if (!args || !CanDockDevTools() || !g_app.environment) return;
    ComPtr<ICoreWebView2Environment9> environment9;
    ComPtr<ICoreWebView2ContextMenuItemCollection> items;
    UINT32 count = 0;
    if (FAILED(g_app.environment.As(&environment9)) || FAILED(args->get_MenuItems(&items)) || !items ||
        FAILED(items->get_Count(&count))) {
        return;
    }
    for (UINT32 i = 0; i < count; ++i) {
        ComPtr<ICoreWebView2ContextMenuItem> item;
        if (FAILED(items->GetValueAtIndex(i, &item)) || !item) continue;
        // The webview's own "Inspect", by the name it has in every language.
        if (ComString(item.Get(), &ICoreWebView2ContextMenuItem::get_Name) != L"inspectElement") continue;

        // One of ours takes its place, saying what it said.
        const std::wstring label =
            ComString(item.Get(), &ICoreWebView2ContextMenuItem::get_Label).value_or(std::wstring(L"Inspect"));
        POINT location{};
        ComPtr<ICoreWebView2ContextMenuItem> ours;
        if (FAILED(args->get_Location(&location)) ||
            FAILED(environment9->CreateContextMenuItem(label.c_str(), nullptr,
                                                       COREWEBVIEW2_CONTEXT_MENU_ITEM_KIND_COMMAND, &ours)) ||
            !ours) {
            return;
        }
        ours->add_CustomItemSelected(
            Callback<ICoreWebView2CustomItemSelectedEventHandler>([tabId, location](ICoreWebView2ContextMenuItem*,
                                                                                    IUnknown*) -> HRESULT {
                InspectElementAt(tabId, location);
                return S_OK;
            }).Get(),
            nullptr);
        if (SUCCEEDED(items->RemoveValueAtIndex(i))) items->InsertValueAtIndex(i, ours.Get());
        return;
    }
}

void MoveDevToolsTo(Tab& tab, HWND client) {
    if (!tab.devtools.controller) return;
    tab.devtools.controller->put_IsVisible(FALSE);
    tab.devtools.controller->put_ParentWindow(client);
}

void DevToolsMoved(Tab& tab) {
    if (tab.devtools.controller) tab.devtools.controller->NotifyParentWindowPositionChanged();
}

LRESULT CALLBACK DevToolsBarProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    BrowserWindow* const win = WindowFromChild(hwnd);
    const auto buttonAt = [hwnd](LPARAM where) {
        return DevToolsButtonAt(ClientRectOf(hwnd).right, {GET_X_LPARAM(where), GET_Y_LPARAM(where)});
    };
    switch (msg) {
    case WM_MOUSEACTIVATE:
        // A click on it leaves the keyboard where it was.
        return MA_NOACTIVATE;
    case WM_SETCURSOR:
        if (LOWORD(lParam) != HTCLIENT) break;
        ::SetCursor(::LoadCursorW(nullptr, IDC_ARROW));
        return TRUE;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        PaintBar(hwnd, win);
        return 0;
    case WM_SIZE:
        // The buttons keep to its right end, and their tips go with them.
        if (win && win->tabTips) {
            for (const DevToolsButton button : kDevToolsButtons) {
                TOOLINFOW tool = TipFor(hwnd, button);
                ::SendMessageW(win->tabTips, TTM_NEWTOOLRECTW, 0, reinterpret_cast<LPARAM>(&tool));
            }
        }
        ::InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_MOUSEMOVE:
        if (win) {
            SetHot(*win, buttonAt(lParam));
            TRACKMOUSEEVENT track{.cbSize = sizeof(track), .dwFlags = TME_LEAVE, .hwndTrack = hwnd};
            ::TrackMouseEvent(&track);
        }
        return 0;
    case WM_MOUSELEAVE:
        if (win) SetHot(*win, DevToolsButton::None);
        return 0;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
        if (win) {
            win->devtools.pressed = buttonAt(lParam);
            if (win->devtools.pressed != DevToolsButton::None) ::SetCapture(hwnd);
            ::InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_LBUTTONUP: {
        const DevToolsButton pressed =
            win ? std::exchange(win->devtools.pressed, DevToolsButton::None) : DevToolsButton::None;
        if (::GetCapture() == hwnd) ::ReleaseCapture();
        ::InvalidateRect(hwnd, nullptr, FALSE);
        if (pressed != DevToolsButton::None && buttonAt(lParam) == pressed) OnBarButton(*win, pressed);
        return 0;
    }
    case WM_CAPTURECHANGED:
        if (win && std::exchange(win->devtools.pressed, DevToolsButton::None) != DevToolsButton::None) {
            ::InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    default:
        break;
    }
    return ::DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT CALLBACK DevToolsSplitterProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    BrowserWindow* const win = WindowFromChild(hwnd);
    switch (msg) {
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_SETCURSOR:
        if (LOWORD(lParam) != HTCLIENT) break;
        ::SetCursor(::LoadCursorW(nullptr, IsBesidePage(g_app.devtoolsPrefs.side) ? IDC_SIZEWE : IDC_SIZENS));
        return TRUE;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        if (const HDC dc = ::BeginPaint(hwnd, &paint)) {
            // The panel's colour, with a line down its middle to take hold of.
            const RECT rect = ClientRectOf(hwnd);
            ::FillRect(dc, &rect, g_app.theme.panel.get());
            RECT line = rect;
            if (IsBesidePage(g_app.devtoolsPrefs.side)) {
                line.left = rect.right / 2;
                line.right = line.left + 1;
            } else {
                line.top = rect.bottom / 2;
                line.bottom = line.top + 1;
            }
            ::FillRect(dc, &line, g_app.theme.border.get());
            ::EndPaint(hwnd, &paint);
        }
        return 0;
    }
    case WM_LBUTTONDOWN:
        if (win && !win->isFullscreen) {
            const DevToolsSplit split = SplitForDevTools(WebViewBounds(*win), g_app.devtoolsPrefs);
            win->devtools.grab = AlongSide(*win, hwnd, lParam) - DevToolsEdge(split, g_app.devtoolsPrefs.side);
            win->devtools.sizing = true;
            ::SetCapture(hwnd);
        }
        return 0;
    case WM_MOUSEMOVE:
        if (win && win->devtools.sizing) {
            DevToolsPrefs& prefs = g_app.devtoolsPrefs;
            const int edge = AlongSide(*win, hwnd, lParam) - win->devtools.grab;
            const int size = DevToolsSizeForEdgeAt(WebViewBounds(*win), prefs.side, edge);
            int& kept = IsBesidePage(prefs.side) ? prefs.width : prefs.height;
            if (std::exchange(kept, size) != size) LayoutActiveWebView(*win);
        }
        return 0;
    case WM_LBUTTONUP:
    case WM_CAPTURECHANGED:
        if (win) StopSizing(*win);
        return 0;
    default:
        break;
    }
    return ::DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace exodium
