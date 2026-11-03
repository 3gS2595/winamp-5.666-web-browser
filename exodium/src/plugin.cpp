// The plugin as Winamp sees it: the entry point it looks up, and what it calls at
// start-up, from its preferences, and at exit.
#include "browser.h"

#include "embed_frame.h"
#include "gdiplus_headers.h"

#include <commctrl.h>
#include <gen.h>

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

// Sees every message on its way to Winamp's main window.
LRESULT CALLBACK WinampProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, UINT_PTR subclassId, DWORD_PTR) {
    // Winamp is closing. The browser goes now, ahead of everything Winamp does about it.
    if (msg == WM_DESTROY) ShutDownBrowser();

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
    theme.border.reset(::CreateSolidBrush(color::kBorder));
    theme.suggestSelected.reset(::CreateSolidBrush(color::kSuggestSelected));
    theme.font = static_cast<HFONT>(::GetStockObject(DEFAULT_GUI_FONT));
}

int __cdecl Init() {
    g_app.instance = g_plugin.hDllInstance;
    g_app.winamp = g_plugin.hwndParent;
    // The frame's place is kept in winamp.ini, which Winamp knows the way to.
    g_app.winampIni = reinterpret_cast<const wchar_t*>(::SendMessageW(g_app.winamp, WM_WA_IPC, 0, IPC_GETINIFILEW));

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
    CreateBrowserWindow(false); // the first window, hidden until shown via menu/config
    return GEN_INIT_SUCCESS;
}

void __cdecl Config() {
    // Show (or create) the first window.
    if (g_app.windows.empty()) {
        CreateBrowserWindow(true);
        return;
    }
    const BrowserWindow& win = *g_app.windows.front();
    if (win.frame) ::ShowWindow(win.frame, SW_SHOWNA);
    if (win.client) ::PostMessageW(win.client, kMsgInitWebView, 0, 0);
}

void __cdecl Quit() {
    ShutDownBrowser(); // long done, unless Winamp went without destroying its main window
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
