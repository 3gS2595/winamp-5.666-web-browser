// The plugin as Winamp sees it: the entry point it looks up, and what it calls at
// start-up, from its preferences, and at exit.
#include "browser.h"

#include "embed_frame.h"
#include "gdiplus_headers.h"
#include "link_ipc.h"

#include <commctrl.h>
#include <gen.h>

#include <iterator>
#include <string_view>
#include <utility>

namespace exodium {

namespace {

constexpr UINT_PTR kWinampSubclassId = 1;

int __cdecl Init();
void __cdecl Config();
void __cdecl Quit();

char g_description[] = "Exodium";
winampGeneralPurposePlugin g_plugin{
    .version = GPPHDR_VER,
    .description = g_description,
    .init = Init,
    .config = Config,
    .quit = Quit,
};

bool g_comInitialized = false;

// How long after the plugin is loaded Winamp can still be starting. It is a limit, not
// a wait: Winamp can be set to keep its main window out of sight, so that may never come.
constexpr ULONGLONG kWinampStartMs = 10000;
// How long it has to have been done for: the windows it opens last follow the first.
constexpr ULONGLONG kWinampSettleMs = 500;
ULONGLONG g_loadedAt = 0;
ULONGLONG g_lastStartingAt = 0;

// Winamp's splash screen is the one dialog there is while it starts.
[[nodiscard]] bool SplashIsUp() noexcept {
    bool found = false;
    ::EnumThreadWindows(
        ::GetCurrentThreadId(),
        [](HWND window, LPARAM result) -> BOOL {
            wchar_t name[8];
            const int length = ::GetClassNameW(window, name, static_cast<int>(std::size(name)));
            if (!::IsWindowVisible(window) || std::wstring_view(name, length) != L"#32770") return TRUE;
            *reinterpret_cast<bool*>(result) = true;
            return FALSE;
        },
        reinterpret_cast<LPARAM>(&found));
    return found;
}

// Winamp's main window owns its other windows, so they all come forward whenever it
// does, past a browser window, which it doesn't own. It does at start-up, as it comes
// up and again as the splash screen goes. A window opened before then ends up behind
// all of them.
[[nodiscard]] bool WinampIsStarting() noexcept {
    const ULONGLONG now = ::GetTickCount64();
    if (now - g_loadedAt >= kWinampStartMs) return false;
    if (!::IsWindowVisible(g_app.winamp) || SplashIsUp()) g_lastStartingAt = now;
    return now - g_lastStartingAt < kWinampSettleMs;
}

// A link from exodium_open.exe, which keeps asking until a link is taken. It can arrive
// whenever Winamp's thread takes messages, which may be in the middle of anything, so
// it is opened from the message loop.
bool TakeLink(const COPYDATASTRUCT& copied) {
    if (g_app.shutDown || g_app.windows.empty() || WinampIsStarting()) return false;
    const auto* link = static_cast<const wchar_t*>(copied.lpData);
    g_app.pendingLinks.emplace_back(std::wstring_view(link, copied.cbData / sizeof(wchar_t)));
    ::PostMessageW(g_app.windows.front()->client, kMsgOpenLinks, 0, 0);
    return true;
}

// Sees every message on its way to Winamp's main window.
LRESULT CALLBACK WinampProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, UINT_PTR subclassId, DWORD_PTR) {
    // Winamp is closing. The browser goes now, ahead of everything Winamp does about it.
    if (msg == WM_DESTROY) ShutDownBrowser();
    if (msg == WM_COPYDATA) {
        const auto& copied = *reinterpret_cast<const COPYDATASTRUCT*>(lParam);
        if (copied.dwData == kOpenLinkCommand) return TakeLink(copied) ? kLinkTaken : 0;
    }
    if (OnClaudeSessionMessage(msg, wParam)) return 0;
    // Winamp started, stopped or paused: whether it plays the pages' sound may change.
    if (msg == WM_WA_IPC && lParam == IPC_CB_MISC && wParam == IPC_CB_MISC_STATUS) FollowPageSound();

    for (const auto& win : g_app.windows) {
        if (win->frame && win->menuId) embed::OnWinampMessage(win->frame, *win->menuId, msg, wParam);
    }
    if (msg == WM_NCDESTROY) ::RemoveWindowSubclass(hwnd, WinampProc, subclassId);
    return ::DefSubclassProc(hwnd, msg, wParam, lParam);
}

void CreateTheme() {
    Theme& theme = g_app.theme;
    theme.background.reset(::CreateSolidBrush(color::kBackground));
    theme.panel.reset(::CreateSolidBrush(color::kPanel));
    theme.activeTab.reset(::CreateSolidBrush(color::kActiveTab));
    theme.selectedTab.reset(::CreateSolidBrush(color::kSelectedTab));
    theme.border.reset(::CreateSolidBrush(color::kBorder));
    theme.selectedBorder.reset(::CreateSolidBrush(color::kSelectedBorder));
    theme.suggestSelected.reset(::CreateSolidBrush(color::kSuggestSelected));
    theme.font = static_cast<HFONT>(::GetStockObject(DEFAULT_GUI_FONT));
}

int __cdecl Init() {
    g_loadedAt = g_lastStartingAt = ::GetTickCount64();
    g_app.instance = g_plugin.hDllInstance;
    g_app.winamp = g_plugin.hwndParent;
    // The frame's place is kept in winamp.ini, which Winamp knows the way to.
    g_app.winampIni = reinterpret_cast<const wchar_t*>(::SendMessageW(g_app.winamp, WM_WA_IPC, 0, IPC_GETINIFILEW));
    g_app.debuggingPort = ::GetPrivateProfileIntW(L"Exodium", L"remote_debugging_port", 0, g_app.winampIni);
    LoadSettings();

    g_comInitialized = SUCCEEDED(::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED));
    const Gdiplus::GdiplusStartupInput gdiplusInput;
    Gdiplus::GdiplusStartup(&g_app.gdiplusToken, &gdiplusInput, nullptr);

    const INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_WIN95_CLASSES | ICC_STANDARD_CLASSES};
    ::InitCommonControlsEx(&controls);

    CreateTheme();
    RegisterWindowClasses();
    g_app.history.Load(HistoryFile());
    g_app.zoom.Load(ZoomFile());

    ::SetWindowSubclass(g_app.winamp, WinampProc, kWinampSubclassId, 0);
    AddPreferencesPage();
    CreateBrowserWindow(false); // the first window, hidden until shown from Winamp's menu
    return GEN_INIT_SUCCESS;
}

// Winamp's Configure button for the plugin, under Preferences > Plug-ins > General
// Purpose, goes to Exodium's own page there. The browser is shown from Winamp's menu.
void __cdecl Config() {
    OpenPreferencesPage();
}

void __cdecl Quit() {
    ShutDownBrowser(); // long done, unless Winamp went without destroying its main window
    RemovePreferencesPage();
    ::RemoveWindowSubclass(g_app.winamp, WinampProc, kWinampSubclassId);
    UnregisterWindowClasses();

    g_app.theme = {};
    if (g_app.gdiplusToken) Gdiplus::GdiplusShutdown(std::exchange(g_app.gdiplusToken, 0));
    if (std::exchange(g_comInitialized, false)) ::CoUninitialize();
}

} // namespace

} // namespace exodium

extern "C" __declspec(dllexport) winampGeneralPurposePlugin* winampGetGeneralPurposePlugin() {
    return &exodium::g_plugin;
}
