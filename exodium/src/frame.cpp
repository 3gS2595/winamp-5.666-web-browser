// Winamp makes the frame around a browser window and draws it. This file teaches
// that frame the things a browser window is expected to do and Winamp's frames
// don't: a taskbar button, snapping, maximizing, minimizing, closing from outside.
#include "browser.h"

#include "embed_frame.h"

#include <commctrl.h>
#include <dwmapi.h>
#include <windowsx.h>

namespace exodium {

namespace {

constexpr UINT_PTR kFrameSubclassId = 1;

// Where a minimized window is parked, far off screen.
constexpr int kMinimizedCoordinate = -32000;
// The parts of Winamp's frame that act on a click: its title bar and its resize grip.
constexpr int kTitleBarHeight = 14;
constexpr int kTitleBarButtonsWidth = 12;
constexpr int kResizeGripSize = 20;

// What the Jump to File Extra plugin takes as leave to let a frame be moved.
constexpr LPARAM kJumpToFileAllowMove = 2003;

[[nodiscard]] bool IsDockedIntoSkin(HWND frame) noexcept {
    return HasStyle(frame, WS_CHILD);
}

// Give the frame its own taskbar button / Alt+Tab slot. Winamp creates the
// embed frame as a top-level window owned by the main window, so it is
// suppressed from the switcher unless it carries WS_EX_APPWINDOW.
void EnableTaskbarButton(HWND frame) noexcept {
    LONG_PTR exStyle = ::GetWindowLongPtrW(frame, GWL_EXSTYLE);
    exStyle |= WS_EX_APPWINDOW;
    exStyle &= ~WS_EX_TOOLWINDOW;
    ::SetWindowLongPtrW(frame, GWL_EXSTYLE, exStyle);
}

// Windows keeps an owner and everything it owns together in the z-order, so while
// Winamp's main window owned the frame, bringing the browser forward dragged the
// library, playlist and the rest up with it. Without an owner it is raised, and
// Alt+Tabbed to, on its own.
void DetachFromWinampWindows(HWND frame) noexcept {
    if (IsDockedIntoSkin(frame)) return;
    if (::GetWindow(frame, GW_OWNER)) ::SetWindowLongPtrW(frame, GWLP_HWNDPARENT, 0);
}

// Windows only maximizes (Win+Up), minimizes (Win+Down) and snaps (Win+Left/Right)
// windows that say they can be, which Winamp's frames don't. Those styles normally bring
// a system border with them. FrameProc hands all of it back to the client area and DWM is
// told to draw none of it, so the frame still looks exactly the way Winamp paints it.
void EnableSnapAndMaximize(HWND frame) noexcept {
    const LONG_PTR style = ::GetWindowLongPtrW(frame, GWL_STYLE);
    ::SetWindowLongPtrW(frame, GWL_STYLE, style | WS_THICKFRAME | WS_MAXIMIZEBOX | WS_MINIMIZEBOX);
    const DWMNCRENDERINGPOLICY policy = DWMNCRP_DISABLED;
    ::DwmSetWindowAttribute(frame, DWMWA_NCRENDERING_POLICY, &policy, sizeof(policy));
    ::SetWindowPos(frame, nullptr, 0, 0, 0, 0,
                   SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}

// Turns a maximized frame back into a normal window at `place` (screen coordinates).
void RestoreFrameTo(HWND frame, const RECT& place) noexcept {
    WINDOWPLACEMENT placement{.length = sizeof(placement)};
    if (!::GetWindowPlacement(frame, &placement)) return;
    placement.rcNormalPosition = place;
    placement.showCmd = SW_SHOWNORMAL;
    ::SetWindowPlacement(frame, &placement);
    // A placement is measured from the work area's corner; this settles any difference.
    ::SetWindowPos(frame, nullptr, place.left, place.top, Width(place), Height(place), SWP_NOZORDER | SWP_NOACTIVATE);
}

// The Jump to File Extra plugin (gen_jumpex) hooks every embedded frame and, from the
// moment one is activated until it is next clicked, strips the move out of any request
// to reposition it while it is the active window. Tools such as AltSnap move windows
// from outside, so after an Alt+Tab the frame could only be resized down and right:
// growing it up or left needs it to move. The plugin lets the next two position changes
// through once it has seen this message, so sending it ahead of each one keeps the
// frame movable. Nothing else in Winamp acts on it.
void AllowOutsideMoves(HWND frame) noexcept {
    ::SendMessageW(frame, WM_USER, 0, kJumpToFileAllowMove);
}

// The same plugin notes the frame's width whenever the mouse is let go over it, and
// forces the next change of size back to that width. The first maximize after the
// window had been clicked came out the full height of the screen but no wider. A
// change that leaves the size alone takes the width from it instead.
void SpendJumpToFileWidth(HWND frame) noexcept {
    ::SetWindowPos(frame, nullptr, 0, 0, 0, 0,
                   SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOACTIVATE);
}

// Winamp's title bar, as a caption: Windows does the dragging, and with it the
// snapping to the screen's edges and corners and the dragging of a maximized window
// back down to its size. Left to Winamp the frame was moved by hand and none of
// that happened. The close button on the title bar stays Winamp's.
[[nodiscard]] bool IsOnTitleBar(HWND frame, POINT screenPoint) noexcept {
    POINT point = screenPoint;
    ::ScreenToClient(frame, &point);
    const RECT client = ClientRectOf(frame);
    return point.y >= 0 && point.y < kTitleBarHeight && point.x >= 0 && point.x < client.right - kTitleBarButtonsWidth;
}

// Winamp sizes the frame by hand from its resize grip, which would leave a maximized
// one still counted as maximized. It becomes a normal window first, where it is.
void RestoreMaximizedForResize(HWND frame, LPARAM click) noexcept {
    const RECT maximized = WindowRectOf(frame);
    const int x = GET_X_LPARAM(click), y = GET_Y_LPARAM(click);
    if (x > Width(maximized) - kResizeGripSize && y > Height(maximized) - kResizeGripSize) {
        RestoreFrameTo(frame, maximized);
    }
}

// Maximized means the work area exactly. Left to itself Windows adds the width of
// the border the frame doesn't have and hangs its edges off screen.
void FitMaximizedToWorkArea(HWND frame, MINMAXINFO& limits) noexcept {
    MONITORINFO monitor{.cbSize = sizeof(monitor)};
    if (!::GetMonitorInfoW(::MonitorFromWindow(frame, MONITOR_DEFAULTTONEAREST), &monitor)) return;
    const RECT& work = monitor.rcWork;
    limits.ptMaxPosition = {work.left - monitor.rcMonitor.left, work.top - monitor.rcMonitor.top};
    limits.ptMaxSize = {Width(work), Height(work)};
    limits.ptMinTrackSize = kMinFrameSize;
}

LRESULT CALLBACK FrameProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, UINT_PTR subclassId, DWORD_PTR data) {
    BrowserWindow& win = *reinterpret_cast<BrowserWindow*>(data);
    switch (msg) {
    case WM_ACTIVATE:
    case WM_ACTIVATEAPP:
    case WM_WINDOWPOSCHANGING:
        AllowOutsideMoves(hwnd);
        break;

    case WM_WINDOWPOSCHANGED: {
        // A minimized frame is a sliver parked far off screen. Winamp would lay the
        // frame's contents out to fit it and remember that as the window's place, so
        // it isn't told. The pages just hear that they are out of sight meanwhile.
        const auto& position = *reinterpret_cast<const WINDOWPOS*>(lParam);
        const bool wasMinimized = win.minimized;
        win.minimized = ::IsIconic(hwnd) || (!(position.flags & SWP_NOMOVE) && position.x <= kMinimizedCoordinate &&
                                             position.y <= kMinimizedCoordinate);
        if (win.minimized) {
            if (!wasMinimized && !win.isFullscreen) HideAllWebViews(win);
            UpdateOrnaments(win);
            return 0;
        }
        const LRESULT result = ::DefSubclassProc(hwnd, msg, wParam, lParam);
        // Winamp only follows the frame's position when it does the moving itself.
        // Without this a move from outside is forgotten when the position is saved.
        if (!IsDockedIntoSkin(hwnd)) ::GetWindowRect(hwnd, &win.embedState.r);
        if (wasMinimized) LayoutActiveWebView(win);
        UpdateOrnaments(win); // they go where the frame's corners go, and out of sight when the frame does
        return result;
    }

    case WM_CLOSE:
    case WM_SYSCOMMAND:
        // Asked to close from outside: the taskbar, the task switcher. Left to Winamp's
        // frame the window stays where it is, and a frame that was sent WM_CLOSE takes
        // Winamp down when it exits. It goes where the frame's own close button does.
        if (msg == WM_SYSCOMMAND && (wParam & 0xFFF0) != SC_CLOSE) break;
        if (win.client) {
            ::SendMessageW(win.client, WM_CLOSE, 0, 0);
            return 0;
        }
        break;

    case WM_NCCALCSIZE:
        // No system border: the whole window stays client area (see EnableSnapAndMaximize).
        if (!IsDockedIntoSkin(hwnd)) return 0;
        break;

    case WM_NCHITTEST:
        if (!IsDockedIntoSkin(hwnd) && IsOnTitleBar(hwnd, {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)})) {
            return HTCAPTION;
        }
        break;

    case WM_NCRBUTTONUP:
        // A right click on the title bar still brings up Winamp's menu.
        if (wParam == HTCAPTION) {
            POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            ::ScreenToClient(hwnd, &point);
            return ::DefSubclassProc(hwnd, WM_RBUTTONUP, 0, MAKELPARAM(point.x, point.y));
        }
        break;

    case WM_GETMINMAXINFO: {
        const LRESULT result = ::DefSubclassProc(hwnd, msg, wParam, lParam);
        if (auto* limits = reinterpret_cast<MINMAXINFO*>(lParam)) FitMaximizedToWorkArea(hwnd, *limits);
        return result;
    }

    case WM_LBUTTONDOWN:
        if (::IsZoomed(hwnd)) RestoreMaximizedForResize(hwnd, lParam);
        break;

    case WM_LBUTTONUP: {
        const LRESULT result = ::DefSubclassProc(hwnd, msg, wParam, lParam);
        SpendJumpToFileWidth(hwnd);
        return result;
    }

    case WM_SHOWWINDOW: {
        // lParam is zero for a show/hide of this window itself, as opposed to one that
        // follows Winamp being minimized or restored.
        const bool shown = wParam != FALSE, ofItself = lParam == 0;
        if (ofItself && win.menuId) embed::CheckMenuItems(*win.menuId, shown);
        if (shown && win.client) ::PostMessageW(win.client, kMsgInitWebView, 0, 0);
        if (shown) DetachFromWinampWindows(hwnd); // in case Winamp gave it its owner back
        // Shown from Winamp's menu after being hidden while minimized: come all the way back.
        if (shown && ofItself && ::IsIconic(hwnd)) ::PostMessageW(hwnd, WM_SYSCOMMAND, SC_RESTORE, 0);
        break;
    }

    case WM_ERASEBKGND: {
        const RECT client = ClientRectOf(hwnd);
        ::FillRect(reinterpret_cast<HDC>(wParam), &client, g_app.theme.background.get());
        return 1;
    }

    case WM_SIZE:
        if (win.client) {
            const RECT client = ClientRectOf(hwnd);
            ::SetWindowPos(win.client, nullptr, kFrameInsetLeft, kFrameInsetTop, client.right - kFrameInsetLeft,
                           client.bottom - kFrameInsetTop, SWP_NOZORDER | SWP_NOACTIVATE);
        }
        break;

    case WM_NCDESTROY:
        ::RemoveWindowSubclass(hwnd, FrameProc, subclassId);
        break;

    default:
        break;
    }
    return ::DefSubclassProc(hwnd, msg, wParam, lParam);
}

} // namespace

void AttachFrame(BrowserWindow& win) {
    // `win` outlives the subclass: DestroyBrowserWindow detaches before the window goes.
    ::SetWindowSubclass(win.frame, FrameProc, kFrameSubclassId, reinterpret_cast<DWORD_PTR>(&win));
    EnableTaskbarButton(win.frame);
    DetachFromWinampWindows(win.frame);
    EnableSnapAndMaximize(win.frame);
}

void DetachFrame(BrowserWindow& win) {
    ::RemoveWindowSubclass(win.frame, FrameProc, kFrameSubclassId);
}

} // namespace exodium
