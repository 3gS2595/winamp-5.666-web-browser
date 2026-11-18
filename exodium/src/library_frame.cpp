// Winamp's library window wears the corner ornaments the browser windows wear (ornament.cpp).
//
// Its frame is one of Winamp's embed frames, as a browser window's is. gen_ml makes it as it
// starts, which is before Exodium does, and keeps it until Winamp exits. Everything about it
// stays Winamp's: Winamp draws, moves and sizes it as ever, and it keeps its place among
// Winamp's windows, with no taskbar button. It stands in App::library as a BrowserWindow that
// has the frame and the ornaments and nothing else, for ornament.cpp to treat it as it treats
// a browser window. A light subclass of the frame takes the ornaments wherever its corners go,
// and lets it be maximized and moved by them.
#include "browser.h"

#include <commctrl.h>

#include <memory>

namespace exodium {

namespace {

constexpr UINT_PTR kLibrarySubclassId = 1;

// The GUID gen_ml gives Winamp for its frame.
constexpr GUID kLibraryGuid{0x6b0edf80, 0xc9a5, 0x11d3, {0x9f, 0x26, 0x00, 0xc0, 0x4f, 0x39, 0xff, 0xc6}};

// Winamp's record of the frame, which gen_ml saves the library's place from as Winamp exits.
embedWindowState* g_libraryState = nullptr;

// A search of Winamp's frames for the library's. Winamp hands each to the function in the
// enumeration, which comes first, so that the function can find the rest.
struct LibrarySearch {
    embedEnumStruct enumeration;
    embedWindowState* found = nullptr;
};

int IsLibrary(embedWindowState* state, embedEnumStruct* enumeration) {
    if (!(state->flags & EMBED_FLAGS_GUID) || GET_EMBED_GUID(state) != kLibraryGuid) return 0;
    reinterpret_cast<LibrarySearch*>(enumeration)->found = state;
    return 1;
}

[[nodiscard]] embedWindowState* FindLibrary() noexcept {
    LibrarySearch search{.enumeration = {.enumProc = IsLibrary}};
    ::SendMessageW(g_app.winamp, WM_WA_IPC, reinterpret_cast<WPARAM>(&search.enumeration), IPC_EMBED_ENUM);
    return search.found;
}

[[nodiscard]] bool IsDockedIntoSkin(HWND frame) noexcept {
    return HasStyle(frame, WS_CHILD);
}

// Whether a move or resize of a maximized frame leaves it maximized: where it asks the frame
// to go is the maximized place on a screen, as maximizing it, or a change to the screens, does.
[[nodiscard]] bool StaysMaximized(HWND frame, const WINDOWPOS& position) noexcept {
    RECT to = WindowRectOf(frame);
    if (!(position.flags & SWP_NOMOVE)) ::OffsetRect(&to, position.x - to.left, position.y - to.top);
    if (!(position.flags & SWP_NOSIZE)) {
        to.right = to.left + position.cx;
        to.bottom = to.top + position.cy;
    }
    MONITORINFO monitor{.cbSize = sizeof(monitor)};
    if (!::GetMonitorInfoW(::MonitorFromRect(&to, MONITOR_DEFAULTTONEAREST), &monitor)) return true;
    const RECT place = MaximizedPlace(monitor);
    return ::EqualRect(&to, &place) != FALSE;
}

// gen_ml saves the library's place from Winamp's record of it as Winamp exits, by when the
// frame has gone. A maximized one is remembered at the place it goes back to, as a browser
// window is, and comes back there.
void RememberPlaceToGoBackTo(HWND frame) noexcept {
    WINDOWPLACEMENT placement{.length = sizeof(placement)};
    if (!g_libraryState || !::IsZoomed(frame) || !::GetWindowPlacement(frame, &placement)) return;
    // A placement is measured from the corner of the main screen's work area.
    MONITORINFO main{.cbSize = sizeof(main)};
    if (!::GetMonitorInfoW(::MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY), &main)) return;
    RECT place = placement.rcNormalPosition;
    ::OffsetRect(&place, main.rcWork.left - main.rcMonitor.left, main.rcWork.top - main.rcMonitor.top);
    g_libraryState->r = place;
}

LRESULT CALLBACK LibraryFrameProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, UINT_PTR subclassId,
                                  DWORD_PTR data) {
    BrowserWindow& library = *reinterpret_cast<BrowserWindow*>(data);
    switch (msg) {
    case WM_WINDOWPOSCHANGING: {
        // Moved by the ironwork, or by a window manager, as a browser window's frame is.
        AllowOutsideMoves(hwnd);
        // Maximized, it stays where it is until it is put back. Winamp moves and sizes it by
        // hand, from its title bar and its resize grip, and would leave it counted as
        // maximized wherever that took it.
        auto& position = *reinterpret_cast<WINDOWPOS*>(lParam);
        if (::IsZoomed(hwnd) && !StaysMaximized(hwnd, position)) position.flags |= SWP_NOMOVE | SWP_NOSIZE;
        break;
    }

    case WM_WINDOWPOSCHANGED: {
        const LRESULT result = ::DefSubclassProc(hwnd, msg, wParam, lParam);
        library.minimized = ::IsIconic(hwnd) != FALSE;
        // Winamp only follows the frame's place when it does the moving itself. Without this
        // a move by anything else would be forgotten when gen_ml saves it.
        if (g_libraryState && !library.minimized && !IsDockedIntoSkin(hwnd)) {
            ::GetWindowRect(hwnd, &g_libraryState->r);
        }
        UpdateOrnaments(library); // where the frame's corners go, and out of sight when the frame goes
        return result;
    }

    case WM_GETMINMAXINFO: {
        // Maximized, it has the work area, as a browser window does. How small it goes is
        // left as it was: Winamp's own business, and Jump to File Extra's when it shades it.
        const LRESULT result = ::DefSubclassProc(hwnd, msg, wParam, lParam);
        if (auto* limits = reinterpret_cast<MINMAXINFO*>(lParam)) {
            const POINT smallest = limits->ptMinTrackSize;
            FitMaximized(hwnd, *limits);
            limits->ptMinTrackSize = smallest;
        }
        return result;
    }

    case WM_LBUTTONUP: {
        const LRESULT result = ::DefSubclassProc(hwnd, msg, wParam, lParam);
        SpendJumpToFileWidth(hwnd);
        return result;
    }

    case WM_DESTROY:
        RememberPlaceToGoBackTo(hwnd);
        library.ornaments.windows = {}; // before the frame that owns them takes them along
        break;

    case WM_NCDESTROY: {
        ::RemoveWindowSubclass(hwnd, LibraryFrameProc, subclassId);
        g_libraryState = nullptr;
        const LRESULT result = ::DefSubclassProc(hwnd, msg, wParam, lParam);
        g_app.library.reset(); // and `library` with it
        return result;
    }

    default:
        break;
    }
    return ::DefSubclassProc(hwnd, msg, wParam, lParam);
}

} // namespace

void AttachLibraryOrnaments() {
    if (g_app.library) return;
    embedWindowState* const state = FindLibrary();
    if (!state || !::IsWindow(state->me)) return;

    auto library = std::make_unique<BrowserWindow>();
    library->frame = state->me;
    library->isLibrary = true;
    g_libraryState = state;
    // It outlives the subclass: the frame's WM_NCDESTROY and DetachLibraryOrnaments end both.
    ::SetWindowSubclass(library->frame, LibraryFrameProc, kLibrarySubclassId,
                        reinterpret_cast<DWORD_PTR>(library.get()));
    g_app.library = std::move(library);
    CreateOrnaments(*g_app.library);
}

void DetachLibraryOrnaments() {
    if (!g_app.library) return;
    const HWND frame = g_app.library->frame;
    RememberPlaceToGoBackTo(frame);
    g_app.library->ornaments.windows = {};
    ::RemoveWindowSubclass(frame, LibraryFrameProc, kLibrarySubclassId);
    g_libraryState = nullptr;
    g_app.library.reset();
}

void MinimizeWinamp() noexcept {
    // Posted, as Winamp's own minimize button is pressed: once the click is done with.
    // Winamp minimizes, or goes to the tray, as its settings say.
    ::PostMessageW(g_app.winamp, WM_SYSCOMMAND, SC_MINIMIZE, 0);
}

void CloseLibrary() noexcept {
    // Winamp's close button on the frame closes the window in it, and gen_ml hides the library.
    if (!g_app.library) return;
    if (const HWND inside = ::FindWindowExW(g_app.library->frame, nullptr, nullptr, nullptr)) {
        ::PostMessageW(inside, WM_CLOSE, 0, 0);
    }
}

} // namespace exodium
