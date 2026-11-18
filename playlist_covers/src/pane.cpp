#include "pane.h"

#include "covers_model.h"
#include "draw.h"
#include "layout.h"
#include "no_art.h"
#include "playlist_file.h"

#include <commctrl.h>
#include <shlobj.h>
#include <windowsx.h>

#include <wa_ipc.h>

#include <algorithm>
#include <iterator>
#include <memory>
#include <numeric>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace covers {

namespace {

constexpr wchar_t kPaneClass[] = L"PlaylistCoversPane"; // the covers' place, and the bar below
constexpr wchar_t kGridClass[] = L"PlaylistCoversGrid"; // the covers, which scroll
// On the playlist view: the pane it has.
constexpr wchar_t kPaneProp[] = L"PlaylistCoversPane";
// On the playlist view, set by ml_playlists: the playlist's name.
constexpr wchar_t kTitleProp[] = L"TITLE";
// For tools\playlist-covers-test.ps1: what the pane holds (wParam says which, see Probe).
constexpr UINT kMsgProbe = WM_APP + 9;

// ml_playlists' controls in its playlist view (its resource.h).
constexpr int kListId = 1012, kPlayId = 1003, kEnqueueId = 1004, kSaveId = 1013;

// The media library's own skinning, which gives the covers' scroll bar the look of its
// lists' (gen_ml's ml_ipc_0313.h).
constexpr UINT kWmMlIpc = WM_USER + 0x1000;
constexpr LPARAM kMlIpcSkinWindow = 0x1400;
constexpr UINT kSkinnedScrollWindow = 0x0002; // SKINNEDWND_TYPE_SCROLLWND
// Sent to the window skinned: it keeps a copy of the scroll bar's place, read again on
// this; and it hides the bar when nothing scrolls, unless told otherwise.
constexpr LPARAM kSkinnedScrollUpdateBars = 0x0010;
constexpr LPARAM kSkinnedScrollDisableNoScroll = 0x0014;
struct MlSkinWindow {
    HWND hwndToSkin;
    UINT skinType;
    UINT style;
};

enum Timer : UINT_PTR { kPollTimer = 1, kRegroupTimer };
constexpr UINT kPollMs = 400, kRegroupMs = 400;
constexpr UINT_PTR kListSubclassId = 0x50434F57;

constexpr int kSplitterHeight = 6;
constexpr int kLeast = 70; // the least the covers, and the list, are left

// The library's own sizes and names for them.
struct ArtSizeChoice {
    int size;
    const wchar_t* name;
};
constexpr ArtSizeChoice kArtSizes[] = {{60, L"Small"}, {90, L"Medium"}, {120, L"Large"}, {180, L"Extra large"}};
constexpr const wchar_t* kOrderNames[] = {L"As in the playlist", L"By artist", L"By album"};

enum Command : UINT {
    kCmdPlay = 1,
    kCmdEnqueue,
    kCmdSelect,
    kCmdOpenFolder,
    kCmdJumpToPlaying,
    kCmdArtSizeAsAudio = 99,
    kCmdArtSize = 100,
    kCmdOrder = 200,
};

// Winamp's play status (IPC_ISPLAYING).
constexpr int kStopped = 0;

[[nodiscard]] std::string ReadWholeFile(const std::wstring& path) {
    std::string bytes;
    const HANDLE file = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return bytes;
    LARGE_INTEGER size{};
    if (::GetFileSizeEx(file, &size) && size.QuadPart > 0 && size.QuadPart < 64LL * 1024 * 1024) {
        bytes.resize(static_cast<size_t>(size.QuadPart));
        DWORD read = 0;
        if (!::ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr)) read = 0;
        bytes.resize(read);
    }
    ::CloseHandle(file);
    return bytes;
}

// A file's size and last write, to tell when it has been saved again.
[[nodiscard]] std::pair<std::uint64_t, std::uint64_t> Stamp(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!::GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) return {0, 0};
    return {static_cast<std::uint64_t>(data.ftLastWriteTime.dwHighDateTime) << 32 | data.ftLastWriteTime.dwLowDateTime,
            static_cast<std::uint64_t>(data.nFileSizeHigh) << 32 | data.nFileSizeLow};
}

[[nodiscard]] bool IsRelative(std::wstring_view path) {
    return !(path.size() >= 2 && path[1] == L':') && !path.starts_with(L"\\\\") && path.find(L"://") == std::wstring_view::npos;
}

[[nodiscard]] std::wstring ClassOf(HWND window) {
    wchar_t name[64] = {};
    ::GetClassNameW(window, name, static_cast<int>(std::size(name)));
    return name;
}

// Where a window is in its parent's coordinates.
[[nodiscard]] Box PlaceOf(HWND window) {
    RECT rect{};
    ::GetWindowRect(window, &rect);
    ::MapWindowPoints(nullptr, ::GetParent(window), reinterpret_cast<POINT*>(&rect), 2);
    return {rect.left, rect.top, rect.right, rect.bottom};
}

class Pane {
public:
    Pane(HWND dialog, HWND list, const PaneHost& host)
        : host_(host), settings_(*host.settings), store_(*host.store), dialog_(dialog), list_(list) {}
    Pane(const Pane&) = delete;
    Pane& operator=(const Pane&) = delete;

    static LRESULT CALLBACK Proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK GridProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK ListProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR self);

    void Results(const Store::Taken& taken);
    void PlaybackChanged() { PollPlaying(); }
    void SkinChanged() {
        ::InvalidateRect(hwnd_, nullptr, FALSE);
        InvalidateGrid();
    }

private:
    LRESULT Handle(UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT HandleGrid(UINT msg, WPARAM wParam, LPARAM lParam);
    void Create();
    void Destroy();

    // Where the pane and the list go.
    void AdjustList(WINDOWPOS& position);
    void Place();
    [[nodiscard]] static Box PaneBox(const Split& split);
    [[nodiscard]] static Box ListBox(const Split& split);
    void Layout();
    void LayoutGrid();
    void UpdateScrollBar();

    // The playlist.
    LRESULT Ipc(WPARAM wParam, LPARAM command) const {
        return ::SendMessageW(host_.winamp, WM_WA_IPC, wParam, command);
    }
    bool FindPlaylistFile();
    void LoadPlaylist();
    void Regroup();
    void MapListRows();
    void FollowList();
    void PollPlaying();
    void Poll();
    void RequestArt();

    // The list's rows.
    [[nodiscard]] int RowCount() const { return static_cast<int>(::SendMessageW(list_, LVM_GETITEMCOUNT, 0, 0)); }
    [[nodiscard]] bool ListUnsaved() const {
        const HWND save = ::GetDlgItem(dialog_, kSaveId);
        return save && ::IsWindowEnabled(save);
    }
    void SelectGroups(const std::vector<int>& groups, bool add);
    void DeselectGroup(int group);
    void Press(int button);
    void OpenFolder(int group);
    void JumpToPlaying();

    // The covers.
    [[nodiscard]] int TileCount() const noexcept { return static_cast<int>(groups_.size()); }
    [[nodiscard]] int ArtSize() const noexcept { return settings_.coverSize ? settings_.coverSize : look_.coverSize; }
    [[nodiscard]] int TileAt(int x, int y) const noexcept { return geometry_.TileAt(TileCount(), x, y + scroll_); }
    [[nodiscard]] bool Picked(int tile) const noexcept {
        return tile >= 0 && tile < static_cast<int>(picked_.size()) && picked_[static_cast<size_t>(tile)];
    }
    void ScrollTo(int offset);
    void EnsureTileVisible(int tile);
    void SetCoverSize(int size);
    void SetOrder(int order);
    [[nodiscard]] LRESULT Probe(WPARAM what) const;

    // Drawing.
    void MakeFonts();
    void PaintPane(HDC dc);
    void PaintGrid(HDC target, const RECT& dirty);
    void PaintTile(HDC dc, HDC art, int tile, bool focused);
    void InvalidateGrid() const {
        if (grid_) ::InvalidateRect(grid_, nullptr, FALSE);
    }

    // Input.
    void GridMouseDown(int x, int y, WPARAM keys, bool doubleClick);
    void Scroll(WPARAM request);
    void Wheel(int delta, bool control);
    void ClickTile(int tile, bool control, bool shift);
    bool Key(WPARAM key);
    void ContextMenu(int screenX, int screenY);

    PaneHost host_;
    Settings& settings_;
    Store& store_;
    HWND hwnd_ = nullptr;
    HWND grid_ = nullptr;
    HWND dialog_ = nullptr;
    HWND list_ = nullptr;
    bool skinned_ = false;
    AudioViewLook look_;

    // Where ml_playlists puts its list, in the view's coordinates: the place shared.
    Box original_;
    bool haveOriginal_ = false;
    bool placing_ = false;

    // The playlist, as its file has it.
    std::wstring title_, file_;
    std::pair<std::uint64_t, std::uint64_t> stamp_{};
    std::vector<Track> tracks_;
    std::unordered_map<std::wstring, std::vector<int>> positionsByFile_;
    std::vector<Group> groups_;
    std::vector<int> groupOfTrack_;
    bool regroupPending_ = false;

    // The list, as it is now.
    std::vector<int> rowTrack_; // -1 for a row whose song isn't in the file
    int listCount_ = -1;
    bool listUnsaved_ = false;
    std::vector<char> picked_; // per group: some of its songs are picked in the list
    int playingGroup_ = -1;
    int playStatus_ = kStopped;
    bool wasVisible_ = false;

    // The covers.
    Box border_, splitter_;
    Grid geometry_;
    int gridWidth_ = 0, gridHeight_ = 0;
    int scroll_ = 0, focus_ = 0, anchor_ = -1;

    // Input.
    bool dragging_ = false; // the bar under the covers
    int dragOffset_ = 0;
    int wheelRest_ = 0;

    // Drawing.
    HFONT font_ = nullptr;
    int textHeight_ = 16;
    HDC bufferDc_ = nullptr;
    HBITMAP buffer_ = nullptr;
    HGDIOBJ bufferOld_ = nullptr;
    int bufferWidth_ = 0, bufferHeight_ = 0;
};

[[nodiscard]] Pane* PaneOf(HWND hwnd) {
    if (!hwnd || !::IsWindow(hwnd)) return nullptr;
    return reinterpret_cast<Pane*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
}

// ---- Life

struct CreateParams {
    HWND dialog;
    HWND list;
    const PaneHost* host;
};

LRESULT CALLBACK Pane::Proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* pane = reinterpret_cast<Pane*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        const auto* params = static_cast<const CreateParams*>(reinterpret_cast<const CREATESTRUCTW*>(lParam)->lpCreateParams);
        pane = new (std::nothrow) Pane(params->dialog, params->list, *params->host);
        if (!pane) return FALSE;
        pane->hwnd_ = hwnd;
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(pane));
    }
    if (!pane) return ::DefWindowProcW(hwnd, msg, wParam, lParam);
    LRESULT result = 0;
    try {
        result = pane->Handle(msg, wParam, lParam);
    } catch (...) {
        // Nothing may get out into Winamp.
        result = ::DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    if (msg == WM_NCDESTROY) {
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        delete pane;
    }
    return result;
}

LRESULT CALLBACK Pane::GridProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_NCCREATE) {
        auto* owner = static_cast<Pane*>(reinterpret_cast<const CREATESTRUCTW*>(lParam)->lpCreateParams);
        owner->grid_ = hwnd; // before CreateWindowEx returns: messages come during it
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(owner));
    }
    auto* pane = reinterpret_cast<Pane*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCDESTROY) ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
    if (!pane || msg == WM_NCCREATE || msg == WM_NCDESTROY) return ::DefWindowProcW(hwnd, msg, wParam, lParam);
    try {
        return pane->HandleGrid(msg, wParam, lParam);
    } catch (...) {
        return ::DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

// Sees where ml_playlists puts its list, and puts it below the covers instead.
LRESULT CALLBACK Pane::ListProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR self) {
    auto* pane = reinterpret_cast<Pane*>(self);
    if (msg == WM_WINDOWPOSCHANGING && !pane->placing_) {
        try {
            pane->AdjustList(*reinterpret_cast<WINDOWPOS*>(lParam));
        } catch (...) {
        }
    }
    if (msg == WM_NCDESTROY) {
        ::RemoveWindowSubclass(hwnd, ListProc, kListSubclassId);
        pane->list_ = nullptr;
    }
    return ::DefSubclassProc(hwnd, msg, wParam, lParam);
}

void Pane::Create() {
    ::SetPropW(dialog_, kPaneProp, hwnd_);
    look_ = ReadAudioViewLook(host_.libraryFolder);
    MakeFonts();
    grid_ = ::CreateWindowExW(0, kGridClass, L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_TABSTOP, 0, 0, 0, 0, hwnd_,
                              nullptr, host_.instance, this);
    // The library draws its scroll bar, as it draws its own lists'.
    MlSkinWindow skin{grid_, kSkinnedScrollWindow, 0};
    skinned_ = ::SendMessageW(::GetParent(dialog_), kWmMlIpc, reinterpret_cast<WPARAM>(&skin), kMlIpcSkinWindow) != 0;
    // Always there, greyed when nothing scrolls: a bar that came and went would change the
    // covers' width, and with it how many rows there are.
    if (skinned_) ::SendMessageW(grid_, kWmMlIpc, TRUE, kSkinnedScrollDisableNoScroll);

    original_ = PlaceOf(list_);
    haveOriginal_ = true;
    ::SetWindowSubclass(list_, ListProc, kListSubclassId, reinterpret_cast<DWORD_PTR>(this));
    Place();
    if (FindPlaylistFile()) LoadPlaylist();
    ::SetTimer(hwnd_, kPollTimer, kPollMs, nullptr);
}

void Pane::Destroy() {
    ::KillTimer(hwnd_, kPollTimer);
    ::KillTimer(hwnd_, kRegroupTimer);
    if (list_ && ::IsWindow(list_)) ::RemoveWindowSubclass(list_, ListProc, kListSubclassId);
    list_ = nullptr;
    if (::GetPropW(dialog_, kPaneProp) == hwnd_) ::RemovePropW(dialog_, kPaneProp);
    if (bufferDc_) {
        ::SelectObject(bufferDc_, bufferOld_);
        ::DeleteDC(bufferDc_);
        bufferDc_ = nullptr;
    }
    if (buffer_) ::DeleteObject(buffer_);
    buffer_ = nullptr;
    if (font_) ::DeleteObject(font_);
    font_ = nullptr;
}

// The list's own font, as the library's album view uses its list's.
void Pane::MakeFonts() {
    LOGFONTW face{};
    const auto listFont = reinterpret_cast<HFONT>(::SendMessageW(list_, WM_GETFONT, 0, 0));
    if (!listFont || !::GetObjectW(listFont, sizeof(face), &face)) {
        NONCLIENTMETRICSW metrics{};
        metrics.cbSize = sizeof(metrics);
        ::SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0);
        face = metrics.lfMessageFont;
    }
    font_ = ::CreateFontIndirectW(&face);
    const HDC dc = ::GetDC(hwnd_);
    const HGDIOBJ old = ::SelectObject(dc, font_);
    TEXTMETRICW text{};
    ::GetTextMetricsW(dc, &text);
    textHeight_ = text.tmHeight;
    ::SelectObject(dc, old);
    ::ReleaseDC(hwnd_, dc);
}

// ---- Messages: the pane (the bar under the covers)

LRESULT Pane::Handle(UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE:
        Create();
        return 0;
    case WM_DESTROY:
        Destroy();
        return 0;
    case WM_SIZE:
        Layout();
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint;
        const HDC dc = ::BeginPaint(hwnd_, &paint);
        PaintPane(dc);
        ::EndPaint(hwnd_, &paint);
        return 0;
    }
    case WM_PRINTCLIENT:
        PaintPane(reinterpret_cast<HDC>(wParam));
        return 0;
    case kMsgProbe:
        return Probe(wParam);
    case WM_TIMER:
        if (wParam == kPollTimer) Poll();
        if (wParam == kRegroupTimer) {
            ::KillTimer(hwnd_, kRegroupTimer);
            regroupPending_ = false;
            Regroup();
        }
        return 0;
    case WM_LBUTTONDOWN:
        if (splitter_.Contains(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam))) {
            dragging_ = true;
            dragOffset_ = GET_Y_LPARAM(lParam) - splitter_.bottom;
            ::SetCapture(hwnd_);
        }
        return 0;
    case WM_MOUSEMOVE:
        if (dragging_) {
            // In the view's coordinates, where the shared place is.
            POINT at{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            ::MapWindowPoints(hwnd_, dialog_, &at, 1);
            settings_.coversShare = ShareFor(original_, at.y - dragOffset_);
            Place();
        }
        return 0;
    case WM_LBUTTONUP:
        if (dragging_) {
            dragging_ = false;
            ::ReleaseCapture();
            SaveSettings(host_.iniFile, settings_);
        }
        return 0;
    case WM_CAPTURECHANGED:
        dragging_ = false;
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT) {
            POINT at;
            ::GetCursorPos(&at);
            ::ScreenToClient(hwnd_, &at);
            ::SetCursor(::LoadCursorW(nullptr, dragging_ || splitter_.Contains(at.x, at.y) ? IDC_SIZENS : IDC_ARROW));
            return TRUE;
        }
        break;
    }
    return ::DefWindowProcW(hwnd_, msg, wParam, lParam);
}

// ---- Messages: the covers

LRESULT Pane::HandleGrid(UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_SIZE:
        LayoutGrid();
        RequestArt();
        InvalidateGrid();
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint;
        const HDC dc = ::BeginPaint(grid_, &paint);
        PaintGrid(dc, paint.rcPaint);
        ::EndPaint(grid_, &paint);
        return 0;
    }
    case WM_PRINTCLIENT: {
        RECT client{};
        ::GetClientRect(grid_, &client);
        PaintGrid(reinterpret_cast<HDC>(wParam), client);
        return 0;
    }
    case WM_VSCROLL:
        Scroll(wParam);
        return 0;
    case WM_MOUSEWHEEL:
        Wheel(GET_WHEEL_DELTA_WPARAM(wParam), (GET_KEYSTATE_WPARAM(wParam) & MK_CONTROL) != 0);
        return 0;
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
        InvalidateGrid();
        return 0;
    case WM_GETDLGCODE:
        return DLGC_WANTARROWS;
    case WM_KEYDOWN:
        if (Key(wParam)) return 0;
        break;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
        GridMouseDown(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam), wParam, msg == WM_LBUTTONDBLCLK);
        return 0;
    case WM_CONTEXTMENU:
        ContextMenu(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        return 0;
    }
    return ::DefWindowProcW(grid_, msg, wParam, lParam);
}

// ---- Where the pane and the list go

// The pane takes the list's border too (ml_playlists draws it a pixel outside the list),
// and the list keeps a pixel above itself for its own.
Box Pane::PaneBox(const Split& split) {
    if (split.covers.Height() <= 0) return {};
    return {split.covers.left - 1, split.covers.top - 1, split.covers.right + 1, split.covers.bottom};
}

Box Pane::ListBox(const Split& split) {
    Box list = split.list;
    if (list.top != split.covers.top) list.top += 1;
    return list;
}

void Pane::AdjustList(WINDOWPOS& position) {
    const bool moves = !(position.flags & SWP_NOMOVE), sizes = !(position.flags & SWP_NOSIZE);
    if (moves || sizes) {
        const Box was = haveOriginal_ ? original_ : PlaceOf(list_);
        const int x = moves ? position.x : was.left, y = moves ? position.y : was.top;
        const int width = sizes ? position.cx : was.Width(), height = sizes ? position.cy : was.Height();
        // The list put back where it already is (below the covers): nothing has changed.
        if (haveOriginal_ && Box{x, y, x + width, y + height} == ListBox(SplitArea(original_, settings_.coversShare, kLeast))) return;
        original_ = {x, y, x + width, y + height};
        haveOriginal_ = true;
        const Split split = SplitArea(original_, settings_.coversShare, kLeast);
        const Box list = ListBox(split);
        position.x = list.left;
        position.y = list.top;
        position.cx = list.Width();
        position.cy = list.Height();
        position.flags &= ~static_cast<UINT>(SWP_NOMOVE | SWP_NOSIZE);
        const Box pane = PaneBox(split);
        ::SetWindowPos(hwnd_, nullptr, pane.left, pane.top, pane.Width(), pane.Height(),
                       SWP_NOZORDER | SWP_NOACTIVATE | (pane.Height() > 0 ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
    }
    if (position.flags & SWP_HIDEWINDOW) ::ShowWindow(hwnd_, SW_HIDE);
}

void Pane::Place() {
    if (!haveOriginal_ || !list_) return;
    const Split split = SplitArea(original_, settings_.coversShare, kLeast);
    const Box pane = PaneBox(split), list = ListBox(split);
    placing_ = true;
    ::SetWindowPos(hwnd_, nullptr, pane.left, pane.top, pane.Width(), pane.Height(),
                   SWP_NOZORDER | SWP_NOACTIVATE | (pane.Height() > 0 ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
    ::SetWindowPos(list_, nullptr, list.left, list.top, list.Width(), list.Height(), SWP_NOZORDER | SWP_NOACTIVATE);
    placing_ = false;
    // The view draws the list's border round wherever the list now is.
    ::InvalidateRect(dialog_, nullptr, TRUE);
}

// The covers inside the border the library draws round its lists, the bar below.
void Pane::Layout() {
    RECT client{};
    ::GetClientRect(hwnd_, &client);
    border_ = {0, 0, client.right, std::max(0L, client.bottom - kSplitterHeight)};
    splitter_ = {0, border_.bottom, client.right, client.bottom};
    if (grid_) {
        ::MoveWindow(grid_, 1, 1, std::max(0, border_.Width() - 2), std::max(0, border_.Height() - 2), TRUE);
    }
    ::InvalidateRect(hwnd_, nullptr, FALSE);
}

void Pane::LayoutGrid() {
    RECT client{};
    ::GetClientRect(grid_, &client);
    gridWidth_ = client.right;
    gridHeight_ = client.bottom;
    geometry_ = MakeGrid(gridWidth_, ArtSize(), look_.names ? textHeight_ : 0);
    scroll_ = ClampOffset(scroll_, geometry_.ContentHeight(TileCount()), gridHeight_);
    UpdateScrollBar();
}

void Pane::UpdateScrollBar() {
    if (!grid_) return;
    SCROLLINFO info{};
    info.cbSize = sizeof(info);
    // Always there, as the library's own lists' are, so the covers don't jump sideways.
    info.fMask = SIF_RANGE | SIF_PAGE | SIF_POS | SIF_DISABLENOSCROLL;
    info.nMin = 0;
    info.nMax = std::max(0, geometry_.ContentHeight(TileCount()) - 1);
    info.nPage = static_cast<UINT>(std::max(0, gridHeight_));
    info.nPos = scroll_;
    ::SetScrollInfo(grid_, SB_VERT, &info, !skinned_);
    if (skinned_) ::SendMessageW(grid_, kWmMlIpc, TRUE, kSkinnedScrollUpdateBars);
}

// ---- The playlist

// The view has only the playlist's name; the library's list of playlists has its file.
// Two of the same name are told apart by how many songs they have.
bool Pane::FindPlaylistFile() {
    const auto* title = static_cast<const wchar_t*>(::GetPropW(dialog_, kTitleProp));
    title_ = title ? title : L"";
    file_.clear();
    if (title_.empty() || !host_.playlists) return false;
    std::vector<std::wstring> candidates;
    host_.playlists->Lock();
    const std::size_t count = host_.playlists->Count();
    for (std::size_t i = 0; i < count; ++i) {
        const wchar_t* name = host_.playlists->Name(i);
        const wchar_t* file = host_.playlists->Filename(i);
        if (!name || !file || title_ != name) continue;
        std::wstring path(file);
        if (IsRelative(path)) path = host_.playlistsFolder + L"\\" + path;
        candidates.push_back(std::move(path));
    }
    host_.playlists->Unlock();
    if (candidates.empty()) return false;
    file_ = candidates.front();
    if (candidates.size() > 1) {
        const int rows = RowCount();
        for (const std::wstring& candidate : candidates) {
            const auto entries = ParsePlaylist(ReadWholeFile(candidate), FolderOf(candidate), true);
            if (static_cast<int>(entries.size()) == rows) {
                file_ = candidate;
                break;
            }
        }
    }
    return true;
}

void Pane::LoadPlaylist() {
    stamp_ = Stamp(file_);
    const std::wstring extension = FoldCase(file_.substr(std::min(file_.size(), file_.rfind(L'.'))));
    const auto entries = ParsePlaylist(ReadWholeFile(file_), FolderOf(file_), extension != L".m3u");
    tracks_.clear();
    tracks_.reserve(entries.size());
    positionsByFile_.clear();
    std::vector<std::wstring> files;
    files.reserve(entries.size());
    for (const PlaylistEntry& entry : entries) {
        Track& track = tracks_.emplace_back();
        track.file = entry.file;
        track.listTitle = entry.title;
        std::wstring key = FoldCase(entry.file);
        if (const Tags* tags = store_.TagsOf(key)) track.tags = *tags;
        positionsByFile_[std::move(key)].push_back(static_cast<int>(tracks_.size()) - 1);
        files.push_back(entry.file);
    }
    store_.WantTags(files);
    Regroup();
    MapListRows();
    FollowList();
    PollPlaying();
}

void Pane::Regroup() {
    groups_ = GroupTracks(tracks_, static_cast<GroupOrder>(settings_.order));
    groupOfTrack_.assign(tracks_.size(), -1);
    for (int g = 0; g < static_cast<int>(groups_.size()); ++g) {
        for (const int index : groups_[static_cast<size_t>(g)].tracks) groupOfTrack_[static_cast<size_t>(index)] = g;
    }
    focus_ = std::clamp(focus_, 0, std::max(0, TileCount() - 1));
    if (anchor_ >= TileCount()) anchor_ = -1;
    picked_.assign(groups_.size(), 0);
    LayoutGrid();
    FollowList();
    PollPlaying();
    RequestArt();
    InvalidateGrid();
}

// Which song each of the list's rows is. While the playlist is as saved, row n is the
// file's song n; edited and not yet saved, the rows are matched by their titles.
void Pane::MapListRows() {
    if (!list_) return;
    const int rows = RowCount();
    listCount_ = rows;
    listUnsaved_ = ListUnsaved();
    rowTrack_.assign(static_cast<size_t>(std::max(0, rows)), -1);
    if (!listUnsaved_ && rows == static_cast<int>(tracks_.size())) {
        std::iota(rowTrack_.begin(), rowTrack_.end(), 0);
        return;
    }
    std::vector<std::wstring> rowTitles(rowTrack_.size()), trackTitles;
    for (int r = 0; r < rows; ++r) {
        wchar_t text[512] = {};
        LVITEMW item{};
        item.iSubItem = 0;
        item.pszText = text;
        item.cchTextMax = static_cast<int>(std::size(text));
        ::SendMessageW(list_, LVM_GETITEMTEXTW, static_cast<WPARAM>(r), reinterpret_cast<LPARAM>(&item));
        rowTitles[static_cast<size_t>(r)] = WithoutNumber(text, r + 1);
    }
    trackTitles.reserve(tracks_.size());
    for (const Track& track : tracks_) trackTitles.push_back(track.listTitle);
    rowTrack_ = MapRows(rowTitles, trackTitles);
}

// The covers some of whose songs are picked in the list show as picked.
void Pane::FollowList() {
    if (!list_) return;
    std::vector<char> picked(groups_.size(), 0);
    for (int row = static_cast<int>(::SendMessageW(list_, LVM_GETNEXTITEM, static_cast<WPARAM>(-1), LVNI_SELECTED));
         row >= 0; row = static_cast<int>(::SendMessageW(list_, LVM_GETNEXTITEM, static_cast<WPARAM>(row), LVNI_SELECTED))) {
        if (row >= static_cast<int>(rowTrack_.size())) break;
        const int track = rowTrack_[static_cast<size_t>(row)];
        if (track >= 0 && groupOfTrack_[static_cast<size_t>(track)] >= 0) {
            picked[static_cast<size_t>(groupOfTrack_[static_cast<size_t>(track)])] = 1;
        }
    }
    if (picked == picked_) return;
    picked_ = std::move(picked);
    InvalidateGrid();
}

void Pane::PollPlaying() {
    const int status = static_cast<int>(Ipc(0, IPC_ISPLAYING));
    int group = -1;
    if (const auto* playing = reinterpret_cast<const wchar_t*>(Ipc(0, IPC_GET_PLAYING_FILENAME)); playing && *playing) {
        if (const auto found = positionsByFile_.find(FoldCase(playing)); found != positionsByFile_.end()) {
            group = groupOfTrack_[static_cast<size_t>(found->second.front())];
        }
    }
    if (group == playingGroup_ && status == playStatus_) return;
    playingGroup_ = group;
    playStatus_ = status;
    InvalidateGrid();
}

void Pane::Poll() {
    if (!::IsWindowVisible(hwnd_)) {
        wasVisible_ = false;
        return;
    }
    // The view is made out of sight and shown after: the covers are asked for once it is.
    // The Audio view's look may have been changed meanwhile, too.
    if (!wasVisible_) {
        wasVisible_ = true;
        const AudioViewLook look = ReadAudioViewLook(host_.libraryFolder);
        if (look.coverSize != look_.coverSize || look.names != look_.names) {
            look_ = look;
            LayoutGrid();
            InvalidateGrid();
        }
        RequestArt();
    }
    // Saved again (its Save button, or ml_playlists on leaving it): the file is read again.
    if (!file_.empty() && Stamp(file_) != stamp_) LoadPlaylist();
    else if (file_.empty() && FindPlaylistFile()) LoadPlaylist();
    if (list_ && (RowCount() != listCount_ || ListUnsaved() != listUnsaved_)) MapListRows();
    FollowList();
    PollPlaying();
}

void Pane::Results(const Store::Taken& taken) {
    bool changed = false;
    for (const std::wstring& key : taken.taggedFiles) {
        const auto found = positionsByFile_.find(key);
        if (found == positionsByFile_.end()) continue;
        const Tags* tags = store_.TagsOf(key);
        for (const int index : found->second) tracks_[static_cast<size_t>(index)].tags = tags ? std::optional<Tags>(*tags) : std::nullopt;
        changed = true;
    }
    if (changed) {
        if (store_.TagsWaiting() == 0) {
            ::KillTimer(hwnd_, kRegroupTimer);
            regroupPending_ = false;
            Regroup();
        } else if (!regroupPending_) {
            // Still coming: the covers are put together again now and then, not for each.
            regroupPending_ = true;
            ::SetTimer(hwnd_, kRegroupTimer, kRegroupMs, nullptr);
        }
    }
    if (taken.art) InvalidateGrid();
}

// The covers in sight, then the next two screens down and one up, unless they are in.
void Pane::RequestArt() {
    if (!::IsWindowVisible(hwnd_)) return;
    const int tiles = TileCount();
    const int size = ArtSize();
    std::vector<ArtRequest> requests;
    const auto want = [&](int tile) {
        if (tile < 0 || tile >= tiles) return;
        const Group& group = groups_[static_cast<size_t>(tile)];
        // Until its tags are read, a song may yet turn out to be part of an album.
        if (group.single && !tracks_[static_cast<size_t>(group.tracks.front())].tags) return;
        if (const Art* art = store_.ArtOf(group.key); art && (art->missing || art->size == size)) return;
        ArtRequest request{group.key, {}, size};
        // An album's first song may be the one without a picture.
        for (const int index : group.tracks) {
            const std::wstring& file = tracks_[static_cast<size_t>(index)].file;
            if (std::ranges::find(request.files, file) == request.files.end()) request.files.push_back(file);
            if (request.files.size() == 3) break;
        }
        requests.push_back(std::move(request));
    };
    int first = 0, last = -1;
    geometry_.VisibleRange(tiles, scroll_, scroll_ + gridHeight_, first, last);
    const int page = std::max(1, last - first + 1);
    for (int tile = first; tile <= last; ++tile) want(tile);
    for (int tile = last + 1; tile <= last + 2 * page; ++tile) want(tile);
    for (int tile = first - 1; tile >= first - page; --tile) want(tile);
    store_.WantArt(std::move(requests));
}

// ---- The list's rows

// Picks the songs of these covers in the list (and only those, unless `add`), and
// brings the first into sight.
void Pane::SelectGroups(const std::vector<int>& groups, bool add) {
    if (!list_) return;
    if (RowCount() != listCount_ || ListUnsaved() != listUnsaved_) MapListRows();
    std::vector<char> wanted(groups_.size(), 0);
    for (const int group : groups) {
        if (group >= 0 && group < static_cast<int>(wanted.size())) wanted[static_cast<size_t>(group)] = 1;
    }
    LVITEMW item{};
    item.stateMask = LVIS_SELECTED;
    if (!add) {
        item.state = 0;
        ::SendMessageW(list_, LVM_SETITEMSTATE, static_cast<WPARAM>(-1), reinterpret_cast<LPARAM>(&item));
    }
    int first = -1;
    item.state = LVIS_SELECTED;
    for (int row = 0; row < static_cast<int>(rowTrack_.size()); ++row) {
        const int track = rowTrack_[static_cast<size_t>(row)];
        if (track < 0 || !wanted[static_cast<size_t>(groupOfTrack_[static_cast<size_t>(track)])]) continue;
        ::SendMessageW(list_, LVM_SETITEMSTATE, static_cast<WPARAM>(row), reinterpret_cast<LPARAM>(&item));
        if (first < 0) first = row;
    }
    if (first >= 0) {
        LVITEMW focus{};
        focus.stateMask = LVIS_FOCUSED;
        focus.state = LVIS_FOCUSED;
        ::SendMessageW(list_, LVM_SETITEMSTATE, static_cast<WPARAM>(first), reinterpret_cast<LPARAM>(&focus));
        ::SendMessageW(list_, LVM_SETSELECTIONMARK, 0, first);
        ::SendMessageW(list_, LVM_ENSUREVISIBLE, static_cast<WPARAM>(first), FALSE);
    }
    FollowList();
}

void Pane::DeselectGroup(int group) {
    if (!list_) return;
    LVITEMW item{};
    item.stateMask = LVIS_SELECTED;
    item.state = 0;
    for (int row = 0; row < static_cast<int>(rowTrack_.size()); ++row) {
        const int track = rowTrack_[static_cast<size_t>(row)];
        if (track >= 0 && groupOfTrack_[static_cast<size_t>(track)] == group) {
            ::SendMessageW(list_, LVM_SETITEMSTATE, static_cast<WPARAM>(row), reinterpret_cast<LPARAM>(&item));
        }
    }
    FollowList();
}

// One of ml_playlists' buttons, which act on the songs picked.
void Pane::Press(int button) {
    const HWND control = ::GetDlgItem(dialog_, button);
    ::SendMessageW(dialog_, WM_COMMAND, MAKEWPARAM(button, 0), reinterpret_cast<LPARAM>(control));
}

void Pane::OpenFolder(int group) {
    if (group < 0 || group >= TileCount()) return;
    const std::wstring& file = tracks_[static_cast<size_t>(groups_[static_cast<size_t>(group)].tracks.front())].file;
    if (PIDLIST_ABSOLUTE item = ::ILCreateFromPathW(file.c_str())) {
        ::SHOpenFolderAndSelectItems(item, 0, nullptr, 0);
        ::ILFree(item);
    }
}

// Picks the song playing in the list, and brings it and its cover into sight.
void Pane::JumpToPlaying() {
    const auto* playing = reinterpret_cast<const wchar_t*>(Ipc(0, IPC_GET_PLAYING_FILENAME));
    if (!playing || !*playing || !list_) return;
    const auto found = positionsByFile_.find(FoldCase(playing));
    if (found == positionsByFile_.end()) return;
    const int track = found->second.front();
    const auto row = std::ranges::find(rowTrack_, track);
    if (row == rowTrack_.end()) return;
    const int index = static_cast<int>(row - rowTrack_.begin());
    LVITEMW item{};
    item.stateMask = LVIS_SELECTED | LVIS_FOCUSED;
    item.state = 0;
    ::SendMessageW(list_, LVM_SETITEMSTATE, static_cast<WPARAM>(-1), reinterpret_cast<LPARAM>(&item));
    item.state = LVIS_SELECTED | LVIS_FOCUSED;
    ::SendMessageW(list_, LVM_SETITEMSTATE, static_cast<WPARAM>(index), reinterpret_cast<LPARAM>(&item));
    ::SendMessageW(list_, LVM_ENSUREVISIBLE, static_cast<WPARAM>(index), FALSE);
    focus_ = groupOfTrack_[static_cast<size_t>(track)];
    EnsureTileVisible(focus_);
    FollowList();
}

// ---- The covers

void Pane::ScrollTo(int offset) {
    offset = ClampOffset(offset, geometry_.ContentHeight(TileCount()), gridHeight_);
    if (offset == scroll_) return;
    scroll_ = offset;
    UpdateScrollBar();
    InvalidateGrid();
    RequestArt();
}

void Pane::EnsureTileVisible(int tile) {
    if (tile < 0 || tile >= TileCount()) return;
    const Box cell = geometry_.Cell(tile);
    if (cell.top < scroll_) ScrollTo(cell.top);
    else if (cell.bottom > scroll_ + gridHeight_) ScrollTo(cell.bottom - gridHeight_);
}

void Pane::SetCoverSize(int size) {
    if (size == settings_.coverSize) return;
    // The cover at the top stays at the top.
    int first = 0, last = -1;
    geometry_.VisibleRange(TileCount(), scroll_, scroll_ + 1, first, last);
    settings_.coverSize = size;
    SaveSettings(host_.iniFile, settings_);
    LayoutGrid();
    ScrollTo(geometry_.Cell(first).top);
    RequestArt();
    InvalidateGrid();
}

void Pane::SetOrder(int order) {
    if (order == settings_.order) return;
    settings_.order = order;
    SaveSettings(host_.iniFile, settings_);
    scroll_ = 0;
    Regroup();
}

// 0 songs in the file, 1 covers, 3 rows matched to songs, 4 tags still to read, 5 covers
// picked, 6 covers with a picture in, 7 covers found to have none, 9 covers still to load,
// 10 the list's rows, 11 rows picked in the list, 12 the pane's bottom and 13 the list's
// top in the view, 14 the playing cover, 15 the cover size, 16 whether the library skins
// the scroll bar, 17 the columns, 18 the grid's cell width and 19 its cell height, 20 how
// far down it is scrolled; 100 + n
// the middle of tile n in the grid window, as MAKELPARAM(x, y).
LRESULT Pane::Probe(WPARAM what) const {
    const auto pictures = [this](bool missing) {
        return std::ranges::count_if(groups_, [this, missing](const Group& group) {
            const Art* art = store_.ArtOf(group.key);
            return art && (missing ? art->missing : art->bitmap != nullptr);
        });
    };
    switch (what) {
    case 0: return static_cast<LRESULT>(tracks_.size());
    case 1: return static_cast<LRESULT>(groups_.size());
    case 3: return std::ranges::count_if(rowTrack_, [](int track) { return track >= 0; });
    case 4: return static_cast<LRESULT>(store_.TagsWaiting());
    case 5: return std::ranges::count(picked_, 1);
    case 6: return pictures(false);
    case 7: return pictures(true);
    case 9: return static_cast<LRESULT>(store_.ArtWaiting());
    case 10: return list_ ? RowCount() : -1;
    case 11: return list_ ? static_cast<LRESULT>(::SendMessageW(list_, LVM_GETSELECTEDCOUNT, 0, 0)) : -1;
    case 12: return PlaceOf(hwnd_).bottom;
    case 13: return list_ ? PlaceOf(list_).top : -1;
    case 14: return playingGroup_;
    case 15: return ArtSize();
    case 16: return skinned_ ? 1 : 0;
    case 17: return geometry_.columns;
    case 18: return geometry_.cellWidth;
    case 19: return geometry_.cellHeight;
    case 20: return scroll_;
    }
    if (what >= 100 && static_cast<int>(what) - 100 < TileCount()) {
        const Box art = Moved(geometry_.Art(static_cast<int>(what) - 100), 0, -scroll_);
        return MAKELPARAM((art.left + art.right) / 2, (art.top + art.bottom) / 2);
    }
    return -1;
}

// ---- Drawing

// The border the media library draws round its lists, and the bar below that sizes.
void Pane::PaintPane(HDC dc) {
    Outline(dc, border_, SkinColor(WADLG_HILITE));
    Fill(dc, splitter_, SkinColor(WADLG_WNDBG));
    const int middle = (splitter_.top + splitter_.bottom) / 2;
    Fill(dc, {splitter_.Width() / 2 - 15, middle - 1, splitter_.Width() / 2 + 15, middle + 1},
         Mix(SkinColor(WADLG_WNDBG), SkinColor(WADLG_WNDFG), 300));
}

void Pane::PaintGrid(HDC screen, const RECT& dirty) {
    if (gridWidth_ <= 0 || gridHeight_ <= 0) return;
    // Drawn off screen first, so nothing flickers.
    if (!bufferDc_ || bufferWidth_ < gridWidth_ || bufferHeight_ < gridHeight_) {
        if (bufferDc_) {
            ::SelectObject(bufferDc_, bufferOld_);
            ::DeleteDC(bufferDc_);
        }
        if (buffer_) ::DeleteObject(buffer_);
        bufferWidth_ = std::max(gridWidth_, bufferWidth_);
        bufferHeight_ = std::max(gridHeight_, bufferHeight_);
        bufferDc_ = ::CreateCompatibleDC(screen);
        buffer_ = ::CreateCompatibleBitmap(screen, bufferWidth_, bufferHeight_);
        bufferOld_ = ::SelectObject(bufferDc_, buffer_);
    }
    const HDC dc = bufferDc_;
    const HDC art = ::CreateCompatibleDC(screen);
    ::SetBkMode(dc, TRANSPARENT);
    const HGDIOBJ oldFont = ::SelectObject(dc, font_);
    const COLORREF background = SkinColor(WADLG_ITEMBG), foreground = SkinColor(WADLG_ITEMFG);
    Fill(dc, {0, 0, gridWidth_, gridHeight_}, background);

    const int tiles = TileCount();
    if (tiles == 0) {
        Text(dc, file_.empty() ? L"Covers: this playlist's file wasn't found" : L"This playlist is empty",
             {0, 0, gridWidth_, gridHeight_}, Mix(background, foreground, 600), DT_CENTER);
    } else {
        int first = 0, last = -1;
        geometry_.VisibleRange(tiles, scroll_, scroll_ + gridHeight_, first, last);
        const bool focused = ::GetFocus() == grid_;
        for (int tile = first; tile <= last; ++tile) PaintTile(dc, art, tile, focused);
    }

    ::SelectObject(dc, oldFont);
    ::DeleteDC(art);
    ::BitBlt(screen, dirty.left, dirty.top, dirty.right - dirty.left, dirty.bottom - dirty.top, dc, dirty.left,
             dirty.top, SRCCOPY);
}

// As the library's album view draws an album (AlbumArtFilter::DrawItemIcon): a picked one
// filled with the list's picked colour, the cover two pixels in, its name below.
void Pane::PaintTile(HDC dc, HDC art, int tile, bool focused) {
    const Group& group = groups_[static_cast<size_t>(tile)];
    const bool picked = Picked(tile);
    const Box cover = Moved(geometry_.Art(tile), 0, -scroll_);

    COLORREF text = SkinColor(WADLG_ITEMFG);
    if (picked) {
        Fill(dc, Moved(geometry_.Highlight(tile), 0, -scroll_),
             SkinColor(focused ? WADLG_SELBAR_BGCOLOR : WADLG_INACT_SELBAR_BGCOLOR));
        text = SkinColor(focused ? WADLG_SELBAR_FGCOLOR : WADLG_INACT_SELBAR_FGCOLOR);
    }

    const BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    if (Art* picture = store_.ArtOf(group.key); picture && picture->bitmap) {
        picture->used = ::GetTickCount64();
        const HGDIOBJ old = ::SelectObject(art, picture->bitmap);
        ::AlphaBlend(dc, cover.left, cover.top, cover.Width(), cover.Height(), art, 0, 0, picture->size, picture->size, blend);
        ::SelectObject(art, old);
    } else if (picture && picture->missing) {
        // The library's llama, in the colour of the text.
        int size = 0;
        if (const HBITMAP llama = NoArtPicture(text, size)) {
            const HGDIOBJ old = ::SelectObject(art, llama);
            ::AlphaBlend(dc, cover.left, cover.top, cover.Width(), cover.Height(), art, 0, 0, size, size, blend);
            ::SelectObject(art, old);
        } else {
            Outline(dc, cover, text);
        }
    }

    if (tile == playingGroup_ && playStatus_ != kStopped) {
        // The skin's highlight, unless it hardly shows against the background.
        const COLORREF background = SkinColor(WADLG_ITEMBG), hilite = SkinColor(WADLG_HILITE);
        const COLORREF mark = std::abs(Luma(hilite) - Luma(background)) >= 60 ? hilite : SkinColor(WADLG_ITEMFG);
        Outline(dc, Inset(cover, -2), mark, 2);
        const int size = std::max(12, cover.Width() / 6);
        const Box badge{cover.left + 3, cover.bottom - 3 - size, cover.left + 3 + size, cover.bottom - 3};
        Fill(dc, badge, mark);
        PlayMark(dc, Inset(badge, size / 4), playStatus_, Luma(mark) > 128 ? RGB(0, 0, 0) : RGB(255, 255, 255));
    }

    if (geometry_.textHeight > 0 && !group.name.empty()) {
        RECT name = ToRect(Moved(geometry_.Name(tile), 0, -scroll_));
        ::SetTextColor(dc, text);
        ::DrawTextW(dc, group.name.c_str(), static_cast<int>(group.name.size()), &name,
                    DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_WORD_ELLIPSIS | DT_NOPREFIX);
    }

    if (focused && tile == focus_) {
        const RECT rect = ToRect(Moved(geometry_.Highlight(tile), 0, -scroll_));
        ::SetTextColor(dc, RGB(0, 0, 0));
        ::SetBkColor(dc, SkinColor(WADLG_ITEMBG));
        ::DrawFocusRect(dc, &rect);
    }
}

// ---- Mouse

void Pane::GridMouseDown(int x, int y, WPARAM keys, bool doubleClick) {
    ::SetFocus(grid_);
    const int tile = TileAt(x, y);
    if (doubleClick && tile >= 0) {
        // Its songs, played as ml_playlists' Play button plays the songs picked.
        SelectGroups({tile}, false);
        Press(kPlayId);
        return;
    }
    ClickTile(tile, (keys & MK_CONTROL) != 0, (keys & MK_SHIFT) != 0);
}

void Pane::Scroll(WPARAM request) {
    const int line = std::max(textHeight_, geometry_.cellHeight / 3);
    switch (LOWORD(request)) {
    case SB_LINEUP: ScrollTo(scroll_ - line); break;
    case SB_LINEDOWN: ScrollTo(scroll_ + line); break;
    case SB_PAGEUP: ScrollTo(scroll_ - gridHeight_); break;
    case SB_PAGEDOWN: ScrollTo(scroll_ + gridHeight_); break;
    case SB_TOP: ScrollTo(0); break;
    case SB_BOTTOM: ScrollTo(geometry_.ContentHeight(TileCount())); break;
    case SB_THUMBTRACK:
    case SB_THUMBPOSITION: {
        SCROLLINFO info{};
        info.cbSize = sizeof(info);
        info.fMask = SIF_TRACKPOS;
        const int position = ::GetScrollInfo(grid_, SB_VERT, &info) ? info.nTrackPos : HIWORD(request);
        ScrollTo(position);
        break;
    }
    }
}

void Pane::Wheel(int delta, bool control) {
    if (control) {
        // Ctrl+wheel goes through the library's sizes, a notch at a time.
        wheelRest_ += delta;
        while (std::abs(wheelRest_) >= WHEEL_DELTA) {
            const int step = wheelRest_ > 0 ? 1 : -1;
            wheelRest_ -= step * WHEEL_DELTA;
            const auto now = std::ranges::find_if(kArtSizes, [this](const ArtSizeChoice& c) { return c.size >= ArtSize(); });
            const int at = static_cast<int>(now - std::begin(kArtSizes));
            SetCoverSize(kArtSizes[std::clamp(at + step, 0, static_cast<int>(std::size(kArtSizes)) - 1)].size);
        }
        return;
    }
    UINT lines = 3;
    ::SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
    const int step = lines == WHEEL_PAGESCROLL ? gridHeight_ : static_cast<int>(lines) * std::max(textHeight_, geometry_.cellHeight / 3);
    ScrollTo(scroll_ - delta * step / WHEEL_DELTA);
}

void Pane::ClickTile(int tile, bool control, bool shift) {
    if (tile < 0) return;
    if (shift && anchor_ >= 0) {
        std::vector<int> range(static_cast<size_t>(std::abs(tile - anchor_) + 1));
        std::iota(range.begin(), range.end(), std::min(tile, anchor_));
        SelectGroups(range, control);
    } else if (control) {
        if (Picked(tile)) DeselectGroup(tile);
        else SelectGroups({tile}, true);
        anchor_ = tile;
    } else {
        SelectGroups({tile}, false);
        anchor_ = tile;
    }
    focus_ = tile;
    InvalidateGrid();
}

// ---- Keyboard

bool Pane::Key(WPARAM key) {
    const int tiles = TileCount();
    if (tiles == 0) return false;
    const bool shift = ::GetKeyState(VK_SHIFT) < 0;
    const int columns = geometry_.columns;
    const int page = std::max(1, gridHeight_ / std::max(1, geometry_.cellHeight)) * columns;
    int tile = std::clamp(focus_, 0, tiles - 1);
    switch (key) {
    case VK_LEFT: tile -= 1; break;
    case VK_RIGHT: tile += 1; break;
    case VK_UP: tile -= columns; break;
    case VK_DOWN: tile += columns; break;
    case VK_PRIOR: tile -= page; break;
    case VK_NEXT: tile += page; break;
    case VK_HOME: tile = 0; break;
    case VK_END: tile = tiles - 1; break;
    case VK_RETURN:
        // (Where ml_playlists' own keys don't take it first.)
        SelectGroups({tile}, false);
        Press(kPlayId);
        return true;
    default:
        return false;
    }
    tile = std::clamp(tile, 0, tiles - 1);
    if (anchor_ < 0) anchor_ = focus_;
    ClickTile(tile, false, shift);
    EnsureTileVisible(tile);
    return true;
}

// ---- Menu

void Pane::ContextMenu(int screenX, int screenY) {
    POINT at{screenX, screenY};
    int tile = -1;
    if (screenX == -1 && screenY == -1) {
        tile = focus_ < TileCount() ? focus_ : -1;
        const Box cell = Moved(geometry_.Cell(std::max(0, tile)), 0, -scroll_);
        at = {cell.left + cell.Width() / 2, cell.top + cell.Height() / 2};
        ::ClientToScreen(grid_, &at);
    } else {
        POINT local = at;
        ::ScreenToClient(grid_, &local);
        tile = TileAt(local.x, local.y);
    }
    ::SetFocus(grid_);
    // As in Explorer: a right click on a cover that isn't picked picks it alone.
    if (tile >= 0 && !Picked(tile)) ClickTile(tile, false, false);

    const HMENU menu = ::CreatePopupMenu();
    const UINT onCover = tile >= 0 ? 0 : MF_GRAYED;
    ::AppendMenuW(menu, MF_STRING | onCover, kCmdPlay, L"Play");
    ::AppendMenuW(menu, MF_STRING | onCover, kCmdEnqueue, L"Enqueue");
    ::AppendMenuW(menu, MF_STRING | onCover, kCmdSelect, L"Select its songs");
    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    const HMENU sizes = ::CreatePopupMenu();
    const std::wstring asAudio = L"As in the Audio view (" + std::to_wstring(look_.coverSize) + L" pixels)";
    ::AppendMenuW(sizes, MF_STRING, kCmdArtSizeAsAudio, asAudio.c_str());
    ::AppendMenuW(sizes, MF_SEPARATOR, 0, nullptr);
    for (UINT i = 0; i < std::size(kArtSizes); ++i) ::AppendMenuW(sizes, MF_STRING, kCmdArtSize + i, kArtSizes[i].name);
    UINT checked = kCmdArtSizeAsAudio;
    if (const auto size = std::ranges::find(kArtSizes, settings_.coverSize, &ArtSizeChoice::size); size != std::end(kArtSizes)) {
        checked = kCmdArtSize + static_cast<UINT>(size - std::begin(kArtSizes));
    }
    ::CheckMenuRadioItem(sizes, kCmdArtSizeAsAudio, kCmdArtSize + static_cast<UINT>(std::size(kArtSizes)) - 1, checked,
                         MF_BYCOMMAND);
    ::AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(sizes), L"Cover size (Ctrl+wheel)");
    const HMENU orders = ::CreatePopupMenu();
    for (UINT i = 0; i < std::size(kOrderNames); ++i) ::AppendMenuW(orders, MF_STRING, kCmdOrder + i, kOrderNames[i]);
    ::CheckMenuRadioItem(orders, kCmdOrder, kCmdOrder + static_cast<UINT>(std::size(kOrderNames)) - 1,
                         kCmdOrder + static_cast<UINT>(settings_.order), MF_BYCOMMAND);
    ::AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(orders), L"Arrange covers");
    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(menu, MF_STRING | onCover, kCmdOpenFolder, L"Open folder");
    ::AppendMenuW(menu, MF_STRING | (playingGroup_ >= 0 ? 0 : MF_GRAYED), kCmdJumpToPlaying, L"Show the playing song");
    if (tile >= 0) ::SetMenuDefaultItem(menu, kCmdPlay, FALSE);

    const UINT command = static_cast<UINT>(
        ::TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, at.x, at.y, grid_, nullptr));
    ::DestroyMenu(menu);

    if (command == kCmdArtSizeAsAudio) {
        SetCoverSize(0);
    } else if (command >= kCmdArtSize && command < kCmdArtSize + std::size(kArtSizes)) {
        SetCoverSize(kArtSizes[command - kCmdArtSize].size);
    } else if (command >= kCmdOrder && command < kCmdOrder + std::size(kOrderNames)) {
        SetOrder(static_cast<int>(command - kCmdOrder));
    } else if (command == kCmdPlay || command == kCmdEnqueue) {
        Press(command == kCmdPlay ? kPlayId : kEnqueueId);
    } else if (command == kCmdSelect) {
        if (list_) ::SetFocus(list_);
    } else if (command == kCmdOpenFolder) {
        OpenFolder(tile);
    } else if (command == kCmdJumpToPlaying) {
        JumpToPlaying();
    }
}

} // namespace

bool RegisterPaneClass(HINSTANCE instance) {
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = Pane::Proc;
    windowClass.hInstance = instance;
    windowClass.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    windowClass.lpszClassName = kPaneClass;
    if (!::RegisterClassExW(&windowClass)) return false;
    windowClass.style = CS_DBLCLKS;
    windowClass.lpfnWndProc = Pane::GridProc;
    windowClass.lpszClassName = kGridClass;
    return ::RegisterClassExW(&windowClass) != 0;
}

void UnregisterPaneClass(HINSTANCE instance) {
    ::UnregisterClassW(kGridClass, instance);
    ::UnregisterClassW(kPaneClass, instance);
    FreeNoArtPictures();
}

bool IsPlaylistView(HWND dialog) {
    if (!dialog || !::IsWindow(dialog) || ClassOf(dialog) != L"#32770") return false;
    if (!::GetPropW(dialog, kTitleProp)) return false;
    const HWND list = ::GetDlgItem(dialog, kListId);
    return list && ClassOf(list) == L"SysListView32";
}

HWND AttachPane(HWND dialog, const PaneHost& host) {
    if (!IsPlaylistView(dialog)) return nullptr;
    if (const auto pane = static_cast<HWND>(::GetPropW(dialog, kPaneProp)); pane && ::IsWindow(pane)) return pane;
    const CreateParams params{dialog, ::GetDlgItem(dialog, kListId), &host};
    return ::CreateWindowExW(0, kPaneClass, L"", WS_CHILD | WS_CLIPSIBLINGS | WS_CLIPCHILDREN, 0, 0, 0, 0, dialog,
                             nullptr, host.instance, const_cast<CreateParams*>(&params));
}

void PaneResults(HWND pane, const Store::Taken& taken) {
    if (Pane* self = PaneOf(pane)) self->Results(taken);
}

void PanePlaybackChanged(HWND pane) {
    if (Pane* self = PaneOf(pane)) self->PlaybackChanged();
}

void PaneSkinChanged(HWND pane) {
    if (Pane* self = PaneOf(pane)) self->SkinChanged();
}

} // namespace covers
