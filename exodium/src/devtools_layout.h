// Where a tab's DevTools go when they are docked in its window, beside its page or under
// it, and what is kept of that between runs. Nothing here has a window in it; the panel
// itself is devtools_panel.cpp.
#pragma once

#include <windows.h>

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace exodium {

// The side of the page the DevTools are docked on.
enum class DockSide { Right, Bottom, Left };

// As winamp.ini keeps it, under devtools_dock.
[[nodiscard]] std::wstring_view DockSideName(DockSide side) noexcept;
// The side kept under a name, in any case; nothing for a name that isn't one.
[[nodiscard]] std::optional<DockSide> ParseDockSide(std::wstring_view name) noexcept;
// Whether the DevTools on this side are as tall as the page, and sized by their width.
[[nodiscard]] constexpr bool IsBesidePage(DockSide side) noexcept {
    return side != DockSide::Bottom;
}

// The bar between the page and the DevTools that sizes them when dragged, and the strip
// along the DevTools' top with the buttons that dock them elsewhere and close them.
inline constexpr int kDevToolsSplitterSize = 5;
inline constexpr int kDevToolsBarHeight = 24;
// The least the DevTools are made, and the least they leave of the page, along the side
// they are sized by.
inline constexpr int kDevToolsMinSize = 160;
inline constexpr int kDevToolsMinPage = 120;

// What the user has made of them: the side, and the size they were dragged to on it, the
// width beside the page and the height under it. 0 for one that was never dragged, which
// is two fifths of the room there is.
struct DevToolsPrefs {
    DockSide side = DockSide::Right;
    int width = 0;
    int height = 0;
    // The size of what the DevTools show, in hundredths: 100 is the normal size.
    int zoomPercent = 100;

    friend bool operator==(const DevToolsPrefs&, const DevToolsPrefs&) = default;
};
inline constexpr int kDevToolsZoomLeast = 50;
inline constexpr int kDevToolsZoomMost = 300;
// The next size up from `percent`, or down, of the sizes Ctrl with + and - step through.
[[nodiscard]] int NextDevToolsZoom(int percent, bool larger) noexcept;

// They are kept in winamp.ini's section for Exodium, a key to each: devtools_dock,
// devtools_width, devtools_height and devtools_zoom. `read` gives the value of a key, or
// nothing for one that isn't there; what isn't there, or is nothing it can be, is as it
// is at first.
struct DevToolsPrefEntry {
    std::wstring_view key;
    std::wstring value;

    friend bool operator==(const DevToolsPrefEntry&, const DevToolsPrefEntry&) = default;
};
[[nodiscard]] DevToolsPrefs
ReadDevToolsPrefs(const std::function<std::optional<std::wstring>(std::wstring_view key)>& read);
[[nodiscard]] std::vector<DevToolsPrefEntry> DevToolsPrefEntries(const DevToolsPrefs& prefs);

// The size on a side that `wanted` comes to in an area `room` long on that side: no less
// than kDevToolsMinSize and leaving the page kDevToolsMinPage, where there is room for
// both, and never more than there is room for with the splitter.
[[nodiscard]] int FitDevToolsSize(int room, int wanted) noexcept;

// The page's place, `area`, shared out between the page and its DevTools.
struct DevToolsSplit {
    RECT page{};
    RECT splitter{};
    RECT bar{};   // along the top of the DevTools
    RECT panel{}; // the DevTools themselves, under the bar
};
[[nodiscard]] DevToolsSplit SplitForDevTools(const RECT& area, const DevToolsPrefs& prefs) noexcept;

// The size the DevTools take on their side when the splitter is dragged until the edge of
// theirs that faces the page (the left one docked right, the top one docked at the bottom,
// the right one docked left) is at `edge`, in the coordinates of `area`.
[[nodiscard]] int DevToolsSizeForEdgeAt(const RECT& area, DockSide side, int edge) noexcept;
// That edge, as SplitForDevTools has it.
[[nodiscard]] int DevToolsEdge(const DevToolsSplit& split, DockSide side) noexcept;

// The buttons at the right end of the bar, from its right end: close, a window of their
// own, and the three sides.
enum class DevToolsButton { None, Close, OwnWindow, DockRight, DockBottom, DockLeft };
inline constexpr DevToolsButton kDevToolsButtons[] = {DevToolsButton::Close, DevToolsButton::OwnWindow,
                                                      DevToolsButton::DockRight, DevToolsButton::DockBottom,
                                                      DevToolsButton::DockLeft};
inline constexpr int kDevToolsButtonSize = 20;
inline constexpr int kDevToolsButtonGap = 2;
// Where a button is in a bar `barWidth` wide, in the bar's own coordinates. In a bar too
// narrow for them all, the ones furthest from its right end are left out: an empty place.
[[nodiscard]] RECT DevToolsButtonRect(int barWidth, DevToolsButton button) noexcept;
// The button at a point of the bar, if any.
[[nodiscard]] DevToolsButton DevToolsButtonAt(int barWidth, POINT point) noexcept;
// What a button says under the mouse.
[[nodiscard]] std::wstring_view DevToolsButtonTip(DevToolsButton button) noexcept;

// The address of the DevTools for the page a tool on the debugging port knows by
// `targetId`, served by that port itself. `panel` is the tool they open on ("console",
// "elements"), or empty for the one they showed last. Nothing for an id that isn't one:
// only what an id is made of goes into the address.
[[nodiscard]] std::optional<std::wstring> DevToolsUrl(unsigned port, std::wstring_view targetId,
                                                      std::wstring_view panel = {});
// Whether an address is one of the DevTools' own on that port, which is all their webview shows.
[[nodiscard]] bool IsDevToolsUrl(unsigned port, std::wstring_view url) noexcept;

// A point of the page's webview, in the pixels its bounds are in, as the place in the
// page's document the DevTools know it by: CSS pixels from the document's top left
// corner. `viewWidth` and `viewHeight` are the webview's size in those pixels, and the
// rest is the page's own account of itself: its window.innerWidth and innerHeight, and
// how far it is scrolled. Nothing if either has no size.
struct PageMetrics {
    double innerWidth = 0;
    double innerHeight = 0;
    double scrollX = 0;
    double scrollY = 0;
};
[[nodiscard]] std::optional<POINT> DocumentPointOf(POINT inView, int viewWidth, int viewHeight,
                                                   const PageMetrics& page) noexcept;
// The page's answer to kPageMetricsScript, which is a JSON array of the four numbers.
inline constexpr wchar_t kPageMetricsScript[] = L"[innerWidth, innerHeight, scrollX, scrollY]";
[[nodiscard]] std::optional<PageMetrics> ParsePageMetrics(std::wstring_view json) noexcept;

} // namespace exodium
