#include "history.h"

#include "text_util.h"
#include "url_util.h"

#include <algorithm>
#include <chrono>
#include <iterator>
#include <ranges>
#include <unordered_set>

namespace exodium {

namespace {

constexpr double kSecondsPerDay = 86400.0;

// Titles are kept one to a line of the history file, between tabs.
std::wstring CleanTitle(std::wstring_view title) {
    std::wstring clean(title.substr(0, History::kMaxTitleLength));
    std::ranges::replace_if(clean, [](wchar_t c) { return c == L'\t' || c == L'\r' || c == L'\n'; }, L' ');
    return clean;
}

std::vector<std::wstring_view> SplitColumns(std::wstring_view line) {
    return line | std::views::split(L'\t') |
           std::views::transform([](auto&& column) { return std::wstring_view(column); }) |
           std::ranges::to<std::vector>();
}

} // namespace

std::int64_t UnixNow() noexcept {
    const auto sinceEpoch = std::chrono::system_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::seconds>(sinceEpoch).count();
}

double Frecency(const HistoryEntry& entry, std::int64_t now) noexcept {
    // Not dated: taken as a month old.
    const double days = entry.lastVisit ? static_cast<double>(now - entry.lastVisit) / kSecondsPerDay : 30.0;
    const double age = days <= 4 ? 1.0 : days <= 14 ? 0.7 : days <= 31 ? 0.5 : days <= 90 ? 0.3 : 0.1;
    return (std::min(entry.visits, 20) + 4.0 * std::min(entry.typed, 20)) * age;
}

std::vector<HistoryEntry>::iterator History::Locate(std::wstring_view url) noexcept {
    return std::ranges::find_if(entries_,
                                [url](const HistoryEntry& entry) { return EqualsIgnoreCase(entry.url, url); });
}

const HistoryEntry* History::Find(std::wstring_view url) const noexcept {
    const auto found =
        std::ranges::find_if(entries_, [url](const HistoryEntry& entry) { return EqualsIgnoreCase(entry.url, url); });
    return found == entries_.end() ? nullptr : std::to_address(found);
}

void History::RecordVisit(std::wstring_view url, std::wstring_view title, bool typed, std::int64_t now) {
    if (!IsLikelyHttpUrl(url) || url == kHistoryUrl) return;

    if (const auto known = Locate(url); known != entries_.end()) {
        std::rotate(entries_.begin(), known, known + 1);
    } else {
        entries_.insert(entries_.begin(), HistoryEntry{.url = std::wstring(url)});
    }
    HistoryEntry& entry = entries_.front();
    ++entry.visits;
    if (typed) ++entry.typed;
    entry.lastVisit = now;
    if (!title.empty()) entry.title = CleanTitle(title);

    if (entries_.size() > kMaxEntries) entries_.resize(kMaxEntries);
}

void History::SetTitle(std::wstring_view url, std::wstring_view title) {
    if (const auto known = Locate(url); known != entries_.end()) known->title = CleanTitle(title);
}

bool History::Remove(std::wstring_view url) {
    const auto known = Locate(url);
    if (known == entries_.end()) return false;
    entries_.erase(known);
    return true;
}

History History::Parse(std::wstring_view text) {
    History history;
    std::unordered_set<std::wstring> seen;
    for (const auto lineRange : text | std::views::split(L'\n')) {
        if (history.entries_.size() >= kMaxEntries) break;
        const auto columns = SplitColumns(std::wstring_view(lineRange));
        if (columns.empty() || !IsLikelyHttpUrl(columns[0])) continue;
        if (!seen.insert(ToLower(columns[0])).second) continue;

        const auto column = [&](std::size_t index) {
            return index < columns.size() ? columns[index] : std::wstring_view();
        };
        history.entries_.push_back({
            .url = std::wstring(columns[0]),
            .title = std::wstring(column(4)),
            .visits = columns.size() > 1 ? std::max(1, ParseNumber<int>(column(1)).value_or(0)) : 1,
            .typed = std::max(0, ParseNumber<int>(column(2)).value_or(0)),
            .lastVisit = ParseNumber<std::int64_t>(column(3)).value_or(0),
        });
    }
    return history;
}

std::wstring History::Serialize() const {
    std::wstring text;
    for (const HistoryEntry& entry : entries_) {
        text.append(entry.url).append(1, L'\t');
        text.append(std::to_wstring(entry.visits)).append(1, L'\t');
        text.append(std::to_wstring(entry.typed)).append(1, L'\t');
        text.append(std::to_wstring(entry.lastVisit)).append(1, L'\t');
        text.append(entry.title).append(1, L'\n');
    }
    return text;
}

void History::Load(const std::filesystem::path& file) {
    const auto text = ReadTextFile(file);
    *this = text ? Parse(*text) : History();
}

bool History::Save(const std::filesystem::path& file) const {
    return WriteTextFile(file, Serialize());
}

} // namespace exodium
