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

// What a line of the journal begins with: the first line, a visit, a title, a removal.
constexpr std::wstring_view kJournalGeneration = L"G";
constexpr std::wstring_view kJournalVisit = L"V";
constexpr std::wstring_view kJournalTitle = L"T";
constexpr std::wstring_view kJournalRemoval = L"R";

// Titles are kept one to a line of the history file, between tabs.
std::wstring CleanTitle(std::wstring_view title) {
    return SingleLine(title, History::kMaxTitleLength);
}

// An address with a tab or a line end in it would be taken for more than one column, or
// line, of a file. No webview gives one.
[[nodiscard]] bool FitsOnALine(std::wstring_view url) noexcept {
    return url.find_first_of(L"\t\r\n") == std::wstring_view::npos;
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

void History::ForgetPendingTitle(std::wstring_view url) noexcept {
    std::erase_if(newTitles_, [url](const auto& given) { return EqualsIgnoreCase(given.first, url); });
}

void History::RecordVisit(std::wstring_view url, std::wstring_view title, bool typed, std::int64_t now) {
    if (!IsLikelyHttpUrl(url) || !PluginPageName(url).empty() || url.contains(kToolStartPage) || !FitsOnALine(url)) {
        return;
    }

    if (const auto known = Locate(url); known != entries_.end()) {
        std::rotate(entries_.begin(), known, known + 1);
    } else {
        entries_.insert(entries_.begin(), HistoryEntry{.url = std::wstring(url)});
    }
    HistoryEntry& entry = entries_.front();
    ++entry.visits;
    if (typed) ++entry.typed;
    entry.lastVisit = now;
    const std::wstring clean = CleanTitle(title);
    if (!clean.empty()) entry.title = clean;

    if (entries_.size() > kMaxEntries) entries_.resize(kMaxEntries);

    if (replaying_) return;
    // The visit's line has the title; one given before it is the older of the two.
    if (!clean.empty()) ForgetPendingTitle(url);
    journal_.append(kJournalVisit).append(1, L'\t').append(url).append(1, L'\t');
    journal_.append(typed ? L"1" : L"0").append(1, L'\t').append(std::to_wstring(now)).append(1, L'\t');
    journal_.append(clean).append(1, L'\n');
}

void History::SetTitle(std::wstring_view url, std::wstring_view title) {
    const auto known = Locate(url);
    if (known == entries_.end()) return;
    std::wstring clean = CleanTitle(title);
    if (known->title == clean) return;
    known->title = clean;
    if (replaying_) return;
    const auto given = std::ranges::find_if(
        newTitles_, [&known](const auto& earlier) { return EqualsIgnoreCase(earlier.first, known->url); });
    if (given != newTitles_.end()) given->second = std::move(clean);
    else newTitles_.emplace_back(known->url, std::move(clean));
}

bool History::Remove(std::wstring_view url) {
    const auto known = Locate(url);
    if (known == entries_.end()) return false;
    entries_.erase(known);
    if (replaying_) return true;
    ForgetPendingTitle(url);
    if (FitsOnALine(url)) journal_.append(kJournalRemoval).append(1, L'\t').append(url).append(1, L'\n');
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

std::wstring History::JournalHeader(std::wstring_view generation) {
    return std::wstring(kJournalGeneration).append(1, L'\t').append(generation).append(1, L'\n');
}

bool History::HasJournal(bool withTitles) const noexcept {
    return !journal_.empty() || (withTitles && !newTitles_.empty());
}

std::wstring History::TakeJournal(bool withTitles) {
    std::wstring lines = std::exchange(journal_, {});
    if (withTitles) {
        for (const auto& [url, title] : std::exchange(newTitles_, {})) {
            lines.append(kJournalTitle).append(1, L'\t').append(url).append(1, L'\t').append(title).append(1, L'\n');
        }
    }
    return lines;
}

void History::ForgetJournal() noexcept {
    journal_.clear();
    newTitles_.clear();
}

bool History::Replay(std::wstring_view journal, std::wstring_view generation) {
    if (!journal.starts_with(JournalHeader(generation))) return false;
    // Whatever follows the last line end is a line that was never finished.
    journal = journal.substr(0, journal.rfind(L'\n') + 1);

    replaying_ = true;
    for (const auto lineRange : journal | std::views::split(L'\n')) {
        const auto columns = SplitColumns(std::wstring_view(lineRange));
        const std::wstring_view kind = columns[0];
        if (kind == kJournalVisit && columns.size() >= 4) {
            const auto when = ParseNumber<std::int64_t>(columns[3]);
            if (!when || (columns[2] != L"0" && columns[2] != L"1")) continue;
            RecordVisit(columns[1], columns.size() > 4 ? columns[4] : std::wstring_view(), columns[2] == L"1", *when);
        } else if (kind == kJournalTitle && columns.size() >= 3) {
            SetTitle(columns[1], columns[2]);
        } else if (kind == kJournalRemoval && columns.size() >= 2) {
            Remove(columns[1]);
        }
    }
    replaying_ = false;
    return true;
}

} // namespace exodium
