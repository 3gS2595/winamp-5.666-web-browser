#include "view.h"

#include "ml_api.h"
#include "resource.h"

#include <commctrl.h>
#include <windowsx.h>

#include <algorithm>
#include <iterator>

namespace folders {

namespace {

using namespace ml;

constexpr LPARAM kIpcNavGetImageList = kIpcNavFirst + 7;
constexpr LPARAM kIpcImageListGetRealList = 0x1260 + 3;
constexpr LPARAM kIpcImageListGetRealIndex = 0x1260 + 4;
constexpr LPARAM kIpcImageListGetIndexFromTag = 0x1260 + 10;
constexpr LPARAM kIpcSkinWadlgGetFunc = 0x0600;
constexpr INT_PTR kTreeImageBranchCollapsed = 4; // MLTREEIMAGE_BRANCH_COLLAPSED: the tree's shut folder
constexpr int kColourItemBg = 0, kColourItemFg = 1; // WADLG_ITEMBG, WADLG_ITEMFG

struct RealIndex { // MLIMAGELISTREALINDEX
    int cbSize;
    void* hmlil;
    int mlilIndex;
    COLORREF rgbBk;
    COLORREF rgbFg;
};
struct ImageTag { // MLIMAGELISTTAG
    void* hmlil;
    int mlilIndex;
    INT_PTR nTag;
};

enum Column { kName, kSongs, kLength };

struct View {
    ViewHost* host = nullptr;
    HWND dialog = nullptr;
    HWND list = nullptr;
    int folder = 0;
    std::vector<Row> rows;
    int folderImage = -1, playlistImage = -1;
};

std::vector<View*> g_views;

View* ViewOf(HWND dialog) {
    return reinterpret_cast<View*>(::GetWindowLongPtrW(dialog, DWLP_USER));
}

std::wstring Duration(std::size_t seconds) {
    wchar_t text[32];
    if (seconds >= 3600) {
        swprintf_s(text, L"%zu:%02zu:%02zu", seconds / 3600, seconds / 60 % 60, seconds % 60);
    } else {
        swprintf_s(text, L"%zu:%02zu", seconds / 60, seconds % 60);
    }
    return text;
}

std::wstring Counted(std::size_t n, const wchar_t* one, const wchar_t* many) {
    return std::to_wstring(n) + L" " + (n == 1 ? one : many);
}

void Totals(Tree& tree, int folder, std::size_t& songs, std::size_t& seconds, std::size_t& playlists) {
    for (const Row& row : tree.RowsOf(folder)) {
        if (row.folder) {
            Totals(tree, row.key, songs, seconds, playlists);
        } else {
            songs += row.songs;
            seconds += row.seconds;
            ++playlists;
        }
    }
}

int Scale(HWND window, int pixels) {
    const UINT dpi = ::GetDpiForWindow(window);
    return ::MulDiv(pixels, dpi ? static_cast<int>(dpi) : 96, 96);
}

COLORREF SkinColour(Library& library, int which, COLORREF fallback) {
    using GetColour = int(__cdecl*)(int);
    const auto get = reinterpret_cast<GetColour>(library.Ipc(kIpcSkinWadlgGetFunc, WPARAM{1}));
    return get ? static_cast<COLORREF>(get(which)) : fallback;
}

// The tree's own pictures, in the list's colours: its shut folder, and ml_playlists'
// playlist.
void SetUpImages(View& view) {
    Library& library = view.host->tree->Lib();
    void* images = reinterpret_cast<void*>(library.Ipc(kIpcNavGetImageList, WPARAM{0}));
    if (!images) return;
    const auto real = reinterpret_cast<HIMAGELIST>(library.Ipc(kIpcImageListGetRealList, images));
    if (!real) return;
    const COLORREF background = SkinColour(library, kColourItemBg, ::GetSysColor(COLOR_WINDOW));
    const COLORREF text = SkinColour(library, kColourItemFg, ::GetSysColor(COLOR_WINDOWTEXT));
    auto realIndex = [&](int index) {
        if (index < 0) return -1;
        RealIndex request{sizeof(RealIndex), images, index, background, text};
        return static_cast<int>(library.Ipc(kIpcImageListGetRealIndex, &request));
    };
    ImageTag tag{images, -1, kTreeImageBranchCollapsed};
    view.folderImage = library.Ipc(kIpcImageListGetIndexFromTag, &tag) ? realIndex(tag.mlilIndex) : -1;
    view.playlistImage = realIndex(view.host->tree->PlaylistImage());
    ListView_SetImageList(view.list, real, LVSIL_SMALL);
}

void SizeColumns(View& view) {
    RECT client{};
    ::GetClientRect(view.list, &client);
    const int songs = Scale(view.list, 60), length = Scale(view.list, 75);
    ListView_SetColumnWidth(view.list, kSongs, songs);
    ListView_SetColumnWidth(view.list, kLength, length);
    const int name = std::max(Scale(view.list, 60), static_cast<int>(client.right) - songs - length);
    ListView_SetColumnWidth(view.list, kName, name);
}

void Fill(View& view) {
    Tree& tree = *view.host->tree;
    view.rows = tree.RowsOf(view.folder);
    std::vector<int> picked;
    for (int i = -1; (i = ListView_GetNextItem(view.list, i, LVNI_SELECTED)) != -1;) picked.push_back(i);
    std::vector<int> pickedIds;
    for (int i : picked) {
        LVITEMW item{LVIF_PARAM, i};
        if (ListView_GetItem(view.list, &item)) pickedIds.push_back(static_cast<int>(item.lParam));
    }

    ::SendMessageW(view.list, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(view.list);
    std::size_t songs = 0, seconds = 0, playlists = 0, folders = 0;
    for (std::size_t i = 0; i < view.rows.size(); ++i) {
        Row& row = view.rows[i];
        std::size_t rowSongs = row.songs, rowSeconds = row.seconds, inner = 0;
        if (row.folder) {
            rowSongs = rowSeconds = 0;
            Totals(tree, row.key, rowSongs, rowSeconds, inner);
            row.songs = rowSongs;
            row.seconds = rowSeconds;
            ++folders;
            playlists += inner;
        } else {
            ++playlists;
        }
        songs += rowSongs;
        seconds += rowSeconds;
        LVITEMW item{};
        item.mask = LVIF_TEXT | LVIF_IMAGE | LVIF_PARAM;
        item.iItem = static_cast<int>(i);
        item.pszText = row.name.data();
        item.iImage = row.folder ? view.folderImage : view.playlistImage;
        item.lParam = row.itemId;
        const int at = ListView_InsertItem(view.list, &item);
        std::wstring count = std::to_wstring(rowSongs), length = Duration(rowSeconds);
        ListView_SetItemText(view.list, at, kSongs, count.data());
        ListView_SetItemText(view.list, at, kLength, length.data());
        if (std::find(pickedIds.begin(), pickedIds.end(), row.itemId) != pickedIds.end()) {
            ListView_SetItemState(view.list, at, LVIS_SELECTED, LVIS_SELECTED);
        }
    }
    ::SendMessageW(view.list, WM_SETREDRAW, TRUE, 0);
    ::InvalidateRect(view.list, nullptr, TRUE);

    std::wstring status;
    if (folders) status += Counted(folders, L"folder", L"folders") + L", ";
    status += Counted(playlists, L"playlist", L"playlists");
    status += L", " + Counted(songs, L"song", L"songs") + L" [" + Duration(seconds) + L"]";
    if (view.rows.empty()) status = L"Empty folder. Drag playlists here, or right-click it for New Playlist Here.";
    ::SetDlgItemTextW(view.dialog, IDC_STATUS, status.c_str());
}

std::vector<Row> PickedRows(View& view) {
    std::vector<Row> rows;
    for (int i = -1; (i = ListView_GetNextItem(view.list, i, LVNI_SELECTED)) != -1;) {
        if (i < static_cast<int>(view.rows.size())) rows.push_back(view.rows[static_cast<std::size_t>(i)]);
    }
    return rows;
}

std::vector<std::wstring> FilesOf(View& view, const std::vector<Row>& rows) {
    std::vector<std::wstring> files;
    for (const Row& row : rows) {
        if (row.folder) {
            const auto inner = view.host->tree->FilesUnder(row.key);
            files.insert(files.end(), inner.begin(), inner.end());
        } else if (!row.file.empty()) {
            files.push_back(row.file);
        }
    }
    return files;
}

// The picked rows, or the whole folder with none picked.
void PlayPicked(View& view, int enqueue) {
    std::vector<Row> rows = PickedRows(view);
    const auto files = rows.empty() ? view.host->tree->FilesUnder(view.folder) : FilesOf(view, rows);
    if (!files.empty()) view.host->play(files, enqueue);
}

void Layout(View& view) {
    RECT client{};
    ::GetClientRect(view.dialog, &client);
    if (client.right <= client.left || client.bottom <= client.top) return;
    const HWND dialog = view.dialog;
    if (client.right > Scale(dialog, 4)) client.right -= Scale(dialog, 4);

    const HWND play = ::GetDlgItem(dialog, IDC_PLAY);
    wchar_t text[128] = {};
    ::GetWindowTextW(play, text, static_cast<int>(std::size(text)));
    const auto ideal = static_cast<DWORD>(::SendMessageW(play, WM_ML_IPC, reinterpret_cast<WPARAM>(text),
                                                         kIpcSkinnedButtonGetIdealSize));
    const int height = HIWORD(ideal) ? HIWORD(ideal) : Scale(dialog, 17);
    const int top = client.bottom - height;
    int left = client.left + Scale(dialog, 1);

    HDWP positions = ::BeginDeferWindowPos(5);
    for (int id : {IDC_PLAY, IDC_ENQUEUE, IDC_NEWFOLDER}) {
        const HWND button = ::GetDlgItem(dialog, id);
        ::GetWindowTextW(button, text, static_cast<int>(std::size(text)));
        const auto size = static_cast<DWORD>(::SendMessageW(button, WM_ML_IPC, reinterpret_cast<WPARAM>(text),
                                                            kIpcSkinnedButtonGetIdealSize));
        const int width = (LOWORD(size) ? LOWORD(size) : Scale(dialog, 50)) + Scale(dialog, 6);
        const bool fits = client.right - left > width;
        positions = ::DeferWindowPos(positions, button, nullptr, left, top, width, height,
                                     SWP_NOZORDER | SWP_NOACTIVATE | (fits ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
        if (fits) left += width + Scale(dialog, 4);
    }
    const HWND status = ::GetDlgItem(dialog, IDC_STATUS);
    const int statusLeft = left + Scale(dialog, 4);
    positions = ::DeferWindowPos(positions, status, nullptr, statusLeft, top,
                                 std::max(0, static_cast<int>(client.right) - statusLeft), height,
                                 SWP_NOZORDER | SWP_NOACTIVATE |
                                     (client.right - statusLeft > Scale(dialog, 20) ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
    const int listBottom = top - Scale(dialog, 3);
    positions = ::DeferWindowPos(positions, view.list, nullptr, client.left + Scale(dialog, 1),
                                 client.top + Scale(dialog, 1), client.right - client.left,
                                 std::max(0, listBottom - static_cast<int>(client.top) - Scale(dialog, 2)),
                                 SWP_NOZORDER | SWP_NOACTIVATE);
    if (positions) ::EndDeferWindowPos(positions);
    SizeColumns(view);
    ::InvalidateRect(dialog, nullptr, TRUE);
}

void Skin(View& view) {
    Library& library = view.host->tree->Lib();
    const UINT common = kSwsUseSkinColors | kSwsUseSkinCursors | kSwsUseSkinFont;
    library.Skin(view.dialog, kSkinnedDialog, common);
    library.Skin(view.list, kSkinnedListView, kSwlvsFullRowSelect | kSwlvsDoubleBuffer | kSwlvsAlternateItems | common);
    ::SendMessageW(view.list, WM_ML_IPC, FALSE, kIpcSkinnedScrollShowHorzBar);
    for (int id : {IDC_PLAY, IDC_ENQUEUE, IDC_NEWFOLDER}) {
        library.Skin(::GetDlgItem(view.dialog, id), kSkinnedButton, common);
    }
    library.Skin(::GetDlgItem(view.dialog, IDC_STATUS), kSkinnedStatic, common);
}

void InitView(View& view) {
    view.list = ::GetDlgItem(view.dialog, IDC_LIST);
    Skin(view);
    ListView_SetExtendedListViewStyleEx(view.list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_INFOTIP,
                                        LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_INFOTIP);
    struct {
        const wchar_t* title;
        int format;
    } columns[] = {{L"Name", LVCFMT_LEFT}, {L"Songs", LVCFMT_RIGHT}, {L"Length", LVCFMT_RIGHT}};
    for (int i = 0; i < static_cast<int>(std::size(columns)); ++i) {
        LVCOLUMNW column{};
        column.mask = LVCF_TEXT | LVCF_FMT | LVCF_WIDTH;
        column.fmt = columns[i].format;
        column.cx = 80;
        column.pszText = const_cast<wchar_t*>(columns[i].title);
        ListView_InsertColumn(view.list, i, &column);
    }
    SetUpImages(view);
    Fill(view);
    Layout(view);
}

void Open(View& view, const Row& row) {
    Library& library = view.host->tree->Lib();
    library.Select(library.FindById(row.itemId));
}

INT_PTR CALLBACK DialogProc(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam) {
    View* view = ViewOf(dialog);
    switch (message) {
    case WM_INITDIALOG: {
        view = reinterpret_cast<View*>(lParam);
        view->dialog = dialog;
        ::SetWindowLongPtrW(dialog, DWLP_USER, reinterpret_cast<LONG_PTR>(view));
        g_views.push_back(view);
        InitView(*view);
        return FALSE;
    }
    case WM_SIZE:
        if (view && wParam != SIZE_MINIMIZED) Layout(*view);
        return FALSE;
    case WM_DISPLAYCHANGE: // the library's way of saying the skin changed
        if (view) {
            SetUpImages(*view);
            Fill(*view);
            Layout(*view);
        }
        return FALSE;
    case WM_COMMAND:
        if (!view) return FALSE;
        switch (LOWORD(wParam)) {
        case IDC_PLAY: PlayPicked(*view, 0); return TRUE;
        case IDC_ENQUEUE: PlayPicked(*view, 1); return TRUE;
        case IDC_NEWFOLDER: view->host->tree->NewFolder(view->folder, true); return TRUE;
        default: return FALSE;
        }
    case WM_CONTEXTMENU:
        if (view && reinterpret_cast<HWND>(wParam) == view->list) {
            POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            if (point.x == -1 && point.y == -1) {
                RECT rect{};
                const int first = ListView_GetNextItem(view->list, -1, LVNI_SELECTED | LVNI_FOCUSED);
                if (first >= 0 && ListView_GetItemRect(view->list, first, &rect, LVIR_LABEL)) {
                    point = {rect.left + 4, rect.bottom};
                } else {
                    point = {4, 4};
                }
                ::ClientToScreen(view->list, &point);
            }
            view->host->rowMenu(dialog, point, view->folder, PickedRows(*view));
            return TRUE;
        }
        return FALSE;
    case WM_NOTIFY: {
        if (!view) return FALSE;
        const auto* header = reinterpret_cast<NMHDR*>(lParam);
        if (header->hwndFrom != view->list) return FALSE;
        switch (header->code) {
        case NM_DBLCLK: {
            const auto* activate = reinterpret_cast<NMITEMACTIVATE*>(lParam);
            if (activate->iItem < 0 || activate->iItem >= static_cast<int>(view->rows.size())) return TRUE;
            const Row row = view->rows[static_cast<std::size_t>(activate->iItem)];
            if (row.folder) Open(*view, row);
            else if (!row.file.empty()) view->host->play({row.file}, -1);
            return TRUE;
        }
        case NM_RETURN: {
            PlayPicked(*view, (::GetKeyState(VK_SHIFT) & 0x8000) ? 1 : 0);
            return TRUE;
        }
        case LVN_KEYDOWN: {
            const auto* key = reinterpret_cast<NMLVKEYDOWN*>(lParam);
            if (key->wVKey == VK_F2) {
                const auto rows = PickedRows(*view);
                if (rows.size() == 1 && rows[0].folder) view->host->tree->BeginRename(rows[0].key);
            }
            return TRUE;
        }
        default: return FALSE;
        }
    }
    case WM_DESTROY:
        if (view) {
            std::erase(g_views, view);
            ::SetWindowLongPtrW(dialog, DWLP_USER, 0);
            delete view;
        }
        return FALSE;
    default: return FALSE;
    }
}

} // namespace

HWND CreateFolderView(ViewHost& host, HWND parent, int folder) {
    auto* view = new View;
    view->host = &host;
    view->folder = folder;
    const HWND dialog = ::CreateDialogParamW(host.instance, MAKEINTRESOURCEW(IDD_FOLDER_VIEW), parent, DialogProc,
                                             reinterpret_cast<LPARAM>(view));
    if (!dialog) delete view;
    return dialog;
}

void RefreshFolderViews(int folder) {
    for (View* view : std::vector<View*>(g_views)) {
        if (folder == -1 || view->folder == folder) {
            if (view->host->tree->Folders().Exists(view->folder) || view->folder == 0) Fill(*view);
        }
    }
}

} // namespace folders
