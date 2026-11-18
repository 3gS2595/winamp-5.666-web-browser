// The browser's windows and tabs as they are kept on disk from one run of Winamp to the
// next, with the pages each tab's Back and Forward lead to, and the windows and tabs that
// were closed and that Ctrl+Shift+T can still bring back. Nothing here knows a window or a
// webview: session.cpp turns the browser into one of these and back again.
//
// The file is text, a line to a record and tabs between its columns:
//
//   exodium-session  1
//   window           normal  100  90  1000  728  1  0   how it was shown, its place, the tab in view, its stacking
//   tab              2                                  the page it shows, of those that follow
//   page             https://example.com/  Example Domain
//   closed-window    maximized  ...  1                  closed whole: shown, place and the tab in view
//   closed-tab       0                                  closed on its own: the page it shows
//
// A `tab` belongs to the window or closed window before it, and a `page` to the tab or
// closed tab before it. A line the reader doesn't know is passed over.
#pragma once

#include "session_history.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace exodium {

// How a window was on the screen.
enum class WindowShow { kHidden, kNormal, kMinimized, kMaximized };

// A place on the screen, in its pixels.
struct SavedPlace {
    int left = 0, top = 0, right = 0, bottom = 0;

    friend bool operator==(const SavedPlace&, const SavedPlace&) = default;
};

struct SavedWindow {
    WindowShow show = WindowShow::kNormal;
    SavedPlace place; // where it goes back to from being maximized or minimized
    // Its place among the browser's windows from the front, 0 being in front of the rest.
    int stacking = 0;
    // In strip order, each with its pages oldest first and the one it shows. One with no
    // pages is a new tab that hadn't been anywhere.
    std::vector<HistoryCopy> tabs;
    std::size_t active = 0; // the tab in view
};

// Closed and not yet brought back: a window with all its tabs, or a tab on its own, which
// is kept as a window with that tab alone.
struct SavedClosed {
    bool wholeWindow = false;
    SavedWindow window;
};

struct SavedSession {
    std::vector<SavedWindow> windows; // open; the first is the window Winamp's menu shows
    std::vector<SavedClosed> closed;  // the one closed last, last
};

// What is kept at most, so that a file gone wrong can't take the browser down with it.
inline constexpr std::size_t kMaxSavedWindows = 64;
inline constexpr std::size_t kMaxSavedTabs = 500;    // in a window
inline constexpr std::size_t kMaxSavedPages = 50;    // of a tab, as many as a webview keeps
inline constexpr std::size_t kMaxSavedClosed = 25;   // a window closed whole counts as one
inline constexpr std::size_t kMaxSavedUrl = 8192;    // longer addresses, data: ones mostly, are left out
inline constexpr std::size_t kMaxSavedTitle = 200;

// A tab's pages made fit to be kept: those whose address can't be (empty, too long, with
// a tab or line break in it) are left out, titles are cut to kMaxSavedTitle and lose
// their tabs and line breaks, and of more than kMaxSavedPages the ones round the shown
// page are kept. The shown page stays the one it was, or becomes the nearest kept one
// before it, or else after it. Nothing is left if no page can be kept.
[[nodiscard]] HistoryCopy KeepablePages(HistoryCopy pages);

// What of a tab's history, as its webview has it, is kept for the tab to be given again:
// the pages that can be made again in another webview (CanCopyEntry), one for each run
// of the same address, which pushState leaves, with the one in view shown.
[[nodiscard]] HistoryCopy KeptHistory(const SessionHistory& history);

[[nodiscard]] std::wstring SerializeSession(const SavedSession& session);
// Whatever of `text` can be read; nothing at all if it isn't a session file.
[[nodiscard]] SavedSession ParseSession(std::wstring_view text);

} // namespace exodium
