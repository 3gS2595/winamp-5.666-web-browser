// Bitwarden's button on the toolbar, and the popup it opens: Bitwarden's own page, the one
// Chrome shows under the extension's icon, in a window of its own under the button.
// WebView2 has no toolbar of its own for an extension to sit on, so this stands in for it.
// Ctrl+Shift+Y, Bitwarden's shortcut in Chrome, opens and closes it too.
//
// The popup goes as Chrome's does: when another window is clicked, on Escape, when the
// button is pressed again, and when Bitwarden closes it, as it does after filling a page.
//
// Bitwarden fills the tab its popup "is in", which it asks the browser for. A webview is a
// browser window of its own, so the answer would be the popup itself. Bitwarden takes the
// tab from its address instead when it is given there (senderTabId, as its own popout
// windows are given it), in the number its tabs API knows the tab by. That number is known
// only to extensions: Exodium's helper extension (extensions.cpp) is opened in the popup
// first and turns the tab's DevTools target id into it, and Bitwarden's page is opened
// after it.
#include "browser.h"

#include "text_util.h"

#include <windowsx.h>

#include <algorithm>

namespace exodium {

namespace {

using Microsoft::WRL::Callback;

constexpr int kBitwardenButtonWidth = 28; // its height is the toolbar's
// The popup's size in the page's pixels, Bitwarden's own: its popup is this wide unless
// it is told otherwise, and Chrome makes no popup taller than 600.
constexpr int kPopupWidth = 480;
constexpr int kPopupHeight = 600;
constexpr int kPopupGap = 2; // between the button and the popup

// Posted to the popup window, to close it after the event that asked has returned. wParam:
// whether the page in view is to have the keyboard back.
constexpr UINT kMsgClosePopup = WM_APP + 1;

struct Popup {
    UniqueWindow window;
    HWND owner = nullptr; // the frame of the browser window it belongs to
    WebViewController controller;
    ComPtr<ICoreWebView2> webview;
    std::wstring targetId;    // the DevTools target id of the tab it is for, if it is for one
    std::optional<int> tabId; // and the number Bitwarden knows that tab by, once the helper has said
    // Opened in place of Bitwarden's window for unlocking (OpenBitwardenPopupToUnlock): it
    // goes once the vault is unlocked, which is when Bitwarden goes on from its lock page.
    bool closeOnceUnlocked = false;
    bool sawLockPage = false;
};
Popup g_popup;
int g_generation = 0; // goes up whenever a popup goes, so that late answers for it are let be

[[nodiscard]] BrowserWindow* OwnerWindow() noexcept {
    if (!g_popup.owner) return nullptr;
    const auto found = std::ranges::find(g_app.windows, g_popup.owner, [](const auto& win) { return win->frame; });
    return found == g_app.windows.end() ? nullptr : found->get();
}

[[nodiscard]] bool IsOpenFor(const BrowserWindow* win) noexcept {
    return win && g_popup.window && g_popup.owner == win->frame;
}

void PostClose(bool refocus) {
    if (g_popup.window) ::PostMessageW(g_popup.window.get(), kMsgClosePopup, refocus ? 1 : 0, 0);
}

// The value after "name": in a JSON object, as text, if it is a string or a number.
[[nodiscard]] std::optional<std::wstring> JsonValue(std::wstring_view json, std::wstring_view name) {
    const std::wstring key = L"\"" + std::wstring(name) + L"\":";
    std::size_t at = json.find(key);
    if (at == std::wstring_view::npos) return std::nullopt;
    at += key.size();
    while (at < json.size() && json[at] == L' ') ++at;
    if (at < json.size() && json[at] == L'"') {
        const std::size_t end = json.find(L'"', at + 1);
        if (end == std::wstring_view::npos) return std::nullopt;
        return std::wstring(json.substr(at + 1, end - at - 1));
    }
    const std::size_t end = json.find_first_not_of(L"0123456789", at);
    if (end == at) return std::nullopt;
    return std::wstring(json.substr(at, end == std::wstring_view::npos ? std::wstring_view::npos : end - at));
}

// ---- the popup's webview ----

void OnHelperAnswered(ICoreWebView2WebMessageReceivedEventArgs* args) {
    const auto source = ComString(args, &ICoreWebView2WebMessageReceivedEventArgs::get_Source);
    const auto json = ComString(args, &ICoreWebView2WebMessageReceivedEventArgs::get_WebMessageAsJson);
    if (!source || !json || !IsTabHelperPage(*source) || !g_popup.webview) return;
    if (const auto number = JsonValue(*json, L"tabId")) g_popup.tabId = ParseNumber<int>(*number);
    g_popup.webview->Navigate(BitwardenPopupUrl(g_popup.tabId).c_str());
}

// The route in Bitwarden's page, after the '#': "/lock", "/tabs/vault?senderTabId=7".
[[nodiscard]] std::wstring_view BitwardenRoute(std::wstring_view url) noexcept {
    const std::size_t hash = url.find(L'#');
    return hash == std::wstring_view::npos ? std::wstring_view() : url.substr(hash + 1);
}

// Bitwarden goes to its vault from wherever it started (unlocking, signing in), and from its
// own tabs along the bottom, without the tab's number. It is given it back, as the page's
// own navigation within it would, so that it goes on offering to fill the tab.
//
// A popup opened to unlock goes once Bitwarden goes on from its lock page to its tabs, as
// its own window for that would: the page in view has the keyboard back, for the login
// asked for. Bitwarden can't close it itself: it closes that window of its, and brings the
// page back to the front, through calls WebView2 doesn't pass on.
void OnPopupSourceChanged(ICoreWebView2* webview) {
    const auto source = ComString(webview, &ICoreWebView2::get_Source);
    if (!source) return;
    const std::wstring_view route = BitwardenRoute(*source);
    if (route.starts_with(L"/lock")) g_popup.sawLockPage = true;
    if (g_popup.closeOnceUnlocked && g_popup.sawLockPage && route.starts_with(L"/tabs/")) {
        PostClose(true);
        return;
    }
    if (!g_popup.tabId || !source->ends_with(L"#/tabs/vault")) return;
    const std::wstring script = L"location.replace(\"" + BitwardenPopupUrl(g_popup.tabId) + L"\")";
    webview->ExecuteScript(script.c_str(), nullptr);
}

void OnPopupKey(ICoreWebView2AcceleratorKeyPressedEventArgs* args) {
    UINT key = 0;
    COREWEBVIEW2_KEY_EVENT_KIND kind{};
    args->get_VirtualKey(&key);
    args->get_KeyEventKind(&kind);
    if (kind != COREWEBVIEW2_KEY_EVENT_KIND_KEY_DOWN) return;
    const Modifiers mods = CurrentModifiers();
    if (key == VK_ESCAPE || (mods == kCtrlShift && key == 'Y')) {
        args->put_Handled(TRUE);
        PostClose(true);
    }
}

void WirePopup(ICoreWebView2* webview, ICoreWebView2Controller* controller) {
    webview->add_WebMessageReceived(Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                                        [](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                                            OnHelperAnswered(args);
                                            return S_OK;
                                        })
                                        .Get(),
                                    nullptr);
    webview->add_SourceChanged(
        Callback<ICoreWebView2SourceChangedEventHandler>([](ICoreWebView2* sender,
                                                            ICoreWebView2SourceChangedEventArgs*) -> HRESULT {
            OnPopupSourceChanged(sender);
            return S_OK;
        }).Get(),
        nullptr);
    // A pop-out window, or a link to Bitwarden's site, goes where a page's would.
    webview->add_NewWindowRequested(
        Callback<ICoreWebView2NewWindowRequestedEventHandler>([](ICoreWebView2*,
                                                                 ICoreWebView2NewWindowRequestedEventArgs* args)
                                                                  -> HRESULT {
            if (BrowserWindow* const win = OwnerWindow(); win && g_app.environment) OpenRequestedWindow(*win, 0, args);
            else args->put_Handled(TRUE);
            return S_OK;
        }).Get(),
        nullptr);
    webview->add_WindowCloseRequested(
        Callback<ICoreWebView2WindowCloseRequestedEventHandler>([](ICoreWebView2*, IUnknown*) -> HRESULT {
            PostClose(true);
            return S_OK;
        }).Get(),
        nullptr);
    webview->add_ProcessFailed(
        Callback<ICoreWebView2ProcessFailedEventHandler>([](ICoreWebView2*, ICoreWebView2ProcessFailedEventArgs*) -> HRESULT {
            PostClose(false);
            return S_OK;
        }).Get(),
        nullptr);
    controller->add_AcceleratorKeyPressed(
        Callback<ICoreWebView2AcceleratorKeyPressedEventHandler>(
            [](ICoreWebView2Controller*, ICoreWebView2AcceleratorKeyPressedEventArgs* args) -> HRESULT {
                OnPopupKey(args);
                return S_OK;
            })
            .Get(),
        nullptr);
}

void CreatePopupWebView(int generation) {
    if (generation != g_generation || !g_popup.window || !g_app.environment) return;
    const HWND window = g_popup.window.get();
    const HRESULT started = g_app.environment->CreateCoreWebView2Controller(
        window,
        Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
            [generation, window](HRESULT result, ICoreWebView2Controller* controller) -> HRESULT {
                if (generation != g_generation || FAILED(result) || !controller) {
                    if (controller) controller->Close();
                    if (generation == g_generation) PostClose(false);
                    return S_OK;
                }
                g_popup.controller = WebViewController(controller);
                controller->get_CoreWebView2(&g_popup.webview);
                RECT bounds = ClientRectOf(window);
                ::InflateRect(&bounds, -1, -1); // inside its border
                controller->put_Bounds(bounds);
                controller->put_IsVisible(TRUE);
                WirePopup(g_popup.webview.Get(), controller);
                const std::wstring url =
                    g_popup.targetId.empty() ? BitwardenPopupUrl(std::nullopt) : TabHelperUrl(g_popup.targetId);
                g_popup.webview->Navigate(url.c_str());
                controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
                return S_OK;
            })
            .Get());
    if (FAILED(started)) PostClose(false);
}

// ---- the popup's window ----

[[nodiscard]] RECT WorkAreaNear(const RECT& place) noexcept {
    MONITORINFO monitor{.cbSize = sizeof(monitor)};
    ::GetMonitorInfoW(::MonitorFromRect(&place, MONITOR_DEFAULTTONEAREST), &monitor);
    return monitor.rcWork;
}

// Under the button, its right edge with the button's, and on the screen.
[[nodiscard]] RECT PopupPlace(const BrowserWindow& win) {
    const int dpi = static_cast<int>(::GetDpiForWindow(win.frame));
    const int width = ::MulDiv(kPopupWidth, dpi, USER_DEFAULT_SCREEN_DPI) + 2;
    const int height = ::MulDiv(kPopupHeight, dpi, USER_DEFAULT_SCREEN_DPI) + 2;
    const RECT under = win.bitwardenButton && ::IsWindowVisible(win.bitwardenButton) ? WindowRectOf(win.bitwardenButton)
                                                                                      : WindowRectOf(win.client);
    const RECT work = WorkAreaNear(under);
    const int x = std::max(static_cast<int>(work.left), std::min(static_cast<int>(under.right) - width,
                                                                 static_cast<int>(work.right) - width));
    const int y = std::max(static_cast<int>(work.top),
                           std::min(static_cast<int>(under.bottom) + kPopupGap, static_cast<int>(work.bottom) - height));
    return {x, y, x + width, y + height};
}

void OpenPopup(BrowserWindow& win) {
    const int generation = ++g_generation;
    const RECT place = PopupPlace(win);
    // Owned by the browser window, so that it stays in front of it, and with no taskbar
    // button of its own.
    g_popup.window.reset(::CreateWindowExW(WS_EX_TOOLWINDOW, kBitwardenPopupClass, L"Bitwarden",
                                           WS_POPUP | WS_CLIPCHILDREN, place.left, place.top, Width(place),
                                           Height(place), win.frame, nullptr, g_app.instance, nullptr));
    if (!g_popup.window) return;
    g_popup.owner = win.frame;
    g_popup.targetId.clear();
    g_popup.tabId.reset();
    g_popup.closeOnceUnlocked = false;
    g_popup.sawLockPage = false;
    ::ShowWindow(g_popup.window.get(), SW_SHOW);
    ::SetForegroundWindow(g_popup.window.get());
    if (win.bitwardenButton) ::InvalidateRect(win.bitwardenButton, nullptr, FALSE);

    // The tab it fills is the one in view, if it is the user's: Claude's are in a profile
    // of their own, which Bitwarden isn't in, and it isn't in a private window's either.
    Tab* const tab = ActiveTab(win);
    if (!tab || !tab->webview || tab->forClaude || tab->isPrivate || !TabHelperReady()) {
        CreatePopupWebView(generation);
        return;
    }
    const HRESULT asked = tab->webview->CallDevToolsProtocolMethod(
        L"Target.getTargetInfo", L"{}",
        Callback<ICoreWebView2CallDevToolsProtocolMethodCompletedHandler>([generation](HRESULT result,
                                                                                      LPCWSTR json) -> HRESULT {
            if (generation != g_generation) return S_OK;
            const auto id = SUCCEEDED(result) && json ? JsonValue(json, L"targetId") : std::nullopt;
            // Only what a target id is made of goes into the helper's address.
            if (id && !id->empty() && id->find_first_not_of(L"0123456789ABCDEFabcdef-") == std::wstring::npos) {
                g_popup.targetId = *id;
            }
            CreatePopupWebView(generation);
            return S_OK;
        }).Get());
    if (FAILED(asked)) CreatePopupWebView(generation);
}

void PaintPopup(HWND hwnd) {
    PAINTSTRUCT paint{};
    const HDC dc = ::BeginPaint(hwnd, &paint);
    if (!dc) return;
    const RECT rect = ClientRectOf(hwnd);
    ::FillRect(dc, &rect, g_app.theme.panel.get());
    ::FrameRect(dc, &rect, g_app.theme.border.get());
    ::EndPaint(hwnd, &paint);
}

// Whether `other`, the window being activated in the popup's stead, is one of the popup's
// own, such as a list the page opened.
[[nodiscard]] bool BelongsToPopup(HWND other) noexcept {
    for (HWND owner = other; owner; owner = ::GetWindow(owner, GW_OWNER)) {
        if (owner == g_popup.window.get()) return true;
    }
    return other && ::GetAncestor(other, GA_ROOT) == g_popup.window.get();
}

// ---- the button ----

void PaintButton(HWND button, const BrowserWindow* win) {
    PAINTSTRUCT paint{};
    const HDC dc = ::BeginPaint(button, &paint);
    if (!dc) return;
    const RECT rect = ClientRectOf(button);
    const Theme& theme = g_app.theme;
    const bool down = win && (win->bitwardenButtonPressed || IsOpenFor(win));
    ::FillRect(dc, &rect, (down ? theme.activeTab : theme.panel).get());
    ::FrameRect(dc, &rect, theme.border.get());
    if (const HICON icon = g_app.extensions.bitwardenIcon.get()) {
        ::DrawIconEx(dc, (rect.right - kBitwardenIconSize) / 2, (rect.bottom - kBitwardenIconSize) / 2, icon,
                     kBitwardenIconSize, kBitwardenIconSize, 0, nullptr, DI_NORMAL);
    }
    ::EndPaint(button, &paint);
}

[[nodiscard]] bool ButtonWanted(const BrowserWindow& win) noexcept {
    return BitwardenReady() && !win.isFullscreen;
}

} // namespace

void CreateBitwardenButton(BrowserWindow& win) {
    win.bitwardenButton = ::CreateWindowExW(0, kBitwardenButtonClass, L"Bitwarden", WS_CHILD | WS_CLIPSIBLINGS, 0, 0, 0,
                                            0, win.client, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdBitwarden)),
                                            g_app.instance, nullptr);
}

int LayoutBitwardenButton(BrowserWindow& win, int right, int top, int height) {
    if (!win.bitwardenButton) return right;
    const bool shown = ButtonWanted(win);
    if (shown) ::MoveWindow(win.bitwardenButton, right - kBitwardenButtonWidth, top, kBitwardenButtonWidth, height, TRUE);
    ::ShowWindow(win.bitwardenButton, shown ? SW_SHOWNA : SW_HIDE);
    return shown ? right - kBitwardenButtonWidth - kButtonGap : right;
}

void ShowBitwardenButton(BrowserWindow& win, bool visible) {
    if (!visible && IsOpenFor(&win)) CloseBitwardenPopup();
    if (win.bitwardenButton) ::ShowWindow(win.bitwardenButton, visible && BitwardenReady() ? SW_SHOWNA : SW_HIDE);
}

void RefreshBitwardenButtons() {
    for (const auto& owned : g_app.windows) {
        BrowserWindow& win = *owned;
        if (!win.bitwardenButton || win.closing) continue;
        if (ButtonWanted(win) != HasStyle(win.bitwardenButton, WS_VISIBLE)) FitAddressBar(win);
        ::InvalidateRect(win.bitwardenButton, nullptr, FALSE);
    }
}

bool ToggleBitwardenPopup(BrowserWindow& win) {
    if (!BitwardenReady() || win.isFullscreen) return false;
    const bool wasOpenHere = IsOpenFor(&win);
    CloseBitwardenPopup(wasOpenHere);
    if (!wasOpenHere) OpenPopup(win);
    return true;
}

bool OpenBitwardenPopupToUnlock(BrowserWindow& win) {
    if (!BitwardenReady() || win.isFullscreen || win.isPrivate) return false;
    if (!IsOpenFor(&win)) {
        CloseBitwardenPopup();
        OpenPopup(win);
        if (!IsOpenFor(&win)) return false;
    } else {
        ::SetForegroundWindow(g_popup.window.get());
    }
    g_popup.closeOnceUnlocked = true;
    return true;
}

void CloseBitwardenPopup(bool refocus) {
    if (!g_popup.window && !g_popup.controller) return;
    ++g_generation;
    BrowserWindow* const win = OwnerWindow();
    g_popup.controller.Close();
    g_popup.webview.Reset();
    g_popup.window.reset();
    g_popup.owner = nullptr;
    g_popup.targetId.clear();
    g_popup.tabId.reset();
    g_popup.closeOnceUnlocked = false;
    g_popup.sawLockPage = false;
    if (!win) return;
    if (win->bitwardenButton) ::InvalidateRect(win->bitwardenButton, nullptr, FALSE);
    if (refocus && ::GetForegroundWindow() == win->frame) {
        if (Tab* const tab = ActiveTab(*win)) MoveFocusToWebView(*tab);
    }
}

void CloseBitwardenPopupOf(const BrowserWindow& win) {
    if (IsOpenFor(&win)) CloseBitwardenPopup();
}

LRESULT CALLBACK BitwardenButtonProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    BrowserWindow* const win = WindowFromChild(hwnd);
    switch (msg) {
    case WM_MOUSEACTIVATE:
        // A click on it leaves the keyboard where it was and makes no window active; while
        // the popup is open, that leaves it open, for the click to close.
        return MA_NOACTIVATE;
    case WM_SETCURSOR:
        if (LOWORD(lParam) != HTCLIENT) break;
        ::SetCursor(::LoadCursorW(nullptr, IDC_ARROW));
        return TRUE;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        PaintButton(hwnd, win);
        return 0;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
        if (win) {
            win->bitwardenButtonPressed = true;
            ::SetCapture(hwnd);
            ::InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_LBUTTONUP: {
        const bool pressed = win && std::exchange(win->bitwardenButtonPressed, false);
        if (::GetCapture() == hwnd) ::ReleaseCapture();
        const RECT rect = ClientRectOf(hwnd);
        const POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        if (pressed && ::PtInRect(&rect, point)) ToggleBitwardenPopup(*win);
        ::InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_CAPTURECHANGED:
        if (win && std::exchange(win->bitwardenButtonPressed, false)) ::InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    default:
        break;
    }
    return ::DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT CALLBACK BitwardenPopupProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        PaintPopup(hwnd);
        return 0;
    case WM_ACTIVATE:
        // Another window was clicked: it goes, as Chrome's popup does.
        if (LOWORD(wParam) == WA_INACTIVE && hwnd == g_popup.window.get() &&
            !BelongsToPopup(reinterpret_cast<HWND>(lParam))) {
            PostClose(false);
        }
        break;
    case WM_SETFOCUS:
        if (g_popup.controller) g_popup.controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
        return 0;
    case kMsgClosePopup:
        if (hwnd == g_popup.window.get()) CloseBitwardenPopup(wParam != 0);
        return 0;
    default:
        break;
    }
    return ::DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace exodium
