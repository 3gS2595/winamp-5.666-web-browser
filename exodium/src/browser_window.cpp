// Browser windows: making and closing them, the window inside Winamp's frame that
// holds a window's controls, and the shortcuts that work anywhere in one.
#include "browser.h"

#include "embed_frame.h"

#include <windowsx.h>

#include <algorithm>
#include <ranges>
#include <utility>

namespace exodium {

namespace {

// How far a new window opens down and right of the one before it.
constexpr int kCascadeOffset = 30;
// What a private window's title ends with.
constexpr wchar_t kPrivateTitleEnd[] = L" (Private)";

// Ctrl+N, and Ctrl+Shift+N for a private window: opens at the size of the window it came
// from, stepped down and right of it.
void OpenWindowLike(const BrowserWindow* win, bool isPrivate = false) {
    RECT place{};
    const bool like = win && ::GetWindowRect(win->frame, &place);
    if (like) ::OffsetRect(&place, kCascadeOffset, kCascadeOffset);
    if (const BrowserWindow* opened = CreateBrowserWindow(true, like ? &place : nullptr, false, isPrivate)) {
        ::SetForegroundWindow(opened->frame);
    }
}

void OnDrawItem(BrowserWindow& win, const DRAWITEMSTRUCT& item) {
    if (item.CtlID == kIdSuggest) {
        DrawSuggestion(win, item);
        return;
    }
    if (item.CtlID == kIdBookmark) {
        // What it says is which star it shows (UpdateBookmarkButton); the star is drawn.
        DrawBookmarkButton(item, WindowText(item.hwndItem) == kGlyphBookmarked);
        return;
    }
    // A tab's button is drawn with its tab's icon; a toolbar button has no tab.
    const int index = ::GetDlgCtrlID(item.hwndItem) - kTabButtonIdBase;
    const bool isTab = index >= 0 && index < kMaxTabButtons;
    const Tab* tab = isTab && IsTabIndex(win, index) ? &win.tabs[index] : nullptr;
    const bool active = isTab && index == win.active;
    const CloseButtonMouse& mouse = g_app.closeButtonMouse;
    DrawOwnerButton(item, tab,
                    {.active = active,
                     .inSelection = ShowsAsSelected(win, index),
                     .closeAlways = active,
                     .closeHot = item.hwndItem == mouse.hot,
                     .closePressed = item.hwndItem == mouse.pressed && mouse.pressedOver});
}

// Says whether the command was one of ours.
bool OnCommand(BrowserWindow& win, int id, int code) {
    if (id == kIdUrl) {
        if (code == EN_CHANGE) {
            if (!win.settingUrl && ::GetFocus() == win.urlEdit) OnUrlEdited(win);
            return true;
        }
        if (code == EN_KILLFOCUS) {
            win.editDirty = false;
            HideSuggestions(win);
            return true;
        }
    }

    if (code == BN_CLICKED) {
        Tab* const tab = ActiveTab(win);
        ICoreWebView2* const webview = tab ? tab->webview.Get() : nullptr;
        switch (id) {
        case kIdBack:
        case kIdForward:
            // The click doesn't say which keys were held with it, but it comes while they still are.
            GoBackOrForward(win, id == kIdForward, WhereClickOpens(false, IsKeyDown(VK_CONTROL), IsKeyDown(VK_SHIFT)));
            return true;
        case kIdReload:
            if (webview && tab->loading) webview->Stop();
            else if (webview) webview->Reload();
            return true;
        case kIdCloseTab:
            CloseActiveTab(win);
            return true;
        case kIdBookmark:
            ToggleBookmark(win);
            // The click took the keyboard to the star, which has no use for it.
            if (tab && tab->controller) MoveFocusToWebView(*tab);
            return true;
        case kIdNewTab:
            NewTab(win);
            FocusAddressBar(win);
            return true;
        case kIdTabsLeft:
        case kIdTabsRight: {
            // A click brings on the next stripful.
            const int by = FitTabStrip(win).shown;
            ScrollTabStrip(win, id == kIdTabsLeft ? -by : by);
            return true;
        }
        default:
            break;
        }
    }

    // A click on a tab from the keyboard; the mouse is taken care of in tab_drag.cpp. The
    // button of the tab that has just been made active sends one too, from inside
    // ActivateTab, so one for the tab in view is let be: switching to it again would
    // leave it the only one selected.
    if (const int index = id - kTabButtonIdBase; index >= 0 && index < kMaxTabButtons) {
        if (code == BN_CLICKED && index != win.active) {
            const int fromTabId = ActiveTabId(win);
            SwitchToTab(win, index);
            NoteUserSwitch(win, fromTabId);
        }
        return true;
    }
    return false;
}

LRESULT CALLBACK ClientProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        return ::DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    auto* const self = reinterpret_cast<BrowserWindow*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!self) return ::DefWindowProcW(hwnd, msg, wParam, lParam);
    BrowserWindow& win = *self;
    const Theme& theme = g_app.theme;

    switch (msg) {
    case WM_ERASEBKGND: {
        const RECT client = ClientRectOf(hwnd);
        ::FillRect(reinterpret_cast<HDC>(wParam), &client, theme.background.get());
        return 1;
    }
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORBTN:
    case WM_CTLCOLORLISTBOX: {
        // The tab strip and the address bar sit on the background; everything else is a panel.
        const HDC dc = reinterpret_cast<HDC>(wParam);
        const bool onBackground =
            msg == WM_CTLCOLOREDIT || (msg == WM_CTLCOLORSTATIC && reinterpret_cast<HWND>(lParam) == win.tabBar);
        ::SetTextColor(dc, color::kText);
        ::SetBkColor(dc, onBackground ? color::kBackground : color::kPanel);
        return reinterpret_cast<LRESULT>((onBackground ? theme.background : theme.panel).get());
    }
    case WM_DRAWITEM:
        if (const auto* item = reinterpret_cast<const DRAWITEMSTRUCT*>(lParam)) {
            OnDrawItem(win, *item);
            return TRUE;
        }
        break;

    case kMsgInitWebView:
        if (const Tab* tab = ActiveTab(win); tab && !tab->controller) CreateWebViewForTab(win, tab->id);
        return 0;
    case kMsgDropTab:
        if (const auto drop = std::exchange(g_app.pendingDrop, std::nullopt)) DropDraggedTab(*drop);
        return 0;
    case kMsgCloseTab:
        CloseTab(win, TabIndexById(win, static_cast<int>(wParam)));
        return 0;
    case kMsgRevive:
        ReviveWebViews();
        return 0;
    case kMsgOpenLinks:
        OpenPendingLinks();
        return 0;
    case kMsgFollowSound:
        LookAtPageSound();
        return 0;
    case kMsgRaiseDocked:
        RaiseDockedWindows();
        return 0;
    case kMsgCloseDevTools:
        OnDevToolsCloseMessage(wParam, lParam);
        return 0;
    case kMsgTabCommand: {
        // The tab is known by its button, as it was when the command was picked. One that
        // has gone since, or moved to another window, has none here.
        const HWND button = reinterpret_cast<HWND>(lParam);
        const auto tab = std::ranges::find_if(win.tabs, [button](const Tab& t) { return t.button.get() == button; });
        if (button && tab != win.tabs.end()) RunTabCommand(win, tab->id, static_cast<TabCommand>(wParam));
        return 0;
    }
    case kMsgDiscardWindow:
        // Nothing of this window, `win` included, is left once this returns.
        DestroyBrowserWindow(win);
        std::erase_if(g_app.windows, [self](const auto& owned) { return owned.get() == self; });
        return 0;

    case WM_WINDOWPOSCHANGING:
        KeepClientInPlace(win, *reinterpret_cast<WINDOWPOS*>(lParam));
        break;
    case WM_SIZE:
        LayoutToolbarAndTabs(win);
        return 0;
    case WM_TIMER:
        if (wParam == kSpinTimer) {
            OnSpinTimer(win);
            return 0;
        }
        if (OnSessionTimer(wParam) || OnHistoryTimer(wParam) || OnSleepTimer(wParam)) return 0;
        break;
    case WM_NOTIFY:
        if (OnTabTipNeeded(win, reinterpret_cast<NMHDR*>(lParam))) return 0;
        break;
    case WM_MOUSEWHEEL: {
        // Over the tab strip the wheel scrolls it. The strip's own controls pass the
        // wheel on to here.
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        ::ScreenToClient(hwnd, &point);
        if (point.y < 0 || point.y >= kTabBarHeight || win.isFullscreen) break;
        ScrollTabStrip(win, GET_WHEEL_DELTA_WPARAM(wParam) > 0 ? -kTabWheelStep : kTabWheelStep);
        return 0;
    }
    case WM_GETDLGCODE:
        // Winamp's message loop puts keys through the dialog manager, which keeps Escape for
        // itself from a window that doesn't ask for it.
        return DLGC_WANTALLKEYS;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: { // a key with Alt comes as the second
        const Modifiers mods = CurrentModifiers();
        if (HandleGlobalShortcut(&win, wParam, mods) || HandleControlShortcut(win, wParam, mods)) return 0;
        // Tab goes on into the page from here as it does from the address bar, where the
        // keyboard was left waiting for a page still being made.
        if (wParam == VK_TAB && !mods.ctrl && !mods.alt) {
            TabIntoPage(win, mods.shift);
            return 0;
        }
        break;
    }
    case WM_SYSCHAR:
        // What Alt+D leaves behind, which Windows would look for a menu with, and beep.
        if (wParam == L'd' || wParam == L'D') return 0;
        break;
    case WM_COMMAND:
        if (OnCommand(win, LOWORD(wParam), HIWORD(wParam))) return 0;
        break;
    case WM_CLOSE: // the frame's close button sends this
        CloseBrowserWindow(win);
        return 0;
    default:
        break;
    }
    return ::DefWindowProcW(hwnd, msg, wParam, lParam);
}

void RegisterWindowClass(const wchar_t* name, WNDPROC proc, HBRUSH background) {
    const WNDCLASSW windowClass{
        .lpfnWndProc = proc,
        .hInstance = g_app.instance,
        .hbrBackground = background,
        .lpszClassName = name,
    };
    ::RegisterClassW(&windowClass);
}

// The work area of the screen `place` is on, or nearest to.
[[nodiscard]] RECT WorkAreaNear(const RECT& place) noexcept {
    MONITORINFO monitor{.cbSize = sizeof(monitor)};
    ::GetMonitorInfoW(::MonitorFromRect(&place, MONITOR_DEFAULTTONEAREST), &monitor);
    return monitor.rcWork;
}

// As near to (x, y) as a window of the given size can be and stay inside `work`.
[[nodiscard]] POINT KeepInside(const RECT& work, int x, int y, int width, int height) noexcept {
    return {std::max(static_cast<int>(work.left), std::min(x, static_cast<int>(work.right) - width)),
            std::max(static_cast<int>(work.top), std::min(y, static_cast<int>(work.bottom) - height))};
}

} // namespace

RECT KeptOnScreen(const RECT& place) noexcept {
    const RECT work = WorkAreaNear(place);
    const int width = std::min(Width(place), Width(work)), height = std::min(Height(place), Height(work));
    const POINT at = KeepInside(work, place.left, place.top, width, height);
    return {at.x, at.y, at.x + width, at.y + height};
}

// ---- finding windows and tabs ----

BrowserWindow* WindowFromClient(HWND client) noexcept {
    const auto found = std::ranges::find(g_app.windows, client, [](const auto& win) { return win->client; });
    return found == g_app.windows.end() ? nullptr : found->get();
}

BrowserWindow* WindowFromChild(HWND child) noexcept {
    return WindowFromClient(::GetParent(child));
}

int TabIndexById(const BrowserWindow& win, int tabId) noexcept {
    const auto found = std::ranges::find(win.tabs, tabId, &Tab::id);
    return found == win.tabs.end() ? -1 : static_cast<int>(found - win.tabs.begin());
}

bool IsTabIndex(const BrowserWindow& win, int index) noexcept {
    return index >= 0 && index < static_cast<int>(win.tabs.size());
}

Tab* ActiveTab(BrowserWindow& win) noexcept {
    return IsTabIndex(win, win.active) ? &win.tabs[win.active] : nullptr;
}

TabLocation FindTab(int tabId) noexcept {
    for (const auto& win : g_app.windows) {
        if (const int index = TabIndexById(*win, tabId); index >= 0) return {win.get(), index};
    }
    return {};
}

BrowserWindow* FrontUserWindow() {
    // Every top-level window, from the front.
    std::vector<HWND> fromFront;
    ::EnumWindows(
        [](HWND window, LPARAM list) -> BOOL {
            reinterpret_cast<std::vector<HWND>*>(list)->push_back(window);
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&fromFront));

    BrowserWindow* front = nullptr;
    std::pair<bool, std::ptrdiff_t> frontPlace;
    for (const auto& win : g_app.windows) {
        const Tab* const tab = ActiveTab(*win);
        if (win->closing || win->isPrivate || !::IsWindowVisible(win->frame) || (tab && tab->forClaude)) continue;
        // A frame docked into a skin is inside the skin's window. A minimized window comes
        // after every one that isn't, wherever it is among them.
        const HWND topLevel = ::GetAncestor(win->frame, GA_ROOT);
        const std::pair place{win->minimized, std::ranges::find(fromFront, topLevel) - fromFront.begin()};
        if (front && place >= frontPlace) continue;
        front = win.get();
        frontPlace = place;
    }
    return front;
}

// ---- windows ----

void RegisterWindowClasses() {
    const auto black = static_cast<HBRUSH>(::GetStockObject(BLACK_BRUSH));
    RegisterWindowClass(kClientClass, ClientProc, black);
    // Plain, borderless top-level window used only while a video/page is fullscreen.
    RegisterWindowClass(kFullscreenClass, FullscreenProc, black);
    // Floating copy of a tab that follows the cursor while the tab is dragged.
    RegisterWindowClass(kTabGhostClass, TabGhostProc, nullptr);
    // The ironwork on a window's corner. It has no background: its picture is all of it.
    RegisterWindowClass(kOrnamentClass, OrnamentProc, nullptr);
    // The downloads button and panel paint all of themselves too.
    RegisterWindowClass(kDownloadsButtonClass, DownloadsButtonProc, nullptr);
    RegisterWindowClass(kDownloadsPanelClass, DownloadsPanelProc, nullptr);
    // And Bitwarden's button, and the window round its popup.
    RegisterWindowClass(kBitwardenButtonClass, BitwardenButtonProc, nullptr);
    RegisterWindowClass(kBitwardenPopupClass, BitwardenPopupProc, nullptr);
    // And the strip along the top of docked DevTools, and the splitter beside them.
    RegisterWindowClass(kDevToolsBarClass, DevToolsBarProc, nullptr);
    RegisterWindowClass(kDevToolsSplitterClass, DevToolsSplitterProc, nullptr);
}

void UnregisterWindowClasses() {
    for (const wchar_t* name :
         {kClientClass, kFullscreenClass, kTabGhostClass, kOrnamentClass, kDownloadsButtonClass, kDownloadsPanelClass,
          kBitwardenButtonClass, kBitwardenPopupClass, kDevToolsBarClass, kDevToolsSplitterClass}) {
        ::UnregisterClassW(name, g_app.instance);
    }
}

Tab& AddTab(BrowserWindow& win, std::wstring_view title, int index) {
    const int count = static_cast<int>(win.tabs.size());
    if (index < 0 || index > count) index = count;
    // The tab in view stays the one in view.
    if (win.active >= index) ++win.active;
    Tab& tab = *win.tabs.emplace(win.tabs.begin() + index);
    tab.id = g_app.nextTabId++;
    tab.title = title;
    tab.seenAt = ::GetTickCount64(); // how long it has been out of view is counted from now
    tab.isPrivate = win.isPrivate;   // every tab opened in a private window is private
    return tab;
}

void UpdateFrameTitle(BrowserWindow& win) {
    // The active tab's title is the frame's caption, so that each window is a
    // distinct, recognizable Alt+Tab entry.
    if (!win.frame) return;
    const Tab* tab = ActiveTab(win);
    const bool untitled = !tab || tab->title.empty() || tab->title == kNewTabTitle;
    std::wstring title = untitled ? std::wstring(kAppName) : tab->title;
    // A private window says so, in its title bar, on the taskbar and in Alt+Tab.
    if (win.isPrivate) title += kPrivateTitleEnd;
    ::SetWindowTextW(win.frame, title.c_str());
}

BrowserWindow* CreateBrowserWindow(bool show, const RECT* place, bool forIncomingTab, bool isPrivate) {
    BrowserWindow& win = *g_app.windows.emplace_back(std::make_unique<BrowserWindow>());
    win.isPrivate = isPrivate;

    // Each window gets its own GUID so Winamp/skin engine can tell frames apart.
    if (FAILED(::CoCreateGuid(&win.guid))) win.guid = {};
    win.frame = embed::CreateFrame(win.embedState, win.guid);
    if (!win.frame) {
        g_app.windows.pop_back();
        return nullptr;
    }
    UpdateFrameTitle(win); // the browser's name, until a tab has a page's
    AttachFrame(win);

    if (place) {
        ::SetWindowPos(win.frame, nullptr, place->left, place->top, Width(*place), Height(*place),
                       SWP_NOZORDER | SWP_NOACTIVATE);
    } else if (g_app.windows.size() > 1) {
        // Cascade additional windows so they don't stack exactly on the first, for as
        // long as there is screen left to step down it. Links can open a lot of them.
        const RECT frame = WindowRectOf(win.frame);
        const int offset = static_cast<int>(g_app.windows.size() - 1) * kCascadeOffset;
        const POINT at =
            KeepInside(WorkAreaNear(frame), frame.left + offset, frame.top + offset, Width(frame), Height(frame));
        ::SetWindowPos(win.frame, nullptr, at.x, at.y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }

    const RECT inside = ClientPlace(win);
    win.client = ::CreateWindowExW(0, kClientClass, L"", WS_CHILD | WS_VISIBLE, inside.left, inside.top, Width(inside),
                                   Height(inside), win.frame, nullptr, g_app.instance, &win);
    CreateControls(win);
    CreateOrnaments(win);

    // Initial tab. A window made to receive a tab, one dragged out of another window
    // or one a page opened, starts empty instead; the caller places and shows it once
    // that tab is in.
    if (!forIncomingTab) {
        AddTab(win).selected = true;
        win.active = 0;
    }
    LayoutToolbarAndTabs(win);

    // Per-window menu entry so each window can be toggled from Winamp.
    win.menuId = embed::RegisterMenuCommand();
    embed::AddMenuItems(*win.menuId, kAppName, show);
    if (forIncomingTab) return &win;

    if (show) ::ShowWindow(win.frame, SW_SHOWNA);
    ::PostMessageW(win.client, kMsgInitWebView, 0, 0);
    return &win;
}

BrowserWindow* CreatePopupWindow(const BrowserWindow& opener, const PopupRequest& request) {
    // A minimized opener is a sliver parked off screen. The place it goes back to
    // stands in for it.
    const RECT from = ::IsIconic(opener.frame) ? opener.embedState.r : WindowRectOf(opener.frame);
    // A window a private page opens is private too.
    BrowserWindow* const win = CreateBrowserWindow(true, &from, true, opener.isPrivate);
    if (!win) return nullptr;

    const RECT work = WorkAreaNear(from);
    const int dpi = static_cast<int>(::GetDpiForWindow(win->frame));
    const auto toScreen = [dpi](LONG pagePixels) { return ::MulDiv(pagePixels, dpi, USER_DEFAULT_SCREEN_DPI); };

    // The size asked for is the page's: the frame, tab strip and toolbar go round it.
    // A window given no size is as big as the one it came from.
    int width = Width(from), height = Height(from);
    if (request.size) {
        const RECT frame = WindowRectOf(win->frame);
        const RECT page = WebViewBounds(*win);
        width = toScreen(request.size->cx) + Width(frame) - Width(page);
        height = toScreen(request.size->cy) + Height(frame) - Height(page);
    }
    width = std::min(width, Width(work));
    height = std::min(height, Height(work));

    // Given no place either, it opens over the middle of the window it came from.
    // Wherever it was sent, it stays on the screen.
    int x = (from.left + from.right - width) / 2, y = (from.top + from.bottom - height) / 2;
    if (request.position) {
        x = toScreen(request.position->x);
        y = toScreen(request.position->y);
    }
    const POINT at = KeepInside(work, x, y, width, height);

    ::SetWindowPos(win->frame, nullptr, at.x, at.y, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
    return win;
}

namespace {

// A link from outside in a window there is already: in a new tab at the end of its strip
// and in view, with the window brought to the front, as in Chrome.
void OpenLinkInNewTab(BrowserWindow& win, std::wstring_view url) {
    LeaveVideoFullscreen(win); // a video that has the screen has the tab strip out of use
    if (::IsIconic(win.frame)) ::ShowWindow(win.frame, SW_RESTORE);
    // The tab in view may be a link's as well, its page still to be made from the message
    // loop. Out of view it would be left until it was looked at, and it is to load now.
    if (const Tab* const inView = ActiveTab(win)) CreateWebViewForTab(win, inView->id);
    // An address bar that is being typed in would otherwise keep the keyboard and its text.
    if (::GetFocus() == win.urlEdit) ::SetFocus(win.client);
    NewTab(win, url);
    ::SetForegroundWindow(win.frame);
}

} // namespace

// A link that leads to no page the browser would show is dropped. No link at all, which
// is what the Start menu's entry sends, is a window on the home page.
void OpenPendingLinks() {
    // Up first, so that they don't come up over the link's window, and are there for it to go in.
    ShowRestoredWindows();
    for (const std::wstring& link : std::exchange(g_app.pendingLinks, {})) {
        const std::optional<std::wstring> url = ResolveOutsideLink(link);
        if (!url) continue;
        // The Start menu's entry, which started Winamp, and the windows of the last session came up with it.
        if (url->empty() && WindowsRestoredForStart()) continue;
        // Claude Code's browser tools open their windows on this page and nothing else does.
        // Such a window is shown once it is in place; see claude_session.cpp.
        const bool forClaude = url->contains(kToolStartPage);
        // A link goes in the window the user has in front, where there is one.
        if (BrowserWindow* const front = url->empty() || forClaude ? nullptr : FrontUserWindow()) {
            OpenLinkInNewTab(*front, *url);
            continue;
        }
        BrowserWindow* const win = CreateBrowserWindow(!forClaude);
        if (!win) continue;
        // Its webview is made from the message loop, so it isn't too late to say where it goes.
        Tab& tab = win->tabs.front();
        // Said here rather than left to the webview, so the address bar has it from the start.
        tab.pendingUrl = url->empty() ? HomePage() : *url;
        tab.forClaude = forClaude;
        if (const auto session = ToolSessionOf(*url)) {
            tab.claudeSession = *session;
            WatchClaudeSession(*session);
        }
        UpdateUrlBox(*win);
        if (forClaude) ShowWithoutFocus(*win);
        else ::SetForegroundWindow(win->frame);
    }
}

// Closing a window closes its tabs, as closing them one by one would: their pages
// stop, and Ctrl+Shift+T brings them back, all of them together as the window they were.
// The first window is the one Winamp's menu shows, so it stays behind, hidden and with a
// blank tab, for the next time it is asked for. Any other window is done with, and its
// tabs' webviews go with it: they come back from their pages, which they make their Back
// and Forward again. Every way of closing a window ends up here: its close button, the
// taskbar, the keyboard, its last tab going.
void CloseBrowserWindow(BrowserWindow& win) {
    if (win.closing) return;
    win.closing = &win != g_app.windows.front().get();
    LeaveVideoFullscreen(win);
    if (g_app.drag.tabId && FindTab(g_app.drag.tabId).window == &win) EndTabDrag();

    ClosedWith with{.window = std::make_shared<const ClosedWindow>(ClosedWindow{
                        .place = NormalPlace(win), .maximized = IsMaximized(win), .activeIndex = win.active})};
    // Taken out from the right, so that each is at the place it had until it goes. The
    // tab in view goes last.
    int last = win.active;
    for (int i = static_cast<int>(win.tabs.size()) - 1; i >= 0; --i) {
        if (i == last) continue;
        with.index = i;
        RemoveTab(win, i, with);
        if (i < last) --last;
    }
    with.index = with.window->activeIndex;
    if (!win.tabs.empty()) RemoveTab(win, 0, with);
    TrimClosedTabs();
    win.active = -1;
    win.editDirty = false; // whatever was being typed belonged to a tab that is gone
    HideSuggestions(win);
    CloseDownloadsPanel(win);
    CloseBitwardenPopupOf(win);
    ::ShowWindow(win.frame, SW_HIDE);

    if (win.closing) {
        // Destroyed from the message loop, not from inside whichever of its own
        // controls or webviews asked for the close.
        ::PostMessageW(win.client, kMsgDiscardWindow, 0, 0);
        return;
    }

    AddTab(win).selected = true;
    win.active = 0;
    LayoutToolbarAndTabs(win);
    UpdateFrameTitle(win);
    UpdateUrlBox(win);
    UpdateReloadButton(win);
}

void DestroyBrowserWindow(BrowserWindow& win) {
    win.fullscreenWindow.reset();
    CloseBitwardenPopupOf(win); // the frame owns it, and would take it along unasked

    // A maximized or minimized window is remembered at the size it goes back to.
    WINDOWPLACEMENT placement{.length = sizeof(placement)};
    if (win.frame && (::IsZoomed(win.frame) || ::IsIconic(win.frame)) && ::GetWindowPlacement(win.frame, &placement)) {
        win.embedState.r = placement.rcNormalPosition;
    }

    // Tabs closed in it earlier were put away in it, and can't outlive it either. They
    // come back from their addresses.
    for (ClosedTab& closed : g_app.closedTabs) {
        HWND parent = nullptr;
        if (closed.tab.controller && SUCCEEDED(closed.tab.controller->get_ParentWindow(&parent)) &&
            parent == win.client) {
            DiscardClosedWebView(closed);
        }
    }

    for (Tab& tab : win.tabs) KeepWebViewForDownloads(tab);
    for (Tab& tab : win.discardedTabs) KeepWebViewForDownloads(tab);
    win.tabs.clear(); // closes their webviews and takes their buttons out of the strip
    win.discardedTabs.clear();
    win.downloads = {}; // its button and panel go with the client, and nothing is to reach them on the way
    win.devtools = {};  // as do the DevTools' strip and splitter
    // The tabs' tips are a window of their own, which the frame owns; it goes before its tabs' buttons do.
    if (win.tabTips) ::DestroyWindow(std::exchange(win.tabTips, nullptr));
    if (win.client) ::DestroyWindow(win.client); // and with it every control
    win.client = nullptr;
    win.suggestList = nullptr;

    if (win.menuId) embed::RemoveMenuItems(*win.menuId);
    win.ornaments.windows = {}; // before the frame that owns them takes them along
    if (win.frame) {
        DetachFrame(win);
        ::DestroyWindow(win.frame);
        win.frame = nullptr;
    }
    embed::SavePlacement(win.embedState);
}

// It is run as Winamp's main window is destroyed, while the rest of Winamp is still
// there. The frames aren't owned by that window (see frame.cpp), so nothing else
// destroys them.
void ShutDownBrowser() {
    if (g_app.shutDown) return;
    // The windows and tabs as Winamp leaves them, before anything of them goes.
    SaveSession();
    g_app.shutDown = true;
    StopWinampNewWindowKey();
    StopGlow(); // before Winamp's windows start to go, which it would follow
    StopRaisingDockedWindows();
    StopWatchingClaudeSessions();
    StopExtensions(); // Bitwarden's popup goes with it
    // Before the webviews go: nothing a download says after this is listened for.
    StopFollowingDownloads();
    SaveHistoryNow();
    EndTabDrag();
    if (g_app.debuggingPort != 0) CloseBrowserProcess();
    DetachLibraryOrnaments(); // the library goes on until gen_ml is done with it

    // Out of sight at once; taking the webviews apart can take a moment.
    for (const auto& win : g_app.windows) {
        if (win->fullscreenWindow) ::ShowWindow(win->fullscreenWindow.get(), SW_HIDE);
        if (win->frame) ::ShowWindow(win->frame, SW_HIDE);
    }

    g_app.closedTabs.clear();
    // The first window goes last: the place it leaves in winamp.ini is where the next
    // session's window opens.
    for (const auto& win : g_app.windows | std::views::reverse) DestroyBrowserWindow(*win);
    g_app.windows.clear();
    g_app.environmentWaiters.clear();
    g_app.environment.Reset();
}

bool HandleGlobalShortcut(BrowserWindow* win, WPARAM key, Modifiers mods) {
    if (g_app.drag.tabId && key == VK_ESCAPE) {
        EndTabDrag();
        return true;
    }
    if (mods == kCtrl && key == 'N') {
        OpenWindowLike(win);
        return true;
    }
    if (!win) return false;

    // A private window, as Chrome's Ctrl+Shift+N opens an Incognito one.
    if (mods == kCtrlShift && key == 'N') {
        OpenWindowLike(win, true);
        return true;
    }

    // F12 and Ctrl+Shift+I, J and C are the DevTools', docked in the window where they can
    // be. Where they can't, the keys are left to the page's webview, which opens its own.
    if (HandleDevToolsShortcut(*win, key, mods)) return true;

    // Escape closes the downloads panel, wherever the keyboard is in the window. While the
    // panel is closed it is left alone, for the page or the control it was pressed in. The
    // modifier keys aren't looked at: one let go of while another program had the keyboard
    // can still count as held here.
    if (key == VK_ESCAPE && !win->isFullscreen && CloseDownloadsPanel(*win)) return true;

    // Ctrl+1 to Ctrl+9, on the row of digits or the number pad: a tab by its place in the strip.
    const int tabNumber = mods != kCtrl                            ? 0
                          : key >= '1' && key <= '9'               ? static_cast<int>(key - '0')
                          : key >= VK_NUMPAD1 && key <= VK_NUMPAD9 ? static_cast<int>(key - VK_NUMPAD0)
                                                                   : 0;
    const bool addressBarKey = (mods == kAlt && key == 'D') || (mods == kNoModifiers && key == VK_F6);
    const bool pageKey = mods == kCtrl && (key == VK_NEXT || key == VK_PRIOR); // Ctrl+Page Down and Up

    // These need the tab strip and toolbar back, so they end fullscreen video first.
    const bool ctrlWithoutAlt = mods.ctrl && !mods.alt;
    const bool needsStrip =
        (ctrlWithoutAlt && (key == 'L' || key == 'T' || key == 'W' || key == 'H' || key == VK_TAB)) ||
        (mods == kCtrlShift && key == 'O') || tabNumber != 0 || addressBarKey || pageKey;
    if (win->isFullscreen && needsStrip) LeaveVideoFullscreen(*win);

    if (tabNumber != 0) {
        SwitchToTabByNumber(*win, tabNumber);
        return true;
    }
    if (pageKey) {
        if (key == VK_NEXT) NextTab(*win);
        else PrevTab(*win);
        return true;
    }
    // Alt+D and F6 are the address bar's as well as Ctrl+L, as in Chrome.
    if (addressBarKey) {
        FocusAddressBar(*win);
        win->editDirty = true;
        return true;
    }

    if (mods == kCtrl) {
        switch (key) {
        case 'H':
            OpenHistory(*win);
            return true;
        case 'D':
            ToggleBookmark(*win);
            return true;
        case 'J':
            // The downloads, while there are any to show; otherwise the key is the page's.
            if (CloseDownloadsPanel(*win)) return true;
            if (g_app.downloads.empty() || win->isFullscreen) break;
            ShowDownloadsPanel(*win, false);
            return true;
        case 'L':
            FocusAddressBar(*win);
            win->editDirty = true;
            return true;
        case 'T':
            NewTab(*win);
            FocusAddressBar(*win);
            return true;
        case 'W':
            CloseActiveTab(*win);
            return true;
        case '0':
        case VK_NUMPAD0:
            ResetSiteZoom(*win);
            return true;
        default:
            break;
        }
    }
    if (mods == kCtrlShift && key == 'T') {
        ReopenLastClosedTab(*win);
        return true;
    }
    if (mods == kCtrlShift && key == 'O') {
        OpenBookmarks(*win);
        return true;
    }
    // Bitwarden's own shortcut in Chrome. Without Bitwarden it is the page's.
    if (mods == kCtrlShift && key == 'Y' && ToggleBitwardenPopup(*win)) return true;
    if ((mods == kCtrlShift && key == 'W') || (mods == kAlt && key == VK_F4)) {
        CloseBrowserWindow(*win);
        return true;
    }
    if (ctrlWithoutAlt && key == VK_TAB) {
        if (mods.shift) PrevTab(*win);
        else NextTab(*win);
        return true;
    }
    if (mods == kAlt && (key == VK_LEFT || key == VK_RIGHT)) {
        if (const Tab* tab = ActiveTab(*win); tab && tab->webview) {
            GoBackOrForward(*win, key == VK_RIGHT, OpenIn::kSameTab);
            return true;
        }
    }
    return false;
}

bool HandleControlShortcut(BrowserWindow& win, WPARAM key, Modifiers mods) {
    const bool reload = (key == VK_F5 && mods == kNoModifiers) || (key == 'R' && mods == kCtrl);
    const bool pastCache = (key == VK_F5 && mods == kCtrl) || (key == 'R' && mods == kCtrlShift);
    if (!reload && !pastCache) return false;
    if (Tab* const tab = ActiveTab(win)) ReloadTab(*tab, pastCache);
    return true;
}

} // namespace exodium
