// Clicking and dragging tabs. A tab can be dragged along its strip to reorder it, onto
// another window's strip to move it there, or away from every strip to give it a window
// of its own. Ctrl+click and Shift+click select several tabs of a window, as in Chrome,
// and dragging any of them takes them all. One drag goes on at a time, whichever window
// it started in.
//
// A tab is closed by the close button at its right end, or by a middle-click anywhere on
// it. Either closes that tab alone, even one of several selected, as in Chrome; Ctrl+W
// and the toolbar's close button are what close all of those. The close button works as
// a button of its own: the mouse going down on it neither brings its tab into view nor
// starts a drag, and the tab only closes if the mouse is let go over it. Where it is, and
// which tabs have one, is for tab_parts.h to say.
//
// After a tab is closed with the mouse, the others keep their width until the mouse
// leaves the strip, as in Chrome, rather than growing into the room it left. The tabs
// after it move up by one, and the next one's close button comes under the mouse, to be
// clicked in turn.
#include "browser.h"

#include "tab_parts.h"

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
// The timer that gives a window's tabs their own width back once the mouse has left the
// strip. It runs on the window's client, beside kSpinTimer, but goes to a procedure of its
// own rather than to the client's.
constexpr UINT_PTR kKeptWidthTimer = 0x7AB0;
constexpr UINT kKeptWidthPollMs = 100;

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

[[nodiscard]] bool IsOneOf(const std::vector<int>& tabIds, int tabId) noexcept {
    return std::ranges::find(tabIds, tabId) != tabIds.end();
}

// Finds the tab strip under a screen point; nothing if the point is on none. The
// tabs being dragged don't count as places to land.
DropTarget FindDropTarget(POINT point, const std::vector<int>& draggedIds) {
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
        if (IsOneOf(draggedIds, tab.id) || !tab.button) continue;
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
    // A tab that is one of several selected takes the others along.
    const TabLocation pressed = FindTab(drag.tabId);
    drag.tabIds = pressed && pressed.tab().selected ? SelectedTabIds(*pressed.window) : std::vector{drag.tabId};
    if (!IsOneOf(drag.tabIds, drag.tabId)) drag.tabIds = {drag.tabId};

    // The ghost shows them side by side, each as wide as the pressed one, and is held
    // where the pressed one was.
    const RECT button = WindowRectOf(drag.button);
    const int step = Width(button) + kTabGap;
    const int count = static_cast<int>(drag.tabIds.size());
    const int before = static_cast<int>(std::ranges::find(drag.tabIds, drag.tabId) - drag.tabIds.begin());
    drag.ghostGrab = {before * step + drag.grab.x, drag.grab.y};

    // Layered + transparent keeps both windows click-through, so they never get in
    // the way of finding the window under the cursor.
    const DWORD exStyle = WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE;
    drag.ghost.reset(::CreateWindowExW(exStyle, kTabGhostClass, WindowText(drag.button).c_str(), WS_POPUP,
                                       button.left - before * step, button.top, count * step - kTabGap,
                                       Height(button), nullptr, nullptr, g_app.instance, nullptr));
    if (drag.ghost) ::SetLayeredWindowAttributes(drag.ghost.get(), 0, kGhostOpacity, LWA_ALPHA);
    drag.marker.reset(::CreateWindowExW(exStyle, L"STATIC", L"", WS_POPUP | SS_WHITERECT, 0, 0, kMarkerWidth,
                                        kTabBarHeight - 2, nullptr, nullptr, g_app.instance, nullptr));
    if (drag.marker) ::SetLayeredWindowAttributes(drag.marker.get(), 0, 255, LWA_ALPHA);
}

void UpdateTabDrag(POINT point) {
    const TabDrag& drag = g_app.drag;
    const UINT flags = SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW;
    if (drag.ghost) {
        ::SetWindowPos(drag.ghost.get(), HWND_TOPMOST, point.x - drag.ghostGrab.x, point.y - drag.ghostGrab.y, 0, 0,
                       flags);
    }
    if (!drag.marker) return;
    if (const DropTarget target = FindDropTarget(point, drag.tabIds); target.window) {
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

// Whether a point in a tab's button, from a mouse message, is on the tab's close button.
// A tab without one has nothing there but itself.
[[nodiscard]] bool OnCloseButton(HWND button, LPARAM lParam) {
    const ButtonTab tab = TabOfButton(button);
    if (!tab) return false;
    const RECT client = ClientRectOf(button);
    const TabBox box = TabCloseBox({client.left, client.top, client.right, client.bottom},
                                   g_app.settings.tabCloseButtons, tab.index == tab.window->active);
    return box.contains(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
}

void Repaint(HWND button) noexcept {
    if (button) ::InvalidateRect(button, nullptr, FALSE);
}

// Lights the close button of the tab with this button, or none, and puts out any other.
void SetHotCloseButton(HWND button) {
    const HWND was = std::exchange(g_app.closeButtonMouse.hot, button);
    if (was == button) return;
    Repaint(was);
    Repaint(button);
    if (button) {
        // To hear when the mouse leaves, and put it out again.
        TRACKMOUSEEVENT track{.cbSize = sizeof(track), .dwFlags = TME_LEAVE, .hwndTrack = button};
        ::TrackMouseEvent(&track);
    }
}

void PressCloseButton(HWND button) {
    CloseButtonMouse& mouse = g_app.closeButtonMouse;
    SetHotCloseButton(nullptr); // drawn pressed instead, for as long as the mouse is on it
    mouse.pressed = button;
    mouse.pressedOver = true;
    ::SetCapture(button);
    Repaint(button);
}

// Lets go of a pressed close button without closing anything.
void EndCloseButtonPress() {
    CloseButtonMouse& mouse = g_app.closeButtonMouse;
    // Cleared first: releasing capture comes back here through WM_CAPTURECHANGED.
    const HWND button = std::exchange(mouse.pressed, nullptr);
    mouse.pressedOver = false;
    if (button && ::GetCapture() == button) ::ReleaseCapture();
    Repaint(button);
}

// Whether the mouse is on a window's tab strip, and not on some other window in front of it.
[[nodiscard]] bool CursorOnStrip(const BrowserWindow& win) {
    POINT cursor{};
    if (!win.tabBar || !::IsWindowVisible(win.tabBar) || !::GetCursorPos(&cursor)) return false;
    const RECT strip = WindowRectOf(win.tabBar);
    if (!::PtInRect(&strip, cursor)) return false;
    const HWND under = ::WindowFromPoint(cursor);
    return under == win.client || ::IsChild(win.client, under);
}

void CALLBACK GiveBackTabWidths(HWND client, UINT, UINT_PTR timer, DWORD) {
    BrowserWindow* const win = WindowFromClient(client);
    if (win && win->keptTabWidth > 0 && CursorOnStrip(*win)) return;
    ::KillTimer(client, timer);
    if (!win || win->keptTabWidth == 0) return;
    win->keptTabWidth = 0;
    LayoutTabStrip(*win);
}

// Closes the tab with this button, which was clicked with the middle button or on its
// close button. Posted, so that the button is never destroyed from inside its own message.
void CloseTabByMouse(HWND button) {
    const ButtonTab clicked = TabOfButton(button);
    if (!clicked) return;
    BrowserWindow& win = *clicked.window;
    // The window goes with its last tab, and has no strip left to keep.
    if (win.tabs.size() > 1) {
        win.keptTabWidth = Width(WindowRectOf(button));
        ::SetTimer(win.client, kKeptWidthTimer, kKeptWidthPollMs, GiveBackTabWidths);
    }
    ::PostMessageW(win.client, kMsgCloseTab, static_cast<WPARAM>(win.tabs[clicked.index].id), 0);
}

LRESULT CALLBACK TabButtonProc(HWND button, UINT msg, WPARAM wParam, LPARAM lParam, UINT_PTR subclassId, DWORD_PTR) {
    TabDrag& drag = g_app.drag;
    CloseButtonMouse& closeMouse = g_app.closeButtonMouse;
    switch (msg) {
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK: {
        // Handled here rather than by the button so that a press can turn into a
        // drag. As in Chrome, the tab is selected on press, not on release. The keys
        // come with the message, which says what they were at the time of the click.
        const ButtonTab pressed = TabOfButton(button);
        if (!pressed) break;
        EndTabDrag();
        EndCloseButtonPress();
        if (OnCloseButton(button, lParam)) {
            PressCloseButton(button);
            return 0;
        }
        ::SetFocus(button);
        BrowserWindow& win = *pressed.window;
        const int tabId = win.tabs[pressed.index].id;
        const int fromTabId = ActiveTabId(win);
        const bool ctrl = (wParam & MK_CONTROL) != 0;
        bool collapseOnRelease = false;
        if (wParam & MK_SHIFT) {
            SelectTabRange(win, pressed.index, ctrl);
        } else if (ctrl) {
            // A tab taken out of the selection has nothing to drag.
            if (!ToggleTabSelected(win, pressed.index)) {
                NoteUserSwitch(win, fromTabId);
                return 0;
            }
        } else if (ShowsAsSelected(win, pressed.index)) {
            // It may be the start of a drag of them all. A plain click picks it alone.
            ActivateTab(win, pressed.index);
            collapseOnRelease = true;
        } else {
            SwitchToTab(win, pressed.index);
        }
        NoteUserSwitch(win, fromTabId);
        drag.tabId = tabId;
        drag.button = button;
        drag.grab = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        drag.collapseOnRelease = collapseOnRelease;
        ::SetCapture(button);
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (closeMouse.pressed == button) {
            // Held down, it looks it while the mouse is on it, as a button does.
            if (const bool over = OnCloseButton(button, lParam); over != closeMouse.pressedOver) {
                closeMouse.pressedOver = over;
                Repaint(button);
            }
            return 0;
        }
        if (drag.button != button) {
            // Not lit while a tab is held down: it is only for clicking.
            if (!drag.tabId) SetHotCloseButton(OnCloseButton(button, lParam) ? button : nullptr);
            break;
        }
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
    case WM_MOUSELEAVE:
        if (closeMouse.hot == button) SetHotCloseButton(nullptr);
        break;
    case WM_LBUTTONUP: {
        if (closeMouse.pressed == button) {
            // Let go anywhere but on the close button, nothing happens.
            const bool over = OnCloseButton(button, lParam);
            EndCloseButtonPress();
            if (over) CloseTabByMouse(button);
            return 0;
        }
        if (drag.button != button) break;
        std::optional<TabDrop> drop;
        if (drag.dragging) {
            drop = TabDrop{.tabId = drag.tabId,
                           .tabIds = drag.tabIds,
                           .point = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)},
                           .grab = drag.ghostGrab};
            ::ClientToScreen(button, &drop->point);
        }
        const bool collapse = drag.collapseOnRelease && !drag.dragging;
        const int pressedId = drag.tabId;
        EndTabDrag();
        if (collapse) {
            if (const TabLocation at = FindTab(pressedId)) SwitchToTab(*at.window, at.index);
        }
        // Posted, so that the drop never destroys this button from inside its own message.
        if (drop) {
            g_app.pendingDrop = drop;
            if (!::PostMessageW(::GetParent(button), kMsgDropTab, 0, 0)) g_app.pendingDrop.reset();
        }
        return 0;
    }
    case WM_MBUTTONUP:
        // Middle-click closes the tab, as its close button does.
        CloseTabByMouse(button);
        return 0;
    case WM_CAPTURECHANGED:
        if (closeMouse.pressed == button) EndCloseButtonPress();
        if (drag.button == button) EndTabDrag();
        break;
    case WM_NCDESTROY:
        if (drag.button == button) EndTabDrag();
        // Capture goes with the button; there is nothing left to repaint.
        if (closeMouse.hot == button) closeMouse.hot = nullptr;
        if (closeMouse.pressed == button) {
            closeMouse.pressed = nullptr;
            closeMouse.pressedOver = false;
        }
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
    // The ones still in that window, in its order: a page may have closed one meanwhile.
    std::vector<int> tabIds;
    for (const Tab& tab : src.tabs) {
        if (tab.id == drop.tabId || IsOneOf(drop.tabIds, tab.id)) tabIds.push_back(tab.id);
    }
    // Where the first of them goes: the ghost's top left corner.
    const POINT tabAt{drop.point.x - drop.grab.x, drop.point.y - drop.grab.y};

    const DropTarget target = FindDropTarget(drop.point, tabIds);
    BrowserWindow* dst = target.window;
    const bool newWindow = dst == nullptr;
    if (newWindow) {
        // Dropped away from every tab strip: the tabs get a window of their own, the
        // size of the one they came from. All of a window's tabs just take their
        // window along.
        if (tabIds.size() == src.tabs.size()) {
            MoveFrameTabTo(src, tabAt);
            return;
        }
        const RECT place = WindowRectOf(src.frame);
        dst = CreateBrowserWindow(true, &place, true);
        if (!dst) return;
    }

    MoveTabs(src, tabIds, *dst, target.insertAt, drop.tabId);

    if (newWindow) {
        MoveFrameTabTo(*dst, tabAt);
        ::ShowWindow(dst->frame, SW_SHOWNA);
    }
    ::SetForegroundWindow(dst->frame);
    if (Tab* tab = ActiveTab(*dst); tab && tab->controller) MoveFocusToWebView(*tab);
}

LRESULT CALLBACK TabGhostProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_PAINT) {
        // Each tab dragged is drawn like the active tab, side by side, as they will land.
        // Their close buttons are as in the strip, where only the pressed tab, the active
        // one, keeps its own however narrow it is.
        PAINTSTRUCT paint;
        const HDC dc = ::BeginPaint(hwnd, &paint);
        const RECT client = ClientRectOf(hwnd);
        ::FillRect(dc, &client, g_app.theme.background.get());

        const std::vector<int>& tabIds = g_app.drag.tabIds;
        const int count = std::max(1, static_cast<int>(tabIds.size()));
        const int width = (client.right - (count - 1) * kTabGap) / count;
        for (int i = 0; i < count; ++i) {
            DRAWITEMSTRUCT item{};
            item.hwndItem = hwnd; // its window text, the pressed tab's title, if a tab has gone
            item.hDC = dc;
            item.rcItem = {i * (width + kTabGap), 0, i * (width + kTabGap) + width, client.bottom};
            const TabLocation dragged = FindTab(i < static_cast<int>(tabIds.size()) ? tabIds[i] : g_app.drag.tabId);
            const Tab* const tab = dragged ? &dragged.tab() : nullptr;
            const std::wstring title = tab && !tab->title.empty() ? tab->title : std::wstring(kNewTabTitle);
            DrawOwnerButton(item, tab,
                            {.active = true,
                             .inSelection = count > 1,
                             .text = tab ? &title : nullptr,
                             .closeAlways = tab && tab->id == g_app.drag.tabId});
        }
        ::EndPaint(hwnd, &paint);
        return 0;
    }
    return ::DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace exodium
