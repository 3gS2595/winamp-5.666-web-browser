// Winamp makes the frame around a browser window and draws it. This file teaches
// that frame the things a browser window is expected to do and Winamp's frames
// don't: a taskbar button, snapping, maximizing, minimizing, closing from outside.
#include "browser.h"

#include "embed_frame.h"

#include <commctrl.h>
#include <dwmapi.h>
#include <windowsx.h>

#include <algorithm>

namespace exodium {

namespace {

constexpr UINT_PTR kFrameSubclassId = 1;

// Where a minimized window is parked, far off screen.
constexpr int kMinimizedCoordinate = -32000;
// The parts of Winamp's frame that act on a click: its title bar and its close button.
constexpr int kTitleBarHeight = 14;
constexpr int kTitleBarButtonsWidth = 12;
// How far in from a frame's edges the mouse sizes it: about the width of Winamp's border on
// the left, the right and the bottom, and a little of the title bar at the top. Along each
// edge, the stretch nearest a corner sizes it from the corner, Winamp's resize grip included.
constexpr int kSizingBorder = 8;
constexpr int kSizingTop = 4;
constexpr int kSizingCorner = 16;
// The close button sits this far in from the title bar's top and right edges, which go on sizing the frame.
constexpr int kCloseButtonMargin = 2;
// How much smaller than the frame Winamp keeps the client whenever the frame changes size,
// to fit it inside the border Winamp paints: 11 pixels on the left and 8 on the right,
// 20 for the title bar and 14 at the bottom.
constexpr int kWinampBorderAcross = 11 + 8;
constexpr int kWinampBorderDown = 20 + 14;

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

} // namespace

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

namespace {

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

// The edge or corner of the frame at a point on it, as the hit test code that has Windows size
// it from there, as it sizes any other window. HTNOWHERE away from the edges, on Winamp's close
// button, and on a frame that can't be sized that way: maximized, minimized or docked.
[[nodiscard]] LRESULT SizingEdgeAt(HWND frame, POINT screenPoint) noexcept {
    if (IsDockedIntoSkin(frame) || ::IsZoomed(frame) || ::IsIconic(frame)) return HTNOWHERE;
    const RECT rect = WindowRectOf(frame);
    const int x = screenPoint.x - rect.left, y = screenPoint.y - rect.top;
    const int width = Width(rect), height = Height(rect);
    if (x < 0 || y < 0 || x >= width || y >= height) return HTNOWHERE;
    const bool closeButton = y >= kCloseButtonMargin && y < kTitleBarHeight && x >= width - kTitleBarButtonsWidth &&
                             x < width - kCloseButtonMargin;
    if (closeButton) return HTNOWHERE;

    const bool left = x < kSizingBorder, right = x >= width - kSizingBorder;
    const bool top = y < kSizingTop, bottom = y >= height - kSizingBorder;
    const bool nearLeft = x < kSizingCorner, nearRight = x >= width - kSizingCorner;
    const bool nearTop = y < kSizingCorner, nearBottom = y >= height - kSizingCorner;
    if (nearRight && nearBottom) return HTBOTTOMRIGHT; // all of it, for Winamp's resize grip there
    if ((left && nearTop) || (top && nearLeft)) return HTTOPLEFT;
    if ((right && nearTop) || (top && nearRight)) return HTTOPRIGHT;
    if ((left && nearBottom) || (bottom && nearLeft)) return HTBOTTOMLEFT;
    if (left) return HTLEFT;
    if (right) return HTRIGHT;
    if (top) return HTTOP;
    if (bottom) return HTBOTTOM;
    return HTNOWHERE;
}

[[nodiscard]] constexpr bool IsSizingEdge(WPARAM hitTest) noexcept {
    return hitTest >= HTLEFT && hitTest <= HTBOTTOMRIGHT;
}

// Puts the client where ClientPlace says, if it isn't there already. Winamp only fits
// it to a change in the frame's size, so a frame maximized or put back without one
// would otherwise keep the client as it was. A minimized frame is left alone, as
// Winamp leaves it (see WM_WINDOWPOSCHANGED below).
void PlaceClient(const BrowserWindow& win) noexcept {
    if (!win.client || IsDockedIntoSkin(win.frame) || ::IsIconic(win.frame)) return;
    const RECT place = ClientPlace(win);
    RECT now = WindowRectOf(win.client);
    ::MapWindowPoints(nullptr, win.frame, reinterpret_cast<POINT*>(&now), 2);
    if (::EqualRect(&now, &place)) return;
    ::SetWindowPos(win.client, nullptr, place.left, place.top, Width(place), Height(place),
                   SWP_NOZORDER | SWP_NOACTIVATE);
}

enum class Side { kNone, kLeft, kTop, kRight, kBottom };

// The side of a screen that a taskbar on it lies along. A taskbar that hides itself hangs off
// that side, with a sliver of it left on the screen for the mouse to bring it back up by.
[[nodiscard]] Side TaskbarSide(const RECT& screen) noexcept {
    for (const wchar_t* name : {L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd"}) {
        for (HWND bar = ::FindWindowExW(nullptr, nullptr, name, nullptr); bar;
             bar = ::FindWindowExW(nullptr, bar, name, nullptr)) {
            const RECT rect = WindowRectOf(bar);
            if (RECT common{}; !::IntersectRect(&common, &rect, &screen)) continue;
            if (Width(rect) >= Height(rect)) {
                return rect.top + rect.bottom < screen.top + screen.bottom ? Side::kTop : Side::kBottom;
            }
            return rect.left + rect.right < screen.left + screen.right ? Side::kLeft : Side::kRight;
        }
    }
    return Side::kNone;
}

} // namespace

// Where a window maximized on a monitor goes: its work area. Left to itself Windows adds the
// width of the border the frame doesn't have and hangs its edges off screen.
//
// Where nothing takes any of the screen for itself, as a taskbar that hides itself doesn't, the
// work area is the whole screen. A window without a caption that covers all of it is taken for
// a game or a video gone full screen: AltSnap leaves those alone, and Windows doesn't bring a
// hidden taskbar up over one. A row of pixels is left uncovered on the taskbar's side, or at
// the bottom, so that a maximized window isn't one.
[[nodiscard]] RECT MaximizedPlace(const MONITORINFO& monitor) noexcept {
    RECT place = monitor.rcWork;
    if (!::EqualRect(&place, &monitor.rcMonitor)) return place;
    switch (TaskbarSide(monitor.rcMonitor)) {
    case Side::kLeft:
        ++place.left;
        break;
    case Side::kTop:
        ++place.top;
        break;
    case Side::kRight:
        --place.right;
        break;
    case Side::kBottom:
    case Side::kNone:
        --place.bottom;
        break;
    }
    return place;
}

void FitMaximized(HWND frame, MINMAXINFO& limits) noexcept {
    MONITORINFO monitor{.cbSize = sizeof(monitor)};
    if (!::GetMonitorInfoW(::MonitorFromWindow(frame, MONITOR_DEFAULTTONEAREST), &monitor)) return;
    const RECT place = MaximizedPlace(monitor);
    limits.ptMaxPosition = {place.left - monitor.rcMonitor.left, place.top - monitor.rcMonitor.top};
    limits.ptMaxSize = {Width(place), Height(place)};
    limits.ptMinTrackSize = kMinFrameSize;
}

namespace {

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
        PlaceClient(win);
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

    case WM_NCHITTEST: {
        if (IsDockedIntoSkin(hwnd)) break;
        const POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        if (const LRESULT edge = SizingEdgeAt(hwnd, point); edge != HTNOWHERE) return edge;
        if (IsOnTitleBar(hwnd, point)) return HTCAPTION;
        break;
    }

    case WM_SETCURSOR:
        // The arrows that say the window sizes from here. Winamp's frame never sized from its edges.
        if (reinterpret_cast<HWND>(wParam) == hwnd && IsSizingEdge(LOWORD(lParam))) {
            return ::DefWindowProcW(hwnd, msg, wParam, lParam);
        }
        break;

    case WM_NCLBUTTONDOWN:
    case WM_NCLBUTTONDBLCLK:
        // Windows sizes the frame from its edges and corners, as it does any window, and
        // stretches it from the top of the screen to the bottom on a double click on either.
        if (IsSizingEdge(wParam)) return ::DefWindowProcW(hwnd, msg, wParam, lParam);
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
        if (auto* limits = reinterpret_cast<MINMAXINFO*>(lParam)) FitMaximized(hwnd, *limits);
        return result;
    }

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
        if (!win.client) break;
        if (IsDockedIntoSkin(hwnd)) {
            const RECT client = ClientRectOf(hwnd);
            ::SetWindowPos(win.client, nullptr, kFrameInsetLeft, kFrameInsetTop, client.right - kFrameInsetLeft,
                           client.bottom - kFrameInsetTop, SWP_NOZORDER | SWP_NOACTIVATE);
        } else {
            PlaceClient(win);
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

bool IsMaximized(const BrowserWindow& win) noexcept {
    return win.frame && !IsDockedIntoSkin(win.frame) && ::IsZoomed(win.frame);
}

LRESULT FrameSizingEdgeAt(const BrowserWindow& win, POINT screenPoint) noexcept {
    return win.frame ? SizingEdgeAt(win.frame, screenPoint) : HTNOWHERE;
}

void RestoreWhereItIs(const BrowserWindow& win) noexcept {
    if (!IsMaximized(win)) return;
    const RECT place = WindowRectOf(win.frame);
    WINDOWPLACEMENT placement{.length = sizeof(placement)};
    MONITORINFO monitor{.cbSize = sizeof(monitor)};
    if (!::GetWindowPlacement(win.frame, &placement) ||
        !::GetMonitorInfoW(::MonitorFromWindow(win.frame, MONITOR_DEFAULTTONEAREST), &monitor)) {
        return;
    }
    // A placement is measured from the corner of the work area, not of the screen.
    placement.rcNormalPosition = place;
    ::OffsetRect(&placement.rcNormalPosition, monitor.rcMonitor.left - monitor.rcWork.left,
                 monitor.rcMonitor.top - monitor.rcWork.top);
    placement.showCmd = SW_SHOWNORMAL;
    ::SetWindowPlacement(win.frame, &placement);
    // In case Windows measures it otherwise: the window stays exactly where it was.
    ::SetWindowPos(win.frame, nullptr, place.left, place.top, Width(place), Height(place),
                   SWP_NOZORDER | SWP_NOACTIVATE);
}

RECT NormalPlace(const BrowserWindow& win) noexcept {
    if (!win.frame) return win.embedState.r;
    WINDOWPLACEMENT placement{.length = sizeof(placement)};
    if (IsDockedIntoSkin(win.frame) || (!::IsZoomed(win.frame) && !::IsIconic(win.frame)) ||
        !::GetWindowPlacement(win.frame, &placement)) {
        return WindowRectOf(win.frame);
    }
    // A placement is measured from the corner of the work area, not of the screen.
    RECT place = placement.rcNormalPosition;
    MONITORINFO monitor{.cbSize = sizeof(monitor)};
    if (::GetMonitorInfoW(::MonitorFromRect(&place, MONITOR_DEFAULTTONEAREST), &monitor)) {
        ::OffsetRect(&place, monitor.rcWork.left - monitor.rcMonitor.left, monitor.rcWork.top - monitor.rcMonitor.top);
    }
    return place;
}

RECT ClientPlace(const BrowserWindow& win) noexcept {
    const RECT frame = ClientRectOf(win.frame); // the whole frame: it has no system border
    // Maximized, the window has the screen to itself. Winamp's border goes from the left,
    // the right and the bottom, and only its title bar stays.
    if (IsMaximized(win)) return {0, kFrameInsetTop, frame.right, frame.bottom};
    // Anywhere else, snapped to half the screen included, it stays inside the border,
    // at the size Winamp gives it.
    return {kFrameInsetLeft, kFrameInsetTop, kFrameInsetLeft + std::max<LONG>(0, frame.right - kWinampBorderAcross),
            kFrameInsetTop + std::max<LONG>(0, frame.bottom - kWinampBorderDown)};
}

void KeepClientInPlace(const BrowserWindow& win, WINDOWPOS& position) noexcept {
    constexpr UINT kStays = SWP_NOMOVE | SWP_NOSIZE;
    if ((position.flags & kStays) == kStays || IsDockedIntoSkin(win.frame) || ::IsIconic(win.frame)) return;
    const RECT place = ClientPlace(win);
    position.x = place.left;
    position.y = place.top;
    position.cx = Width(place);
    position.cy = Height(place);
    position.flags &= ~kStays;
}

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
