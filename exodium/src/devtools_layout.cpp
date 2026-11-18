#include "devtools_layout.h"

#include "text_util.h"

#include <algorithm>
#include <cmath>
#include <iterator>

namespace exodium {

namespace {

struct SideName {
    DockSide side;
    std::wstring_view name;
};
constexpr SideName kSideNames[] = {
    {DockSide::Right, L"right"},
    {DockSide::Bottom, L"bottom"},
    {DockSide::Left, L"left"},
};

// How long `area` is along the side the DevTools are sized by.
[[nodiscard]] int RoomOn(const RECT& area, DockSide side) noexcept {
    return IsBesidePage(side) ? area.right - area.left : area.bottom - area.top;
}

constexpr std::wstring_view kDevToolsPage = L"/devtools/devtools_app.html";

constexpr std::wstring_view kDockKey = L"devtools_dock";
constexpr std::wstring_view kWidthKey = L"devtools_width";
constexpr std::wstring_view kHeightKey = L"devtools_height";
constexpr std::wstring_view kZoomKey = L"devtools_zoom";
// A size kept that is larger than any screen is nothing that was dragged to.
constexpr int kMostKeptSize = 20000;

[[nodiscard]] std::wstring PortOrigin(unsigned port) {
    return L"http://127.0.0.1:" + std::to_wstring(port);
}

} // namespace

std::wstring_view DockSideName(DockSide side) noexcept {
    const auto found = std::ranges::find(kSideNames, side, &SideName::side);
    return found == std::ranges::end(kSideNames) ? kSideNames[0].name : found->name;
}

std::optional<DockSide> ParseDockSide(std::wstring_view name) noexcept {
    const std::wstring_view word = Trim(name);
    const auto found =
        std::ranges::find_if(kSideNames, [word](const SideName& side) { return EqualsIgnoreCase(side.name, word); });
    if (found == std::ranges::end(kSideNames)) return std::nullopt;
    return found->side;
}

int NextDevToolsZoom(int percent, bool larger) noexcept {
    // The sizes Chrome steps through, between the least and the most there is here.
    constexpr int kSteps[] = {50, 67, 75, 80, 90, 100, 110, 125, 150, 175, 200, 250, 300};
    if (larger) {
        const auto next = std::ranges::upper_bound(kSteps, percent);
        return next == std::ranges::end(kSteps) ? kDevToolsZoomMost : *next;
    }
    const auto here = std::ranges::lower_bound(kSteps, percent);
    return here == std::ranges::begin(kSteps) ? kDevToolsZoomLeast : *(here - 1);
}

DevToolsPrefs ReadDevToolsPrefs(const std::function<std::optional<std::wstring>(std::wstring_view key)>& read) {
    DevToolsPrefs prefs;
    if (const auto side = read(kDockKey)) {
        if (const auto known = ParseDockSide(*side)) prefs.side = *known;
    }
    const auto number = [&read](std::wstring_view key, int least, int most, int otherwise) {
        const auto text = read(key);
        const auto value = text ? ParseNumber<int>(*text) : std::nullopt;
        return value && *value >= least && *value <= most ? *value : otherwise;
    };
    prefs.width = number(kWidthKey, 1, kMostKeptSize, 0);
    prefs.height = number(kHeightKey, 1, kMostKeptSize, 0);
    prefs.zoomPercent = number(kZoomKey, kDevToolsZoomLeast, kDevToolsZoomMost, 100);
    return prefs;
}

std::vector<DevToolsPrefEntry> DevToolsPrefEntries(const DevToolsPrefs& prefs) {
    return {
        {kDockKey, std::wstring(DockSideName(prefs.side))},
        {kWidthKey, std::to_wstring(prefs.width)},
        {kHeightKey, std::to_wstring(prefs.height)},
        {kZoomKey, std::to_wstring(prefs.zoomPercent)},
    };
}

int FitDevToolsSize(int room, int wanted) noexcept {
    const int most = std::max(0, room - kDevToolsSplitterSize);
    // Too little room for the least of both: they share what there is.
    if (most < kDevToolsMinSize + kDevToolsMinPage) return most / 2;
    if (wanted <= 0) wanted = room * 2 / 5;
    return std::clamp(wanted, kDevToolsMinSize, most - kDevToolsMinPage);
}

DevToolsSplit SplitForDevTools(const RECT& area, const DevToolsPrefs& prefs) noexcept {
    const DockSide side = prefs.side;
    const int size = FitDevToolsSize(RoomOn(area, side), IsBesidePage(side) ? prefs.width : prefs.height);
    DevToolsSplit split;
    RECT block = area; // the DevTools with their bar
    split.page = area;
    split.splitter = area;
    switch (side) {
    case DockSide::Right:
        block.left = area.right - size;
        split.splitter.right = block.left;
        split.splitter.left = std::max(area.left, block.left - kDevToolsSplitterSize);
        split.page.right = split.splitter.left;
        break;
    case DockSide::Left:
        block.right = area.left + size;
        split.splitter.left = block.right;
        split.splitter.right = std::min(area.right, block.right + kDevToolsSplitterSize);
        split.page.left = split.splitter.right;
        break;
    case DockSide::Bottom:
        block.top = area.bottom - size;
        split.splitter.bottom = block.top;
        split.splitter.top = std::max(area.top, block.top - kDevToolsSplitterSize);
        split.page.bottom = split.splitter.top;
        break;
    }
    split.bar = block;
    split.bar.bottom = std::min(block.bottom, block.top + kDevToolsBarHeight);
    split.panel = block;
    split.panel.top = split.bar.bottom;
    return split;
}

int DevToolsSizeForEdgeAt(const RECT& area, DockSide side, int edge) noexcept {
    int wanted = 0;
    switch (side) {
    case DockSide::Right:
        wanted = area.right - edge;
        break;
    case DockSide::Left:
        wanted = edge - area.left;
        break;
    case DockSide::Bottom:
        wanted = area.bottom - edge;
        break;
    }
    // Nothing, or less, is not "never dragged": it is as small as they get.
    return FitDevToolsSize(RoomOn(area, side), std::max(1, wanted));
}

int DevToolsEdge(const DevToolsSplit& split, DockSide side) noexcept {
    switch (side) {
    case DockSide::Left:
        return split.bar.right;
    case DockSide::Bottom:
        return split.bar.top;
    case DockSide::Right:
        break;
    }
    return split.bar.left;
}

RECT DevToolsButtonRect(int barWidth, DevToolsButton button) noexcept {
    const auto found = std::ranges::find(kDevToolsButtons, button);
    if (found == std::ranges::end(kDevToolsButtons)) return {};
    const int fromRight = static_cast<int>(found - std::ranges::begin(kDevToolsButtons));
    const int right = barWidth - kDevToolsButtonGap - fromRight * (kDevToolsButtonSize + kDevToolsButtonGap);
    const int left = right - kDevToolsButtonSize;
    if (left < kDevToolsButtonGap) return {};
    const int top = (kDevToolsBarHeight - kDevToolsButtonSize) / 2;
    return {left, top, right, top + kDevToolsButtonSize};
}

DevToolsButton DevToolsButtonAt(int barWidth, POINT point) noexcept {
    for (const DevToolsButton button : kDevToolsButtons) {
        const RECT rect = DevToolsButtonRect(barWidth, button);
        if (point.x >= rect.left && point.x < rect.right && point.y >= rect.top && point.y < rect.bottom) return button;
    }
    return DevToolsButton::None;
}

std::wstring_view DevToolsButtonTip(DevToolsButton button) noexcept {
    switch (button) {
    case DevToolsButton::Close:
        return L"Close DevTools (F12)";
    case DevToolsButton::OwnWindow:
        return L"Open in a window of their own";
    case DevToolsButton::DockRight:
        return L"Dock to the right";
    case DevToolsButton::DockBottom:
        return L"Dock to the bottom";
    case DevToolsButton::DockLeft:
        return L"Dock to the left";
    case DevToolsButton::None:
        break;
    }
    return {};
}

std::optional<std::wstring> DevToolsUrl(unsigned port, std::wstring_view targetId, std::wstring_view panel) {
    constexpr std::wstring_view kIdCharacters = L"0123456789ABCDEFabcdef-";
    constexpr std::wstring_view kPanelCharacters = L"abcdefghijklmnopqrstuvwxyz-_";
    if (port == 0 || port > 65535 || targetId.empty() || targetId.size() > 64 ||
        targetId.find_first_not_of(kIdCharacters) != std::wstring_view::npos ||
        panel.find_first_not_of(kPanelCharacters) != std::wstring_view::npos) {
        return std::nullopt;
    }
    // The port takes the page's own connection back to it from this address and no other:
    // by number, not as "localhost".
    const std::wstring host = L"127.0.0.1:" + std::to_wstring(port);
    std::wstring url = PortOrigin(port);
    url.append(kDevToolsPage).append(L"?ws=").append(host).append(L"/devtools/page/").append(targetId);
    if (!panel.empty()) url.append(L"&panel=").append(panel);
    return url;
}

bool IsDevToolsUrl(unsigned port, std::wstring_view url) noexcept {
    if (port == 0) return false;
    const std::wstring prefix = PortOrigin(port) + L"/devtools/";
    return url.starts_with(prefix);
}

std::optional<POINT> DocumentPointOf(POINT inView, int viewWidth, int viewHeight, const PageMetrics& page) noexcept {
    if (viewWidth <= 0 || viewHeight <= 0 || !(page.innerWidth > 0) || !(page.innerHeight > 0)) return std::nullopt;
    // The view's pixels are the page's, times the screen's scale and the page's zoom, which
    // the two widths have between them.
    const double x = inView.x * page.innerWidth / viewWidth + page.scrollX;
    const double y = inView.y * page.innerHeight / viewHeight + page.scrollY;
    if (!std::isfinite(x) || !std::isfinite(y) || std::abs(x) > 1e9 || std::abs(y) > 1e9) return std::nullopt;
    return POINT{static_cast<LONG>(std::floor(x)), static_cast<LONG>(std::floor(y))};
}

std::optional<PageMetrics> ParsePageMetrics(std::wstring_view json) noexcept {
    std::wstring_view text = Trim(json);
    if (text.size() < 2 || text.front() != L'[' || text.back() != L']') return std::nullopt;
    text = text.substr(1, text.size() - 2);
    double numbers[4] = {};
    std::size_t count = 0;
    while (true) {
        const std::size_t comma = text.find(L',');
        const std::wstring_view part = Trim(text.substr(0, comma));
        if (count == std::size(numbers) || part.empty() ||
            part.find_first_not_of(L"0123456789+-.eE") != std::wstring_view::npos) {
            return std::nullopt;
        }
        const auto number = ParseNumber<double>(part);
        if (!number || !std::isfinite(*number)) return std::nullopt;
        numbers[count++] = *number;
        if (comma == std::wstring_view::npos) break;
        text.remove_prefix(comma + 1);
    }
    if (count != std::size(numbers)) return std::nullopt;
    return PageMetrics{
        .innerWidth = numbers[0], .innerHeight = numbers[1], .scrollX = numbers[2], .scrollY = numbers[3]};
}

} // namespace exodium
