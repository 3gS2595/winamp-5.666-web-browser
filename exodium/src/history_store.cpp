// The history and the bookmarks on disk.
//
// The history file is written whole as Winamp exits, as it always was. Until 2026-11-09
// that was the only time, so a Winamp that crashed, was ended from outside or lost its
// power took the history of its whole run with it, days of it at times. Now what happens
// to the history is added to a journal beside the file every couple of seconds, a line
// for each visit (history.h says what the lines are), and the next start does what the
// journal says to the file it reads. Adding a few lines costs next to nothing, where
// writing the file whole takes longer the more it holds, and it holds ten times what it
// did.
//
// The journal is done with whenever the file is written whole: at exit, at a start that
// found a journal, and once it has grown past half a megabyte. It says which file it goes
// with, by that file's time and size, so one left behind by another version of the
// plugin, or by a write that got as far as the file and no further, is never applied to a
// file that has what it says already.
//
// The bookmarks are few, and their file is simply written whenever they change.
#include "browser.h"

#include "text_util.h"

namespace exodium {

namespace {

constexpr UINT kSaveEveryMs = 2000;
// Titles are written every this many times, which is once a minute: a page can change its
// title every second, and a title lost is the least of what could be.
constexpr int kTitlesEvery = 30;
constexpr std::size_t kMaxJournalChars = 256 * 1024; // half a megabyte as it is written

std::wstring g_generation;      // what the history file on disk is known by
bool g_journalBegun = false;    // the journal is there, and its first line names that file
std::size_t g_journalChars = 0; // how much has been added to it
int g_ticksToTitles = kTitlesEvery;

// What a file is known by: when it was last written and how much it holds. "0" for none.
[[nodiscard]] std::wstring GenerationOf(const std::filesystem::path& file) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!::GetFileAttributesExW(file.c_str(), GetFileExInfoStandard, &data)) return L"0";
    const std::uint64_t written =
        static_cast<std::uint64_t>(data.ftLastWriteTime.dwHighDateTime) << 32 | data.ftLastWriteTime.dwLowDateTime;
    return std::to_wstring(written) + L"-" + std::to_wstring(data.nFileSizeLow);
}

// Writes the history file whole, which leaves the journal nothing to say.
bool WriteWhole() {
    if (!g_app.history.Save(HistoryFile())) {
        LogLine(L"The history file couldn't be written.");
        return false;
    }
    g_app.history.ForgetJournal();
    g_generation = GenerationOf(HistoryFile());
    ::DeleteFileW(HistoryJournalFile().c_str());
    g_journalBegun = false;
    g_journalChars = 0;
    return true;
}

} // namespace

void LoadHistory() {
    g_app.history.Load(HistoryFile());
    g_generation = GenerationOf(HistoryFile());
    const std::optional<std::wstring> journal = ReadTextFile(HistoryJournalFile());
    if (!journal) return; // the last run wrote the file as it exited, as most do
    if (!g_app.history.Replay(*journal, g_generation)) {
        // Another file's. What it had is in the file there is, or was never to be.
        ::DeleteFileW(HistoryJournalFile().c_str());
        return;
    }
    LogLine(L"The last run didn't get to write the history as it ended; its journal was read instead.");
    if (WriteWhole()) return;
    // The file is as it was, so the journal still goes with it, and goes on being added to.
    g_journalBegun = true;
    g_journalChars = journal->size();
}

void SaveHistorySoon(bool withTitles) {
    History& history = g_app.history;
    if (g_app.shutDown || !history.HasJournal(withTitles)) return;
    if (g_journalChars > kMaxJournalChars && WriteWhole()) return;

    std::wstring lines = history.TakeJournal(withTitles);
    if (!g_journalBegun) {
        // A new one, in the place of any that was left there.
        ::DeleteFileW(HistoryJournalFile().c_str());
        lines.insert(0, History::JournalHeader(g_generation));
    }
    if (AppendTextFile(HistoryJournalFile(), lines)) {
        g_journalBegun = true;
        g_journalChars += lines.size();
        return;
    }
    // Those lines are lost to the journal, but not what they said, which the file has
    // once it is written whole.
    LogLine(L"The history's journal couldn't be added to.");
    if (!WriteWhole()) g_journalChars = kMaxJournalChars + 1; // again the next time
}

void SaveHistoryNow() {
    if (!WriteWhole() && g_app.history.HasJournal(true)) {
        // Whatever stood in the way of the file, the journal may still be had.
        std::wstring lines = g_app.history.TakeJournal(true);
        if (!g_journalBegun) lines.insert(0, History::JournalHeader(g_generation));
        AppendTextFile(HistoryJournalFile(), lines);
    }
}

void StartSavingHistory() {
    if (g_app.windows.empty() || !g_app.windows.front()->client) return;
    ::SetTimer(g_app.windows.front()->client, kHistorySaveTimer, kSaveEveryMs, nullptr);
}

bool OnHistoryTimer(UINT_PTR timer) {
    if (timer != kHistorySaveTimer) return false;
    const bool withTitles = --g_ticksToTitles <= 0;
    if (withTitles) g_ticksToTitles = kTitlesEvery;
    SaveHistorySoon(withTitles);
    return true;
}

void LoadBookmarks() {
    g_app.bookmarks.Load(BookmarksFile());
}

void SaveBookmarks() {
    if (!g_app.bookmarks.Save(BookmarksFile())) LogLine(L"The bookmarks file couldn't be written.");
}

} // namespace exodium
