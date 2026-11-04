// The browser's model and the functions that work on it.
//
// All state lives in one App: the browser windows with their tabs, and what the
// windows share (the WebView2 environment, history, zoom, closed tabs, the tab being
// dragged). Everything runs on Winamp's UI thread, so none of it is locked.
#pragma once

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
    kIdTabBar = 1100,
    kIdNewTab = 1101,
    kIdTabsLeft = 1102,
    kIdTabsRight = 1103,
    kIdSuggest = 1200,
    kIdDownloadsPanel = 1201,
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
// Where the client sits inside Winamp's frame: past its left border and title bar.
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

// Glyphs on the toolbar buttons.
inline constexpr wchar_t kGlyphBack[] = L"\u2190";    // leftwards arrow
inline constexpr wchar_t kGlyphForward[] = L"\u2192"; // rightwards arrow
inline constexpr wchar_t kGlyphReload[] = L"\u21BA";  // anticlockwise open circle arrow
inline constexpr wchar_t kGlyphStop[] = L"\u23F9";    // black square for stop

// ---- colours and shared GDI objects ----

namespace color {
inline constexpr COLORREF kBackground = RGB(0, 0, 0);
inline constexpr COLORREF kPanel = RGB(16, 16, 16);
inline constexpr COLORREF kActiveTab = RGB(32, 32, 32);
inline constexpr COLORREF kSelectedTab = RGB(26, 32, 46);     // one of several selected, other than the active one
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
    bool selected = false;           // dragged and closed along with the others selected; the active tab always is
    int openerId = 0;                // the tab whose page it was opened from, while that counts; see tab_order.h
    bool forClaude = false;          // opened by Claude Code's tools, or from a page of such a tab; see zoom.cpp
    std::uint32_t claudeSession = 0; // and the session's process, when it said; see claude_session.cpp
    std::wstring menuLink;           // the link its page's own context menu was last opened on, if any
    ULONGLONG menuLinkAt = 0;        // and when, by GetTickCount64
    // Opened by Ctrl+click on Back or Forward, and given a copy of the other tab's history
    // until the page it shows has started loading; see back_forward.cpp.
    std::shared_ptr<HistoryCopyJob> historyCopy;
    // The entries are being made, as stand-ins that nothing of the tab may show: not its
    // title, its address, the history or the zoom.
    bool copyingHistory = false;

    // A busy tab shows a spinner in place of its page's icon.
    [[nodiscard]] bool busy() const noexcept { return loading || creating; }
    // Lets go of the webview. The tab itself stays, to be given a new one later.
    void CloseWebView() noexcept {
        controller.Close();
        webview.Reset();
        playingAudio = false;
        historyCopy.reset(); // a new webview starts over at pendingUrl
        copyingHistory = false;
    }
};

// A closed tab keeps its webview for a while, and with it the pages Back and Forward
// lead to. The webview is put away on kParkedUrl, so the page it showed is unloaded
// as it would be by any other close, and reopening the tab is a step back from there.
struct ClosedTab {
    std::wstring url; // the page it was on
    Tab tab;          // has no webview once that has been let go; reopening then starts over at url
};
inline constexpr std::size_t kClosedTabsKept = 10; // how many of the latest keep their webview

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
    std::vector<int> tabIds;        // once dragging: the tabs going, the pressed one and the others selected, in strip order
    POINT ghostGrab{};              // where inside the ghost it is held
    UniqueWindow ghost;             // floating copy of the tabs that follows the cursor
    UniqueWindow marker;            // insertion mark shown in the tab strip under the cursor
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

// ---- everything ----

struct App {
    HINSTANCE instance = nullptr;
    HWND winamp = nullptr;              // Winamp's main window
    const wchar_t* winampIni = nullptr; // winamp.ini, where the window's place is kept
    ULONG_PTR gdiplusToken = 0;
    Theme theme;
    // The ornaments' pictures at the size they are shown, premultiplied BGRA, top row
    // first: the one with the buttons, and the plain one as it is before it is turned.
    std::vector<std::uint32_t> ornamentArt;
    std::vector<std::uint32_t> ornamentPlainArt;

    // The WebView2 environment every webview is made from.
    ComPtr<ICoreWebView2Environment> environment;
    bool environmentPending = false;
    std::vector<std::move_only_function<void()>> environmentWaiters;
    bool shutDown = false; // the browser has been taken down for Winamp's exit
    bool reviving = false; // the browser process died and the webviews are about to be made again

    // The first window is the one Winamp's menu shows; it is never destroyed before exit.
    std::vector<std::unique_ptr<BrowserWindow>> windows;
    // Tab ids are unique across all windows, so a tab keeps its id (and its event
    // routing) when it is dragged from one window to another.
    int nextTabId = 1;

    TabDrag drag;
    CloseButtonMouse closeButtonMouse;
    std::optional<TabDrop> pendingDrop;     // set by the drop, taken by kMsgDropTab
    std::vector<std::wstring> pendingLinks; // from exodium_open.exe, taken by kMsgOpenLinks

    History history;
    SiteZoom zoom;
    std::vector<ClosedTab> closedTabs;

    Settings settings; // as Exodium's page in Winamp's Preferences has them; see settings_page.cpp
    std::vector<Download> downloads; // newest first
    int nextDownloadId = 1;
    // Webviews closed while downloads they started were still going, kept out of sight
    // until those are done; see KeepWebViewForDownloads.
    std::vector<Tab> downloadKeepers;

    UINT debuggingPort = 0; // the port tools drive the browser through; see webview.cpp. 0 for none

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
[[nodiscard]] std::filesystem::path ZoomFile();
// exodium_open.exe, which Windows runs for links and the Start menu's entry runs. It
// belongs beside the plugin, but may not have been copied there.
[[nodiscard]] std::filesystem::path OpenerProgram();

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

// ---- the pages' sound in Winamp's visualizers (winamp_sound.cpp) ----

// Has Winamp play the pages' sound for its visualizers, or stop, as sound_rule.h says,
// soon. Called whenever that may have changed: a tab starting or stopping making sound or
// changing its title, Winamp starting, stopping or pausing, the setting changing.
void FollowPageSound();

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

// ---- windows (browser_window.cpp) ----

void RegisterWindowClasses();
void UnregisterWindowClasses();
// Adds a tab with an id of its own to a window's tabs, at index or else at the end.
Tab& AddTab(BrowserWindow& win, std::wstring_view title = {}, int index = -1);
BrowserWindow* CreateBrowserWindow(bool show, const RECT* place = nullptr, bool forIncomingTab = false);
// What a page asked of a window it opens, in the page's own pixels. What it left out
// is ours to choose.
struct PopupRequest {
    std::optional<POINT> position; // of the window, on the screen
    std::optional<SIZE> size;      // of the page inside it
};
// A window for a page that asked for one of its own. Like one made for a dragged-out
// tab it starts empty and out of sight; the caller shows it once its tab is in.
BrowserWindow* CreatePopupWindow(const BrowserWindow& opener, const PopupRequest& request);
// Gives each link in App::pendingLinks a window of its own.
void OpenPendingLinks();
// Closing a window closes its tabs; see the definition for what is left behind.
void CloseBrowserWindow(BrowserWindow& win);
void DestroyBrowserWindow(BrowserWindow& win);
// Takes the browser down for Winamp's exit: every webview and every window.
void ShutDownBrowser();
// Shortcuts that work wherever the keyboard is: a control, a page, or no window at all.
bool HandleGlobalShortcut(BrowserWindow* win, WPARAM key, Modifiers mods);
void UpdateFrameTitle(BrowserWindow& win);

// ---- Winamp's frame around a window (frame.cpp) ----

void AttachFrame(BrowserWindow& win);
void DetachFrame(BrowserWindow& win);

// ---- the ornaments on a window's corners (ornament.cpp) ----

void CreateOrnaments(BrowserWindow& win);
// Puts the ornaments on their frame's corners as those now are, or out of sight with the frame.
void UpdateOrnaments(BrowserWindow& win);
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
    bool active = false;      // the tab in view
    bool inSelection = false; // one of several tabs selected together
    const std::wstring* text = nullptr; // what it says, if not the button's own text
    // A tab's close button: kept however narrow the tab is, as the active tab's is; under
    // the mouse; held down with the mouse on it.
    bool closeAlways = false;
    bool closeHot = false;
    bool closePressed = false;
};
// Draws a toolbar button, or a tab when `tab` is given.
void DrawOwnerButton(const DRAWITEMSTRUCT& item, const Tab* tab, const ButtonLook& look);
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
void CloseTab(BrowserWindow& win, int index);
// Closes these tabs of a window, which take the window with them if they are all it has.
void CloseTabs(BrowserWindow& win, const std::vector<int>& tabIds);
// Closes the active tab, and the other selected tabs with it.
void CloseActiveTab(BrowserWindow& win);
// Takes the tab at index out of its window and closes it. The window is left for
// the caller to put right. Ctrl+Shift+T brings the tab back: with its webview, kept
// in the meantime, unless its window is closing for good.
void RemoveTab(BrowserWindow& win, int index);
// Moves tabs of src, live webviews included, side by side in the order they have there,
// to position insertAt of dst, counted without the moved tabs. dst may be src. The tab
// activeTabId, one of them, is the active one in dst, and they are its selected tabs.
void MoveTabs(BrowserWindow& src, const std::vector<int>& tabIds, BrowserWindow& dst, int insertAt, int activeTabId);
void ReopenLastClosedTab(BrowserWindow& win);
void OpenHistory(BrowserWindow& win);
void UpdateTabTitle(BrowserWindow& win, int index, std::wstring_view title);
// Lets go of a closed tab's webview. Reopening the tab then starts over at its address.
void DiscardClosedWebView(ClosedTab& closed);

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
void SetTabMuted(Tab& tab, bool muted);
// Gives a tab its speaker, or takes it away, by whether its page is making sound.
void UpdateTabAudio(Tab& tab);
void MoveFocusToWebView(Tab& tab);
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
// Stops copying a tab's history wherever that has got to, as when the tab is closed.
void EndHistoryCopy(Tab& tab);

// ---- zoom (zoom.cpp) ----

// The page in a tab has changed: it is shown at the zoom of the site it is on now.
void ApplySiteZoom(Tab& tab);
// The user zoomed the page in a tab.
void SetSiteZoom(Tab& tab, double factor);
void ResetSiteZoom(BrowserWindow& win);

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

} // namespace exodium
