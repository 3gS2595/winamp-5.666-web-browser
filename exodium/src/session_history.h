// A tab's own history, the pages its Back and Forward lead to, and how a copy of it is
// planned for a new tab. Ctrl+click on Back or Forward opens the page it leads to in a
// new tab with the same history, as in Chrome; back_forward.cpp carries the plan out.
// Nothing in here knows a window or a webview.
#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace exodium {

struct SessionEntry {
    int id = 0; // the webview's own number for it, which Page.navigateToHistoryEntry takes
    std::wstring url;
    std::wstring title;
};

struct SessionHistory {
    std::vector<SessionEntry> entries; // oldest first
    int current = -1;                  // the one in view
};

// Reads the DevTools protocol's answer to Page.getNavigationHistory. Nothing if it isn't
// one, or if it says nothing is in view.
[[nodiscard]] std::optional<SessionHistory> ParseSessionHistory(std::wstring_view json);

// What a new tab is given of another's history: the entries to make, oldest first, and
// the one it shows.
struct HistoryCopy {
    std::vector<SessionEntry> entries;
    std::size_t shown = 0;
};

// Whether an entry can be made again in another tab. Not the page a closed tab is put
// away on, which a reopened tab has ahead of it and which nobody is to go to, nor
// addresses that only meant something in the page that made them (script and blob).
[[nodiscard]] bool CanCopyEntry(std::wstring_view url) noexcept;

// The copy of `history` that shows the page Back leads to, or Forward with `forward`:
// the nearest entry that way that can be made again. The entries that can't are left
// out of it, and one with the address of the entry before it, which pushState leaves,
// is taken as that one: going to the address a tab is on makes no new entry. Nothing
// when there is no page that way.
[[nodiscard]] std::optional<HistoryCopy> PlanHistoryCopy(const SessionHistory& history, bool forward);

// Where the shown entry of `copy` ended up among the entries the new tab made, `made`.
// They are the same unless the webview left some out, which it does with an address it
// won't go to; then it is the entry with the shown one's address nearest its place.
// Nothing if there is none.
[[nodiscard]] std::optional<std::size_t> FindShownEntry(const SessionHistory& made, const HistoryCopy& copy);

// Where a click on Back or Forward opens the page it leads to, as in Chrome: in the tab
// itself; with Ctrl or the middle button, in a new tab behind the one in view, or in
// front of it with Shift as well; with Shift alone, in a new window.
enum class OpenIn { kSameTab, kBackgroundTab, kForegroundTab, kNewWindow };
[[nodiscard]] OpenIn WhereClickOpens(bool middleButton, bool ctrl, bool shift) noexcept;

// Whether `url` is one of the pages the plugin makes itself, such as kHistoryUrl.
[[nodiscard]] bool IsPluginPage(std::wstring_view url) noexcept;

// The page that stands in for an entry while a copy is made: blank and dark, with the
// entry's title. Should one ever come back from the back/forward cache, it loads its
// address again, which is then the real page.
[[nodiscard]] std::wstring StandInPageHtml(std::wstring_view title);

} // namespace exodium
