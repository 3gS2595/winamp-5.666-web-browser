// The parts of Winamp's media library interface (gen_ml) the plugin uses, declared as
// Winamp's SDK declares them in gen_ml's ml.h and ml_ipc_0313.h. The numbers are
// Winamp's own; they have been the same since Winamp 5.5 (the navigation messages) and
// 5.66 (the plugin header version).
#pragma once

#include <windows.h>

#include <commctrl.h>

#include <cstdint>

namespace folders::ml {

// ---- The plugin header (ml.h)

inline constexpr int kHeaderVersion = 0x17; // MLHDR_VER, Winamp 5.66 and later: description is wchar_t*

struct Plugin {
    int version;
    const char* description; // a wchar_t* with kHeaderVersion
    int(__cdecl* init)();
    void(__cdecl* quit)();
    INT_PTR(__cdecl* MessageProc)(int message, INT_PTR param1, INT_PTR param2, INT_PTR param3);
    HWND hwndWinampParent;
    HWND hwndLibraryParent;
    HINSTANCE hDllInstance;
    void* service; // api_service, filled in by 5.66 and later
};

inline constexpr int kInitSuccess = 0;
inline constexpr int kInitFailure = 1;

// ---- Messages to plugins (MessageProc)

inline constexpr int kMsgTreeOnCreateView = 0x100; // p1 = item id, p2 = parent HWND; return the view's HWND
inline constexpr int kMsgTreeOnClick = 0x101;      // p1 = item id, p2 = action, p3 = HWND
inline constexpr int kMsgTreeOnDropTarget = 0x102; // p1 = item id, p2 = ML_TYPE_*, p3 = data or null; -1 no, 1 yes, 0 not mine
inline constexpr int kMsgTreeOnDrag = 0x103;       // p1 = item id, p2 = POINT*, p3 = int* type
inline constexpr int kMsgTreeOnDrop = 0x104;       // p1 = item id, p2 = POINT* (screen), p3 = id dropped after or 0
inline constexpr int kMsgTreeOnKeyDown = 0x105;    // p1 = item id, p2 = NMTVKEYDOWN*, p3 = tree HWND
inline constexpr int kMsgNavigationFirst = 0x106;
inline constexpr int kMsgNavOnMove = kMsgNavigationFirst + 1;
inline constexpr int kMsgNavOnDelete = kMsgNavigationFirst + 2;
inline constexpr int kMsgNavOnBeginTitleEdit = kMsgNavigationFirst + 3; // p1 = HNAVITEM; nonzero refuses
inline constexpr int kMsgNavOnEndTitleEdit = kMsgNavigationFirst + 4;   // p1 = HNAVITEM, p2 = new title or null
inline constexpr int kMsgNavOnDestroy = kMsgNavigationFirst + 8;
inline constexpr int kMsgNavContextMenu = kMsgNavigationFirst + 9; // p1 = HNAVITEM, p2 = tree HWND, p3 = MAKELONG(x, y), screen
inline constexpr int kMsgWriteConfig = 0x397;
inline constexpr int kMsgConfig = 0x400;
inline constexpr int kMsgNoConfig = 0x401;

inline constexpr int kActionRClick = 0, kActionDblClick = 1, kActionEnter = 2, kActionLClick = 3;

inline constexpr int kTypeFileNames = 1;   // double-null-terminated char*
inline constexpr int kTypePlaylist = 5;    // Playlist*
inline constexpr int kTypeFileNamesW = 7;  // double-null-terminated wchar_t*
inline constexpr int kTypePlaylists = 9;   // Playlist**, null-terminated
inline constexpr int kTypeTreeItem = 69;   // moves the item among its siblings, gen_ml's own handling

struct Playlist { // mlPlaylist
    const wchar_t* filename;
    const wchar_t* title;
    int numItems;
    int length;
};

// ---- Messages to the library (WM_ML_IPC, sent to hwndLibraryParent: wParam = param, lParam = command)

inline constexpr UINT WM_ML_IPC = WM_USER + 0x1000;

inline constexpr LPARAM kIpcDelTreeItem = 0x0103;    // param = item id
inline constexpr LPARAM kIpcGetCurTreeItem = 0x0104; // returns the selected item's id
inline constexpr LPARAM kIpcSetCurTreeItem = 0x0105; // param = item id
inline constexpr LPARAM kIpcTreeItemSetInfo = 0x125;
inline constexpr LPARAM kIpcTreeItemDelete = 0x127; // param = HNAVITEM
inline constexpr LPARAM kIpcTreeItemAdd = 0x126;
inline constexpr LPARAM kIpcTreeItemInsert = 0x130;
inline constexpr LPARAM kIpcTreeItemAddW = 0x133;
inline constexpr LPARAM kIpcTreeItemInsertW = 0x134; // MLTREEITEMW*: id = the item to insert after
inline constexpr LPARAM kIpcTreeItemSetInfoW = 0x135;
inline constexpr LPARAM kIpcHandleDrag = 0x0300;
inline constexpr LPARAM kIpcHandleDrop = 0x0301;
inline constexpr LPARAM kIpcSendToWinamp = 0x0302;
inline constexpr LPARAM kIpcSkinWindow = 0x1400;
inline constexpr LPARAM kIpcTrackSkinnedPopupEx = 0x1402;

inline constexpr LPARAM kIpcNavFirst = 0x1280;
inline constexpr LPARAM kIpcNavBeginUpdate = kIpcNavFirst + 1;
inline constexpr LPARAM kIpcNavDeleteItem = kIpcNavFirst + 2;
inline constexpr LPARAM kIpcNavEndUpdate = kIpcNavFirst + 3;
inline constexpr LPARAM kIpcNavFindItemById = kIpcNavFirst + 5;
inline constexpr LPARAM kIpcNavGetFirst = kIpcNavFirst + 8;
inline constexpr LPARAM kIpcNavGetSelection = kIpcNavFirst + 9;
inline constexpr LPARAM kIpcNavGetHwnd = kIpcNavFirst + 10;
inline constexpr LPARAM kIpcNavHitTest = kIpcNavFirst + 11;
inline constexpr LPARAM kIpcNavInsertItem = kIpcNavFirst + 12;
inline constexpr LPARAM kIpcNavEndEditTitle = kIpcNavFirst + 13;
inline constexpr LPARAM kIpcItemEnsureVisible = kIpcNavFirst + 40;
inline constexpr LPARAM kIpcItemExpand = kIpcNavFirst + 41;
inline constexpr LPARAM kIpcItemGetChild = kIpcNavFirst + 42;
inline constexpr LPARAM kIpcItemGetInfo = kIpcNavFirst + 45;
inline constexpr LPARAM kIpcItemGetNext = kIpcNavFirst + 46;
inline constexpr LPARAM kIpcItemGetParent = kIpcNavFirst + 48;
inline constexpr LPARAM kIpcItemGetPrevious = kIpcNavFirst + 49;
inline constexpr LPARAM kIpcItemGetRect = kIpcNavFirst + 50;
inline constexpr LPARAM kIpcItemGetState = kIpcNavFirst + 51;
inline constexpr LPARAM kIpcItemMove = kIpcNavFirst + 54;
inline constexpr LPARAM kIpcItemSelect = kIpcNavFirst + 55;
inline constexpr LPARAM kIpcItemSetInfo = kIpcNavFirst + 56;
inline constexpr LPARAM kIpcItemEditTitle = kIpcNavFirst + 58;

using HNAVITEM = void*;

struct NavItem { // NAVITEM
    int cbSize;
    HNAVITEM hItem;
    UINT mask;
    int id;
    LPWSTR pszText;
    int cchTextMax;
    LPWSTR pszInvariant;
    int cchInvariantMax;
    int iImage;
    int iSelectedImage;
    UINT state;
    UINT stateMask;
    UINT style;
    UINT styleMask;
    HFONT hFont;
    LPARAM lParam;
};

struct NavInsert { // NAVINSERTSTRUCT
    HNAVITEM hParent;
    HNAVITEM hInsertAfter;
    NavItem item;
};

struct NavHitTest { // NAVHITTEST
    POINT pt; // in the tree's client coordinates
    UINT flags;
    HNAVITEM hItem;
};

struct NavItemExpand { // NAVITEMEXPAND
    HNAVITEM hItem;
    UINT fCmdExpand;
};

struct NavItemGetRect { // NAVITEMGETRECT
    HNAVITEM hItem;
    RECT rc;
    BOOL fItem;
};

struct NavItemMove { // NAVITEMMOVE: within the same parent only
    HNAVITEM hItem;
    HNAVITEM hItemDest;
    BOOL fAfter;
};

inline constexpr UINT kNimfItemId = 0x0001, kNimfText = 0x0002, kNimfTextInvariant = 0x0004, kNimfImage = 0x0008,
                      kNimfImageSel = 0x0010, kNimfState = 0x0020, kNimfStyle = 0x0040, kNimfFont = 0x0080,
                      kNimfParam = 0x0100;
inline constexpr UINT kNisSelected = 0x0001, kNisExpanded = 0x0002; // states
inline constexpr UINT kNisHasChildren = 0x0001, kNisAllowChildMove = 0x0002, kNisAllowEdit = 0x0004; // styles
inline constexpr UINT kNavItemToggle = 0, kNavItemExpand = 1, kNavItemCollapse = 2;
inline constexpr UINT kNavHtOnItem = 0x0002, kNavHtOnItemButton = 0x0004, kNavHtOnItemIndent = 0x0010,
                      kNavHtOnItemRight = 0x0020;
inline const HNAVITEM kNciFirst = reinterpret_cast<HNAVITEM>(static_cast<ULONG_PTR>(-0x0FFFF));
inline const HNAVITEM kNciLast = nullptr;
inline constexpr WPARAM kNufLockNone = 0, kNufLockSelected = 1, kNufLockTop = 2;

struct TreeItemW { // MLTREEITEMW
    size_t size;
    UINT_PTR id;
    UINT_PTR parentId;
    wchar_t* title;
    size_t titleLen;
    BOOL hasChildren;
    int imageIndex;
};

struct TreeItemInfoW { // MLTREEITEMINFOW
    TreeItemW item;
    UINT mask;
    UINT_PTR handle;
};
inline constexpr UINT kMltiId = TVIF_PARAM, kMltiText = TVIF_TEXT;

struct SendToWinamp { // mlSendToWinampStruct
    int type;
    void* data;
    int enqueue; // bit 0: enqueue; bit 1: as said, not the library's default
};

struct DropItem { // mlDropItemStruct
    int type;
    void* data;
    int result;
    POINT p;
    int flags;
    char* name;
};

using MenuCustomizeProc = INT(CALLBACK*)(INT, HMENU, HDC, LPARAM, ULONG_PTR);
struct SkinnedPopup { // MLSKINNEDPOPUP
    INT cbSize;
    HMENU hmenu;
    UINT fuFlags;
    int x;
    int y;
    HWND hwnd;
    LPTPMPARAMS lptpm;
    void* hmlil;
    INT width;
    UINT skinStyle;
    MenuCustomizeProc customProc;
    ULONG_PTR customParam;
};
inline constexpr UINT kSmsUseSkinFont = 0x00000001;

struct SkinWindow { // MLSKINWINDOW
    HWND hwndToSkin;
    UINT skinType;
    UINT style;
};
inline constexpr UINT kSkinnedDialog = 0x0003, kSkinnedListView = 0x0005, kSkinnedButton = 0x0006,
                      kSkinnedStatic = 0x0009;
inline constexpr UINT kSwsUseSkinFont = 0x1, kSwsUseSkinColors = 0x2, kSwsUseSkinCursors = 0x4;
inline constexpr UINT kSwlvsFullRowSelect = 0x00010000, kSwlvsDoubleBuffer = 0x00020000,
                      kSwlvsAlternateItems = 0x00040000;
// Sent to a skinned window itself (WM_ML_IPC).
inline constexpr LPARAM kIpcSkinnedScrollShowHorzBar = 0x0011;
inline constexpr LPARAM kIpcSkinnedButtonGetIdealSize = 0x0013; // returns MAKELONG(width, height)

// Sent to a view by the library when something is dragged over it.
inline constexpr UINT WM_ML_CHILDIPC = WM_APP + 0x800;
inline constexpr LPARAM kChildIpcDropItem = 0x100;

} // namespace folders::ml
