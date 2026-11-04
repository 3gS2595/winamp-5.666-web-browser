// Claude Code's windows. Its browser tools (tools\exodium_mcp.mjs) open them on
// kToolStartPage, so they are known from the start, and two things set them apart:
//
// - They come up without the keyboard, behind every other window (but in front of the
//   desktop). A session may open one at any moment, and it would otherwise take over from
//   whatever the user is doing. Behind other windows a page carries on drawing, so
//   Claude's work isn't held up; the user can bring one forward from the taskbar.
// - They go when their session does. The tools give their process's id in the start
//   page's address, and the session's tabs, the pages theirs opened included, are closed
//   once that process has ended, however it ended. Otherwise every session would leave a
//   window behind.
#include "browser.h"

#include "embed_frame.h"

namespace exodium {

namespace {

// Posted to Winamp's main window, from the thread pool, when a session's process ends.
UINT SessionEndedMessage() {
    static const UINT message = ::RegisterWindowMessageW(L"Exodium.ClaudeSessionEnded");
    return message;
}

void CALLBACK OnProcessEnded(void* pid, BOOLEAN) {
    ::PostMessageW(g_app.winamp, SessionEndedMessage(), reinterpret_cast<WPARAM>(pid), 0);
}

void Unwatch(App::ClaudeSession& session) {
    // Waits for a call of OnProcessEnded under way, so that none comes after this.
    if (session.wait) ::UnregisterWaitEx(session.wait, INVALID_HANDLE_VALUE);
    if (session.process) ::CloseHandle(session.process);
    session = {};
}

// The desktop's own windows, which lie under all the others: the shell's, and the WorkerW
// windows that hold the wallpaper and the icons just above it.
[[nodiscard]] bool IsDesktopWindow(HWND hwnd) {
    wchar_t name[16] = {};
    ::GetClassNameW(hwnd, name, static_cast<int>(std::size(name)));
    return hwnd == ::GetShellWindow() || std::wstring_view(name) == L"WorkerW";
}

// What a window put at the back goes in behind: the window just above the desktop's,
// which may be that window itself. Behind the desktop it would never be seen. Without a
// desktop, as on a desktop of its own, the very back.
[[nodiscard]] HWND InFrontOfTheDesktop() {
    const HWND shell = ::GetShellWindow();
    if (!shell) return HWND_BOTTOM;
    HWND desktop = shell;
    for (HWND above = ::GetWindow(shell, GW_HWNDPREV); above && IsDesktopWindow(above);
         above = ::GetWindow(above, GW_HWNDPREV)) {
        desktop = above;
    }
    const HWND after = ::GetWindow(desktop, GW_HWNDPREV);
    return after ? after : HWND_TOP; // nothing but the desktop: in front of it is the top
}

} // namespace

void ShowWithoutFocus(BrowserWindow& win) {
    // Shown and put in its place at once. ShowWindow, even without activating, brings a
    // window to the front as it shows it.
    UINT flags = SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW;
    const HWND after = InFrontOfTheDesktop();
    if (after == win.frame) flags |= SWP_NOZORDER;
    ::SetWindowPos(win.frame, after == win.frame ? nullptr : after, 0, 0, 0, 0, flags);
    // Which ShowWindow would have seen to, by way of WM_SHOWWINDOW.
    if (win.menuId) embed::CheckMenuItems(*win.menuId, true);
}

void WatchClaudeSession(std::uint32_t pid) {
    if (pid == 0 || g_app.claudeSessions.contains(pid) || SessionEndedMessage() == 0) return;
    App::ClaudeSession session;
    session.process = ::OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!session.process) {
        // Gone already, and its window with it. Any other failure, such as a session
        // running as administrator, leaves the window to be closed by hand.
        if (::GetLastError() == ERROR_INVALID_PARAMETER) ::PostMessageW(g_app.winamp, SessionEndedMessage(), pid, 0);
        return;
    }
    if (!::RegisterWaitForSingleObject(&session.wait, session.process, OnProcessEnded,
                                       reinterpret_cast<void*>(static_cast<UINT_PTR>(pid)), INFINITE,
                                       WT_EXECUTEONLYONCE)) {
        ::CloseHandle(session.process);
        return;
    }
    g_app.claudeSessions.emplace(pid, session);
}

bool OnClaudeSessionMessage(UINT msg, WPARAM wParam) {
    if (msg == 0 || msg != SessionEndedMessage()) return false;
    const auto pid = static_cast<std::uint32_t>(wParam);
    if (const auto found = g_app.claudeSessions.find(pid); found != g_app.claudeSessions.end()) {
        Unwatch(found->second);
        g_app.claudeSessions.erase(found);
    }
    if (g_app.shutDown) return true;
    // Closing a window only hides it until the message loop comes round, so the list of
    // windows stays as it is while this goes through it.
    for (const auto& win : g_app.windows) {
        std::vector<int> ids;
        for (const Tab& tab : win->tabs) {
            if (tab.claudeSession == pid) ids.push_back(tab.id);
        }
        if (!ids.empty()) CloseTabs(*win, ids);
    }
    return true;
}

void StopWatchingClaudeSessions() {
    for (auto& [pid, session] : g_app.claudeSessions) Unwatch(session);
    g_app.claudeSessions.clear();
}

} // namespace exodium
