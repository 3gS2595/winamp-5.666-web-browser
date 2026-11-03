// Dragging tabs. A tab can be dragged along its strip to reorder it, onto another
// window's strip to move it there, or away from every strip to give it a window of
// its own. One tab is dragged at a time, whichever window it started in.
#include "browser.h"

#include <commctrl.h>
#include <windowsx.h>

#include <algorithm>
#include <cstdlib>
#include <utility>

namespace exodium {

namespace {

constexpr UINT_PTR kTabButtonSubclassId = 3;
constexpr BYTE kGhostOpacity = 200;
constexpr int kMarkerWidth = 2;
constexpr int kMarkerOffset = 3; // how far in front of a tab the insertion mark sits

// Where a dragged tab would land.
struct DropTarget {
    BrowserWindow* window = nullptr;
    int insertAt = 0; // the position it would take among that window's other tabs
    POINT marker{};   // where to draw the insertion mark, in screen coordinates
};

// The browser window at a screen point, if there is one.
BrowserWindow* WindowAtPoint(POINT point) noexcept {
    const HWND desktop = ::GetDesktopWindow();
    for (HWND hwnd = ::WindowFromPoint(point); hwnd && hwnd != desktop; hwnd = ::GetAncestor(hwnd, GA_PARENT)) {
        const auto isIt = [hwnd](const UniqueWindow& ornament) { return ornament.get() == hwnd; };
        for (const auto& win : g_app.windows) {
            if (hwnd == win->client || hwnd == win->frame || std::ranges::any_of(win->ornaments.windows, isIt)) {
                return win.get();
            }
        }
    }
    return nullptr;
}

// Finds the tab strip under a screen point; nothing if the point is on none.
DropTarget FindDropTarget(POINT point, int dragTabId) {
    BrowserWindow* const dst = WindowAtPoint(point);
    if (!dst || dst->isFullscreen || !dst->tabBar) return {};

    const RECT strip = WindowRectOf(dst->tabBar);
    RECT hit = strip;
    ::InflateRect(&hit, 0, kTabDetachSlop);
    if (!::PtInRect(&hit, point)) return {};

    // The mark sits in the gap in front of the first tab whose middle is right of the cursor.
    const int firstTabLeft = strip.left + OrnamentStripCover(*dst).left + kMargin;
    DropTarget target{.window = dst, .insertAt = 0, .marker = {firstTabLeft - kMarkerOffset, strip.top + 1}};
    for (int i = 0; i < static_cast<int>(dst->tabs.size()); ++i) {
        const Tab& tab = dst->tabs[i];
        if (tab.id == dragTabId || !tab.button) continue;
        // Tabs scrolled out of the strip aren't anywhere to be measured. The ones off
        // its left end are still in front of the mark; there is nothing past the others.
        if (!::IsWindowVisible(tab.button.get())) {
            if (i >= dst->firstTab) break;
            ++target.insertAt;
            continue;
        }
        const RECT button = WindowRectOf(tab.button.get());
        if (point.x < (button.left + button.right) / 2) {
            target.marker.x = button.left - kMarkerOffset;
            break;
        }
        ++target.insertAt;
        target.marker.x = button.right + 1;
    }
    return target;
}

// Moves a window so that its first tab's top-left corner lands on a screen point.
void MoveFrameTabTo(BrowserWindow& win, POINT at) {
    if (win.tabs.empty() || !win.tabs[0].button) return;
    const RECT frame = WindowRectOf(win.frame);
    const RECT button = WindowRectOf(win.tabs[0].button.get());
    const int x = frame.left + at.x - button.left;
    int y = frame.top + at.y - button.top;
    MONITORINFO monitor{.cbSize = sizeof(monitor)};
    if (::GetMonitorInfoW(::MonitorFromPoint(at, MONITOR_DEFAULTTONEAREST), &monitor)) {
        y = std::max(y, static_cast<int>(monitor.rcWork.top));
    }
    ::SetWindowPos(win.frame, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

void BeginTabDrag() {
    TabDrag& drag = g_app.drag;
    drag.dragging = true;

    // Layered + transparent keeps both windows click-through, so they never get in
    // the way of finding the window under the cursor.
    const DWORD exStyle = WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE;
    const RECT button = WindowRectOf(drag.button);
    drag.ghost.reset(::CreateWindowExW(exStyle, kTabGhostClass, WindowText(drag.button).c_str(), WS_POPUP, button.left,
                                       button.top, Width(button), Height(button), nullptr, nullptr, g_app.instance,
                                       nullptr));
    if (drag.ghost) ::SetLayeredWindowAttributes(drag.ghost.get(), 0, kGhostOpacity, LWA_ALPHA);
    drag.marker.reset(::CreateWindowExW(exStyle, L"STATIC", L"", WS_POPUP | SS_WHITERECT, 0, 0, kMarkerWidth,
                                        kTabBarHeight - 2, nullptr, nullptr, g_app.instance, nullptr));
    if (drag.marker) ::SetLayeredWindowAttributes(drag.marker.get(), 0, 255, LWA_ALPHA);
}

void UpdateTabDrag(POINT point) {
    const TabDrag& drag = g_app.drag;
    const UINT flags = SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW;
    if (drag.ghost) {
        ::SetWindowPos(drag.ghost.get(), HWND_TOPMOST, point.x - drag.grab.x, point.y - drag.grab.y, 0, 0, flags);
    }
    if (!drag.marker) return;
    if (const DropTarget target = FindDropTarget(point, drag.tabId); target.window) {
        ::SetWindowPos(drag.marker.get(), HWND_TOPMOST, target.marker.x, target.marker.y, 0, 0, flags);
    } else {
        ::ShowWindow(drag.marker.get(), SW_HIDE);
    }
}

// The tab a tab button stands for, in the window it is in.
struct ButtonTab {
    BrowserWindow* window = nullptr;
    int index = -1;
    explicit operator bool() const noexcept { return window != nullptr; }
};
ButtonTab TabOfButton(HWND button) noexcept {
    BrowserWindow* const win = WindowFromChild(button);
    const int index = ::GetDlgCtrlID(button) - kTabButtonIdBase;
    return win && IsTabIndex(*win, index) ? ButtonTab{win, index} : ButtonTab{};
}

LRESULT CALLBACK TabButtonProc(HWND button, UINT msg, WPARAM wParam, LPARAM lParam, UINT_PTR subclassId, DWORD_PTR) {
    TabDrag& drag = g_app.drag;
    switch (msg) {
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK: {
        // Handled here rather than by the button so that a press can turn into a
        // drag. As in Chrome, the tab is selected on press, not on release.
        const ButtonTab pressed = TabOfButton(button);
        if (!pressed) break;
        EndTabDrag();
        ::SetFocus(button);
        SwitchToTab(*pressed.window, pressed.index);
        drag.tabId = pressed.window->tabs[pressed.index].id;
        drag.button = button;
        drag.grab = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        ::SetCapture(button);
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (drag.button != button) break;
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        if (!drag.dragging) {
            if (std::abs(point.x - drag.grab.x) <= ::GetSystemMetrics(SM_CXDRAG) &&
                std::abs(point.y - drag.grab.y) <= ::GetSystemMetrics(SM_CYDRAG)) {
                return 0;
            }
            BeginTabDrag();
        }
        ::ClientToScreen(button, &point);
        UpdateTabDrag(point);
        return 0;
    }
    case WM_LBUTTONUP: {
        if (drag.button != button) break;
        std::optional<TabDrop> drop;
        if (drag.dragging) {
            drop =
                TabDrop{.tabId = drag.tabId, .point = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)}, .grab = drag.grab};
            ::ClientToScreen(button, &drop->point);
        }
        EndTabDrag();
        // Posted, so that the drop never destroys this button from inside its own message.
        if (drop) {
            g_app.pendingDrop = drop;
            if (!::PostMessageW(::GetParent(button), kMsgDropTab, 0, 0)) g_app.pendingDrop.reset();
        }
        return 0;
    }
    case WM_MBUTTONUP:
        // Middle-click closes the tab. Posted, for the same reason as a drop.
        if (const ButtonTab clicked = TabOfButton(button)) {
            const int tabId = clicked.window->tabs[clicked.index].id;
            ::PostMessageW(clicked.window->client, kMsgCloseTab, static_cast<WPARAM>(tabId), 0);
        }
        return 0;
    case WM_CAPTURECHANGED:
        if (drag.button == button) EndTabDrag();
        break;
    case WM_NCDESTROY:
        if (drag.button == button) EndTabDrag();
        ::RemoveWindowSubclass(button, TabButtonProc, subclassId);
        break;
    default:
        break;
    }
    return ::DefSubclassProc(button, msg, wParam, lParam);
}

} // namespace

void SubclassTabButton(HWND button) {
    ::SetWindowSubclass(button, TabButtonProc, kTabButtonSubclassId, 0);
}

void EndTabDrag() {
    // Cleared first: releasing capture re-enters via WM_CAPTURECHANGED.
    TabDrag ended = std::exchange(g_app.drag, {});
    ended.ghost.reset();
    ended.marker.reset();
    if (ended.button && ::GetCapture() == ended.button) ::ReleaseCapture();
}

void DropDraggedTab(const TabDrop& drop) {
    const TabLocation from = FindTab(drop.tabId);
    if (!from || from.window->isFullscreen) return;
    BrowserWindow& src = *from.window;
    const POINT tabAt{drop.point.x - drop.grab.x, drop.point.y - drop.grab.y};

    const DropTarget target = FindDropTarget(drop.point, drop.tabId);
    BrowserWindow* dst = target.window;
    const bool newWindow = dst == nullptr;
    if (newWindow) {
        // Dropped away from every tab strip: the tab gets a window of its own, the
        // size of the one it came from. A window's only tab just takes its window along.
        if (src.tabs.size() == 1) {
            MoveFrameTabTo(src, tabAt);
            return;
        }
        const RECT place = WindowRectOf(src.frame);
        dst = CreateBrowserWindow(true, &place, true);
        if (!dst) return;
    }

    MoveTab(src, from.index, *dst, target.insertAt);

    if (newWindow) {
        MoveFrameTabTo(*dst, tabAt);
        ::ShowWindow(dst->frame, SW_SHOWNA);
    }
    ::SetForegroundWindow(dst->frame);
    if (Tab* tab = ActiveTab(*dst); tab && tab->controller) MoveFocusToWebView(*tab);
}

LRESULT CALLBACK TabGhostProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_PAINT) {
        // Drawn exactly like the active tab; the ghost's window text is the tab title.
        PAINTSTRUCT paint;
        DRAWITEMSTRUCT item{};
        item.hwndItem = hwnd;
        item.hDC = ::BeginPaint(hwnd, &paint);
        item.rcItem = ClientRectOf(hwnd);
        const TabLocation dragged = FindTab(g_app.drag.tabId);
        DrawOwnerButton(item, dragged ? &dragged.tab() : nullptr, true);
        ::EndPaint(hwnd, &paint);
        return 0;
    }
    return ::DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace exodium
