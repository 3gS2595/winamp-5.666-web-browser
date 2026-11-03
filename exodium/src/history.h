// The pages that have been visited, shared by every window: what the address bar
// suggests from and what the history page lists.
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace exodium {

struct HistoryEntry {
    std::wstring url;
    std::wstring title;
    int visits = 0;             // page loads that ended on this URL
    int typed = 0;              // how many of them were asked for by address in the address bar
    std::int64_t lastVisit = 0; // when the latest was, in seconds since 1970; 0 if not known
};

// The time now, in the seconds since 1970 that HistoryEntry::lastVisit is kept in.
[[nodiscard]] std::int64_t UnixNow() noexcept;

// How much a history entry is worth suggesting. A visit counts for more when the
// address was typed in, and all of them for less the longer ago the last one was.
[[nodiscard]] double Frecency(const HistoryEntry& entry, std::int64_t now) noexcept;

class History {
public:
    static constexpr std::size_t kMaxEntries = 2000;
    static constexpr std::size_t kMaxTitleLength = 200;

    // Most recently visited first.
    [[nodiscard]] std::span<const HistoryEntry> entries() const noexcept { return entries_; }
    [[nodiscard]] const HistoryEntry* Find(std::wstring_view url) const noexcept;

    // Counts a page load that ended on `url` and moves it to the front. Addresses the
    // history doesn't keep are ignored. An empty title leaves the one already known.
    void RecordVisit(std::wstring_view url, std::wstring_view title, bool typed, std::int64_t now);
    // Gives the entry for `url` a title, if there is such an entry.
    void SetTitle(std::wstring_view url, std::wstring_view title);
    // Takes the entry for `url` out. Says whether there was one.
    bool Remove(std::wstring_view url);

    // One entry to a line: URL, visits, typed visits, time of the last visit, title,
    // with tabs between. Files from before visits were counted hold the URL alone.
    [[nodiscard]] static History Parse(std::wstring_view text);
    [[nodiscard]] std::wstring Serialize() const;

    void Load(const std::filesystem::path& file);
    bool Save(const std::filesystem::path& file) const;

private:
    [[nodiscard]] std::vector<HistoryEntry>::iterator Locate(std::wstring_view url) noexcept;

    std::vector<HistoryEntry> entries_;
};

} // namespace exodium
