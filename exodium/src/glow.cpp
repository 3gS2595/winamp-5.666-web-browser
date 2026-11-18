// The black glow round Winamp's windows while Winamp is in front, as Exodium's page in
// Winamp's Preferences asks for it (Settings::glow). What it looks like, and which of the
// windows glow together, is glow_shape.h's to say; this finds the windows and shows it.
//
// It is done on a thread of its own, out of Winamp's way: a picture of the glow can be the
// size of the screen, and working one out takes a while, which Winamp's thread is better
// left to move the windows in. The glow's thread never waits on Winamp's. Of the browser it
// touches only the ornaments' outlines, which Winamp's thread hands it under a lock; the rest
// it finds out from Windows, which answers for any thread. Windows' accessibility events tell
// it of Winamp's windows moving, showing and hiding, and of any window coming to the front.
// While the glow is shown it looks again four times a second as well, for what no event
// tells of, such as another program's window coming between Winamp's in the Z order, and
// once a second while it isn't. The glow is shown while Winamp is in front, or always, as
// the settings ask (Settings::glowAlways).
//
// A move comes as a burst of them: a window's ornaments follow it, and Winamp moves the windows
// docked to the one dragged, one after another. The glow lets a burst end before it looks, so
// it never sees them apart. A picture whose windows have all moved alike is the same picture
// somewhere else, and its window is only moved, which takes next to no time; it is painted
// again only when something in it changed.
//
// Each picture is a window of its own: see-through but for the glow, which lets the mouse
// through, can't be activated, and has no taskbar button. Its pixels over the windows docked
// with those it goes round are left clear, so it never darkens one of them, whatever comes of
// the Z order; a window that isn't docked with them has it lying over it, as a shadow.
#include "browser.h"

#include "glow_shape.h"

#include <atomic>
#include <mutex>

namespace exodium {

namespace {

constexpr wchar_t kGlowClass[] = L"ExodiumGlow";       // a picture of the glow
constexpr wchar_t kGlowHubClass[] = L"ExodiumGlowHub"; // the thread's own window, for messages

constexpr UINT kMsgLook = WM_APP + 1; // something may have changed: look again
constexpr UINT_PTR kWatchTimer = 1;
constexpr UINT kWatchMs = 250; // while the glow is shown
constexpr UINT kIdleMs = 1000; // while it isn't

// Winamp's window classes, which the glow goes round when it goes round all of Winamp, as it
// does Winamp's main window, whose class Winamp can be started with another name for.
constexpr std::wstring_view kWinampFrameClass = L"Winamp Gen"; // an embed frame: the library, a browser window...
constexpr std::wstring_view kWinampClasses[] = {
    L"Winamp EQ", L"Winamp PE", L"Winamp Video",
    L"BaseWindow_RootWnd", // a modern skin's windows
};

// ---- shared with Winamp's thread ----

// What is solid of a see-through window of the browser's, a corner ornament: the alpha of
// each of its pixels, top row first.
struct Outline {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> alpha;
    unsigned version = 0; // a new one for every outline, for the glow to see that it changed
};
std::mutex g_outlinesLock;
std::map<HWND, std::shared_ptr<const Outline>> g_outlines; // under the lock
unsigned g_lastOutlineVersion = 0;                         // under the lock

std::atomic<GlowAround> g_around{GlowAround::Nothing};
std::atomic<bool> g_always{false}; // shown whether Winamp is in front or not
std::mutex g_lookLock;
GlowLook g_look; // under the lock

[[nodiscard]] GlowLook CurrentLook() {
    const std::lock_guard lock(g_lookLock);
    return g_look;
}
std::atomic<HWND> g_hub{nullptr}; // the glow thread's window, while it runs
HWND g_winampMain = nullptr;      // set before the glow thread starts
DWORD g_winampThread = 0;         // and the thread it belongs to
HANDLE g_thread = nullptr;        // Winamp's thread's alone

void LookSoonFromOutside() noexcept {
    if (const HWND hub = g_hub.load()) ::PostMessageW(hub, kMsgLook, 0, 0);
}

// ---- the glow's thread ----

// A picture of the glow, and the window it is shown in.
struct GlowWindow {
    UniqueWindow window;
    UniqueBitmap bitmap;
    std::uint32_t* bits = nullptr;
    SIZE size{};
    bool painted = false;               // has a picture, to be shown
    std::vector<std::intptr_t> basis;   // what it was painted from; see Basis
    RECT at{};                          // where it is
};

struct GlowState {
    std::vector<GlowWindow> windows; // one for each run of windows glowing together, and spares
    GlowScratch scratch;
    bool lookPending = false;
    UINT watchMs = 0; // how often it looks again, when nothing tells it to
    LARGE_INTEGER lastEvent{}; // when Windows last told of a window moving, by QueryPerformanceCounter
    HANDLE settleTimer = nullptr; // for waiting out a burst of moves to the fraction of a millisecond
};
GlowState* g_glow = nullptr; // the glow thread's, while it runs

using UniqueRegion = UniqueHandle<HRGN, &::DeleteObject>;

[[nodiscard]] bool IsOurProcess(HWND window) noexcept {
    DWORD process = 0;
    ::GetWindowThreadProcessId(window, &process);
    return process == ::GetCurrentProcessId();
}

// One of Winamp's windows, or a window another program shows for one of them, as WebView2
// does for a page's drop-down lists and the like.
[[nodiscard]] bool IsWinampWindow(HWND window) noexcept {
    if (IsOurProcess(window)) return true;
    const HWND owner = ::GetAncestor(window, GA_ROOTOWNER);
    return owner && owner != window && IsOurProcess(owner);
}

[[nodiscard]] bool WinampInFront() noexcept {
    if (const HWND front = ::GetForegroundWindow()) return IsWinampWindow(front);
    // Windows tells of no window in front on a desktop that isn't the one on the screen, as the
    // smoke test's is. There Winamp's thread having an active window says it: a thread that
    // isn't in front has none.
    GUITHREADINFO info{.cbSize = sizeof(info)};
    return ::GetGUIThreadInfo(g_winampThread, &info) && info.hwndActive && IsWinampWindow(info.hwndActive);
}

[[nodiscard]] bool IsGlowWindow(HWND window) noexcept {
    return g_glow && std::ranges::any_of(g_glow->windows, [window](const GlowWindow& glow) {
               return glow.window.get() == window;
           });
}

[[nodiscard]] std::wstring ClassOf(HWND window) {
    wchar_t name[64];
    const int length = ::GetClassNameW(window, name, static_cast<int>(std::size(name)));
    return std::wstring(name, static_cast<std::size_t>(std::max(length, 0)));
}

[[nodiscard]] bool IsBrowserFrame(HWND frame) noexcept {
    return ::FindWindowExW(frame, nullptr, kClientClass, nullptr) != nullptr;
}

// Whether the glow goes round one of Winamp's own windows. Not round a maximized one, whose
// glow would be off the screen but for a line along its edge.
[[nodiscard]] bool Casts(HWND window, std::wstring_view className, GlowAround around) noexcept {
    if (::IsZoomed(window)) return false;
    if (className == kOrnamentClass) {
        const HWND frame = ::GetWindow(window, GW_OWNER);
        if (!frame || ::IsZoomed(frame)) return false;
        return around == GlowAround::Winamp || IsBrowserFrame(frame);
    }
    if (className == kWinampFrameClass) return around == GlowAround::Winamp || IsBrowserFrame(window);
    return around == GlowAround::Winamp && (window == g_winampMain || std::ranges::find(kWinampClasses, className) !=
                                                                          std::ranges::end(kWinampClasses));
}



// The rectangles a region is made of.
[[nodiscard]] std::vector<RECT> PartsOf(HRGN region) {
    const DWORD bytes = ::GetRegionData(region, 0, nullptr);
    if (bytes < sizeof(RGNDATAHEADER)) return {};
    std::vector<std::byte> buffer(bytes);
    auto* const data = reinterpret_cast<RGNDATA*>(buffer.data());
    if (::GetRegionData(region, bytes, data) != bytes) return {};
    const auto* const rects = reinterpret_cast<const RECT*>(data->Buffer);
    return std::vector<RECT>(rects, rects + data->rdh.nCount);
}

// A window as the glow sees it.
struct Seen {
    HWND window = nullptr;
    StackedWindow stacked;
    bool docks = false;       // one of Winamp's that docks with the others; see IsDockingWindow
    std::vector<RECT> region; // on the screen, what its window region keeps; none for all of it
    UniqueRegion shape;       // for Winamp's: the same as a region
    std::shared_ptr<const Outline> outline; // for a see-through one
};

[[nodiscard]] std::shared_ptr<const Outline> OutlineOf(HWND window, const RECT& rect) {
    const std::lock_guard lock(g_outlinesLock);
    const auto found = g_outlines.find(window);
    if (found == g_outlines.end()) return nullptr;
    const Outline& outline = *found->second;
    if (outline.width != Width(rect) || outline.height != Height(rect)) return nullptr; // being changed
    return found->second;
}

[[nodiscard]] GlowShape ShapeOf(const Seen& seen, const std::vector<RECT>& parts) {
    GlowShape shape{.rect = seen.stacked.rect, .parts = parts};
    if (seen.outline) shape.alpha = seen.outline->alpha;
    return shape;
}

// Gives each of Winamp's windows its group: the windows that dock are grouped by which are
// docked together, and the rest go with the window that owns them, as an ornament goes with
// its frame and a dialog with Winamp's main window. A window that belongs to none of them is a
// group of its own.
void GroupDockedWindows(std::vector<Seen>& seen) {
    std::vector<RECT> docking;
    std::vector<std::size_t> placeOf;
    for (std::size_t i = 0; i < seen.size(); ++i) {
        if (!seen[i].docks) continue;
        docking.push_back(seen[i].stacked.rect);
        placeOf.push_back(i);
    }
    const std::vector<int> groups = DockedGroups(docking);
    std::map<HWND, int> groupOf;
    int next = 0;
    for (std::size_t k = 0; k < placeOf.size(); ++k) {
        Seen& one = seen[placeOf[k]];
        one.stacked.group = groups[k];
        groupOf[one.window] = groups[k];
        next = std::max(next, groups[k] + 1);
    }
    for (Seen& one : seen) {
        if (one.stacked.role == GlowRole::Other || one.docks) continue;
        int group = -1;
        HWND owner = one.window;
        for (int depth = 0; depth < 16 && group < 0; ++depth) {
            owner = ::GetWindow(owner, GW_OWNER);
            if (!owner) break;
            if (const auto found = groupOf.find(owner); found != groupOf.end()) group = found->second;
        }
        one.stacked.group = group >= 0 ? group : next++;
    }
}

// The windows on the screen, from the top of the Z order down, but for those the glow can
// leave out: hidden, minimized, on another virtual desktop, or another program's see-through
// overlays that let the mouse through, which may cover the whole screen and show nothing.
[[nodiscard]] std::vector<Seen> LookAtWindows(GlowAround around) {
    std::vector<HWND> stack;
    ::EnumWindows(
        [](HWND window, LPARAM list) -> BOOL {
            reinterpret_cast<std::vector<HWND>*>(list)->push_back(window);
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&stack));

    std::vector<Seen> seen;
    const UniqueRegion scratch(::CreateRectRgn(0, 0, 0, 0));
    for (const HWND window : stack) {
        if (!::IsWindowVisible(window) || ::IsIconic(window) || IsGlowWindow(window)) continue;
        DWORD cloaked = 0;
        if (SUCCEEDED(::DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked) continue;
        const RECT rect = WindowRectOf(window);
        if (::IsRectEmpty(&rect)) continue;

        if (!IsWinampWindow(window)) {
            const LONG_PTR extended = ::GetWindowLongPtrW(window, GWL_EXSTYLE);
            if ((extended & WS_EX_LAYERED) && (extended & (WS_EX_TRANSPARENT | WS_EX_NOACTIVATE))) continue;
            seen.push_back({.window = window, .stacked = {rect, GlowRole::Other}});
            continue;
        }

        const std::wstring className = ClassOf(window);
        const bool ours = IsOurProcess(window);
        Seen& one = seen.emplace_back();
        one.window = window;
        one.stacked = {rect, ours && Casts(window, className, around) ? GlowRole::Casts : GlowRole::Winamp};
        one.docks = ours && IsDockingWindow(window, className);
        if (className == kOrnamentClass) {
            one.outline = OutlineOf(window, rect);
            if (!one.outline) {
                seen.pop_back(); // its picture is being changed; it will be looked at again then
                continue;
            }
        }
        // A skin can shape Winamp's windows with a window region, in the window's own coordinates.
        one.shape.reset(::CreateRectRgnIndirect(&rect));
        if (const int kind = ::GetWindowRgn(window, scratch.get()); kind == SIMPLEREGION || kind == COMPLEXREGION) {
            ::OffsetRgn(scratch.get(), rect.left, rect.top);
            ::CombineRgn(one.shape.get(), one.shape.get(), scratch.get(), RGN_AND);
            one.region = PartsOf(one.shape.get());
        }
    }
    GroupDockedWindows(seen);
    return seen;
}

// A window a run's picture is kept off, and the parts of it to keep it off.
struct Clear {
    std::size_t index;
    std::vector<RECT> parts;
};

// What a run's picture is kept off: the windows of its own group, the run's and those under
// it, as far as no other window lies over them between the picture and them. Windows of other
// groups, and other programs', have the glow lying over them. Windows above the run aren't
// looked at: they lie over the picture as well, so what is under them isn't seen either way,
// and they can move about without the picture being painted again, as menus, a tab being
// dragged, and other programs' windows over Winamp's when the glow is always shown do.
[[nodiscard]] std::vector<Clear> KeptClearFor(const std::vector<Seen>& seen, const GlowRun& run) {
    std::vector<Clear> clear;
    const UniqueRegion over(::CreateRectRgn(0, 0, 0, 0)); // windows between the picture and the one looked at
    const UniqueRegion visible(::CreateRectRgn(0, 0, 0, 0));
    for (std::size_t i = run.casting.front(); i < seen.size(); ++i) {
        const Seen& one = seen[i];
        if (one.stacked.role != GlowRole::Other && one.stacked.group == run.group) {
            if (one.shape && ::CombineRgn(visible.get(), one.shape.get(), over.get(), RGN_DIFF) != NULLREGION) {
                clear.push_back({i, PartsOf(visible.get())});
            }
        } else if (i > run.bottom) {
            if (one.shape) {
                ::CombineRgn(over.get(), over.get(), one.shape.get(), RGN_OR);
            } else {
                const UniqueRegion area(::CreateRectRgnIndirect(&one.stacked.rect));
                ::CombineRgn(over.get(), over.get(), area.get(), RGN_OR);
            }
        }
    }
    return clear;
}

// Everything a run's picture is painted from, as numbers, to paint it again only when that
// changes. Places are taken from the picture's corner, so windows that have all moved alike
// make the same numbers, and the picture is only moved.
[[nodiscard]] std::vector<std::intptr_t> Basis(const std::vector<Seen>& seen, const GlowRun& run,
                                               const std::vector<Clear>& clear, const RECT& area,
                                               const GlowLook& look) {
    std::vector<std::intptr_t> basis;
    const auto addRect = [&basis, &area](const RECT& rect) {
        basis.insert(basis.end(), {rect.left - area.left, rect.top - area.top, rect.right - area.left, rect.bottom - area.top});
    };
    const auto addShape = [&](const Seen& one, const std::vector<RECT>& parts) {
        basis.push_back(reinterpret_cast<std::intptr_t>(one.window));
        addRect(one.stacked.rect);
        basis.push_back(one.outline ? static_cast<std::intptr_t>(one.outline->version) : 0);
        basis.push_back(static_cast<std::intptr_t>(parts.size()));
        for (const RECT& part : parts) addRect(part);
    };
    addRect(area); // its size
    basis.insert(basis.end(), {look.size, look.opacity, look.spread, static_cast<std::intptr_t>(look.color)});
    basis.push_back(static_cast<std::intptr_t>(run.casting.size()));
    for (const std::size_t index : run.casting) addShape(seen[index], seen[index].region);
    basis.push_back(-1);
    for (const Clear& kept : clear) addShape(seen[kept.index], kept.parts);
    return basis;
}

[[nodiscard]] RECT VirtualScreen() noexcept {
    const int left = ::GetSystemMetrics(SM_XVIRTUALSCREEN), top = ::GetSystemMetrics(SM_YVIRTUALSCREEN);
    return {left, top, left + ::GetSystemMetrics(SM_CXVIRTUALSCREEN), top + ::GetSystemMetrics(SM_CYVIRTUALSCREEN)};
}

[[nodiscard]] GlowWindow MakeGlowWindow() {
    GlowWindow glow;
    glow.window.reset(::CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                                        kGlowClass, L"", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, g_app.instance,
                                        nullptr));
    return glow;
}

void Hide(GlowWindow& glow) noexcept {
    if (glow.window && ::IsWindowVisible(glow.window.get())) ::ShowWindow(glow.window.get(), SW_HIDE);
}

// Paints the glow round `casting` into a window's picture, and moves the window over `area`.
void Paint(GlowWindow& glow, const RECT& area, std::span<const GlowShape> casting,
           std::span<const GlowShape> keptClear, const GlowLook& look) {
    if (!glow.window) return;
    SIZE size{Width(area), Height(area)};
    const WindowDc screen(nullptr);
    if (!glow.bitmap || glow.size.cx != size.cx || glow.size.cy != size.cy) {
        glow.painted = false;
        glow.bitmap.reset();
        glow.bits = nullptr;
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(info.bmiHeader);
        info.bmiHeader.biWidth = size.cx;
        info.bmiHeader.biHeight = -size.cy; // top row first
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        glow.bitmap.reset(::CreateDIBSection(screen.get(), &info, DIB_RGB_COLORS, &bits, nullptr, 0));
        if (!glow.bitmap || !bits) {
            glow.bitmap.reset();
            return;
        }
        glow.bits = static_cast<std::uint32_t*>(bits);
        glow.size = size;
    }
    const std::size_t count = static_cast<std::size_t>(size.cx) * static_cast<std::size_t>(size.cy);
    PaintGlow(area, casting, keptClear, look, std::span(glow.bits, count), g_glow->scratch);
    ::GdiFlush();

    const UniqueDc dc(::CreateCompatibleDC(screen.get()));
    const SelectedObject selected(dc.get(), glow.bitmap.get());
    POINT place{area.left, area.top}, origin{};
    BLENDFUNCTION blend{.BlendOp = AC_SRC_OVER, .SourceConstantAlpha = 255, .AlphaFormat = AC_SRC_ALPHA};
    glow.painted = ::UpdateLayeredWindow(glow.window.get(), screen.get(), &place, &size, dc.get(), &origin, 0, &blend,
                                         ULW_ALPHA) != FALSE;
    glow.at = area;
}

// Moves a picture that is still right to where it now goes.
void MoveTo(GlowWindow& glow, const RECT& area) noexcept {
    if (::EqualRect(&glow.at, &area)) return;
    ::SetWindowPos(glow.window.get(), nullptr, area.left, area.top, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    glow.at = area;
}

[[nodiscard]] bool IsTopmost(HWND window) noexcept {
    return (::GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
}

// Shows a picture of the glow right under the lowest window of its run.
void PutUnder(GlowWindow& glow, HWND lowest) noexcept {
    const HWND window = glow.window.get();
    if (!window || !glow.painted) return;
    const bool topmost = IsTopmost(lowest);
    if (::IsWindowVisible(window) && ::GetWindow(lowest, GW_HWNDNEXT) == window && IsTopmost(window) == topmost) return;
    constexpr UINT kFlags = SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER;
    if (IsTopmost(window) != topmost) {
        // Out of sight while it changes bands, rather than over everything for a moment.
        ::SetWindowPos(window, topmost ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, kFlags | SWP_HIDEWINDOW);
    }
    ::SetWindowPos(window, lowest, 0, 0, 0, 0, kFlags | SWP_SHOWWINDOW);
}

// Has it look again every `ms`.
void Watch(UINT ms) noexcept {
    GlowState& glow = *g_glow;
    if (std::exchange(glow.watchMs, ms) == ms) return;
    ::SetTimer(g_hub.load(), kWatchTimer, ms, nullptr);
}

void Look() {
    GlowState& glow = *g_glow;
    const GlowAround around = g_around.load();
    if (around == GlowAround::Nothing || (!g_always.load() && !WinampInFront())) {
        for (GlowWindow& window : glow.windows) Hide(window);
        Watch(kIdleMs);
        return;
    }
    Watch(kWatchMs);

    const std::vector<Seen> seen = LookAtWindows(around);
    std::vector<StackedWindow> stack;
    stack.reserve(seen.size());
    for (const Seen& one : seen) stack.push_back(one.stacked);
    const GlowLook look = CurrentLook();
    const int reach = GlowGridFor(look.size).reach();
    const std::vector<GlowRun> runs = FindGlowRuns(stack, reach);
    const RECT screen = VirtualScreen();

    while (glow.windows.size() < runs.size()) glow.windows.push_back(MakeGlowWindow());
    for (std::size_t i = 0; i < glow.windows.size(); ++i) {
        GlowWindow& window = glow.windows[i];
        if (i >= runs.size()) {
            Hide(window);
            continue;
        }
        std::vector<GlowShape> casting;
        for (const std::size_t index : runs[i].casting) casting.push_back(ShapeOf(seen[index], seen[index].region));
        const RECT area = GlowArea(casting, reach, screen);
        if (::IsRectEmpty(&area)) {
            Hide(window);
            continue;
        }
        const std::vector<Clear> clear = KeptClearFor(seen, runs[i]);
        std::vector<std::intptr_t> basis = Basis(seen, runs[i], clear, area, look);
        if (window.painted && basis == window.basis) {
            MoveTo(window, area);
        } else {
            std::vector<GlowShape> keptClear;
            keptClear.reserve(clear.size());
            for (const Clear& kept : clear) keptClear.push_back(ShapeOf(seen[kept.index], kept.parts));
            Paint(window, area, casting, keptClear, look);
            window.basis = std::move(basis);
        }
        PutUnder(window, seen[runs[i].bottom].window);
    }
}

void LookSoon() noexcept {
    GlowState& glow = *g_glow;
    if (std::exchange(glow.lookPending, true)) return;
    ::PostMessageW(g_hub.load(), kMsgLook, 0, 0);
}

void CALLBACK OnWinEvent(HWINEVENTHOOK, DWORD event, HWND window, LONG object, LONG child, DWORD, DWORD) {
    if (!g_glow) return;
    if (event != EVENT_SYSTEM_FOREGROUND) {
        // Winamp's windows: not their controls, the caret or the glow's own.
        if (!window || object != OBJID_WINDOW || child != CHILDID_SELF || IsGlowWindow(window)) return;
        if (::GetWindowLongPtrW(window, GWL_STYLE) & WS_CHILD) return;
    }
    ::QueryPerformanceCounter(&g_glow->lastEvent);
    LookSoon();
}

[[nodiscard]] double MsSince(const LARGE_INTEGER& then) noexcept {
    LARGE_INTEGER now, frequency;
    ::QueryPerformanceCounter(&now);
    ::QueryPerformanceFrequency(&frequency);
    return static_cast<double>(now.QuadPart - then.QuadPart) * 1000.0 / static_cast<double>(frequency.QuadPart);
}

// Waits for a burst of moves to end: until Windows has told of none for kQuietMs, or for
// kLongestMs at most, as a drag's own moves may never leave so long between them. Says whether
// to look then; not once the thread is to end.
bool Settle() {
    constexpr double kQuietMs = 1.0, kLongestMs = 12;
    GlowState& glow = *g_glow;
    LARGE_INTEGER start;
    ::QueryPerformanceCounter(&start);
    for (;;) {
        // Windows hands its news over as the thread takes messages. Another look, or the watch's
        // tick, is this one.
        MSG msg;
        while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                ::PostQuitMessage(static_cast<int>(msg.wParam));
                return false;
            }
            if (msg.hwnd == g_hub.load() && (msg.message == kMsgLook || msg.message == WM_TIMER)) continue;
            ::TranslateMessage(&msg);
            ::DispatchMessageW(&msg);
        }
        if (!g_hub.load()) return false;
        const double quiet = MsSince(glow.lastEvent);
        if (quiet >= kQuietMs || MsSince(start) >= kLongestMs) return true;
        LARGE_INTEGER due{};
        due.QuadPart = -static_cast<LONGLONG>((kQuietMs - quiet) * 10000.0) - 1; // relative, in 100 ns
        if (glow.settleTimer && ::SetWaitableTimer(glow.settleTimer, &due, 0, nullptr, nullptr, FALSE)) {
            ::WaitForSingleObject(glow.settleTimer, 50);
        } else {
            ::Sleep(1);
        }
    }
}

LRESULT CALLBACK HubProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case kMsgLook:
        if (g_glow && Settle()) {
            g_glow->lookPending = false;
            Look();
        }
        return 0;
    case WM_TIMER:
        if (g_glow && wParam == kWatchTimer) Look();
        return 0;
    case WM_CLOSE:
        ::DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        ::PostQuitMessage(0);
        return 0;
    default:
        return ::DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

DWORD WINAPI GlowThread(void* ready) {
    const WNDCLASSEXW hubClass{.cbSize = sizeof(WNDCLASSEXW), .lpfnWndProc = HubProc, .hInstance = g_app.instance,
                               .lpszClassName = kGlowHubClass};
    const WNDCLASSEXW glowClass{.cbSize = sizeof(WNDCLASSEXW), .lpfnWndProc = ::DefWindowProcW,
                                .hInstance = g_app.instance, .lpszClassName = kGlowClass};
    ::RegisterClassExW(&hubClass);
    ::RegisterClassExW(&glowClass);

    GlowState state;
    g_glow = &state;
    // To the fraction of a millisecond where Windows has it (Windows 10 1803 on), and to its
    // timer's tick otherwise.
    constexpr DWORD kHighResolution = 0x00000002; // CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
    state.settleTimer = ::CreateWaitableTimerExW(nullptr, nullptr, kHighResolution, TIMER_ALL_ACCESS);
    if (!state.settleTimer) state.settleTimer = ::CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
    const HWND hub = ::CreateWindowExW(0, kGlowHubClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, g_app.instance, nullptr);
    std::vector<HWINEVENTHOOK> hooks;
    if (hub) {
        const DWORD process = ::GetCurrentProcessId();
        constexpr DWORD kOutOfContext = WINEVENT_OUTOFCONTEXT;
        // Any window coming to the front, and Winamp's moving, showing, hiding and being minimized.
        hooks.push_back(::SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, OnWinEvent, 0, 0,
                                          kOutOfContext));
        hooks.push_back(::SetWinEventHook(EVENT_SYSTEM_MOVESIZESTART, EVENT_SYSTEM_MINIMIZEEND, nullptr, OnWinEvent,
                                          process, 0, kOutOfContext));
        hooks.push_back(::SetWinEventHook(EVENT_OBJECT_SHOW, EVENT_OBJECT_HIDE, nullptr, OnWinEvent, process, 0,
                                          kOutOfContext));
        hooks.push_back(::SetWinEventHook(EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE, nullptr, OnWinEvent,
                                          process, 0, kOutOfContext));
        g_hub = hub;
        Watch(kIdleMs);
        ::PostMessageW(hub, kMsgLook, 0, 0);
    }
    ::SetEvent(static_cast<HANDLE>(ready));

    if (hub) {
        MSG msg;
        while (::GetMessageW(&msg, nullptr, 0, 0) > 0) {
            ::TranslateMessage(&msg);
            ::DispatchMessageW(&msg);
        }
    }

    for (const HWINEVENTHOOK hook : hooks) {
        if (hook) ::UnhookWinEvent(hook);
    }
    g_hub = nullptr;
    if (::IsWindow(hub)) ::DestroyWindow(hub);
    state.windows.clear();
    if (state.settleTimer) ::CloseHandle(state.settleTimer);
    g_glow = nullptr;
    ::UnregisterClassW(kGlowClass, g_app.instance);
    ::UnregisterClassW(kGlowHubClass, g_app.instance);
    return 0;
}

void StartGlowThread() {
    g_winampMain = g_app.winamp;
    g_winampThread = ::GetWindowThreadProcessId(g_app.winamp, nullptr);
    const HANDLE ready = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ready) return;
    g_thread = ::CreateThread(nullptr, 0, GlowThread, ready, 0, nullptr);
    if (g_thread) ::WaitForSingleObject(ready, INFINITE);
    ::CloseHandle(ready);
}

void StopGlowThread() {
    if (!g_thread) return;
    if (const HWND hub = g_hub.load()) ::PostMessageW(hub, WM_CLOSE, 0, 0);
    // Taking any message the thread sends meanwhile, though it is made to send none.
    while (::MsgWaitForMultipleObjects(1, &g_thread, FALSE, INFINITE, QS_SENDMESSAGE) == WAIT_OBJECT_0 + 1) {
        MSG msg;
        ::PeekMessageW(&msg, nullptr, 0, 0, PM_NOREMOVE | PM_QS_SENDMESSAGE);
    }
    ::CloseHandle(std::exchange(g_thread, nullptr));
}

} // namespace

void FollowGlow() {
    const GlowAround around = g_app.winamp && !g_app.shutDown ? g_app.settings.glow : GlowAround::Nothing;
    g_around = around;
    g_always = g_app.settings.glowAlways;
    {
        const std::lock_guard lock(g_lookLock);
        g_look = g_app.settings.glowLook;
    }
    if (around == GlowAround::Nothing) {
        StopGlowThread();
        return;
    }
    if (!g_thread) StartGlowThread();
    else LookSoonFromOutside();
}

void StopGlow() {
    g_around = GlowAround::Nothing;
    StopGlowThread();
}

void SetGlowOutline(HWND window, std::span<const std::uint32_t> picture, int width) {
    if (width <= 0) return;
    auto outline = std::make_shared<Outline>();
    outline->width = width;
    outline->height = static_cast<int>(picture.size() / static_cast<std::size_t>(width));
    outline->alpha.resize(static_cast<std::size_t>(outline->width) * static_cast<std::size_t>(outline->height));
    std::ranges::transform(picture.first(outline->alpha.size()), outline->alpha.begin(),
                           [](std::uint32_t pixel) { return static_cast<std::uint8_t>(pixel >> 24); });
    {
        const std::lock_guard lock(g_outlinesLock);
        std::shared_ptr<const Outline>& kept = g_outlines[window];
        // A button lit up under the mouse changes no outline.
        if (kept && kept->width == outline->width && kept->height == outline->height && kept->alpha == outline->alpha) {
            return;
        }
        outline->version = ++g_lastOutlineVersion;
        kept = std::move(outline);
    }
    LookSoonFromOutside();
}

void ForgetGlowOutline(HWND window) {
    const std::lock_guard lock(g_outlinesLock);
    g_outlines.erase(window);
}

} // namespace exodium
