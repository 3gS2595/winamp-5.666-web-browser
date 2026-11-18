// The browser's model and the functions that work on it.
//
// All state lives in one App: the browser windows with their tabs, and what the
// windows share (the WebView2 environment, history, zoom, closed tabs, the tab being
// dragged). Everything runs on Winamp's UI thread, so none of it is locked.
#pragma once

#include "bookmarks.h"
#include "devtools_layout.h"
#include "download_status.h"
#include "history.h"
#include "session_history.h"
#include "settings.h"
#include "site_zoom.h"
#include "suggestions.h"
#include "tab_order.h"
#include "url_util.h"
#include "win32_util.h"

#include <windows.h>

#include <WebView2.h>
#include <wa_ipc.h>
#include <wrl/client.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace exodium {

using Microsoft::WRL::ComPtr;

// ---- control ids, messages and measurements ----

enum ControlId : int {
    kIdBack = 1001,
    kIdForward = 1002,
    kIdUrl = 1003,
    kIdReload = 1005,
    kIdCloseTab = 1006,
    kIdDownloads = 1007,
    kIdBitwarden = 1008,
    kIdBookmark = 1009, // the star at the address bar's right end
    kIdTabBar = 1100,
    kIdNewTab = 1101,
    kIdTabsLeft = 1102,
    kIdTabsRight = 1103,
    kIdSuggest = 1200,
    kIdDownloadsPanel = 1201,
    kIdDevToolsBar = 1202,      // along the top of the DevTools docked in the window
    kIdDevToolsSplitter = 1203, // between them and the page
};
// A tab's button has this id plus the tab's position in its window.
inline constexpr int kTabButtonIdBase = WM_APP + 43;
inline constexpr int kMaxTabButtons = 0x1000;

// Messages a browser window's client posts to itself, for work that must not be
// done from inside the control or webview event that asked for it.
enum ClientMessage : UINT {
    kMsgInitWebView = WM_APP + 42,   // make the active tab's webview if it has none
    kMsgDropTab = WM_APP + 44,       // a dragged tab was dropped; see App::pendingDrop
    kMsgCloseTab = WM_APP + 45,      // wParam: the id of the tab to close
    kMsgRevive = WM_APP + 46,        // the browser process died; make every webview again
    kMsgDiscardWindow = WM_APP + 47, // the window was closed for good; destroy it
    kMsgOpenLinks = WM_APP + 48,     // links came in from outside; see App::pendingLinks
    kMsgFollowSound = WM_APP + 49,   // see whether Winamp is to play the pages' sound; see FollowPageSound
    kMsgRaiseDocked = WM_APP + 50,   // a window of Winamp's was activated; see RaiseDockedWindows
    kMsgTabCommand = WM_APP + 51,    // wParam: a TabCommand; lParam: the button of the tab it is for
    kMsgCloseDevTools = WM_APP + 52, // wParam: the id of the tab whose docked DevTools go; lParam: a DevToolsClose
};

inline constexpr int kTabBarHeight = 26;
inline constexpr int kToolbarHeight = 32;
inline constexpr int kTabWidth = 140;
inline constexpr int kTabMinWidth = 24;
inline constexpr int kTabGap = 4;
inline constexpr int kTabArrowWidth = 16; // each of the two buttons that scroll a tab strip
inline constexpr int kTabArrowGap = 2;
inline constexpr int kTabWheelStep = 3; // tabs a notch of the wheel scrolls it by
inline constexpr int kNewTabSlot = 26;  // room kept at the strip's right end for the new tab button
inline constexpr int kNewTabWidth = 24;
inline constexpr int kButtonWidth = 64;
inline constexpr int kButtonMinWidth = 28;
inline constexpr int kButtonGap = 6;
inline constexpr int kUrlMinWidth = 40;
inline constexpr int kMargin = 6;
inline constexpr int kTabDetachSlop = 12; // how far above/below a tab strip a dragged tab still counts as on it
// Where the client sits inside Winamp's frame: past its left border and title bar. A
// maximized window's client covers the left border instead (see ClientPlace).
inline constexpr int kFrameInsetLeft = 8;
inline constexpr int kFrameInsetTop = 24;
// The smallest frame Winamp can draw.
inline constexpr POINT kMinFrameSize{275, 116};

inline constexpr UINT_PTR kSpinTimer = 1; // runs in a window while any of its tabs shows a spinner
inline constexpr UINT kSpinTickMs = 50;   // between its frames
inline constexpr UINT kSpinTurnMs = 800;  // for a full turn

inline constexpr wchar_t kNewTabTitle[] = L"(new tab)";
inline constexpr wchar_t kAppName[] = L"Exodium";

// Window classes of our own.
inline constexpr wchar_t kClientClass[] = L"ExodiumClient";         // the inside of a browser window
inline constexpr wchar_t kFullscreenClass[] = L"ExodiumFullscreen"; // holds a tab's webview while it is fullscreen
inline constexpr wchar_t kTabGhostClass[] = L"ExodiumTabGhost";     // the copy of a tab that follows a drag
inline constexpr wchar_t kOrnamentClass[] = L"ExodiumOrnament";     // the ironwork on a window's corners
// The downloads button on the toolbar, while there are downloads, and the list it opens.
inline constexpr wchar_t kDownloadsButtonClass[] = L"ExodiumDownloadsButton";
inline constexpr wchar_t kDownloadsPanelClass[] = L"ExodiumDownloads";
// Bitwarden's button on the toolbar, and the popup it opens.
inline constexpr wchar_t kBitwardenButtonClass[] = L"ExodiumBitwardenButton";
inline constexpr wchar_t kBitwardenPopupClass[] = L"ExodiumBitwarden";
// The strip along the top of the DevTools docked in a window, and the bar between them and
// the page that sizes them.
inline constexpr wchar_t kDevToolsBarClass[] = L"ExodiumDevToolsBar";
inline constexpr wchar_t kDevToolsSplitterClass[] = L"ExodiumDevToolsSplitter";

// Claude Code's tabs have a browser profile of their own, apart from the user's: none of
// the user's sign-ins, site data or extensions (their password manager) reach them.
inline constexpr wchar_t kClaudeProfile[] = L"Claude";
// The DevTools docked in a window are a page too, in a profile of its own: what they keep
// of themselves (their settings, the tool they showed last) stays from run to run, and
// none of the user's extensions are in them.
inline constexpr wchar_t kDevToolsProfile[] = L"DevTools";

// The extensions in the user's profile (extensions.cpp): Bitwarden, as the Chrome Web Store
// knows it, and Exodium's own helper for its popup.
inline constexpr char kBitwardenId[] = "nngceckbapebfimnlniiiahkandclblb";
inline constexpr char kTabHelperId[] = "koaopjcjkmmaliakphhcgokicjebieef";
inline constexpr int kBitwardenIconSize = 16; // on its toolbar button

// Glyphs on the toolbar buttons.
inline constexpr wchar_t kGlyphBack[] = L"\u2190";       // leftwards arrow
inline constexpr wchar_t kGlyphForward[] = L"\u2192";    // rightwards arrow
inline constexpr wchar_t kGlyphReload[] = L"\u21BA";     // anticlockwise open circle arrow
inline constexpr wchar_t kGlyphStop[] = L"\u23F9";       // black square for stop
inline constexpr wchar_t kGlyphBookmark[] = L"\u2606";   // a white star: the page in view isn't a bookmark
inline constexpr wchar_t kGlyphBookmarked[] = L"\u2605"; // a black star: it is

// ---- colours and shared GDI objects ----

namespace color {
inline constexpr COLORREF kBackground = RGB(0, 0, 0);
inline constexpr COLORREF kPanel = RGB(16, 16, 16);
inline constexpr COLORREF kActiveTab = RGB(32, 32, 32);
inline constexpr COLORREF kSelectedTab = RGB(26, 32, 46);      // one of several selected, other than the active one
inline constexpr COLORREF kSelectedBorder = RGB(96, 128, 184); // round each of several selected tabs
inline constexpr COLORREF kCloseCross = RGB(190, 190, 190);    // the cross on a tab's close button
inline constexpr COLORREF kCloseHot = RGB(72, 72, 72);         // the disc behind it while the mouse is over it
inline constexpr COLORREF kClosePressed = RGB(104, 104, 104);  // and while it is held down
inline constexpr COLORREF kText = RGB(255, 255, 255);
inline constexpr COLORREF kBorder = RGB(60, 60, 60);
inline constexpr COLORREF kSpinner = RGB(138, 180, 248);
inline constexpr COLORREF kCrashed = RGB(242, 139, 130);
inline constexpr COLORREF kSuggestSelected = RGB(52, 52, 52);
inline constexpr COLORREF kSuggestDim = RGB(150, 150, 150);
inline constexpr COLORREF kSuggestUrl = RGB(138, 180, 248);
} // namespace color

struct Theme {
    UniqueBrush background;
    UniqueBrush panel;
    UniqueBrush activeTab;
    UniqueBrush selectedTab;
    UniqueBrush border;
    UniqueBrush selectedBorder;
    UniqueBrush suggestSelected;
    HFONT font = nullptr; // a stock font, not ours to delete
};

// ---- tabs ----

// Owns a WebView2 controller. Closing the controller is what takes its webview down;
// one that is merely released stays on screen. Doing it here means a tab that goes
// away, however it goes, takes its webview with it.
class WebViewController {
public:
    WebViewController() = default;
    explicit WebViewController(ICoreWebView2Controller* controller) noexcept : controller_(controller) {}
    WebViewController(WebViewController&&) noexcept = default;
    WebViewController& operator=(WebViewController&& other) noexcept {
        if (this != &other) {
            Close();
            controller_ = std::move(other.controller_);
        }
        return *this;
    }
    ~WebViewController() { Close(); }

    void Close() noexcept {
        if (controller_) controller_->Close();
        controller_.Reset();
    }

    [[nodiscard]] ICoreWebView2Controller* get() const noexcept { return controller_.Get(); }
    ICoreWebView2Controller* operator->() const noexcept { return controller_.Get(); }
    explicit operator bool() const noexcept { return controller_ != nullptr; }

private:
    ComPtr<ICoreWebView2Controller> controller_;
};

struct HistoryCopyJob; // see back_forward.cpp

// A tab's DevTools, docked in the tab's window beside its page or under it: a webview of
// their own, on the DevTools the debugging port serves for the page. See devtools_panel.cpp.
struct DockedDevTools {
    WebViewController controller;
    ComPtr<ICoreWebView2> webview;
    bool open = false;   // asked for: their side of the window is theirs while the tab is in view
    int serial = 0;      // which opening this is, for what comes late for an earlier one to be told by
    bool loaded = false; // their page has loaded, and can be told what to show
    // What they are told once it has: scripts for their page, in the order they were asked for.
    std::vector<std::wstring> waiting;
};

struct Tab {
    WebViewController controller;
    ComPtr<ICoreWebView2> webview;
    UniqueWindow button; // its button in the tab strip, a child of the window it is in
    std::wstring title;
    std::wstring pendingUrl; // where it is headed while it has no webview yet
    std::wstring lastUrl;    // the page its last finished navigation ended on
    UniqueIcon favicon;
    int id = 0;                      // stable id for async event routing
    bool creating = false;           // a controller is being created for this tab
    bool loading = false;            // a navigation is in progress
    bool typedNav = false;           // the next navigation to start was asked for by address in the address bar
    UINT64 typedNavId = 0;           // that navigation's id, once it has started; 0 when there is none
    double zoom = SiteZoom::kNormal; // the zoom factor this tab's webview was last given
    bool parked = false;             // closed and sent to kParkedUrl, and not yet brought back from there
    bool spinning = false;           // its button was showing a spinner at the spin timer's last tick
    bool crashed = false;            // the process showing its page died, and no page has loaded since
    bool playingAudio = false;       // its page is making sound; the tab shows a speaker
    bool muted = false;              // the user muted it, from the tab's menu; the speaker is crossed out
    bool selected = false;           // dragged and closed along with the others selected; the active tab always is
    // When it was last in view, by GetTickCount64: the tab in view of a window that is on the
    // screen. One left out of view long enough is put to sleep; see sleeping_tabs.cpp.
    ULONGLONG seenAt = 0;
    bool askingToSleep = false;      // its page is being asked whether anything was typed into it
    int openerId = 0;                // the tab whose page it was opened from, while that counts; see tab_order.h
    bool forClaude = false;          // opened by Claude Code's tools, or from a page of such a tab; see zoom.cpp
    std::uint32_t claudeSession = 0; // and the session's process, when it said; see claude_session.cpp
    // In a private window (BrowserWindow::isPrivate), as every tab of one is: its webview is
    // in the profile's private session, and nothing of it is kept; see CreateBrowserWindow.
    bool isPrivate = false;
    std::wstring menuLink;    // the link its page's own context menu was last opened on, if any
    ULONGLONG menuLinkAt = 0; // and when, by GetTickCount64
    // Opened by Ctrl+click on Back or Forward, and given a copy of the other tab's history
    // until the page it shows has started loading; see back_forward.cpp.
    std::shared_ptr<HistoryCopyJob> historyCopy;
    // The entries are being made, as stand-ins that nothing of the tab may show: not its
    // title, its address, the history or the zoom.
    bool copyingHistory = false;
    // The pages its Back and Forward lead to, with the one it is on shown, as last heard
    // from its webview, or as it was given them (GiveTabPages). What it is reopened with,
    // and what the session file keeps of it. Empty for a tab that hasn't been anywhere.
    HistoryCopy pages;
    DockedDevTools devtools; // its DevTools, while they are docked in its window

    // A busy tab shows a spinner in place of its page's icon.
    [[nodiscard]] bool busy() const noexcept { return loading || creating; }
    // Lets go of the webview. The tab itself stays, to be given a new one later.
    void CloseWebView() noexcept {
        devtools = {}; // they were the page's, and go with it
        controller.Close();
        webview.Reset();
        playingAudio = false;
        historyCopy.reset(); // a new webview starts over at pendingUrl
        copyingHistory = false;
    }
};

// A window closed whole. Ctrl+Shift+T brings its tabs back together, in a window of their
// own at the place it had.
struct ClosedWindow {
    RECT place{}; // on the screen, as it was when neither maximized nor minimized
    bool maximized = false;
    int activeIndex = 0; // the tab in view, by its place in the strip
};

// A closed tab keeps its webview for a while, and with it the pages Back and Forward
// lead to. The webview is put away on kParkedUrl, so the page it showed is unloaded
// as it would be by any other close, and reopening the tab is a step back from there.
// Without its webview, it is given its pages again (GiveTabPages).
struct ClosedTab {
    std::wstring url; // the page it was on
    std::wstring title;
    HistoryCopy pages; // where Back and Forward led, with that page shown; may be empty
    Tab tab;           // has no webview once that has been let go
    // The window it was closed with, when it went with the rest of the window's tabs, and
    // its place in that window's strip. The tabs of a window closed whole are side by
    // side in App::closedTabs, and come back together.
    std::shared_ptr<const ClosedWindow> window;
    int indexInWindow = 0;
};
inline constexpr std::size_t kClosedTabsKept = 10; // how many of the latest keep their webview
// How many closed tabs and windows can be brought back; a window closed whole counts as one.
inline constexpr std::size_t kClosedItemsKept = 25;
// The window a tab is taken out of with, when the whole window is being closed.
struct ClosedWith {
    std::shared_ptr<const ClosedWindow> window;
    int index = 0;
};

// ---- downloads ----

// A download a page started, followed until the user clears it from the list or Winamp
// exits. It belongs to the browser as a whole, not to the tab that started it, which may
// since have moved to another window or closed: every window's panel lists them all.
struct Download {
    int id = 0;                                       // stable id for routing the operation's events
    ComPtr<ICoreWebView2DownloadOperation> operation; // none once it is no longer followed
    ComPtr<IUnknown> source;                          // the webview it came from, known by its IUnknown
    EventRegistrationToken bytesToken{};
    EventRegistrationToken stateToken{};
    std::wstring path; // where the file is saved
    std::wstring name; // and its name there
    UniqueIcon icon;   // its type's
    DownloadPhase phase = DownloadPhase::InProgress;
    DownloadFailure failure = DownloadFailure::Unknown;
    bool canResume = false;
    bool paused = false;   // the user paused it, so its interruption is a pause, whatever the reason given
    bool fileGone = false; // finished, and the file isn't there any more
    std::int64_t received = 0;
    std::int64_t total = 0; // 0 or less when not known
    DownloadSpeed speed;
    // While it is held as a type of file that can harm the computer: WebView2's question,
    // and the deferral that holds it until the user keeps it or deletes it.
    ComPtr<ICoreWebView2SaveFileSecurityCheckStartingEventArgs> keepCheck;
    ComPtr<ICoreWebView2Deferral> keepDeferral;
};

// A window's downloads button and the panel it opens (downloads_panel.cpp). Neither ever
// takes the keyboard: the page keeps it, and with it the browser's shortcuts.
struct DownloadsUi {
    HWND button = nullptr; // on the toolbar, while there are downloads to list
    HWND panel = nullptr;
    bool autoOpened = false;   // the panel came up by itself, for a download that started, and goes by itself
    ULONGLONG closeAt = 0;     // when, by GetTickCount64, unless the mouse is on it by then
    ULONGLONG refreshedAt = 0; // when the panel was last redrawn to keep its speeds and times current
    BYTE mouseButtons = 0;     // the mouse buttons down at the last look, to see a click outside the panel
    bool buttonPressed = false;
    // The parts of the panel under the mouse, and that the mouse went down on: one of
    // downloads_panel.cpp's Part, and the download it belongs to, if any.
    int hotPart = 0, hotId = 0;
    int pressedPart = 0, pressedId = 0;
};

// A window's strip along the top of the DevTools in view, and the bar between them and the
// page (devtools_panel.cpp). Neither takes the keyboard.
struct DevToolsUi {
    HWND bar = nullptr;
    HWND splitter = nullptr;
    DevToolsButton hot = DevToolsButton::None;     // the strip's button under the mouse
    DevToolsButton pressed = DevToolsButton::None; // and the one the mouse went down on
    bool sizing = false;                           // the splitter is being dragged
    int grab = 0;                                  // and where it is held: how far the mouse is from the DevTools' edge
};

// ---- windows ----

// An ornament being moved or sized from outside, the way a window manager such as
// AltSnap does it to whichever window is under the cursor. The frame goes in the
// ornament's stead, but the window manager works out each step from where it last
// asked the ornament to be, not from where the ornament is.
struct OutsideDrag {
    HWND ornament = nullptr;
    RECT asked{};  // where the window manager last asked the ornament to be
    RECT wanted{}; // where that would take the frame, before its smallest size is held to
    RECT frame{};  // where the frame went
};

// The ironwork on a window's corners (ornament.cpp). The piece on the top right has
// the buttons that minimize, maximize and close the window; the others are plain.
struct Ornaments {
    // See-through around their pictures; the frame owns them. Top right first, then
    // top left, bottom right and bottom left.
    std::array<UniqueWindow, 4> windows;
    bool tucked = false;   // the first is inside the frame's corner, for want of screen to stick out over
    int hot = -1;          // the button under the cursor, if any
    int pressed = -1;      // the button the mouse went down on and has yet to let go of
    bool ownDoing = false; // being placed or painted by ornament.cpp, not moved from outside
    std::optional<OutsideDrag> outside;
};

struct BrowserWindow {
    HWND frame = nullptr;  // Winamp's embed frame: top-level, unless a skin has docked it
    HWND client = nullptr; // our window inside it, and the parent of every control below
    Ornaments ornaments;
    // Stands for Winamp's library window instead, App::library: Winamp's frame with the
    // ornaments on its corners, and nothing else of a browser window's. See library_frame.cpp.
    bool isLibrary = false;
    // A private window, as Ctrl+Shift+N opens: see CreateBrowserWindow. Its title says so.
    bool isPrivate = false;

    // The controls are children of `client` and go when it does.
    HWND tabBar = nullptr;
    HWND newTabButton = nullptr;
    HWND tabsLeftButton = nullptr, tabsRightButton = nullptr;
    int firstTab = 0; // the first tab the strip shows, once it has more than it can show
    // The width its tabs keep after one was closed with the mouse, until the mouse leaves
    // the strip; 0 when they have their own. See tab_drag.cpp.
    int keptTabWidth = 0;
    HWND backButton = nullptr, forwardButton = nullptr, reloadButton = nullptr, closeTabButton = nullptr;
    HWND urlEdit = nullptr;
    HWND bookmarkButton = nullptr; // the star, right of the address bar
    // What a tab says when the mouse rests on it: its page's whole title and its site, which
    // a narrow tab has no room for. See tab_menu.cpp.
    HWND tabTips = nullptr;
    std::wstring tabTipText; // what the last one said, kept for the control to read

    HWND suggestList = nullptr;
    bool suggestVisible = false;
    std::vector<Suggestion> suggestions; // the list's rows; Enter goes to the selected one
    std::wstring userText;               // the address bar's text as typed, with nothing suggested in it
    std::wstring inlineText;             // the completion shown selected after userText, if any
    bool typingChar = false;             // the address bar is taking a typed character right now

    bool editDirty = false;      // the address bar holds text the user is working on
    bool settingUrl = false;     // the address bar's text is being set by us, not typed
    bool urlClickSelect = false; // the click in progress is the one that focused the address bar

    DownloadsUi downloads;
    DevToolsUi devtools;
    HWND bitwardenButton = nullptr; // on the toolbar, while Bitwarden is there to be used
    bool bitwardenButtonPressed = false;

    std::vector<Tab> tabs;
    int active = -1;
    int anchorTabId = 0; // the tab Shift+click selects from: the last one clicked without Shift

    embedWindowState embedState{}; // Winamp keeps a pointer to this for as long as the frame lives
    std::optional<UINT> menuId;    // the command its entry in Winamp's menus sends
    GUID guid{};

    bool minimized = false; // the frame is minimized; its pages are told they are hidden
    bool closing = false;   // closed for good, and waiting for its turn to be destroyed
    // Its tabs once it is closed for good, until it is destroyed. Their webviews are
    // closed with it.
    std::vector<Tab> discardedTabs;

    bool isFullscreen = false;
    UniqueWindow fullscreenWindow; // dedicated borderless top-level window used while a tab is fullscreen
    int fullscreenTabId = -1;      // id of the tab whose controller is parented into it
};

// ---- tab drag (one at a time, shared across windows) ----

struct TabDrag {
    int tabId = 0;                  // 0 = no tab is being pressed/dragged
    HWND button = nullptr;          // the pressed tab button; holds mouse capture
    POINT grab{};                   // where inside the tab it was grabbed
    bool dragging = false;          // moved far enough to count as a drag, not a click
    bool collapseOnRelease = false; // pressed as one of several selected: let go without a drag, it is the only one
    std::vector<int> tabIds; // once dragging: the tabs going, the pressed one and the others selected, in strip order
    POINT ghostGrab{};       // where inside the ghost it is held
    UniqueWindow ghost;      // floating copy of the tabs that follows the cursor
    UniqueWindow marker;     // insertion mark shown in the tab strip under the cursor
};
struct TabDrop {
    int tabId = 0;           // the tab that was pressed, which is the active one wherever they land
    std::vector<int> tabIds; // all the tabs dropped, in strip order
    POINT point{};           // where they were let go, in screen coordinates
    POINT grab{};            // where inside the ghost they were held
};

// The mouse on the close button of a tab (tab_drag.cpp), by the tab's button.
struct CloseButtonMouse {
    HWND hot = nullptr;       // the cursor is over this one's close button
    HWND pressed = nullptr;   // the mouse went down on this one's close button, and has yet to let go
    bool pressedOver = false; // and is over it still, so that letting go closes the tab
};

// ---- extensions (extensions.cpp) ----

// The user's profile's extensions, as WebView2 has them, once a webview of the profile is
// up in the browser process there is now.
struct Extensions {
    ComPtr<ICoreWebView2Profile7> profile;
    ComPtr<ICoreWebView2BrowserExtension> bitwarden;
    ComPtr<ICoreWebView2BrowserExtension> tabHelper;
    UniqueIcon bitwardenIcon; // its picture, for its toolbar button
};

// ---- everything ----

struct App {
    HINSTANCE instance = nullptr;
    HWND winamp = nullptr;              // Winamp's main window
    const wchar_t* winampIni = nullptr; // winamp.ini, where the window's place is kept
    ULONG_PTR gdiplusToken = 0;
    Theme theme;
    // The skin the ornaments on the windows' corners have, once one has been loaded, and
    // its pictures at the size they are shown: premultiplied BGRA, top row first, by the
    // corner they go on, as they are before they are turned to it. See ornament.cpp.
    const CornerSkin* ornamentSkin = nullptr;
    std::array<std::vector<std::uint32_t>, 4> ornamentArt;

    // The WebView2 environment every webview is made from.
    ComPtr<ICoreWebView2Environment> environment;
    bool environmentPending = false;
    std::vector<std::move_only_function<void()>> environmentWaiters;
    bool shutDown = false; // the browser has been taken down for Winamp's exit
    bool reviving = false; // the browser process died and the webviews are about to be made again

    // The first window is the one Winamp's menu shows; it is never destroyed before exit.
    std::vector<std::unique_ptr<BrowserWindow>> windows;
    // Winamp's library window, while it wears the ornaments; see library_frame.cpp.
    std::unique_ptr<BrowserWindow> library;
    // Tab ids are unique across all windows, so a tab keeps its id (and its event
    // routing) when it is dragged from one window to another.
    int nextTabId = 1;

    TabDrag drag;
    CloseButtonMouse closeButtonMouse;
    std::optional<TabDrop> pendingDrop;     // set by the drop, taken by kMsgDropTab
    std::vector<std::wstring> pendingLinks; // from exodium_open.exe, taken by kMsgOpenLinks

    History history;
    Bookmarks bookmarks;
    std::wstring bookmarksNotice; // what the bookmarks page says at its top the next time it is made
    SiteZoom zoom;
    std::vector<ClosedTab> closedTabs;
    // How long a tab is left out of view before it is put to sleep, in milliseconds; from
    // winamp.ini's sleep_tabs_minutes at start-up. See sleeping_tabs.cpp.
    ULONGLONG sleepTabsAfterMs = 0;

    Settings settings;               // as Exodium's page in Winamp's Preferences has them; see settings_page.cpp
    std::vector<Download> downloads; // newest first
    int nextDownloadId = 1;
    // Webviews closed while downloads they started were still going, kept out of sight
    // until those are done; see KeepWebViewForDownloads.
    std::vector<Tab> downloadKeepers;

    Extensions extensions;

    UINT debuggingPort = 0; // the port tools drive the browser through; see webview.cpp. 0 for none
    // Where the DevTools are docked and how large, as the user left them; see devtools_panel.cpp.
    DevToolsPrefs devtoolsPrefs;

    // The Claude Code sessions with tabs open, by their process's id; see claude_session.cpp.
    struct ClaudeSession {
        HANDLE process = nullptr; // opened to wait on
        HANDLE wait = nullptr;    // the thread pool's wait for it to end
    };
    std::map<std::uint32_t, ClaudeSession> claudeSessions;
};
extern App g_app;

// ---- where the plugin keeps its files (paths.cpp) ----

[[nodiscard]] const std::filesystem::path& DataDir();
[[nodiscard]] std::filesystem::path WebViewProfileDir();
[[nodiscard]] std::filesystem::path HistoryFile();
// What has happened to the history since its file was last written; see history_store.cpp.
[[nodiscard]] std::filesystem::path HistoryJournalFile();
[[nodiscard]] std::filesystem::path BookmarksFile();
[[nodiscard]] std::filesystem::path LogFile();
[[nodiscard]] std::filesystem::path ZoomFile();
// The windows and tabs, kept from one run of Winamp to the next; see session.cpp.
[[nodiscard]] std::filesystem::path SessionFile();
// exodium_open.exe, which Windows runs for links and the Start menu's entry runs. It
// belongs beside the plugin, but may not have been copied there.
[[nodiscard]] std::filesystem::path OpenerProgram();

// ---- the log (log.cpp) ----

// Adds a line to exodium.log in the data folder, with the time. From any thread.
void LogLine(std::wstring_view text);
// The same for a call that failed: what was being done, and the code it failed with.
void LogFailure(std::wstring_view what, HRESULT result);

// ---- the history and the bookmarks on disk (history_store.cpp) ----

// On the first window's client, beside the session's timers.
inline constexpr UINT_PTR kHistorySaveTimer = 0x5E52;

// At start-up: reads the history file, and whatever a journal says happened after it was written.
void LoadHistory();
// Has what happens to the history added to its journal every couple of seconds from now on.
void StartSavingHistory();
// The timer's work. Says whether the timer was that one.
bool OnHistoryTimer(UINT_PTR timer);
// Adds what has happened to the history to its journal now; with `withTitles`, the titles
// pages were given as well, which are otherwise left for once a minute.
void SaveHistorySoon(bool withTitles);
// Writes the history file whole, as Winamp exits.
void SaveHistoryNow();
void LoadBookmarks();
void SaveBookmarks();

// ---- Exodium's entry in the Start menu (start_menu.cpp) ----

// The shortcut that is the entry, Exodium.lnk in the Start menu's Programs folder, as
// tools\register-browser.ps1 makes it too. Empty if Windows can't say where that is.
[[nodiscard]] std::filesystem::path StartMenuShortcut();
// Whether it is there. Nothing else keeps track: it can be made and removed from
// outside the plugin as well.
[[nodiscard]] bool HasStartMenuShortcut();
// Makes it, or makes it again, to run OpenerProgram(), which has to be there.
[[nodiscard]] HRESULT AddStartMenuShortcut();
// Removes it; there being none is no failure.
[[nodiscard]] HRESULT RemoveStartMenuShortcut();

// ---- settings and their page in Winamp's Preferences (settings_page.cpp) ----

// Where new windows and tabs open.
[[nodiscard]] const std::wstring& HomePage() noexcept;
// Reads App::settings from winamp.ini, at start-up.
void LoadSettings();
// Makes the browser follow App::settings as they are now. Called whenever they change;
// whatever has to happen at once when a setting changes goes here.
void ApplySettings();
// Lays every window's tab strip out again and repaints it, for a change in what its
// tabs show.
void RefreshTabStrips();
void AddPreferencesPage();
void RemovePreferencesPage();
// Opens Winamp's Preferences at Exodium's page.
void OpenPreferencesPage();

// ---- Ctrl+N in Winamp's own windows (winamp_key_hook.cpp) ----

// Watches Winamp's windows for Ctrl+N while Settings::ctrlNInWinamp asks for it, and
// stops when it doesn't.
void FollowWinampNewWindowKey();
// Stops for good, as Winamp exits.
void StopWinampNewWindowKey();

// ---- the black glow round Winamp's windows while Winamp is in front (glow.cpp) ----

// Shows the glow round what Settings::glow asks for, or stops, and goes on following it.
void FollowGlow();
// Stops for good, as Winamp exits.
void StopGlow();
// A see-through window of the browser's, a corner ornament, was given a picture: the glow
// goes round what is solid of it. Premultiplied BGRA, top row first, `width` wide.
void SetGlowOutline(HWND window, std::span<const std::uint32_t> picture, int width);
// The window is going.
void ForgetGlowOutline(HWND window);

// ---- Winamp's windows docked together come to the front together (docked_windows.cpp) ----

// Whether a window of Winamp's is one that docks with the others: its main window, the
// equalizer, the playlist editor, the video window, and its frames, the library and the
// browser's. Ornaments and dialogs go with the window they belong to. Safe on any thread.
[[nodiscard]] bool IsDockingWindow(HWND window, std::wstring_view className) noexcept;
// Watches Winamp's thread for its windows being activated, for the windows docked with the
// one activated to come up with it; and stops, as Winamp exits.
void StartRaisingDockedWindows();
void StopRaisingDockedWindows();
// Does it, when the first window's client gets the kMsgRaiseDocked the watching posts.
void RaiseDockedWindows();

// ---- the pages' sound in Winamp's visualizers (winamp_sound.cpp) ----

// Has Winamp play the pages' sound for its visualizers, or stop, as sound_rule.h says,
// soon. Called whenever that may have changed: a tab starting or stopping making sound or
// changing its title, Winamp starting, stopping or pausing, the setting changing.
void FollowPageSound();
// Does it, when the first window's client gets the kMsgFollowSound FollowPageSound posts.
void LookAtPageSound();

// ---- finding windows and tabs (browser_window.cpp) ----

[[nodiscard]] BrowserWindow* WindowFromClient(HWND client) noexcept;
// The window a control belongs to.
[[nodiscard]] BrowserWindow* WindowFromChild(HWND child) noexcept;
[[nodiscard]] int TabIndexById(const BrowserWindow& win, int tabId) noexcept;
[[nodiscard]] bool IsTabIndex(const BrowserWindow& win, int index) noexcept;
[[nodiscard]] Tab* ActiveTab(BrowserWindow& win) noexcept;

// Where a tab currently lives. Handlers look this up by id on every event instead of
// capturing it, so they keep working after the tab is dragged to another window.
struct TabLocation {
    BrowserWindow* window = nullptr;
    int index = -1;

    explicit operator bool() const noexcept { return window != nullptr; }
    [[nodiscard]] Tab& tab() const noexcept { return window->tabs[static_cast<std::size_t>(index)]; }
    [[nodiscard]] bool isActive() const noexcept { return index == window->active; }
};
[[nodiscard]] TabLocation FindTab(int tabId) noexcept;
// The window of the user's nearest the front, which is where a tab from elsewhere goes: one
// on show that is neither private nor Claude Code's. A minimized one only if no other is
// on show, and null if none is at all.
[[nodiscard]] BrowserWindow* FrontUserWindow();

// ---- windows (browser_window.cpp) ----

void RegisterWindowClasses();
void UnregisterWindowClasses();
// Adds a tab with an id of its own to a window's tabs, at index or else at the end.
Tab& AddTab(BrowserWindow& win, std::wstring_view title = {}, int index = -1);
// Makes a browser window. A private one (`isPrivate`, Ctrl+Shift+N) is for pages that are
// to leave nothing behind, as Chrome's Incognito windows and Edge's InPrivate ones are:
//
// - Its tabs' webviews are in the profile's private session, apart from the other windows'
//   cookies, sign-ins and site data, and gone once the last private tab is closed. A
//   runtime too old to have one gives its tabs no webview rather than an ordinary one.
// - Nothing of them goes into the history, the windows and tabs kept for the next start,
//   or the tabs Ctrl+Shift+T brings back; a zoom set in one isn't kept for its site.
// - Every tab opened in it is private too, and so is a window one of its pages opens. Tabs
//   aren't dragged between private windows and the others: one dropped on the other kind's
//   strip gets a window of its own kind instead.
// - Extensions aren't in them, Bitwarden included, and their tabs aren't put to sleep,
//   which would end the private session of a tab that was the last one awake.
//
// Files downloaded and bookmarks made in one are kept, as anywhere.
BrowserWindow* CreateBrowserWindow(bool show, const RECT* place = nullptr, bool forIncomingTab = false,
                                   bool isPrivate = false);
// What a page asked of a window it opens, in the page's own pixels. What it left out
// is ours to choose.
struct PopupRequest {
    std::optional<POINT> position; // of the window, on the screen
    std::optional<SIZE> size;      // of the page inside it
};
// A window for a page that asked for one of its own. Like one made for a dragged-out
// tab it starts empty and out of sight; the caller shows it once its tab is in.
BrowserWindow* CreatePopupWindow(const BrowserWindow& opener, const PopupRequest& request);
// Opens each link in App::pendingLinks: in a new tab of FrontUserWindow(), or in a window
// of its own if there is none, or the link is Claude Code's or no link at all.
void OpenPendingLinks();
// A window's place, moved onto the screen nearest it and made no larger than that screen,
// for a place kept from a time when the screens may have been otherwise.
[[nodiscard]] RECT KeptOnScreen(const RECT& place) noexcept;
// Closing a window closes its tabs; see the definition for what is left behind.
void CloseBrowserWindow(BrowserWindow& win);
void DestroyBrowserWindow(BrowserWindow& win);
// Takes the browser down for Winamp's exit: every webview and every window.
void ShutDownBrowser();
// Shortcuts that work wherever the keyboard is: a control, a page, or no window at all.
bool HandleGlobalShortcut(BrowserWindow* win, WPARAM key, Modifiers mods);
// Shortcuts that are the page's own while it has the keyboard, where the webview sees to
// them (and a page may take them for itself), and the browser's from its controls: F5 and
// Ctrl+R load the page in view again, and Ctrl+Shift+R and Ctrl+F5 past the cache.
bool HandleControlShortcut(BrowserWindow& win, WPARAM key, Modifiers mods);
void UpdateFrameTitle(BrowserWindow& win);

// ---- Winamp's frame around a window (frame.cpp) ----

void AttachFrame(BrowserWindow& win);
void DetachFrame(BrowserWindow& win);
// Whether a window is maximized. Winamp's border is out of sight then, but for its
// title bar, and the page runs to the screen's edges.
[[nodiscard]] bool IsMaximized(const BrowserWindow& win) noexcept;
// The edge or corner of a window's frame that sizes it from a point on the screen, as a hit
// test code (HTLEFT to HTBOTTOMRIGHT), or HTNOWHERE.
[[nodiscard]] LRESULT FrameSizingEdgeAt(const BrowserWindow& win, POINT screenPoint) noexcept;
// Makes a maximized window a normal one at the same place and size, for it to be sized from there.
void RestoreWhereItIs(const BrowserWindow& win) noexcept;
// Where a window is on the screen when it is neither maximized nor minimized: where it is,
// or where it goes back to.
[[nodiscard]] RECT NormalPlace(const BrowserWindow& win) noexcept;
// Where a window's client goes inside its frame, in the frame's coordinates.
[[nodiscard]] RECT ClientPlace(const BrowserWindow& win) noexcept;
// Holds a move or resize of a window's client to ClientPlace. Winamp sizes the client to
// fit inside its border whenever the frame changes size, maximized or not.
void KeepClientInPlace(const BrowserWindow& win, WINDOWPOS& position) noexcept;
// What the Jump to File Extra plugin does to every frame of Winamp's, undone: the moves it
// holds back from an active frame, and the width it forces on the next size.
void AllowOutsideMoves(HWND frame) noexcept;
void SpendJumpToFileWidth(HWND frame) noexcept;
// Where a window maximized on a monitor goes, and the limits that send a frame there.
[[nodiscard]] RECT MaximizedPlace(const MONITORINFO& monitor) noexcept;
void FitMaximized(HWND frame, MINMAXINFO& limits) noexcept;

// ---- Winamp's library window, which wears the ornaments too (library_frame.cpp) ----

// Puts the ornaments on the corners of the library's frame, which gen_ml makes as it starts.
void AttachLibraryOrnaments();
// Takes them off again, as Winamp exits.
void DetachLibraryOrnaments();
// What the library's ornament buttons do in the stead of the browser windows' ones. It has no
// taskbar button to be minimized to: Winamp is, and takes it along, as from its main window.
void MinimizeWinamp() noexcept;
// Closes the library as the close button on its title bar does.
void CloseLibrary() noexcept;

// ---- the ornaments on a window's corners (ornament.cpp) ----

void CreateOrnaments(BrowserWindow& win);
// Puts the ornaments on their frame's corners as those now are, or out of sight with the frame.
void UpdateOrnaments(BrowserWindow& win);
// Gives every window's corners the skin the settings ask for, if they have another.
void FollowCornerSkin();
// How much of each end of a window's tab strip its ornaments lie over.
struct StripCover {
    int left = 0;
    int right = 0;
};
[[nodiscard]] StripCover OrnamentStripCover(const BrowserWindow& win) noexcept;
LRESULT CALLBACK OrnamentProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

// ---- drawing (drawing.cpp) ----

// How a button is drawn.
struct ButtonLook {
    bool active = false;                // the tab in view
    bool inSelection = false;           // one of several tabs selected together
    const std::wstring* text = nullptr; // what it says, if not the button's own text
    // A tab's close button: kept however narrow the tab is, as the active tab's is; under
    // the mouse; held down with the mouse on it.
    bool closeAlways = false;
    bool closeHot = false;
    bool closePressed = false;
};
// Draws a toolbar button, or a tab when `tab` is given.
void DrawOwnerButton(const DRAWITEMSTRUCT& item, const Tab* tab, const ButtonLook& look);
// Draws the star that bookmarks the page in view: filled when it is a bookmark.
void DrawBookmarkButton(const DRAWITEMSTRUCT& item, bool marked);
void DrawSuggestion(const BrowserWindow& win, const DRAWITEMSTRUCT& item);

// ---- layout (layout.cpp) ----

// How a window's tabs fit its strip.
struct TabStripFit {
    int tabWidth = kTabWidth;
    int shown = 0;        // how many tabs the strip has room for
    bool scrolls = false; // not all of them, so it shows from BrowserWindow::firstTab on
};
[[nodiscard]] TabStripFit FitTabStrip(const BrowserWindow& win);
// Makes a window's controls. They are placed by the layout functions below.
void CreateControls(BrowserWindow& win);
void LayoutToolbarAndTabs(BrowserWindow& win);
// The address bar makes room for a toolbar button that came, or takes back the room one
// that went gave, without the rest of the window being laid out again: it may be being
// typed in.
void FitAddressBar(BrowserWindow& win);
void LayoutTabStrip(BrowserWindow& win);
void LayoutActiveWebView(BrowserWindow& win);
void HideAllWebViews(BrowserWindow& win);
// The bounds a tab's webview takes inside the window's client.
[[nodiscard]] RECT WebViewBounds(const BrowserWindow& win);
// Scrolls a tab strip by a number of tabs, to the left for a negative one.
void ScrollTabStrip(BrowserWindow& win, int by);
// Scrolls a tab strip no further than it takes to show the tab at index.
void ShowTabInStrip(BrowserWindow& win, int index);
void SetToolbarControlsVisible(BrowserWindow& win, bool visible);
// Sets a window's spinners turning. The timer stops itself once no tab is busy.
void StartTabSpinners(BrowserWindow& win);
void OnSpinTimer(BrowserWindow& win);

// ---- tabs (tabs.cpp) ----

void NewTab(BrowserWindow& win, std::wstring_view url = HomePage());
// Makes the tab at index the active one, and the only one selected.
void SwitchToTab(BrowserWindow& win, int index);
// Makes the tab at index the active one and leaves the selection as it is, which the
// caller sees has the tab in it.
void ActivateTab(BrowserWindow& win, int index);
// Ctrl+click: adds the tab at index to those selected, or takes it out of them. Says
// whether it is selected now.
bool ToggleTabSelected(BrowserWindow& win, int index);
// Shift+click, and with Ctrl as well (`add`): see SelectRange.
void SelectTabRange(BrowserWindow& win, int index, bool add);
// The ids of the selected tabs, in strip order.
[[nodiscard]] std::vector<int> SelectedTabIds(const BrowserWindow& win);
// Whether the tab at index is drawn as one of several selected.
[[nodiscard]] bool ShowsAsSelected(const BrowserWindow& win, int index) noexcept;
[[nodiscard]] int ActiveTabId(const BrowserWindow& win) noexcept; // 0 for none
// Where a tab opened from the page in the active tab goes; see LinkTabPlace.
[[nodiscard]] int LinkTabIndex(const BrowserWindow& win, bool foreground);
// Forgets which of a window's tabs were opened from which.
void ForgetOpeners(BrowserWindow& win);
// The user went from the tab fromTabId to the active one, by clicking a tab or from the
// keyboard: see SwitchForgetsOpeners.
void NoteUserSwitch(BrowserWindow& win, int fromTabId);
// The user typed an address, or a search, for the active tab: a task of its own, after
// which the tabs opened before it no longer count as opened from anywhere. Not in a new
// tab at the end of the strip that hasn't been anywhere yet, which is how a quick look
// at something starts, as Chrome has it.
void NoteTypedNavigation(BrowserWindow& win);
void NextTab(BrowserWindow& win);
void PrevTab(BrowserWindow& win);
// Ctrl+1 to Ctrl+8 go to the tab with that number in the strip, and Ctrl+9 to the last
// one, as in Chrome.
void SwitchToTabByNumber(BrowserWindow& win, int number);
// The page a tab is on, or headed for while it has shown none.
[[nodiscard]] std::wstring TabUrl(const Tab& tab);
void CloseTab(BrowserWindow& win, int index);
// Closes these tabs of a window, which take the window with them if they are all it has.
void CloseTabs(BrowserWindow& win, const std::vector<int>& tabIds);
// Closes the active tab, and the other selected tabs with it.
void CloseActiveTab(BrowserWindow& win);
// Takes the tab at index out of its window and closes it. The window is left for
// the caller to put right. Ctrl+Shift+T brings the tab back: with its webview, kept
// in the meantime, unless its window is closing for good, and otherwise with its pages.
// `with` says which closed window it goes with, when the whole window is closing.
void RemoveTab(BrowserWindow& win, int index, const ClosedWith& with = {});
// Lets go of the closed tabs and windows past the latest kClosedItemsKept.
void TrimClosedTabs();
// Moves tabs of src, live webviews included, side by side in the order they have there,
// to position insertAt of dst, counted without the moved tabs. dst may be src. The tab
// activeTabId, one of them, is the active one in dst, and they are its selected tabs.
void MoveTabs(BrowserWindow& src, const std::vector<int>& tabIds, BrowserWindow& dst, int insertAt, int activeTabId);
// Ctrl+Shift+T: the tab closed last, into `win`; or, if the last to close was a whole
// window, that window with all its tabs, in a window of its own at the place it had, or
// into `win` if all it has is a tab that hasn't been anywhere.
void ReopenLastClosedTab(BrowserWindow& win);
// Whether a tab hasn't been anywhere but the home page, and a window has that tab alone.
[[nodiscard]] bool IsBlankTab(const Tab& tab);
[[nodiscard]] bool IsBlankWindow(const BrowserWindow& win);
// Ctrl+H and Ctrl+Shift+O: the history page and the bookmarks page, each in the tab that
// already shows it if there is one.
void OpenHistory(BrowserWindow& win);
void OpenBookmarks(BrowserWindow& win);
void UpdateTabTitle(BrowserWindow& win, int index, std::wstring_view title);
// Lets go of a closed tab's webview. Reopening the tab then starts over at its address.
void DiscardClosedWebView(ClosedTab& closed);

// ---- a tab's menu, muting, and what it says under the mouse (tab_menu.cpp) ----

// What a tab's menu can do to it. A pick from the menu is posted to the window's client
// as a kMsgTabCommand, and the smoke test posts them the same way, by these numbers.
enum class TabCommand : int {
    kNewTabAfter = 1, // a new tab right after it
    kReload = 2,
    kDuplicate = 3, // another tab on the same page, with the same Back and Forward
    kToggleMute = 4,
    kClose = 5,
    kCloseOthers = 6,
    kCloseToRight = 7,
    kReopenClosed = 8, // the tab or window closed last, as Ctrl+Shift+T
};
// Does one of them to the tab `tabId` of a window. Where it makes sense for several tabs
// at once (reload, mute, close), a tab that is one of several selected takes the others
// along, as dragging it does.
void RunTabCommand(BrowserWindow& win, int tabId, TabCommand command);
// The menu a right click on a tab brings up, at a place on the screen.
void ShowTabMenu(BrowserWindow& win, int tabId, POINT screenPoint);
// Mutes a tab's page, or lets it be heard again, and keeps it so whatever becomes of its
// webview. Its speaker in the strip is crossed out meanwhile.
void SetTabUserMuted(Tab& tab, bool muted);
void CreateTabTips(BrowserWindow& win);
// Has a tab's button say its page's title and site under the mouse, until it is destroyed.
void AddTabTip(BrowserWindow& win, HWND button);
void RemoveTabTip(HWND button);
// For the client's WM_NOTIFY: gives the control its text, if that is what it asked for.
bool OnTabTipNeeded(BrowserWindow& win, NMHDR* header);

// ---- the star, Ctrl+D and the bookmarks page (bookmarks_ui.cpp) ----

// Bookmarks the page in a window's tab in view, or takes its bookmark away.
void ToggleBookmark(BrowserWindow& win);
// Shows whether the page in view is a bookmark: a window's star, or every window's.
void UpdateBookmarkButton(BrowserWindow& win);
void RefreshBookmarkButtons();
// The bookmarks page as it is now, and what it sends back when something on it is clicked.
[[nodiscard]] std::wstring BookmarksPageHtml();
void OnBookmarksPageMessage(ICoreWebView2* sender, std::wstring_view message);

// ---- tabs left out of view are put to sleep (sleeping_tabs.cpp) ----

// On the first window's client, beside the session's timers.
inline constexpr UINT_PTR kSleepTimer = 0x5E53;
// Looks every so often for tabs to put to sleep, from now on.
void StartSleepingTabs();
bool OnSleepTimer(UINT_PTR timer);
// Lets go of a tab's webview, and everything its page held with it, and leaves the tab as
// one is before it is first looked at: with its title, and the pages its Back and Forward
// lead to, which the webview it is given when it is next looked at makes its history
// again. The page loads afresh then.
void PutTabToSleep(Tab& tab);

// ---- address bar and suggestions (address_bar.cpp) ----

void CreateAddressBar(BrowserWindow& win);
// Gives a toolbar or tab button the keyboard handling every control shares.
void SubclassControl(HWND control);
void FocusAddressBar(BrowserWindow& win);
// Shows a page's address in the address bar.
void ShowPageUrl(BrowserWindow& win, std::wstring_view url);
// Shows the active tab's address, unless the user is in the middle of typing one.
void UpdateUrlBox(BrowserWindow& win);
[[nodiscard]] bool IsEditingUrl(const BrowserWindow& win) noexcept;
void UpdateReloadButton(BrowserWindow& win);
void HideSuggestions(BrowserWindow& win);
// The address bar's text was changed by the user.
void OnUrlEdited(BrowserWindow& win);
void NavigateFromUrlBox(BrowserWindow& win);

// ---- tab dragging (tab_drag.cpp) ----

void SubclassTabButton(HWND button);
// Ends the press/drag without dropping anything; also used to cancel one.
void EndTabDrag();
void DropDraggedTab(const TabDrop& drop);
LRESULT CALLBACK TabGhostProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

// ---- fullscreen video (fullscreen.cpp) ----

void EnterVideoFullscreen(BrowserWindow& win);
// The page left fullscreen: the webview goes back into its window.
void ExitVideoFullscreen(BrowserWindow& win);
// Ends fullscreen from our side (a shortcut, a closing tab) rather than the page's.
void LeaveVideoFullscreen(BrowserWindow& win);
LRESULT CALLBACK FullscreenProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

// ---- webviews (webview.cpp, webview_events.cpp) ----

// The address a webview is on; empty if it has none to give.
[[nodiscard]] std::wstring SourceOf(ICoreWebView2* webview);
void CreateWebViewForTab(BrowserWindow& owner, int tabId);
// Hooks a tab's freshly made webview up to the browser.
void WireTabEvents(int tabId);
// The page in the tab sourceTabId of win opened another: it gets a webview to load into,
// in a new tab, or in a window of its own if that is what the page asked for.
void OpenRequestedWindow(BrowserWindow& win, int sourceTabId, ICoreWebView2NewWindowRequestedEventArgs* args);
// Whether a tab's webview gives out sound. What the user asked of the tab is Tab::muted
// (SetTabUserMuted); this is how a tab put away is kept quiet as well.
void SetTabMuted(Tab& tab, bool muted);
// Gives a tab its speaker, or takes it away, by whether its page is making sound.
void UpdateTabAudio(Tab& tab);
// Loads the page in a tab again; with `pastCache`, without what the browser kept of it.
void ReloadTab(Tab& tab, bool pastCache = false);
void MoveFocusToWebView(Tab& tab);
// Tab pressed in the address bar or on a toolbar button: the keyboard goes on into the
// page in view, as it does in Chrome, to the first thing there that takes it, or with
// Shift+Tab (`backward`) to the last.
void TabIntoPage(BrowserWindow& win, bool backward);
// Makes a webview in the profile of that name, apart from the user's, as Claude's tabs and
// the docked DevTools have; with `isPrivate`, in that profile's private session, which
// keeps nothing. A runtime too old to have profiles makes none.
HRESULT CreateWebViewInProfile(HWND parent, const wchar_t* profileName, bool isPrivate,
                               ICoreWebView2CreateCoreWebView2ControllerCompletedHandler* done);
// Asks the pages of a webview's profile for their dark version, to go with the rest of the window.
void PreferDarkColorScheme(ICoreWebView2* webview);
// The browser process behind every webview has died; gives every tab a new one.
void ReviveWebViews();
// Asks the browser process to end, along with any window a tool opened for itself.
void CloseBrowserProcess();

// ---- Back and Forward (back_forward.cpp) ----

// Back, or Forward with `forward`, from the active tab: in the tab, or the page it leads
// to in a new tab or window that is given a copy of the tab's history.
void GoBackOrForward(BrowserWindow& win, bool forward, OpenIn where);
// The webview of a tab GoBackOrForward opened is up: it is given its history.
void StartHistoryCopy(int tabId);
// Gives a tab that has no webview yet the pages `pages` has, as a reopened or restored tab
// is given them: its webview makes them its history once it is up, and shows the shown
// one. Until then the tab has that page's title and address. Pages that are none leave it
// as it is.
void GiveTabPages(Tab& tab, HistoryCopy pages);
// Stops copying a tab's history wherever that has got to, as when the tab is closed.
void EndHistoryCopy(Tab& tab);
// Opens another tab on the page a tab is on, right after it and in view, with the same
// Back and Forward.
void DuplicateTab(BrowserWindow& win, int tabId);

// ---- zoom (zoom.cpp) ----

// The page in a tab has changed: it is shown at the zoom of the site it is on now.
void ApplySiteZoom(Tab& tab);
// The user zoomed the page in a tab.
void SetSiteZoom(Tab& tab, double factor);
void ResetSiteZoom(BrowserWindow& win);

// ---- the windows and tabs kept from one run of Winamp to the next (session.cpp) ----

// Timers on the first window's client, which others share (kSpinTimer, winamp_sound.cpp's,
// tab_drag.cpp's): the session file is kept up to date, and the windows brought back come
// up once Winamp has started.
inline constexpr UINT_PTR kSessionSaveTimer = 0x5E50;
inline constexpr UINT_PTR kSessionShowTimer = 0x5E51;

// At start-up, once the first window is there: brings back the windows and tabs that were
// open when Winamp last closed, if Settings::restoreWindows asks for it, or else keeps them
// for Ctrl+Shift+T; and the tabs and windows closed before that, for Ctrl+Shift+T as well.
// The windows come up once Winamp has finished starting.
void RestoreSession();
// The timers' work. Says whether the timer was one of these.
bool OnSessionTimer(UINT_PTR timer);
// Brings up the windows RestoreSession brought back, now, if they are still waiting for
// Winamp to finish starting.
void ShowRestoredWindows();
// Writes the session file, if the windows and tabs aren't as it has them. It is kept up to
// date every two seconds, and once more as Winamp exits.
void SaveSession();
// A window on the home page was asked for from outside, as the Start menu's entry does.
// Says whether the windows just brought back stand in for it instead: that is what
// started Winamp, and they came up with it.
bool WindowsRestoredForStart();
// The tab's history changed: its pages are asked of its webview again.
void NoteTabHistory(int tabId);
// The page the tab is on has a new title, which its pages take.
void NoteTabTitle(Tab& tab, std::wstring_view url, std::wstring_view title);

// Whether Winamp is still starting, and would bury a browser window shown now under its own
// (plugin.cpp).
[[nodiscard]] bool WinampIsStarting() noexcept;

// ---- Claude Code's windows (claude_session.cpp) ----

// Shows a window of Claude's without the keyboard, behind every other window.
void ShowWithoutFocus(BrowserWindow& win);
// Closes the session's tabs once its process has ended.
void WatchClaudeSession(std::uint32_t pid);
// For Winamp's main window, which hears of a session ending: whether `msg` was that.
bool OnClaudeSessionMessage(UINT msg, WPARAM wParam);
void StopWatchingClaudeSessions();

// ---- downloads (downloads.cpp) ----

// Has the downloads a tab's page starts followed in the downloads panel, in place of
// WebView2's own dialog. Claude's tabs are left to WebView2 and the tools driving them.
void WireTabDownloads(int tabId);
// For a tab whose webview is about to be closed. A download goes on once the webview that
// started it has closed, but nothing more is heard of it; so while it has downloads still
// going, its webview is taken from it and kept out of sight until they are done.
void KeepWebViewForDownloads(Tab& tab);
// Whether a download a tab's page started is still going.
[[nodiscard]] bool TabHasDownloadsGoing(const Tab& tab);
[[nodiscard]] Download* FindDownload(int downloadId) noexcept;
// What the panel tells of a download.
[[nodiscard]] DownloadFacts FactsOf(const Download& download);
// Looks again at whether each finished download's file is still where it was saved.
void CheckDownloadedFiles();
// What the panel's buttons do.
void OpenDownload(int downloadId);
void ShowDownloadInFolder(int downloadId);
void OpenDownloadsFolder();
void PauseDownload(int downloadId);
void ResumeDownload(int downloadId);
void CancelDownload(int downloadId);
// The answer to a download held as a type of file that can harm the computer.
void KeepDownload(int downloadId);
void DeleteDownload(int downloadId);
// Takes the finished downloads off the list.
void ClearFinishedDownloads();
// The browser process has died, and the downloads in progress with it.
void AbandonDownloads();
// Lets go of every download, for Winamp's exit.
void StopFollowingDownloads();

// ---- the downloads button and panel (downloads_panel.cpp) ----

void CreateDownloadsControls(BrowserWindow& win);
// Puts the downloads button at `right`, the end of the toolbar, if the window shows one,
// and the panel under it if it is open. Says where the address bar has to end.
[[nodiscard]] int LayoutDownloads(BrowserWindow& win, int right, int top, int height);
// `byItself`: for a download that has just started, rather than from the button. The
// panel then goes again after a while, and only comes up where the user can see it.
void ShowDownloadsPanel(BrowserWindow& win, bool byItself);
// Says whether it was open.
bool CloseDownloadsPanel(BrowserWindow& win);
// Keeps an open panel in front of a webview that has just been put in its window.
void RaiseDownloadsPanel(BrowserWindow& win);
// The toolbar button goes with the rest of the toolbar while a page is fullscreen.
void ShowDownloadsButton(BrowserWindow& win, bool visible);
enum class DownloadsChange {
    Progress, // bytes came in
    State,    // a download finished, failed, was paused or the like
    List,     // one came or went
};
// Brings every window's button and panel up to date.
void RefreshDownloadsUi(DownloadsChange change);
LRESULT CALLBACK DownloadsButtonProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
LRESULT CALLBACK DownloadsPanelProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

// ---- a tab's DevTools, docked in its window (devtools_panel.cpp) ----

// Whether DevTools can be docked: they are served by the debugging port, so only while
// winamp.ini names one. Without it they are the webview's own, in a window of theirs.
[[nodiscard]] bool CanDockDevTools() noexcept;
// Reads App::devtoolsPrefs from winamp.ini, at start-up.
void LoadDevToolsPrefs();
void CreateDevToolsControls(BrowserWindow& win);
// Shares `area`, the page's place in a window, between the page of the tab in view and its
// DevTools, if they are open: puts them, their strip and the splitter in their places, or
// out of sight. Says what is left for the page.
[[nodiscard]] RECT LayoutDevTools(BrowserWindow& win, Tab* tab, const RECT& area);
// Takes a window's DevTools out of sight, with their strip and splitter, while a page of
// it has the screen.
void HideDevTools(BrowserWindow& win);
// F12 and Ctrl+Shift+I open the DevTools of the tab in view, docked, or close them;
// Ctrl+Shift+J opens them on the console, and Ctrl+Shift+C to pick an element of the page
// with the mouse. Says whether the key was one of these and they can be docked.
bool HandleDevToolsShortcut(BrowserWindow& win, WPARAM key, Modifiers mods);
// What becomes of a tab's docked DevTools as they close.
enum class DevToolsClose : int {
    kPlain = 0,
    kRefocus = 1,   // the page has the keyboard back
    kOwnWindow = 2, // and they open in a window of their own instead, as the webview has them
};
void CloseDevTools(int tabId, DevToolsClose how);
// The same from the message loop, as kMsgCloseDevTools asks: for the DevTools as they were
// when it was posted, and not for ones opened since.
void OnDevToolsCloseMessage(WPARAM wParam, LPARAM lParam);
// The page's context menu is about to open: its "Inspect" is made to open the docked
// DevTools, on the element the menu was opened on.
void OfferDockedInspect(int tabId, ICoreWebView2ContextMenuRequestedEventArgs* args);
// A tab is going to another window, and its DevTools with it; and it has arrived.
void MoveDevToolsTo(Tab& tab, HWND client);
void DevToolsMoved(Tab& tab);
LRESULT CALLBACK DevToolsBarProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
LRESULT CALLBACK DevToolsSplitterProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

// ---- extensions in the user's profile (extensions.cpp) ----

// At start-up, before the browser process: puts a version of Bitwarden fetched last time in
// its place, and starts looking for newer ones in the Chrome Web Store.
void StartExtensions();
// Stops looking, as Winamp exits.
void StopExtensions();
// A webview of the user's profile is up: the profile is given the extensions the settings
// ask for, once in each browser process. While Bitwarden is on, that process is then kept
// up until Winamp exits, so that an unlocked vault stays unlocked with every window closed.
void LoadExtensions(ICoreWebView2* webview);
// The browser process has died, and the profile and its extensions with it.
void ForgetExtensions();
// Bitwarden was turned on or off in the settings.
void FollowExtensionSettings();
// For Winamp's main window, which hears when Bitwarden's first version is in place:
// whether `msg` was that.
bool OnExtensionsMessage(UINT msg);
// Whether Bitwarden is on, in the profile and running: its button shows.
[[nodiscard]] bool BitwardenReady() noexcept;
[[nodiscard]] bool TabHelperReady() noexcept;
// Bitwarden's state changed: its picture is loaded, and every window's button follows.
void RefreshBitwarden();
// Bitwarden's popup page, for the tab Bitwarden knows by `tabId`, if it is for one.
[[nodiscard]] std::wstring BitwardenPopupUrl(std::optional<int> tabId);
// Whether a window asked for is the one Bitwarden opens for unlocking the vault, as from
// the "Unlock account" in its menu in a page.
[[nodiscard]] bool IsBitwardenUnlockPopout(std::wstring_view url);
// The helper's page that tells the popup the number of the tab with this DevTools target id.
[[nodiscard]] std::wstring TabHelperUrl(std::wstring_view targetId);
[[nodiscard]] bool IsTabHelperPage(std::wstring_view url);

// ---- Bitwarden's button and popup (bitwarden_popup.cpp) ----

void CreateBitwardenButton(BrowserWindow& win);
// Puts the button at `right`, the end of the toolbar, if the window shows one. Says where
// the rest of the toolbar has to end.
[[nodiscard]] int LayoutBitwardenButton(BrowserWindow& win, int right, int top, int height);
// The button goes with the rest of the toolbar while a page is fullscreen.
void ShowBitwardenButton(BrowserWindow& win, bool visible);
// Every window's button comes or goes with Bitwarden, and is drawn again.
void RefreshBitwardenButtons();
// Opens the popup under the window's button, or closes it if it is open there. False if
// there is no Bitwarden to open.
bool ToggleBitwardenPopup(BrowserWindow& win);
// Opens the popup under the window's button to unlock the vault, in place of the window
// Bitwarden asked for, and closes it again once the vault is unlocked. False if there is
// no popup to open there.
bool OpenBitwardenPopupToUnlock(BrowserWindow& win);
// Closes the popup, wherever it is. `refocus`: the page in view has the keyboard back.
void CloseBitwardenPopup(bool refocus = false);
// Closes it if it is the window's, which is closing.
void CloseBitwardenPopupOf(const BrowserWindow& win);
LRESULT CALLBACK BitwardenButtonProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
LRESULT CALLBACK BitwardenPopupProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

} // namespace exodium
