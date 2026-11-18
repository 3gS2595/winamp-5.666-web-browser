// ml_playlist_folders: folders under Playlists in Winamp's media library, to put saved
// playlists in. The playlists stay ml_playlists' own (their files, views, menus and
// keys); the plugin only moves their items in the tree into folders of its own, which it
// keeps in Plugins\ml\playlist_folders.txt in Winamp's settings folder.
//
// ml_playlists is loaded before any other library plugin (gen_ml's preload list), so its
// items are there when this one starts. Its messages from the library go through
// PlaylistsProc first (its header's MessageProc is swapped for it), for drops into
// folders and a few more menu items; its messages to the library go through LibraryProc
// (the library window, subclassed), where Tree puts its items in their folders.
#include "ml_api.h"
#include "resource.h"
#include "tree.h"
#include "view.h"
#include "wasabi.h"

#include <windows.h>

#include <commctrl.h>
#include <shlwapi.h>
#include <windowsx.h>

#include <wa_ipc.h>

#include <algorithm>
#include <string>
#include <vector>

namespace folders {

namespace {

using namespace ml;

int __cdecl Init();
void __cdecl Quit();
INT_PTR __cdecl MessageProc(int message, INT_PTR param1, INT_PTR param2, INT_PTR param3);

wchar_t g_description[] = L"Playlist Folders v1.0";
Plugin g_plugin{
    .version = kHeaderVersion,
    .description = reinterpret_cast<const char*>(g_description),
    .init = Init,
    .quit = Quit,
    .MessageProc = MessageProc,
};

constexpr wchar_t kHostClass[] = L"PlaylistFoldersHost";
constexpr UINT kMsgDeferred = WM_APP + 1; // Tree's work after the library's message
constexpr UINT kMsgCommand = WM_APP + 2;  // a menu command: wParam command, lParam the item's id
constexpr UINT kMsgRefresh = WM_APP + 3;  // the folder views, filled again
constexpr UINT kMsgRename = WM_APP + 4;   // a folder's name, typed in the tree
constexpr UINT kMsgTestMenu = WM_APP + 5; // the test script: an item's right-click menu (lParam its id)
constexpr UINT_PTR kArrangeTimer = 1;     // trying again for Playlists, if it wasn't there at start
constexpr UINT_PTR kExpandedTimer = 2;    // a folder opened or shut: saved a little later
constexpr UINT_PTR kSubclassId = 0x50464C44;
constexpr ULONG_PTR kTestCopyData = 0x50465431; // 'PFT1': the test script's commands

constexpr int kIdNewPlaylist = 40059; // ml_playlists' "New Playlist..." (IDC_NEWPLAYLIST)
constexpr int kIdSortTitle = 40083;   // its "Sort playlists by title (A->Z)"

enum Command : int {
    kCmdPlay = 1,
    kCmdEnqueue,
    kCmdNewFolder,      // in the item's folder (Playlists: at the top)
    kCmdNewSubfolder,   // in the folder
    kCmdNewPlaylistHere,
    kCmdRename,
    kCmdDelete,
    kCmdOpen,
    kCmdNewFolderWithIt, // a new folder where the playlist is, the playlist moved into it
    kCmdMoveFirst = 0x20, // + the index in g_moveTargets
    kCmdLast = 0xFF,
};

Tree g_tree;
ViewHost g_viewHost;
HWND g_host = nullptr;
HWND g_library = nullptr;
Plugin* g_playlistsPlugin = nullptr;
INT_PTR(__cdecl* g_playlistsProc)(int, INT_PTR, INT_PTR, INT_PTR) = nullptr;
wasabi::ServiceFactory* g_playlistsFactory = nullptr;
wasabi::Playlists* g_playlists = nullptr;
int g_arrangeTries = 0;

// ml_playlists' right-click menu being shown, and for what (see LibraryProc).
struct MenuFor {
    Kind kind = Kind::Other;
    int itemId = 0;
} g_menuFor;
int g_autoPick = 0; // a command to take from ml_playlists' next menu, without showing it
std::vector<int> g_moveTargets; // "Move To" in the last menu: folder keys, 0 for Playlists
std::vector<Row> g_menuRows;    // the folder view's rows the last menu was for
struct PendingRename {
    int key = 0;
    std::wstring name;
} g_rename;

// ---- Small things

std::wstring IniDirectory(HWND winamp) {
    const auto* dir = reinterpret_cast<const wchar_t*>(::SendMessageW(winamp, WM_WA_IPC, 0, IPC_GETINIDIRECTORYW));
    return dir ? dir : L"";
}

std::wstring Join(const std::wstring& dir, const wchar_t* name) {
    std::wstring path = dir;
    if (!path.empty() && path.back() != L'\\') path += L'\\';
    return path + name;
}

// ml_playlists' folder for its files: Plugins\ml\playlists, unless Winamp keeps its
// playlists somewhere else (as ml_playlists' own init decides).
std::wstring PlaylistsDirectory(HWND winamp, const std::wstring& iniDir) {
    const auto* m3u = reinterpret_cast<const wchar_t*>(::SendMessageW(winamp, WM_WA_IPC, 0, IPC_GETM3UDIRECTORYW));
    if (m3u && *m3u && ::lstrcmpiW(m3u, iniDir.c_str()) != 0) return m3u;
    return Join(iniDir, L"Plugins\\ml\\playlists");
}

void Post(UINT message, WPARAM wParam = 0, LPARAM lParam = 0) {
    if (g_host) ::PostMessageW(g_host, message, wParam, lParam);
}

bool MenuHasId(HMENU menu, UINT id) {
    const int count = ::GetMenuItemCount(menu);
    for (int i = 0; i < count; ++i) {
        if (::GetMenuItemID(menu, i) == id) return true;
        if (const HMENU sub = ::GetSubMenu(menu, i); sub && MenuHasId(sub, id)) return true;
    }
    return false;
}

// A base for the plugin's ids that the menu doesn't use already.
UINT FreeBase(HMENU menu) {
    for (UINT base = 0x5F00; base < 0x7F00; base += 0x100) {
        bool used = false;
        for (UINT id = base; id <= base + kCmdLast && !used; ++id) used = MenuHasId(menu, id);
        if (!used) return base;
    }
    return 0;
}

POINT MenuPoint(HNAVITEM item, LPARAM packed) {
    POINT point{GET_X_LPARAM(packed), GET_Y_LPARAM(packed)};
    if (point.x == -1 && point.y == -1) {
        RECT rect{};
        if (g_tree.Lib().GetRect(item, rect)) {
            point = {rect.left + 2, rect.top + 2};
            ::ClientToScreen(g_tree.Lib().TreeWindow(), &point);
        } else {
            ::GetCursorPos(&point);
        }
    }
    return point;
}

void Play(const std::vector<std::wstring>& files, int enqueue) {
    if (files.empty()) return;
    std::wstring list;
    for (const std::wstring& file : files) {
        list += file;
        list += L'\0';
    }
    list += L'\0';
    SendToWinamp send{};
    send.type = kTypeFileNamesW;
    send.data = list.data();
    send.enqueue = enqueue < 0 ? 0 : (enqueue | 2);
    g_tree.Lib().Ipc(kIpcSendToWinamp, &send);
}

// "Move To": Playlists, then every folder (indented by depth), with the one it's in now
// checked; `moving` (a folder) and what is inside it left out.
HMENU MoveMenu(UINT base, int current, int moving) {
    const HMENU menu = ::CreatePopupMenu();
    g_moveTargets.clear();
    auto add = [&](int key, const std::wstring& text) {
        const auto index = static_cast<UINT>(g_moveTargets.size());
        if (kCmdMoveFirst + index > kCmdLast) return;
        g_moveTargets.push_back(key);
        UINT flags = MF_STRING;
        if (key == current) flags |= MF_CHECKED | MF_GRAYED;
        ::AppendMenuW(menu, flags, base + kCmdMoveFirst + index, text.c_str());
    };
    add(0, L"Playlists (no folder)");
    const Model& model = g_tree.Folders();
    for (int key : model.TreeOrder()) {
        if (moving && model.IsWithin(key, moving)) continue;
        const std::size_t depth = model.PathOf(key).size();
        std::wstring text(depth * 3, L' ');
        text += model.Find(key)->name;
        add(key, text);
    }
    return menu;
}

void AppendSubmenu(HMENU menu, HMENU sub, const wchar_t* text) {
    ::AppendMenuW(menu, MF_POPUP | MF_STRING, reinterpret_cast<UINT_PTR>(sub), text);
}

// ---- Commands

void NewPlaylistIn(int folder) {
    const HNAVITEM root = g_tree.Lib().FindById(kPlaylistsRootId);
    if (!root || !g_playlistsProc) return;
    // ml_playlists' own "New Playlist..." from its menu on Playlists, the menu not shown
    // (LibraryProc answers it); the playlist it makes goes into the folder (Tree).
    g_tree.SetNewPlaylistFolder(folder);
    const MenuFor saved = g_menuFor;
    g_menuFor = {Kind::Root, kPlaylistsRootId};
    g_autoPick = kIdNewPlaylist;
    g_playlistsProc(kMsgNavContextMenu, reinterpret_cast<INT_PTR>(root),
                    reinterpret_cast<INT_PTR>(g_tree.Lib().TreeWindow()), MAKELONG(-1, -1));
    g_autoPick = 0;
    g_menuFor = saved;
    g_tree.SetNewPlaylistFolder(0);
    g_tree.Lib().Expand(g_tree.ItemOfFolder(folder), kNavItemExpand);
}

void Run(int command, int itemId) {
    const Kind kind = g_tree.KindOf(itemId);
    const int folder = kind == Kind::Folder ? Tree::FolderKeyOf(itemId) : 0;
    if (command >= kCmdMoveFirst && command <= kCmdLast) {
        const auto index = static_cast<std::size_t>(command - kCmdMoveFirst);
        if (index >= g_moveTargets.size()) return;
        const int target = g_moveTargets[index];
        if (kind == Kind::Folder) g_tree.MoveFolder(folder, target);
        else if (kind == Kind::Playlist) g_tree.MovePlaylist(itemId, target);
        return;
    }
    switch (command) {
    case kCmdPlay:
    case kCmdEnqueue: Play(g_tree.FilesUnder(folder), command == kCmdEnqueue ? 1 : 0); break;
    case kCmdNewFolder: {
        const int container = kind == Kind::Playlist ? std::max(0, g_tree.ContainerOf(itemId)) : 0;
        g_tree.NewFolder(container, true);
        break;
    }
    case kCmdNewSubfolder: g_tree.NewFolder(folder, true); break;
    case kCmdNewPlaylistHere: NewPlaylistIn(folder); break;
    case kCmdRename: g_tree.BeginRename(folder); break;
    case kCmdDelete: g_tree.DeleteFolder(folder, g_library); break;
    case kCmdOpen: g_tree.Lib().Select(g_tree.Lib().FindById(itemId)); break;
    case kCmdNewFolderWithIt: {
        const int key = g_tree.NewFolder(std::max(0, g_tree.ContainerOf(itemId)), false);
        if (key && g_tree.MovePlaylist(itemId, key)) g_tree.BeginRename(key);
        break;
    }
    default: break;
    }
}

// The menu on a folder in the tree.
void FolderMenu(HNAVITEM item, HWND owner, LPARAM packed) {
    const int id = g_tree.Lib().IdOf(item);
    const int key = Tree::FolderKeyOf(id);
    const Folder* folder = g_tree.Folders().Find(key);
    if (!folder) return;
    const UINT base = 0;
    const HMENU menu = ::CreatePopupMenu();
    ::AppendMenuW(menu, MF_STRING, base + kCmdPlay, L"Play\tEnter");
    ::AppendMenuW(menu, MF_STRING, base + kCmdEnqueue, L"Enqueue\tShift+Enter");
    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(menu, MF_STRING, base + kCmdNewPlaylistHere, L"New Playlist Here...\tShift+Insert");
    ::AppendMenuW(menu, MF_STRING, base + kCmdNewSubfolder, L"New Folder\tCtrl+Shift+N");
    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendSubmenu(menu, MoveMenu(base, folder->parent, key), L"Move To");
    ::AppendMenuW(menu, MF_STRING, base + kCmdRename, L"Rename Folder\tF2");
    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(menu, MF_STRING, base + kCmdDelete, L"Delete Folder\tDel");
    if (g_tree.FilesUnder(key).empty()) {
        ::EnableMenuItem(menu, kCmdPlay, MF_BYCOMMAND | MF_GRAYED);
        ::EnableMenuItem(menu, kCmdEnqueue, MF_BYCOMMAND | MF_GRAYED);
    }
    const POINT point = MenuPoint(item, packed);
    const int chosen = g_tree.Lib().TrackPopup(menu, TPM_RIGHTBUTTON | TPM_LEFTBUTTON, point.x, point.y, owner);
    ::DestroyMenu(menu);
    if (chosen > 0) Run(chosen - static_cast<int>(base), id);
}

// The menu on rows of a folder's view.
void RowMenu(HWND owner, POINT screen, int folder, const std::vector<Row>& rows) {
    if (rows.empty()) return;
    g_menuRows = rows;
    const HMENU menu = ::CreatePopupMenu();
    ::AppendMenuW(menu, MF_STRING, kCmdPlay, L"Play\tEnter");
    ::AppendMenuW(menu, MF_STRING, kCmdEnqueue, L"Enqueue\tShift+Enter");
    if (rows.size() == 1) ::AppendMenuW(menu, MF_STRING, kCmdOpen, rows[0].folder ? L"Open Folder" : L"View Playlist");
    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    const int moving = rows.size() == 1 && rows[0].folder ? rows[0].key : 0;
    AppendSubmenu(menu, MoveMenu(0, folder, moving), L"Move To");
    if (rows.size() == 1 && rows[0].folder) {
        ::AppendMenuW(menu, MF_STRING, kCmdRename, L"Rename Folder\tF2");
        ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        ::AppendMenuW(menu, MF_STRING, kCmdDelete, L"Delete Folder");
    }
    const int chosen = g_tree.Lib().TrackPopup(menu, TPM_RIGHTBUTTON | TPM_LEFTBUTTON, screen.x, screen.y, owner);
    ::DestroyMenu(menu);
    if (chosen <= 0) return;
    if (chosen >= kCmdMoveFirst) {
        const auto index = static_cast<std::size_t>(chosen - kCmdMoveFirst);
        if (index >= g_moveTargets.size()) return;
        const int target = g_moveTargets[index];
        for (const Row& row : g_menuRows) {
            if (row.folder) {
                if (!(target != 0 && g_tree.Folders().IsWithin(target, row.key))) g_tree.MoveFolder(row.key, target);
            } else if (row.itemId) {
                g_tree.MovePlaylist(row.itemId, target);
            }
        }
        return;
    }
    switch (chosen) {
    case kCmdPlay:
    case kCmdEnqueue: {
        std::vector<std::wstring> files;
        for (const Row& row : g_menuRows) {
            if (row.folder) {
                const auto inner = g_tree.FilesUnder(row.key);
                files.insert(files.end(), inner.begin(), inner.end());
            } else if (!row.file.empty()) {
                files.push_back(row.file);
            }
        }
        Play(files, chosen == kCmdEnqueue ? 1 : 0);
        break;
    }
    case kCmdOpen: g_tree.Lib().Select(g_tree.Lib().FindById(g_menuRows[0].itemId)); break;
    case kCmdRename: g_tree.BeginRename(g_menuRows[0].key); break;
    case kCmdDelete: g_tree.DeleteFolder(g_menuRows[0].key, owner); break;
    default: break;
    }
}

bool Ctrl() { return (::GetKeyState(VK_CONTROL) & 0x8000) != 0; }
bool Shift() { return (::GetKeyState(VK_SHIFT) & 0x8000) != 0; }
bool Alt() { return (::GetKeyState(VK_MENU) & 0x8000) != 0; }

// ---- ml_playlists' messages from the library, seen first

INT_PTR __cdecl PlaylistsProc(int message, INT_PTR param1, INT_PTR param2, INT_PTR param3) {
    switch (message) {
    case kMsgNavContextMenu: {
        const int id = g_tree.Lib().IdOf(reinterpret_cast<HNAVITEM>(param1));
        const Kind kind = g_tree.KindOf(id);
        if (kind == Kind::Root || kind == Kind::Playlist) {
            const MenuFor saved = g_menuFor;
            g_menuFor = {kind, id};
            const INT_PTR result = g_playlistsProc(message, param1, param2, param3);
            g_menuFor = saved;
            return result;
        }
        break;
    }
    case kMsgTreeOnDrag:
        if (g_tree.KindOf(static_cast<int>(param1)) == Kind::Playlist) g_tree.NoteDragStart(static_cast<int>(param1));
        break;
    case kMsgTreeOnDrop:
        if (param2 && g_tree.KindOf(static_cast<int>(param1)) == Kind::Playlist &&
            g_tree.Drop(static_cast<int>(param1), *reinterpret_cast<POINT*>(param2))) {
            return 1;
        }
        break;
    case kMsgTreeOnDropTarget:
        // A folder dragged over Playlists or a playlist (ml_playlists' items).
        if (param2 == kFolderDragType) {
            const Kind kind = g_tree.KindOf(static_cast<int>(param1));
            if (kind == Kind::Root || kind == Kind::Playlist) return g_tree.TakesDrop(static_cast<int>(param1), kFolderDragType) ? 1 : -1;
        }
        break;
    case kMsgTreeOnKeyDown: {
        const auto* key = reinterpret_cast<NMTVKEYDOWN*>(param2);
        const Kind kind = g_tree.KindOf(static_cast<int>(param1));
        if (key && key->wVKey == 'N' && Ctrl() && Shift() && !Alt() && (kind == Kind::Root || kind == Kind::Playlist)) {
            Post(kMsgCommand, kCmdNewFolder, param1);
            return 1;
        }
        break;
    }
    case kMsgNavOnDestroy:
        g_tree.SyncExpanded();
        g_tree.Save();
        break;
    default: break;
    }
    return g_playlistsProc(message, param1, param2, param3);
}

// ---- The plugin's own messages from the library (its folders)

INT_PTR __cdecl MessageProc(int message, INT_PTR param1, INT_PTR param2, INT_PTR param3) {
    switch (message) {
    case kMsgNoConfig: return TRUE;
    case kMsgTreeOnCreateView:
        if (g_tree.KindOf(static_cast<int>(param1)) == Kind::Folder) {
            return reinterpret_cast<INT_PTR>(
                CreateFolderView(g_viewHost, reinterpret_cast<HWND>(param2), Tree::FolderKeyOf(static_cast<int>(param1))));
        }
        return 0;
    case kMsgTreeOnClick:
        if (g_tree.KindOf(static_cast<int>(param1)) == Kind::Folder && param2 == kActionEnter) {
            Play(g_tree.FilesUnder(Tree::FolderKeyOf(static_cast<int>(param1))), Shift() ? 1 : 0);
            return 1;
        }
        return 0;
    case kMsgTreeOnKeyDown: {
        const int id = static_cast<int>(param1);
        if (g_tree.KindOf(id) != Kind::Folder || !param2) return 0;
        const auto* key = reinterpret_cast<NMTVKEYDOWN*>(param2);
        const int folder = Tree::FolderKeyOf(id);
        if (key->wVKey == VK_F2 && !Ctrl() && !Shift()) {
            g_tree.BeginRename(folder);
            return 1;
        }
        if (key->wVKey == VK_DELETE && !Ctrl() && !Shift()) {
            Post(kMsgCommand, kCmdDelete, id);
            return 1;
        }
        if (key->wVKey == 'N' && Ctrl() && Shift() && !Alt()) {
            Post(kMsgCommand, kCmdNewSubfolder, id);
            return 1;
        }
        if (key->wVKey == VK_INSERT && Shift() && !Ctrl()) {
            Post(kMsgCommand, kCmdNewPlaylistHere, id);
            return 1;
        }
        return 0;
    }
    case kMsgTreeOnDrag:
        if (g_tree.KindOf(static_cast<int>(param1)) == Kind::Folder && param3) {
            *reinterpret_cast<int*>(param3) = kFolderDragType;
            g_tree.NoteDragStart(static_cast<int>(param1));
            return 1;
        }
        return 0;
    case kMsgTreeOnDrop:
        if (g_tree.KindOf(static_cast<int>(param1)) == Kind::Folder) {
            if (param2) g_tree.Drop(static_cast<int>(param1), *reinterpret_cast<POINT*>(param2));
            return 1;
        }
        return 0;
    case kMsgTreeOnDropTarget: {
        const int id = static_cast<int>(param1);
        if (g_tree.KindOf(id) != Kind::Folder) return 0;
        if (param3 && param2 == kTypePlaylist) {
            // A playlist ml_playlists hands on (it does for a drop it doesn't take).
            const auto* playlist = reinterpret_cast<const Playlist*>(param3);
            if (playlist->title) {
                const int item = g_tree.ItemIdOfName(playlist->title);
                if (item && g_tree.MovePlaylist(item, Tree::FolderKeyOf(id))) return 1;
            }
            return -1;
        }
        return g_tree.TakesDrop(id, static_cast<int>(param2)) ? 1 : -1;
    }
    case kMsgNavContextMenu: {
        const auto item = reinterpret_cast<HNAVITEM>(param1);
        if (g_tree.KindOfItem(item) != Kind::Folder) return 0;
        FolderMenu(item, reinterpret_cast<HWND>(param2), param3);
        return 1;
    }
    case kMsgNavOnEndTitleEdit: {
        const int id = g_tree.Lib().IdOf(reinterpret_cast<HNAVITEM>(param1));
        if (g_tree.KindOf(id) != Kind::Folder) return 0;
        if (param2) {
            // Taken after the edit is over; the tree keeps its old text till then.
            g_rename = {Tree::FolderKeyOf(id), reinterpret_cast<const wchar_t*>(param2)};
            Post(kMsgRename);
        }
        return 0;
    }
    case kMsgNavOnDestroy:
        g_tree.SyncExpanded();
        g_tree.Save();
        return 0;
    default: return 0;
    }
}

// ---- The library window: what's sent to it

LRESULT CALLBACK LibraryProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR) {
    if (message == WM_ML_IPC) {
        if (lParam == kIpcTrackSkinnedPopupEx && wParam && g_menuFor.kind != Kind::Other &&
            !Library::Own::Active()) {
            // ml_playlists' menu on Playlists or a playlist: the plugin's items added.
            auto* popup = reinterpret_cast<SkinnedPopup*>(wParam);
            const MenuFor menuFor = g_menuFor;
            g_menuFor = {};
            if (g_autoPick) {
                const int pick = g_autoPick;
                g_autoPick = 0;
                if (MenuHasId(popup->hmenu, static_cast<UINT>(pick))) return pick;
                return ::GetMenuItemID(popup->hmenu, 0) == UINT(-1) ? 0 : ::GetMenuItemID(popup->hmenu, 0);
            }
            const UINT base = FreeBase(popup->hmenu);
            const int countBefore = ::GetMenuItemCount(popup->hmenu);
            if (base && countBefore >= 0) {
                ::AppendMenuW(popup->hmenu, MF_SEPARATOR, 0, nullptr);
                if (menuFor.kind == Kind::Root) {
                    ::AppendMenuW(popup->hmenu, MF_STRING, base + kCmdNewFolder, L"New Folder\tCtrl+Shift+N");
                } else {
                    const int current = std::max(0, g_tree.ContainerOf(menuFor.itemId));
                    const HMENU move = MoveMenu(base, current, 0);
                    ::AppendMenuW(move, MF_SEPARATOR, 0, nullptr);
                    ::AppendMenuW(move, MF_STRING, base + kCmdNewFolderWithIt, L"New Folder...");
                    AppendSubmenu(popup->hmenu, move, L"Move To Folder");
                }
            }
            const LRESULT chosen = ::DefSubclassProc(window, message, wParam, lParam);
            if (base && countBefore >= 0) {
                // ml_playlists keeps its menus, so they go again.
                for (int i = ::GetMenuItemCount(popup->hmenu) - 1; i >= countBefore; --i) {
                    ::DeleteMenu(popup->hmenu, static_cast<UINT>(i), MF_BYPOSITION); // destroys "Move To" too
                }
                const auto id = static_cast<UINT>(chosen);
                if (id > base && id <= base + kCmdLast) {
                    Post(kMsgCommand, id - base, menuFor.itemId);
                    return 0;
                }
            }
            return chosen;
        }
        LRESULT result = 0;
        if (g_tree.BeforeIpc(lParam, wParam, result)) return result;
        result = ::DefSubclassProc(window, message, wParam, lParam);
        g_tree.AfterIpc(lParam, wParam, result);
        return result;
    }
    if (message == WM_NOTIFY) {
        const auto* header = reinterpret_cast<NMHDR*>(lParam);
        if (header && (header->code == TVN_ITEMEXPANDEDW || header->code == TVN_ITEMEXPANDEDA) && g_host) {
            ::SetTimer(g_host, kExpandedTimer, 1000, nullptr);
        }
    }
    if (message == WM_NCDESTROY) {
        g_tree.SyncExpanded();
        ::RemoveWindowSubclass(window, LibraryProc, kSubclassId);
    }
    return ::DefSubclassProc(window, message, wParam, lParam);
}

// ---- The test script's commands (tools\playlist-folders-test.ps1)

std::vector<std::wstring> SplitTabs(const std::wstring& text) {
    std::vector<std::wstring> parts;
    std::size_t start = 0;
    while (true) {
        const std::size_t end = text.find(L'\t', start);
        parts.push_back(text.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start));
        if (end == std::wstring::npos) break;
        start = end + 1;
    }
    return parts;
}

bool WriteUtf8(const std::wstring& path, const std::wstring& text) {
    const int size = ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string data(static_cast<std::size_t>(size), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), data.data(), size, nullptr, nullptr);
    const HANDLE file = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    ::WriteFile(file, data.data(), static_cast<DWORD>(data.size()), &written, nullptr);
    ::CloseHandle(file);
    return true;
}

// The middle of an item's line in the tree, on the screen.
bool CentreOf(HNAVITEM item, POINT& point) {
    RECT rect{};
    if (!item) return false;
    g_tree.Lib().EnsureVisible(item);
    if (!g_tree.Lib().GetRect(item, rect)) return false;
    point = {(rect.left + rect.right) / 2, (rect.top + rect.bottom) / 2};
    return ::ClientToScreen(g_tree.Lib().TreeWindow(), &point) != FALSE;
}

LRESULT TestCommand(const std::wstring& text) {
    const auto parts = SplitTabs(text);
    const std::wstring& verb = parts[0];
    auto arg = [&](std::size_t i) { return i < parts.size() ? parts[i] : std::wstring(); };
    auto number = [&](std::size_t i) { return _wtoi(arg(i).c_str()); };
    if (verb == L"dump") return WriteUtf8(arg(1), g_tree.Dump()) ? 1 : 0;
    if (verb == L"newfolder") {
        const int key = g_tree.NewFolder(number(1), false);
        if (key && !arg(2).empty()) g_tree.FinishRename(key, arg(2));
        return key;
    }
    if (verb == L"rename") {
        g_tree.FinishRename(number(1), arg(2));
        return 1;
    }
    if (verb == L"moveplaylist") return g_tree.MovePlaylist(g_tree.ItemIdOfName(arg(1)), number(2)) ? 1 : 0;
    if (verb == L"movefolder") return g_tree.MoveFolder(number(1), number(2)) ? 1 : 0;
    if (verb == L"deletefolder") {
        g_tree.DeleteFolder(number(1), nullptr, false);
        return 1;
    }
    if (verb == L"drop") {
        // As the library sends it: to ml_playlists (through PlaylistsProc) for a playlist,
        // to the plugin for a folder.
        const HNAVITEM dragged = g_tree.FindByText(arg(1));
        const HNAVITEM target = arg(2) == L"Playlists" ? g_tree.Lib().FindById(kPlaylistsRootId) : g_tree.FindByText(arg(2));
        POINT point{};
        if (!dragged || !CentreOf(target, point)) return -100;
        const int id = g_tree.Lib().IdOf(dragged);
        const int after = number(3) ? g_tree.Lib().IdOf(target) : 0;
        if (g_tree.KindOf(id) == Kind::Folder) return MessageProc(kMsgTreeOnDrop, id, reinterpret_cast<INT_PTR>(&point), after);
        g_tree.NoteDragStart(id);
        return g_playlistsPlugin->MessageProc(kMsgTreeOnDrop, id, reinterpret_cast<INT_PTR>(&point), after);
    }
    if (verb == L"select") {
        const HNAVITEM item = arg(1) == L"Playlists" ? g_tree.Lib().FindById(kPlaylistsRootId) : g_tree.FindByText(arg(1));
        return g_tree.Lib().Select(item) ? 1 : 0;
    }
    if (verb == L"expand") return g_tree.Lib().Expand(g_tree.ItemOfFolder(number(1)), number(2) ? kNavItemExpand : kNavItemCollapse) ? 1 : 0;
    if (verb == L"rows") return static_cast<LRESULT>(g_tree.RowsOf(number(1)).size());
    if (verb == L"files") return static_cast<LRESULT>(g_tree.FilesUnder(number(1)).size());
    if (verb == L"selected") return g_tree.Lib().IdOf(g_tree.Lib().Selection());
    if (verb == L"idof") {
        const HNAVITEM item = g_tree.FindByText(arg(1));
        return item ? g_tree.Lib().IdOf(item) : 0;
    }
    if (verb == L"save") {
        g_tree.SyncExpanded();
        g_tree.Save();
        return 1;
    }
    if (verb == L"beginrename") {
        g_tree.BeginRename(number(1));
        return 1;
    }
    // Posted: these show menus and ml_playlists' dialogs, which the script answers.
    if (verb == L"menu") {
        const HNAVITEM item = arg(1) == L"Playlists" ? g_tree.Lib().FindById(kPlaylistsRootId) : g_tree.FindByText(arg(1));
        if (!item) return 0;
        Post(kMsgTestMenu, 0, g_tree.Lib().IdOf(item));
        return 1;
    }
    if (verb == L"newplaylisthere") {
        Post(kMsgCommand, kCmdNewPlaylistHere, Tree::FolderIdOf(number(1)));
        return 1;
    }
    if (verb == L"sort") {
        // ml_playlists' "Sort playlists by title", from its own menu on Playlists.
        const HNAVITEM root = g_tree.Lib().FindById(kPlaylistsRootId);
        g_menuFor = {Kind::Root, kPlaylistsRootId};
        g_autoPick = kIdSortTitle;
        g_playlistsProc(kMsgNavContextMenu, reinterpret_cast<INT_PTR>(root),
                        reinterpret_cast<INT_PTR>(g_tree.Lib().TreeWindow()), MAKELONG(-1, -1));
        g_autoPick = 0;
        g_menuFor = {};
        return 1;
    }
    if (verb == L"renameplaylist" && g_playlists) {
        // As ml_playlists' Rename does it: through the library's list, which tells it.
        for (const PlaylistRecord& record : g_tree.Snapshot()) {
            if (record.name == arg(1)) {
                wasabi::PlaylistsLock lock(g_playlists);
                return g_playlists->RenamePlaylist(record.index, arg(2).c_str()) == 0 ? 1 : 0;
            }
        }
        return 0;
    }
    if (verb == L"playlistcount") return static_cast<LRESULT>(g_tree.Snapshot().size());
    return -1;
}

// ---- The plugin's own window (message-only)

LRESULT CALLBACK HostProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case kMsgDeferred: g_tree.Deferred(); return 0;
    case kMsgCommand: Run(static_cast<int>(wParam), static_cast<int>(lParam)); return 0;
    case kMsgRefresh: RefreshFolderViews(-1); return 0;
    case kMsgTestMenu: {
        // As the library asks for it (ml_local and the others before take no part).
        const int id = static_cast<int>(lParam);
        const HNAVITEM item = g_tree.Lib().FindById(id);
        const auto tree = reinterpret_cast<INT_PTR>(g_tree.Lib().TreeWindow());
        if (!item) return 0;
        if (g_tree.KindOf(id) == Kind::Folder) MessageProc(kMsgNavContextMenu, reinterpret_cast<INT_PTR>(item), tree, MAKELONG(-1, -1));
        else g_playlistsPlugin->MessageProc(kMsgNavContextMenu, reinterpret_cast<INT_PTR>(item), tree, MAKELONG(-1, -1));
        return 0;
    }
    case kMsgRename: {
        const PendingRename rename = std::move(g_rename);
        g_rename = {};
        if (rename.key) g_tree.FinishRename(rename.key, rename.name);
        return 0;
    }
    case WM_TIMER:
        if (wParam == kArrangeTimer) {
            if (g_tree.Arrange() || ++g_arrangeTries > 40) ::KillTimer(window, kArrangeTimer);
        } else if (wParam == kExpandedTimer) {
            ::KillTimer(window, kExpandedTimer);
            g_tree.SyncExpanded();
            g_tree.Save();
        }
        return 0;
    case WM_COPYDATA: {
        const auto* data = reinterpret_cast<const COPYDATASTRUCT*>(lParam);
        if (!data || data->dwData != kTestCopyData || !data->lpData) return 0;
        const std::wstring text(static_cast<const wchar_t*>(data->lpData), data->cbData / sizeof(wchar_t));
        return TestCommand(text);
    }
    default: return ::DefWindowProcW(window, message, wParam, lParam);
    }
}

// ---- Start and stop

int __cdecl Init() {
    const HWND winamp = g_plugin.hwndWinampParent;
    g_library = g_plugin.hwndLibraryParent;
    if (!winamp || !g_library) return kInitFailure;

    // ml_playlists, whose items go into the folders.
    const HMODULE playlistsModule = ::GetModuleHandleW(L"ml_playlists.dll");
    using GetPlugin = Plugin*(__cdecl*)();
    const auto getPlugin = playlistsModule ? reinterpret_cast<GetPlugin>(::GetProcAddress(playlistsModule, "winampGetMediaLibraryPlugin")) : nullptr;
    g_playlistsPlugin = getPlugin ? getPlugin() : nullptr;
    if (!g_playlistsPlugin || !g_playlistsPlugin->MessageProc) return kInitFailure;

    auto* services = static_cast<wasabi::ServiceManager*>(g_plugin.service);
    if (!services) services = reinterpret_cast<wasabi::ServiceManager*>(::SendMessageW(winamp, WM_WA_IPC, 0, IPC_GET_API_SERVICE));
    if (!services) return kInitFailure;
    g_playlistsFactory = services->ByGuid(wasabi::kPlaylistsGuid);
    g_playlists = g_playlistsFactory ? static_cast<wasabi::Playlists*>(g_playlistsFactory->GetInterface()) : nullptr;
    if (!g_playlists) return kInitFailure;

    WNDCLASSEXW windowClass{sizeof(windowClass)};
    windowClass.lpfnWndProc = HostProc;
    windowClass.hInstance = g_plugin.hDllInstance;
    windowClass.lpszClassName = kHostClass;
    ::RegisterClassExW(&windowClass);
    g_host = ::CreateWindowExW(0, kHostClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, g_plugin.hDllInstance, nullptr);

    g_viewHost.instance = g_plugin.hDllInstance;
    g_viewHost.tree = &g_tree;
    g_viewHost.play = Play;
    g_viewHost.rowMenu = RowMenu;

    g_playlistsProc = g_playlistsPlugin->MessageProc;
    g_playlistsPlugin->MessageProc = PlaylistsProc;
    ::SetWindowSubclass(g_library, LibraryProc, kSubclassId, 0);

    const std::wstring iniDir = IniDirectory(winamp);
    ::CreateDirectoryW(Join(iniDir, L"Plugins").c_str(), nullptr);
    ::CreateDirectoryW(Join(iniDir, L"Plugins\\ml").c_str(), nullptr);
    g_tree.SetChanged([](int) { Post(kMsgRefresh); });
    g_tree.Start(g_library, g_playlists, Join(iniDir, L"Plugins\\ml\\playlist_folders.txt"),
                 PlaylistsDirectory(winamp, iniDir), [] { Post(kMsgDeferred); });
    if (!g_tree.Started() && g_host) ::SetTimer(g_host, kArrangeTimer, 500, nullptr);
    return kInitSuccess;
}

void __cdecl Quit() {
    g_tree.Stop();
    if (g_library && ::IsWindow(g_library)) ::RemoveWindowSubclass(g_library, LibraryProc, kSubclassId);
    // Plugins quit in the reverse of their loading, so ml_playlists is still there.
    if (g_playlistsPlugin && g_playlistsPlugin->MessageProc == PlaylistsProc) g_playlistsPlugin->MessageProc = g_playlistsProc;
    if (g_playlistsFactory && g_playlists) g_playlistsFactory->ReleaseInterface(g_playlists);
    g_playlists = nullptr;
    if (g_host) ::DestroyWindow(g_host);
    g_host = nullptr;
    ::UnregisterClassW(kHostClass, g_plugin.hDllInstance);
}

} // namespace

} // namespace folders

extern "C" __declspec(dllexport) folders::ml::Plugin* winampGetMediaLibraryPlugin() {
    return &folders::g_plugin;
}
