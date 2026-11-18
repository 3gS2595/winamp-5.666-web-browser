// The pages that have been visited, shared by every window: what the address bar
// suggests from and what the history page lists.
//
// It is kept in two files. The history file has every entry, and is written whole: as
// Winamp exits, and now and then while it runs. In between, what happens to the history
// is added a line at a time to a journal beside it, which costs next to nothing to write,
// so a Winamp that crashes or is cut off loses a couple of seconds of history and not the
// whole run's. The next start reads the history file and then does what the journal says.
// What is in here is the lines and what they mean; history_store.cpp reads and writes the
// files.
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <utility>
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
    // Some ten months of pages at seventy new ones a day. Until 2026-11-09 it was 2000,
    // which that much browsing filled in a month.
    static constexpr std::size_t kMaxEntries = 20000;
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

    // ---- the journal ----

    // A journal belongs to the history file as it was when the journal was begun, which
    // its first line says by that file's `generation`: any text that is another for every
    // time the file is written, such as when it was. A journal left over from another
    // time's file would count its visits twice, or put old ones first.
    [[nodiscard]] static std::wstring JournalHeader(std::wstring_view generation);
    // Whether anything has happened that TakeJournal would give lines for.
    [[nodiscard]] bool HasJournal(bool withTitles) const noexcept;
    // The lines for what has happened since they were last taken: a visit counted or an
    // entry removed, and with `withTitles` the titles given too. Those are held back to be
    // taken less often, as some pages change theirs every second; only the last one a page
    // was given is kept.
    [[nodiscard]] std::wstring TakeJournal(bool withTitles);
    // Nothing that has happened so far is to be given lines for: the history has just been
    // written whole.
    void ForgetJournal() noexcept;
    // Does what a journal's lines say, if it is one that goes with `generation`; says
    // whether it was. A last line that was cut short is left out, and so is any line that
    // can't be read.
    bool Replay(std::wstring_view journal, std::wstring_view generation);

private:
    [[nodiscard]] std::vector<HistoryEntry>::iterator Locate(std::wstring_view url) noexcept;
    void ForgetPendingTitle(std::wstring_view url) noexcept;

    std::vector<HistoryEntry> entries_;
    std::wstring journal_;                                         // the lines not yet taken
    std::vector<std::pair<std::wstring, std::wstring>> newTitles_; // an address and the title it was given
    bool replaying_ = false; // what happens now is a journal's, and makes no lines
};

} // namespace exodium
