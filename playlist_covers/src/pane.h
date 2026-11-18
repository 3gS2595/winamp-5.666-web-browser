// The covers in the media library's view of a saved playlist (ml_playlists): they take
// the top of the place its list had, and the list, with its buttons, keeps the rest. A
// click on a cover picks that album's songs in the list.
#pragma once

#include "settings.h"
#include "store.h"
#include "wasabi.h"

#include <windows.h>

#include <string>

namespace covers {

struct PaneHost {
    HWND winamp = nullptr;
    HINSTANCE instance = nullptr;
    Settings* settings = nullptr;
    std::wstring iniFile;
    std::wstring libraryFolder;   // Winamp's Plugins settings: gen_ml.ini, ml\views
    std::wstring playlistsFolder; // where the library keeps its playlists' files
    Store* store = nullptr;
    wasabi::Playlists* playlists = nullptr;
};

[[nodiscard]] bool RegisterPaneClass(HINSTANCE instance);
void UnregisterPaneClass(HINSTANCE instance);

// Whether `dialog` is the library's view of one saved playlist.
[[nodiscard]] bool IsPlaylistView(HWND dialog);
// Gives such a view its covers, unless it has them. The pane, or null.
HWND AttachPane(HWND dialog, const PaneHost& host);

// What the plug-in hears, passed on.
void PaneResults(HWND pane, const Store::Taken& taken);
void PanePlaybackChanged(HWND pane);
void PaneSkinChanged(HWND pane);

} // namespace covers
