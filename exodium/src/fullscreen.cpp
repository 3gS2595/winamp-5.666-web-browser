// Fullscreen video.
//
// Standard WebView2 fullscreen pattern (matches Microsoft's own WebView2Browser
// sample): reparent the active tab's controller into a dedicated, plain,
// borderless top-level window sized to the monitor, instead of resizing or
// restyling the Winamp-owned embed frame. This avoids fighting Winamp's own
// window chrome entirely, so there is no leftover title bar/border and no
// frame-recalculation glitches on the first transition.
#include "browser.h"

#include <algorithm>

namespace exodium {

void EnterVideoFullscreen(BrowserWindow& win) {
    if (win.isFullscreen) return;
    Tab* const tab = ActiveTab(win);
    if (!tab || !tab->controller) return;

    MONITORINFO monitor{.cbSize = sizeof(monitor)};
    if (!::GetMonitorInfoW(::MonitorFromWindow(win.frame, MONITOR_DEFAULTTONEAREST), &monitor)) return;
    const RECT& screen = monitor.rcMonitor;

    // WS_EX_TOOLWINDOW keeps it out of the taskbar/Alt-Tab list, matching how
    // browsers don't add a separate taskbar entry for fullscreen video.
    UniqueWindow fullscreen(::CreateWindowExW(WS_EX_TOOLWINDOW, kFullscreenClass, L"", WS_POPUP, screen.left,
                                              screen.top, Width(screen), Height(screen), nullptr, nullptr,
                                              g_app.instance, nullptr));
    if (!fullscreen) return;
    const HWND host = fullscreen.get();

    win.isFullscreen = true;
    win.fullscreenWindow = std::move(fullscreen);
    win.fullscreenTabId = tab->id;
    SetToolbarControlsVisible(win, false);

    tab->controller->put_ParentWindow(host);
    tab->controller->put_Bounds({0, 0, Width(screen), Height(screen)});
    tab->controller->put_IsVisible(TRUE);
    tab->controller->NotifyParentWindowPositionChanged();

    ::ShowWindow(host, SW_SHOW);
    ::SetForegroundWindow(host);
}

void ExitVideoFullscreen(BrowserWindow& win) {
    if (!win.isFullscreen) return;
    win.isFullscreen = false;

    if (const int index = TabIndexById(win, win.fullscreenTabId); index >= 0 && win.tabs[index].controller) {
        const WebViewController& controller = win.tabs[index].controller;
        controller->put_ParentWindow(win.client);
        controller->NotifyParentWindowPositionChanged();
    }
    win.fullscreenTabId = -1;
    win.fullscreenWindow.reset();

    SetToolbarControlsVisible(win, true);
    LayoutToolbarAndTabs(win); // recomputes normal bounds via LayoutActiveWebView
}

void LeaveVideoFullscreen(BrowserWindow& win) {
    if (!win.isFullscreen) return;
    // The page is told as well, so it doesn't keep its fullscreen layout inside the tab.
    if (const int index = TabIndexById(win, win.fullscreenTabId); index >= 0 && win.tabs[index].webview) {
        win.tabs[index].webview->ExecuteScript(L"document.exitFullscreen()", nullptr);
    }
    ExitVideoFullscreen(win);
}

LRESULT CALLBACK FullscreenProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    // Alt+F4 must not destroy this window with a tab's webview still inside it.
    if (msg == WM_CLOSE) {
        const auto owner =
            std::ranges::find(g_app.windows, hwnd, [](const auto& win) { return win->fullscreenWindow.get(); });
        if (owner != g_app.windows.end()) LeaveVideoFullscreen(**owner);
        return 0;
    }
    return ::DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace exodium
