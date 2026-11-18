// Winamp's windows docked together come to the front together.
//
// Winamp's main window owns its other windows, the library and the equalizer among them, and
// Windows brings an owner's windows up with it whenever one of them is activated. A browser
// window has a taskbar button of its own, so nothing owns it, and it came up alone: docked to
// Winamp's windows, Alt+Tab to it left them behind whatever else was open, and Alt+Tab to
// Winamp left it behind. So whenever one of Winamp's windows is activated, the windows docked
// with it, as the glow sees docking (glow_shape.h's DockedGroups), are brought up right under
// it, in the order they were in, without taking the keyboard. Winamp's main window brings the
// windows it owns along, as it always does.
//
// Activation is seen by a WH_CALLWNDPROCRET hook on Winamp's thread, which sees WM_ACTIVATE
// reach any of its windows. The windows are raised from the message loop afterwards, once
// Windows is done bringing the activated one up.
#include "browser.h"

#include "glow_shape.h"

#include <algorithm>
#include <iterator>
#include <map>
#include <string>
#include <vector>

namespace exodium {

namespace {

constexpr std::wstring_view kWinampFrameClass = L"Winamp Gen"; // an embed frame: the library, a browser window...
constexpr std::wstring_view kDockingClasses[] = {
    L"Winamp EQ", L"Winamp PE", L"Winamp Video",
    L"BaseWindow_RootWnd", // a modern skin's windows
};

UniqueHook g_hook;
HWND g_activated = nullptr; // the window activated last, whose docked windows are to come up
bool g_raisePending = false;

[[nodiscard]] std::wstring ClassOf(HWND window) {
    wchar_t name[64];
    const int length = ::GetClassNameW(window, name, static_cast<int>(std::size(name)));
    return std::wstring(name, static_cast<std::size_t>(std::max(length, 0)));
}

[[nodiscard]] bool IsOurs(HWND window) noexcept {
    DWORD process = 0;
    ::GetWindowThreadProcessId(window, &process);
    return process == ::GetCurrentProcessId();
}

[[nodiscard]] bool IsTopmost(HWND window) noexcept {
    return (::GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
}

// The docking window an activated one stands for: itself, or the first of its owners that
// docks, as for a dialog of Winamp's.
[[nodiscard]] HWND DockingWindowOf(HWND window) {
    for (int depth = 0; window && depth < 16; ++depth, window = ::GetWindow(window, GW_OWNER)) {
        if (IsDockingWindow(window, ClassOf(window))) return window;
    }
    return nullptr;
}

// The windows of Winamp's on the screen that can be docked, from the top of the Z order
// down. Maximized and minimized ones aren't docked to anything.
[[nodiscard]] std::vector<HWND> DockingWindowsInSight() {
    std::vector<HWND> found;
    ::EnumWindows(
        [](HWND window, LPARAM list) -> BOOL {
            if (::IsWindowVisible(window) && !::IsIconic(window) && !::IsZoomed(window) && IsOurs(window) &&
                IsDockingWindow(window, ClassOf(window))) {
                reinterpret_cast<std::vector<HWND>*>(list)->push_back(window);
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&found));
    return found;
}

// How far down the Z order each top-level window is.
[[nodiscard]] std::map<HWND, int> ZOrder() {
    std::map<HWND, int> depth;
    ::EnumWindows(
        [](HWND window, LPARAM map) -> BOOL {
            auto& depths = *reinterpret_cast<std::map<HWND, int>*>(map);
            depths.emplace(window, static_cast<int>(depths.size()));
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&depth));
    return depth;
}

void RaiseDockedWith(HWND activated) {
    if (!::IsWindow(activated) || !::IsWindowVisible(activated)) return;
    const HWND docking = DockingWindowOf(activated);
    if (!docking || ::IsZoomed(docking) || ::IsIconic(docking)) return;

    const std::vector<HWND> windows = DockingWindowsInSight();
    std::vector<RECT> rects;
    rects.reserve(windows.size());
    for (const HWND window : windows) rects.push_back(WindowRectOf(window));
    const std::vector<int> groups = DockedGroups(rects);
    const auto at = std::ranges::find(windows, docking);
    if (at == windows.end()) return;
    const int group = groups[static_cast<std::size_t>(at - windows.begin())];

    // What comes up is each docked window's top-level owner, with all it owns: Winamp's main
    // window for the library and the equalizer, a browser window for itself. They go right
    // under the activated window's own, in the order they were in.
    const HWND top = ::GetAncestor(activated, GA_ROOTOWNER);
    std::vector<HWND> coming;
    for (std::size_t i = 0; i < windows.size(); ++i) {
        if (groups[i] != group) continue;
        const HWND owner = ::GetAncestor(windows[i], GA_ROOTOWNER);
        if (owner && owner != top && std::ranges::find(coming, owner) == coming.end()) coming.push_back(owner);
    }
    if (coming.empty()) return;
    const std::map<HWND, int> depth = ZOrder();
    const auto depthOf = [&depth](HWND window) {
        const auto found = depth.find(window);
        return found == depth.end() ? 0 : found->second;
    };
    std::ranges::sort(coming, std::ranges::greater{}, depthOf); // the lowest first
    constexpr UINT kFlags = SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE;
    for (const HWND window : coming) {
        // A window can't go under one of another band, topmost or not: it goes to the top of its own.
        const HWND after = IsTopmost(window) == IsTopmost(top) ? top : HWND_TOP;
        ::SetWindowPos(window, after, 0, 0, 0, 0, kFlags);
    }
}

// Sees each message sent to a window of Winamp's thread once the window is done with it.
LRESULT CALLBACK OnMessageDone(int code, WPARAM wParam, LPARAM lParam) {
    if (code == HC_ACTION) {
        const auto& sent = *reinterpret_cast<const CWPRETSTRUCT*>(lParam);
        if (sent.message == WM_ACTIVATE && LOWORD(sent.wParam) != WA_INACTIVE && !g_app.shutDown &&
            !g_app.windows.empty() && g_app.windows.front()->client) {
            g_activated = sent.hwnd;
            if (!g_raisePending) {
                g_raisePending = ::PostMessageW(g_app.windows.front()->client, kMsgRaiseDocked, 0, 0) != FALSE;
            }
        }
    }
    return ::CallNextHookEx(nullptr, code, wParam, lParam);
}

} // namespace

bool IsDockingWindow(HWND window, std::wstring_view className) noexcept {
    return window == g_app.winamp || className == kWinampFrameClass ||
           std::ranges::find(kDockingClasses, className) != std::ranges::end(kDockingClasses);
}

void StartRaisingDockedWindows() {
    if (g_hook || !g_app.winamp) return;
    const DWORD thread = ::GetWindowThreadProcessId(g_app.winamp, nullptr);
    g_hook.reset(::SetWindowsHookExW(WH_CALLWNDPROCRET, OnMessageDone, nullptr, thread));
}

void StopRaisingDockedWindows() {
    g_hook.reset();
    g_activated = nullptr;
}

void RaiseDockedWindows() {
    g_raisePending = false;
    if (g_app.shutDown) return;
    RaiseDockedWith(std::exchange(g_activated, nullptr));
}

} // namespace exodium
