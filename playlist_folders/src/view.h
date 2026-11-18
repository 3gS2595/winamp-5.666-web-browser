// A folder's view in the library: what is in it, folders first, then its playlists, with
// their songs and length, and Play, Enqueue and New Folder below, as ml_playlists' own
// Playlists view has them.
#pragma once

#include "tree.h"

#include <windows.h>

#include <functional>
#include <string>
#include <vector>

namespace folders {

struct ViewHost {
    HINSTANCE instance = nullptr;
    Tree* tree = nullptr;
    // Plays (enqueue 0), enqueues (1), or does what the library is set to do on a double
    // click (-1), the playlists' files given.
    std::function<void(const std::vector<std::wstring>& files, int enqueue)> play;
    // The right-click menu for the rows picked.
    std::function<void(HWND owner, POINT screen, int folder, const std::vector<Row>& rows)> rowMenu;
};

HWND CreateFolderView(ViewHost& host, HWND parent, int folder);
// Fills the open views again: the one of `folder`, or (-1) all.
void RefreshFolderViews(int folder);

} // namespace folders
