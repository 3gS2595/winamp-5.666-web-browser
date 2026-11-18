#include "session_file.h"

#include "text_util.h"

#include <algorithm>
#include <optional>
#include <ranges>

namespace exodium {

namespace {

constexpr std::wstring_view kHeader = L"exodium-session";
constexpr int kVersion = 1;

struct ShowName {
    WindowShow show;
    std::wstring_view name;
};
constexpr ShowName kShowNames[] = {
    {WindowShow::kHidden, L"hidden"},
    {WindowShow::kNormal, L"normal"},
    {WindowShow::kMinimized, L"minimized"},
    {WindowShow::kMaximized, L"maximized"},
};

[[nodiscard]] std::wstring_view NameOf(WindowShow show) noexcept {
    for (const ShowName& entry : kShowNames) {
        if (entry.show == show) return entry.name;
    }
    return L"normal";
}

[[nodiscard]] std::optional<WindowShow> ShowNamed(std::wstring_view name) noexcept {
    for (const ShowName& entry : kShowNames) {
        if (entry.name == name) return entry.show;
    }
    return std::nullopt;
}

[[nodiscard]] bool IsKeepableUrl(std::wstring_view url) noexcept {
    return !url.empty() && url.size() <= kMaxSavedUrl && !url.contains(L'\t') && !url.contains(L'\r') &&
           !url.contains(L'\n');
}

[[nodiscard]] std::wstring CleanTitle(std::wstring_view title) {
    return SingleLine(title, kMaxSavedTitle);
}

// A line with nothing on it has no columns, and is passed over.
[[nodiscard]] std::vector<std::wstring_view> Columns(std::wstring_view line) {
    return line.empty() ? std::vector<std::wstring_view>() : SplitColumns(line);
}

[[nodiscard]] int NumberIn(const std::vector<std::wstring_view>& columns, std::size_t index, int fallback = 0) {
    return index < columns.size() ? ParseNumber<int>(columns[index]).value_or(fallback) : fallback;
}

void WriteTab(std::wstring& text, std::wstring_view keyword, const HistoryCopy& tab) {
    const HistoryCopy pages = KeepablePages(tab);
    text.append(keyword).append(1, L'\t').append(std::to_wstring(pages.shown)).append(1, L'\n');
    for (const SessionEntry& page : pages.entries) {
        text.append(L"page\t").append(page.url).append(1, L'\t').append(page.title).append(1, L'\n');
    }
}

void WriteWindow(std::wstring& text, std::wstring_view keyword, const SavedWindow& window, bool withStacking) {
    text.append(keyword).append(1, L'\t').append(NameOf(window.show));
    for (const int value : {window.place.left, window.place.top, window.place.right, window.place.bottom}) {
        text.append(1, L'\t').append(std::to_wstring(value));
    }
    text.append(1, L'\t').append(std::to_wstring(window.active));
    if (withStacking) text.append(1, L'\t').append(std::to_wstring(window.stacking));
    text.append(1, L'\n');
    const std::size_t count = std::min(window.tabs.size(), kMaxSavedTabs);
    for (std::size_t i = 0; i < count; ++i) WriteTab(text, L"tab", window.tabs[i]);
}

[[nodiscard]] SavedWindow ReadWindow(const std::vector<std::wstring_view>& columns) {
    SavedWindow window;
    window.show = columns.size() > 1 ? ShowNamed(columns[1]).value_or(WindowShow::kNormal) : WindowShow::kNormal;
    window.place = {NumberIn(columns, 2), NumberIn(columns, 3), NumberIn(columns, 4), NumberIn(columns, 5)};
    window.active = static_cast<std::size_t>(std::max(0, NumberIn(columns, 6)));
    window.stacking = NumberIn(columns, 7);
    return window;
}

// The windows' and tabs' numbers, made to agree with what was read of them.
void Settle(SavedWindow& window) {
    for (HistoryCopy& tab : window.tabs) tab = KeepablePages(std::move(tab));
    if (window.active >= window.tabs.size()) window.active = window.tabs.empty() ? 0 : window.tabs.size() - 1;
}

} // namespace

HistoryCopy KeepablePages(HistoryCopy pages) {
    HistoryCopy kept;
    std::optional<std::size_t> shown;
    for (std::size_t i = 0; i < pages.entries.size(); ++i) {
        SessionEntry& page = pages.entries[i];
        if (IsKeepableUrl(page.url)) {
            kept.entries.push_back({.url = std::move(page.url), .title = CleanTitle(page.title)});
        }
        // The shown page, or the nearest kept one before it.
        if (i <= pages.shown && !kept.entries.empty()) shown = kept.entries.size() - 1;
    }
    if (kept.entries.empty()) return {};
    kept.shown = shown.value_or(0); // none before it: the nearest after it, which is the first kept

    if (kept.entries.size() > kMaxSavedPages) {
        // The pages round the shown one, as many behind it as can be up to half, and the rest ahead.
        const std::size_t size = kept.entries.size();
        const std::size_t first =
            std::min(kept.shown - std::min(kept.shown, kMaxSavedPages / 2), size - kMaxSavedPages);
        kept.entries.erase(kept.entries.begin() + static_cast<std::ptrdiff_t>(first + kMaxSavedPages),
                           kept.entries.end());
        kept.entries.erase(kept.entries.begin(), kept.entries.begin() + static_cast<std::ptrdiff_t>(first));
        kept.shown -= first;
    }
    return kept;
}

HistoryCopy KeptHistory(const SessionHistory& history) {
    HistoryCopy kept;
    std::optional<std::size_t> shown;
    for (int i = 0; i < static_cast<int>(history.entries.size()); ++i) {
        const SessionEntry& entry = history.entries[i];
        if (CanCopyEntry(entry.url) && (kept.entries.empty() || kept.entries.back().url != entry.url)) {
            kept.entries.push_back({.url = entry.url, .title = entry.title});
        }
        // The one in view, or the nearest kept one before it.
        if (i <= history.current && !kept.entries.empty()) shown = kept.entries.size() - 1;
    }
    kept.shown = shown.value_or(0);
    return KeepablePages(std::move(kept));
}

std::wstring SerializeSession(const SavedSession& session) {
    std::wstring text;
    text.append(kHeader).append(1, L'\t').append(std::to_wstring(kVersion)).append(1, L'\n');
    const std::size_t windows = std::min(session.windows.size(), kMaxSavedWindows);
    for (std::size_t i = 0; i < windows; ++i) WriteWindow(text, L"window", session.windows[i], true);

    // The latest of those closed, the last kept last.
    const std::size_t closed = session.closed.size();
    for (std::size_t i = closed - std::min(closed, kMaxSavedClosed); i < closed; ++i) {
        const SavedClosed& item = session.closed[i];
        if (item.wholeWindow) {
            WriteWindow(text, L"closed-window", item.window, false);
        } else if (!item.window.tabs.empty()) {
            WriteTab(text, L"closed-tab", item.window.tabs.front());
        }
    }
    return text;
}

SavedSession ParseSession(std::wstring_view text) {
    SavedSession session;
    SavedWindow* window = nullptr; // the window or closed window that tabs go in
    HistoryCopy* tab = nullptr;    // the tab or closed tab that pages go in
    bool headerSeen = false;

    for (const auto lineRange : text | std::views::split(L'\n')) {
        std::wstring_view line(lineRange);
        if (line.ends_with(L'\r')) line.remove_suffix(1);
        const std::vector<std::wstring_view> columns = Columns(line);
        if (columns.empty()) continue;
        const std::wstring_view keyword = columns[0];

        if (!headerSeen) {
            // A file of a later version may hold what this one can't read.
            if (keyword != kHeader || NumberIn(columns, 1, -1) != kVersion) return {};
            headerSeen = true;
            continue;
        }

        if (keyword == L"window" || keyword == L"closed-window") {
            const bool open = keyword == L"window";
            tab = nullptr;
            window = nullptr;
            if (open && session.windows.size() < kMaxSavedWindows) {
                window = &session.windows.emplace_back(ReadWindow(columns));
            } else if (!open) {
                window = &session.closed.emplace_back(SavedClosed{.wholeWindow = true, .window = ReadWindow(columns)})
                              .window;
            }
        } else if (keyword == L"closed-tab") {
            window = nullptr;
            tab = &session.closed.emplace_back().window.tabs.emplace_back();
            tab->shown = static_cast<std::size_t>(std::max(0, NumberIn(columns, 1)));
        } else if (keyword == L"tab") {
            tab = nullptr;
            if (window && window->tabs.size() < kMaxSavedTabs) {
                tab = &window->tabs.emplace_back();
                tab->shown = static_cast<std::size_t>(std::max(0, NumberIn(columns, 1)));
            }
        } else if (keyword == L"page") {
            // Many more than are kept is a file gone wrong; KeepablePages keeps the right ones of the rest.
            if (tab && columns.size() >= 2 && tab->entries.size() < 4 * kMaxSavedPages) {
                tab->entries.push_back(
                    {.url = std::wstring(columns[1]), .title = std::wstring(columns.size() > 2 ? columns[2] : L"")});
            }
        }
        // `window` and `tab` stay good: each is set afresh, or cleared, before the vector
        // it points into grows.
    }

    for (SavedWindow& open : session.windows) Settle(open);
    for (SavedClosed& item : session.closed) Settle(item.window);
    // A closed tab or window with nothing left in it brings nothing back.
    std::erase_if(session.closed, [](const SavedClosed& item) {
        return std::ranges::all_of(item.window.tabs, [](const HistoryCopy& tab) { return tab.entries.empty(); });
    });
    if (session.closed.size() > kMaxSavedClosed) {
        session.closed.erase(session.closed.begin(),
                             session.closed.end() - static_cast<std::ptrdiff_t>(kMaxSavedClosed));
    }
    return session;
}

} // namespace exodium
