// gen_playlist_covers: album covers above the songs of each saved playlist in Winamp's
// media library (Library > Playlists), the way its Audio view shows the library's
// albums above its songs. The playlist view stays ml_playlists' own, list and buttons
// included; the covers take the top of the place its list had.
#include "pane.h"
#include "settings.h"
#include "store.h"
#include "wasabi.h"

#include <windows.h>

#include <commctrl.h>
#include <objbase.h>

#include <gen.h>
#include <wa_dlg.h>
#include <wa_ipc.h>

#include <algorithm>
#include <string>
#include <vector>

namespace covers {

namespace {

int __cdecl Init();
void __cdecl Config();
void __cdecl Quit();

char g_description[] = "Playlist Covers v1.1";
winampGeneralPurposePlugin g_plugin{
    .version = GPPHDR_VER,
    .description = g_description,
    .init = Init,
    .config = Config,
    .quit = Quit,
};

constexpr wchar_t kHostClass[] = L"PlaylistCoversHost";
constexpr UINT kMsgLoaded = WM_APP + 1;  // the store's threads have results
constexpr UINT kMsgLookAt = WM_APP + 2;  // a window was made: is it a playlist view?
constexpr UINT_PTR kLookTimer = 1;       // a look round once Winamp has started
constexpr UINT kLookAfterStartMs = 1500;
constexpr UINT_PTR kWinampSubclassId = 0x50434F56;

struct Acquired {
    wasabi::ServiceFactory* factory = nullptr;
    void* object = nullptr;
};

HWND g_winamp = nullptr;
HWND g_host = nullptr; // a message-only window of the plug-in's own
std::wstring g_ini;
Settings g_settings;
Store g_store;
PaneHost g_paneHost;
std::vector<HWND> g_panes;
HWINEVENTHOOK g_hook = nullptr;
Acquired g_metadata, g_albumArt, g_memory, g_playlists;
bool g_comInitialized = false;
bool g_subclassed = false;
bool g_shutDown = false;

LRESULT Ipc(WPARAM wParam, LPARAM command) {
    return ::SendMessageW(g_winamp, WM_WA_IPC, wParam, command);
}

Acquired Acquire(wasabi::ServiceManager* services, const GUID& guid) {
    Acquired acquired;
    if (!services) return acquired;
    acquired.factory = services->ByGuid(guid);
    if (acquired.factory) acquired.object = acquired.factory->GetInterface();
    return acquired;
}

void Release(Acquired& acquired) {
    if (acquired.factory && acquired.object) acquired.factory->ReleaseInterface(acquired.object);
    acquired = {};
}

template <typename Action>
void ForEachPane(Action action) {
    std::erase_if(g_panes, [](HWND pane) { return !::IsWindow(pane); });
    for (const HWND pane : std::vector<HWND>(g_panes)) action(pane);
}

void LookAt(HWND window) {
    if (g_shutDown || !IsPlaylistView(window)) return;
    const HWND pane = AttachPane(window, g_paneHost);
    if (pane && std::ranges::find(g_panes, pane) == g_panes.end()) g_panes.push_back(pane);
}

// Every dialog of Winamp's thread: a playlist view open before the plug-in looked.
void LookEverywhere() {
    ::EnumThreadWindows(
        ::GetCurrentThreadId(),
        [](HWND top, LPARAM) -> BOOL {
            ::EnumChildWindows(
                top,
                [](HWND child, LPARAM) -> BOOL {
                    LookAt(child);
                    return TRUE;
                },
                0);
            return TRUE;
        },
        0);
}

// Every window made on Winamp's thread passes here; ml_playlists makes a view each time
// a playlist is picked in the library. It is looked at once it is done being made.
void CALLBACK WinEventProc(HWINEVENTHOOK, DWORD, HWND window, LONG object, LONG child, DWORD, DWORD) {
    if (object != OBJID_WINDOW || child != CHILDID_SELF || !window || !g_host) return;
    wchar_t name[8] = {};
    if (::GetClassNameW(window, name, static_cast<int>(std::size(name))) != 6 || std::wstring_view(name) != L"#32770") return;
    ::PostMessageW(g_host, kMsgLookAt, reinterpret_cast<WPARAM>(window), 0);
}

LRESULT CALLBACK HostProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    try {
        switch (msg) {
        case kMsgLoaded: {
            const Store::Taken taken = g_store.Take();
            ForEachPane([&taken](HWND pane) { PaneResults(pane, taken); });
            return 0;
        }
        case kMsgLookAt:
            LookAt(reinterpret_cast<HWND>(wParam));
            return 0;
        case WM_TIMER:
            if (wParam == kLookTimer) {
                ::KillTimer(hwnd, kLookTimer);
                LookEverywhere();
            }
            return 0;
        }
    } catch (...) {
        // Nothing may get out into Winamp.
    }
    return ::DefWindowProcW(hwnd, msg, wParam, lParam);
}

void ShutDown() {
    if (g_shutDown) return;
    g_shutDown = true;
    if (g_hook) ::UnhookWinEvent(g_hook);
    g_hook = nullptr;
    // None of the plug-in's windows may outlive it: Winamp lets go of its code after quit().
    ForEachPane([](HWND pane) { ::DestroyWindow(pane); });
    g_panes.clear();
    g_store.Stop();
    if (g_host) ::DestroyWindow(g_host);
    g_host = nullptr;
    Release(g_metadata);
    Release(g_albumArt);
    Release(g_memory);
    Release(g_playlists);
    UnregisterPaneClass(g_plugin.hDllInstance);
    ::UnregisterClassW(kHostClass, g_plugin.hDllInstance);
    WADlg_close();
}

// Sees every message on its way to Winamp's main window.
LRESULT CALLBACK WinampProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, UINT_PTR subclassId, DWORD_PTR) {
    switch (msg) {
    case WM_WA_IPC:
        if (lParam == IPC_CB_MISC && (wParam == IPC_CB_MISC_STATUS || wParam == IPC_CB_MISC_TITLE)) {
            ForEachPane(PanePlaybackChanged);
        } else if (lParam == IPC_SKIN_CHANGED) {
            WADlg_init(g_winamp);
            ForEachPane(PaneSkinChanged);
        }
        break;
    case WM_DESTROY:
        // Winamp is closing: the covers go now, while all of Winamp is still there.
        ShutDown();
        break;
    case WM_NCDESTROY:
        ::RemoveWindowSubclass(hwnd, WinampProc, subclassId);
        g_subclassed = false;
        break;
    }
    return ::DefSubclassProc(hwnd, msg, wParam, lParam);
}

int __cdecl Init() {
    g_winamp = g_plugin.hwndParent;
    if (const auto* ini = reinterpret_cast<const wchar_t*>(Ipc(0, IPC_GETINIFILEW))) g_ini = ini;
    g_settings = LoadSettings(g_ini);

    // The tags read before are kept beside Winamp's other plug-ins' settings, and the
    // library keeps its playlists' files below them.
    std::wstring plugins;
    if (const auto* folder = reinterpret_cast<const wchar_t*>(Ipc(0, IPC_GETINIDIRECTORYW))) {
        plugins = std::wstring(folder) + L"\\Plugins";
        ::CreateDirectoryW(plugins.c_str(), nullptr);
    }

    // For "Open folder", which goes through the shell.
    g_comInitialized = SUCCEEDED(::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED));
    const INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_LISTVIEW_CLASSES};
    ::InitCommonControlsEx(&controls);
    WADlg_init(g_winamp);

    const LRESULT serviceManager = Ipc(0, IPC_GET_API_SERVICE);
    auto* services = serviceManager > 1 ? reinterpret_cast<wasabi::ServiceManager*>(serviceManager) : nullptr;
    g_metadata = Acquire(services, wasabi::kMetadataGuid);
    g_albumArt = Acquire(services, wasabi::kAlbumArtGuid);
    g_memory = Acquire(services, wasabi::kMemoryManagerGuid);
    g_playlists = Acquire(services, wasabi::kPlaylistsGuid);

    WNDCLASSEXW hostClass{};
    hostClass.cbSize = sizeof(hostClass);
    hostClass.lpfnWndProc = HostProc;
    hostClass.hInstance = g_plugin.hDllInstance;
    hostClass.lpszClassName = kHostClass;
    ::RegisterClassExW(&hostClass);
    g_host = ::CreateWindowExW(0, kHostClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, g_plugin.hDllInstance, nullptr);
    if (!g_host || !RegisterPaneClass(g_plugin.hDllInstance)) return GEN_INIT_SUCCESS;

    Services loaderServices;
    loaderServices.metadata = static_cast<wasabi::Metadata*>(g_metadata.object);
    loaderServices.albumArt = static_cast<wasabi::AlbumArt*>(g_albumArt.object);
    loaderServices.memory = static_cast<wasabi::MemoryManager*>(g_memory.object);
    g_store.Start(g_host, kMsgLoaded, loaderServices, plugins.empty() ? L"" : plugins + L"\\playlist_covers_tags.txt");

    g_paneHost.winamp = g_winamp;
    g_paneHost.instance = g_plugin.hDllInstance;
    g_paneHost.settings = &g_settings;
    g_paneHost.iniFile = g_ini;
    g_paneHost.libraryFolder = plugins;
    g_paneHost.playlistsFolder = plugins + L"\\ml\\playlists";
    g_paneHost.store = &g_store;
    g_paneHost.playlists = static_cast<wasabi::Playlists*>(g_playlists.object);

    g_hook = ::SetWinEventHook(EVENT_OBJECT_CREATE, EVENT_OBJECT_CREATE, nullptr, WinEventProc, ::GetCurrentProcessId(),
                               ::GetCurrentThreadId(), WINEVENT_OUTOFCONTEXT);
    g_subclassed = ::SetWindowSubclass(g_winamp, WinampProc, kWinampSubclassId, 0) != FALSE;
    // The library may already show a playlist, and may yet as Winamp finishes starting.
    LookEverywhere();
    ::SetTimer(g_host, kLookTimer, kLookAfterStartMs, nullptr);
    return GEN_INIT_SUCCESS;
}

// Winamp's Configure button, in its plug-in preferences.
void __cdecl Config() {
    ::MessageBoxW(g_winamp,
                  L"Playlist Covers shows album covers above the songs of each saved playlist in the Media Library "
                  L"(Library > Playlists).\n\n"
                  L"Click a cover to pick its songs in the list, double-click it to play them. Drag the bar under the "
                  L"covers to give them more or less room; right-click them for their size and order.",
                  L"Playlist Covers", MB_OK | MB_ICONINFORMATION);
}

void __cdecl Quit() {
    ShutDown();
    if (g_subclassed && ::IsWindow(g_winamp)) ::RemoveWindowSubclass(g_winamp, WinampProc, kWinampSubclassId);
    g_subclassed = false;
    if (g_comInitialized) ::CoUninitialize();
    g_comInitialized = false;
}

} // namespace

} // namespace covers

extern "C" __declspec(dllexport) winampGeneralPurposePlugin* __cdecl winampGetGeneralPurposePlugin() {
    return &covers::g_plugin;
}
