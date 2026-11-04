// Ctrl+N in Winamp's own windows: a new browser window, as Ctrl+N in a browser window
// opens one, wherever Winamp doesn't use the key itself. What decides is winamp_keys.h;
// this is the part that watches the keys and finds out where they were pressed.
//
// The keys are watched with a WH_GETMESSAGE hook on Winamp's thread, which sees each
// message as any loop on the thread takes it from the queue, before anything is done with
// it: before Winamp's loop looks it up in the shortcut tables of the window that has the
// keyboard. A key the hook leaves be goes on to that as it always did. Subclassing would
// take every window that can have the keyboard, which in the Media Library is any of a
// great many controls, and Winamp offers plugins no way in of its own:
// IPC_TRANSLATEACCELERATOR has done nothing since 5.53.
#include "browser.h"

#include "winamp_keys.h"

#include <algorithm>
#include <iterator>
#include <optional>
#include <ranges>
#include <span>
#include <string_view>
#include <vector>

namespace exodium {

namespace {

static_assert(kTableVirtualKey == FVIRTKEY && kTableShift == FSHIFT && kTableControl == FCONTROL && kTableAlt == FALT);

UniqueHook g_hook;
// Winamp's tables that have Ctrl+N, read the first time it is pressed. A language pack,
// which can bring tables of its own, only changes with a restart.
std::optional<std::vector<WinampKeyTable>> g_tablesWithKey;

[[nodiscard]] std::optional<bool> HasNewWindowKey(HACCEL accelerators) {
    const int count = ::CopyAcceleratorTableW(accelerators, nullptr, 0);
    if (count <= 0) return std::nullopt;
    std::vector<ACCEL> entries(static_cast<std::size_t>(count));
    ::CopyAcceleratorTableW(accelerators, entries.data(), count);
    std::vector<TableKey> keys;
    for (const ACCEL& entry : entries) keys.push_back({.flags = entry.fVirt, .key = entry.key});
    return TableHasNewWindowKey(keys);
}

// From wherever Winamp takes its tables: the language pack, if it has them, or else
// winamp.exe. Tables loaded from resources go when their module does, and aren't ours to
// destroy. A table that can't be read is taken to be as it is in Winamp 5.666.
[[nodiscard]] std::span<const WinampKeyTable> TablesWithKey() {
    if (g_tablesWithKey) return *g_tablesWithKey;
    const auto language =
        reinterpret_cast<HINSTANCE>(::SendMessageW(g_app.winamp, WM_WA_IPC, 0, IPC_GETLANGUAGEPACKINSTANCE));
    const HINSTANCE winamp = ::GetModuleHandleW(nullptr);
    std::vector<WinampKeyTable> found;
    for (const WinampKeyTable table : kWinampKeyTables) {
        const wchar_t* const id = MAKEINTRESOURCEW(static_cast<WORD>(table));
        HACCEL accelerators = language ? ::LoadAcceleratorsW(language, id) : nullptr;
        if (!accelerators) accelerators = ::LoadAcceleratorsW(winamp, id);
        const std::optional<bool> has = accelerators ? HasNewWindowKey(accelerators) : std::nullopt;
        if (has.value_or(std::ranges::contains(kTablesWithNewWindowKeyIn5666, table))) found.push_back(table);
    }
    return g_tablesWithKey.emplace(std::move(found));
}

[[nodiscard]] HWND WinampWindowOf(WPARAM which) noexcept {
    return reinterpret_cast<HWND>(::SendMessageW(g_app.winamp, WM_WA_IPC, which, IPC_GETWND));
}

// One of the browser's windows: a frame, or a window of a class of its own, such as a
// corner ornament, which its frame owns but which is a window of its own.
[[nodiscard]] bool IsBrowserWindow(HWND root, std::wstring_view className) noexcept {
    for (const auto& win : g_app.windows) {
        if (win->frame == root) return true;
    }
    for (const std::wstring_view ours : {kClientClass, kFullscreenClass, kTabGhostClass, kOrnamentClass}) {
        if (className == ours) return true;
    }
    return false;
}

// The kind of window a key was pressed in, from the window that has the keyboard: what is
// round it, Winamp's own windows by their handles and the rest by their classes.
[[nodiscard]] WinampWindow KindOfWindow(HWND focus) {
    const HWND root = ::GetAncestor(focus, GA_ROOT);
    if (!root) return WinampWindow::kOther;
    wchar_t name[64] = {};
    const int length = ::GetClassNameW(root, name, static_cast<int>(std::size(name)));
    const std::wstring_view className(name, static_cast<std::size_t>(std::max(length, 0)));
    if (IsBrowserWindow(root, className)) return WinampWindow::kBrowser;
    if (root == g_app.winamp) return WinampWindow::kMain;
    if (root == WinampWindowOf(IPC_GETWND_PE)) return WinampWindow::kPlaylistEditor;
    if (root == WinampWindowOf(IPC_GETWND_EQ)) return WinampWindow::kEqualizer;
    if (root == WinampWindowOf(IPC_GETWND_VIDEO)) return WinampWindow::kVideo;
    return WinampWindowOfClass(className);
}

// Winamp is in the middle of something the key may be meant for.
[[nodiscard]] bool WinampIsBusy() {
    const DWORD thread = ::GetCurrentThreadId();
    GUITHREADINFO gui{.cbSize = sizeof(gui)};
    constexpr DWORD kMenuOrDrag = GUI_INMENUMODE | GUI_POPUPMENUMODE | GUI_SYSTEMMENUMODE | GUI_INMOVESIZE;
    if (::GetGUIThreadInfo(thread, &gui) && ((gui.flags & kMenuOrDrag) || gui.hwndCapture)) return true;

    // A modal dialog or a message box disables the window it belongs to, and so does a
    // file dialog. No window of Winamp's in sight is disabled otherwise.
    bool disabled = false;
    ::EnumThreadWindows(
        thread,
        [](HWND window, LPARAM found) -> BOOL {
            if (!::IsWindowVisible(window) || ::IsWindowEnabled(window)) return TRUE;
            *reinterpret_cast<bool*>(found) = true;
            return FALSE;
        },
        reinterpret_cast<LPARAM>(&disabled));
    if (disabled) return true;

    // Winamp's Preferences aren't modal, but while they are open the key is left alone too.
    const auto prefs = reinterpret_cast<HWND>(::SendMessageW(g_app.winamp, WM_WA_IPC, 0, IPC_GETPREFSWND));
    return prefs && ::IsWindowVisible(prefs);
}

// A key going down, as Winamp's thread takes it from its queue. Says whether it opened a
// window, in which case nothing else is to have it.
bool TakeNewWindowKey(const MSG& msg) {
    if (g_app.shutDown || !msg.hwnd) return false;
    // The rest takes some finding out, and only Ctrl+N is worth it.
    const Modifiers mods = CurrentModifiers();
    if (mods != kCtrl) return false;
    const WinampKeyPress press{
        .key = static_cast<unsigned>(msg.wParam),
        .ctrl = mods.ctrl,
        .shift = mods.shift,
        .alt = mods.alt,
        .window = KindOfWindow(msg.hwnd),
        .busy = WinampIsBusy(),
        .enabled = g_app.settings.ctrlNInWinamp,
        .tablesWithKey = TablesWithKey(),
    };
    if (!OpensBrowserWindow(press)) return false;
    // The browser's own Ctrl+N, from no window of its own: a window where new ones go,
    // brought to the front with the keyboard in it.
    HandleGlobalShortcut(nullptr, msg.wParam, mods);
    return true;
}

LRESULT CALLBACK OnMessageTaken(int code, WPARAM removed, LPARAM lParam) {
    // A message only looked at is seen again when it is taken.
    if (code == HC_ACTION && (removed & PM_REMOVE)) {
        MSG& msg = *reinterpret_cast<MSG*>(lParam);
        // Winamp's loop is left nothing to look up or hand on.
        if (msg.message == WM_KEYDOWN && msg.wParam == 'N' && TakeNewWindowKey(msg)) msg.message = WM_NULL;
    }
    return ::CallNextHookEx(nullptr, code, removed, lParam);
}

} // namespace

void FollowWinampNewWindowKey() {
    const bool wanted = g_app.settings.ctrlNInWinamp && g_app.winamp && !g_app.shutDown;
    if (wanted == static_cast<bool>(g_hook)) return;
    if (!wanted) {
        g_hook.reset();
        return;
    }
    // On Winamp's thread, where its windows and the browser's are.
    const DWORD thread = ::GetWindowThreadProcessId(g_app.winamp, nullptr);
    g_hook.reset(::SetWindowsHookExW(WH_GETMESSAGE, OnMessageTaken, nullptr, thread));
}

void StopWinampNewWindowKey() {
    g_hook.reset();
}

} // namespace exodium
