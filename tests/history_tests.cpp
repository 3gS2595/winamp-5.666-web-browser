#include "test_framework.h"

#include "history.h"
#include "history_page.h"
#include "site_zoom.h"
#include "url_util.h"

#include <format>

using namespace exodium;

namespace {

constexpr std::int64_t kNow = 1'760'000'000;
constexpr std::int64_t kDay = 86'400;

} // namespace

TEST_CASE(LatestVisitComesFirst) {
    History history;
    history.RecordVisit(L"https://a.example/", L"A", false, 100);
    history.RecordVisit(L"https://b.example/", L"B", true, 200);
    EXPECT_EQ(history.entries().size(), std::size_t{2});
    EXPECT_EQ(history.entries()[0].url, L"https://b.example/");
    EXPECT_EQ(history.entries()[0].typed, 1);

    // A revisit, in whatever case, counts for the entry there already is.
    history.RecordVisit(L"HTTPS://A.example/", L"", false, 300);
    EXPECT_EQ(history.entries().size(), std::size_t{2});
    const HistoryEntry& front = history.entries()[0];
    EXPECT_EQ(front.url, L"https://a.example/");
    EXPECT_EQ(front.title, L"A");
    EXPECT_EQ(front.visits, 2);
    EXPECT_EQ(front.typed, 0);
    EXPECT_EQ(front.lastVisit, std::int64_t{300});
}

TEST_CASE(KeepsOnlyWebAddresses) {
    History history;
    history.RecordVisit(L"about:blank", L"", false, kNow);
    history.RecordVisit(kHistoryUrl, L"History", false, kNow);
    history.RecordVisit(kBookmarksUrl, L"Bookmarks", false, kNow);
    history.RecordVisit(L"example.com", L"", true, kNow);
    history.RecordVisit(L"https://example.com/a\tb", L"", true, kNow); // would be two columns of its file
    EXPECT(history.entries().empty());
}

TEST_CASE(ClaudeCodesStartPageIsNotKept) {
    History history;
    history.RecordVisit(L"file:///C:/Winamp/Plugins/exodium_start.html#1234", L"Claude Code", false, kNow);
    EXPECT(history.entries().empty());
}

TEST_CASE(TitlesFitOnALine) {
    History history;
    history.RecordVisit(L"https://a.example/", L"one\ttwo\r\nthree", false, kNow);
    EXPECT_EQ(history.entries()[0].title, L"one two  three");

    history.SetTitle(L"https://A.EXAMPLE/", std::wstring(500, L'x'));
    EXPECT_EQ(history.entries()[0].title.size(), History::kMaxTitleLength);

    history.SetTitle(L"https://unknown.example/", L"ignored");
    EXPECT_EQ(history.entries().size(), std::size_t{1});
}

TEST_CASE(FindsAndRemovesEntries) {
    History history;
    history.RecordVisit(L"https://a.example/", L"A", false, kNow);
    EXPECT(history.Find(L"https://A.example/") != nullptr);
    EXPECT(history.Find(L"https://b.example/") == nullptr);
    EXPECT(history.Remove(L"https://a.example/"));
    EXPECT(!history.Remove(L"https://a.example/"));
    EXPECT(history.entries().empty());
}

TEST_CASE(HistoryIsCapped) {
    // Read from a file that holds more than it keeps, the oldest of which are left out.
    std::wstring text;
    for (std::size_t i = 0; i < History::kMaxEntries + 5; ++i) {
        text += std::format(L"https://site{}.example/\t1\t0\t{}\t\n", i, kNow);
    }
    History history = History::Parse(text);
    EXPECT_EQ(history.entries().size(), History::kMaxEntries);
    EXPECT_EQ(history.entries().back().url, std::format(L"https://site{}.example/", History::kMaxEntries - 1));

    // And a page visited when it is full takes the place of the one visited longest ago.
    history.RecordVisit(L"https://new.example/", L"", false, kNow + 1);
    EXPECT_EQ(history.entries().size(), History::kMaxEntries);
    EXPECT_EQ(history.entries()[0].url, L"https://new.example/");
    EXPECT_EQ(history.entries().back().url, std::format(L"https://site{}.example/", History::kMaxEntries - 2));
}

TEST_CASE(ParsesHistoryFilesOldAndNew) {
    const History history = History::Parse(L"https://a.example/\t3\t1\t1700000000\tTitle A\n"
                                           L"https://legacy.example/\n"
                                           L"not a url\t1\n"
                                           L"\n"
                                           L"https://A.example/\t9\n"
                                           L"https://b.example/\tjunk\t-4\n");
    EXPECT_EQ(history.entries().size(), std::size_t{3});

    const HistoryEntry& full = history.entries()[0];
    EXPECT_EQ(full.url, L"https://a.example/");
    EXPECT_EQ(full.visits, 3);
    EXPECT_EQ(full.typed, 1);
    EXPECT_EQ(full.lastVisit, std::int64_t{1700000000});
    EXPECT_EQ(full.title, L"Title A");

    // A file from before visits were counted holds the URL alone.
    const HistoryEntry& legacy = history.entries()[1];
    EXPECT_EQ(legacy.url, L"https://legacy.example/");
    EXPECT_EQ(legacy.visits, 1);
    EXPECT_EQ(legacy.typed, 0);
    EXPECT_EQ(legacy.lastVisit, std::int64_t{0});
    EXPECT_EQ(legacy.title, L"");

    const HistoryEntry& damaged = history.entries()[2];
    EXPECT_EQ(damaged.visits, 1);
    EXPECT_EQ(damaged.typed, 0);
}

TEST_CASE(HistorySurvivesARoundTrip) {
    History history;
    history.RecordVisit(L"https://a.example/", L"Title A", true, 1700000000);
    history.RecordVisit(L"https://b.example/x?y=z", L"", false, 1700000100);
    const std::wstring text = history.Serialize();
    EXPECT_EQ(text, L"https://b.example/x?y=z\t1\t0\t1700000100\t\n"
                    L"https://a.example/\t1\t1\t1700000000\tTitle A\n");
    EXPECT_EQ(History::Parse(text).Serialize(), text);
}

TEST_CASE(TheJournalSaysWhatHappenedSinceTheHistoryWasWritten) {
    History history;
    history.RecordVisit(L"https://a.example/", L"A", false, 100);
    history.RecordVisit(L"https://b.example/", L"B", false, 200);
    const std::wstring written = history.Serialize();
    history.ForgetJournal();
    EXPECT(!history.HasJournal(true));

    history.RecordVisit(L"https://c.example/", L"C\tsee", true, 300);
    history.RecordVisit(L"https://a.example/", L"", false, 400);
    history.SetTitle(L"https://b.example/", L"B once");
    history.SetTitle(L"https://b.example/", L"B twice");
    EXPECT(history.Remove(L"https://c.example/"));
    // Titles are held back, and only the last one a page was given is kept.
    EXPECT(history.HasJournal(false));
    const std::wstring visits = history.TakeJournal(false);
    EXPECT_EQ(visits, L"V\thttps://c.example/\t1\t300\tC see\n"
                      L"V\thttps://a.example/\t0\t400\t\n"
                      L"R\thttps://c.example/\n");
    EXPECT(!history.HasJournal(false));
    EXPECT(history.HasJournal(true));
    const std::wstring titles = history.TakeJournal(true);
    EXPECT_EQ(titles, L"T\thttps://b.example/\tB twice\n");
    EXPECT(!history.HasJournal(true));

    // The file and its journal make the same history again.
    History again = History::Parse(written);
    EXPECT(again.Replay(History::JournalHeader(L"7") + visits + titles, L"7"));
    EXPECT_EQ(again.Serialize(), history.Serialize());
    EXPECT_EQ(again.Serialize(), L"https://a.example/\t2\t0\t400\tA\n"
                                 L"https://b.example/\t1\t0\t200\tB twice\n");
    // What a journal says is done makes no lines of its own.
    EXPECT(!again.HasJournal(true));
}

TEST_CASE(AVisitsTitleIsNewerThanOneGivenBeforeIt) {
    History history;
    history.RecordVisit(L"https://a.example/", L"First", false, 100);
    history.ForgetJournal();
    history.SetTitle(L"https://a.example/", L"Given");
    history.RecordVisit(L"https://A.example/", L"Second", false, 200);
    EXPECT_EQ(history.TakeJournal(true), L"V\thttps://A.example/\t0\t200\tSecond\n");
    // And a page removed has no title to be given.
    history.SetTitle(L"https://a.example/", L"Third");
    EXPECT(history.Remove(L"https://a.example/"));
    EXPECT_EQ(history.TakeJournal(true), L"R\thttps://a.example/\n");
}

TEST_CASE(AJournalOfAnotherFileIsLeftAlone) {
    const std::wstring written = L"https://a.example/\t1\t0\t100\tA\n";
    const std::wstring lines = L"V\thttps://b.example/\t0\t200\tB\n";

    History other = History::Parse(written);
    EXPECT(!other.Replay(History::JournalHeader(L"7") + lines, L"8"));
    EXPECT(!other.Replay(lines, L"7")); // no first line at all
    EXPECT(!other.Replay(L"", L"7"));
    EXPECT_EQ(other.Serialize(), written);
}

TEST_CASE(AJournalCutShortOrDamagedIsReadAsFarAsItGoes) {
    History history = History::Parse(L"https://a.example/\t1\t0\t100\tA\n");
    EXPECT(history.Replay(History::JournalHeader(L"1") + L"V\thttps://b.example/\t0\t200\tB\n"
                                                         L"V\thttps://c.example/\tmaybe\t300\tC\n" // neither 0 nor 1
                                                         L"V\thttps://d.example/\t0\tsoon\tD\n"    // no time
                                                         L"X\tsomething a later version writes\n"
                                                         L"T\thttps://a.example/\n" // no title
                                                         L"\n"
                                                         L"V\thttps://e.example/\t0\t400\tE\n"
                                                         L"V\thttps://f.exam", // the write that was under way
                          L"1"));
    EXPECT_EQ(history.Serialize(), L"https://e.example/\t1\t0\t400\tE\n"
                                   L"https://b.example/\t1\t0\t200\tB\n"
                                   L"https://a.example/\t1\t0\t100\tA\n");
}

TEST_CASE(FrecencyFavoursTypedAndRecent) {
    const HistoryEntry visited{.visits = 2, .lastVisit = kNow};
    const HistoryEntry typed{.visits = 2, .typed = 1, .lastVisit = kNow};
    const HistoryEntry stale{.visits = 2, .lastVisit = kNow - 10 * kDay};
    const HistoryEntry undated{.visits = 2};
    const HistoryEntry heavy{.visits = 500, .lastVisit = kNow};

    EXPECT_EQ(Frecency(visited, kNow), 2.0);
    EXPECT_EQ(Frecency(typed, kNow), 6.0);
    EXPECT_EQ(Frecency(stale, kNow), 2.0 * 0.7);
    EXPECT_EQ(Frecency(undated, kNow), 2.0 * 0.5);
    EXPECT_EQ(Frecency(heavy, kNow), 20.0);
}

TEST_CASE(HistoryPageEscapesItsEntries) {
    History history;
    history.RecordVisit(L"https://a.example/?x=<b>", L"He said \"hi\"", false, 1700000000);
    const std::wstring html = BuildHistoryPageHtml(history.entries());
    EXPECT(html.starts_with(L"<!doctype html>"));
    EXPECT(html.contains(L"[\"https://a.example/?x=\\u003cb>\",\"He said \\\"hi\\\"\",1700000000],\n"));
    EXPECT(html.ends_with(L"</script></body></html>"));
}

TEST_CASE(AFullHistoryStillMakesItsPage) {
    std::wstring text;
    for (std::size_t i = 0; i < History::kMaxEntries; ++i) {
        text += std::format(L"https://site{}.example/a/long/enough/path?with=parameters\t1\t0\t{}\tA title\n", i, kNow);
    }
    const History history = History::Parse(text);
    const std::wstring html = BuildHistoryPageHtml(history.entries());
    EXPECT(html.contains(std::format(L"[\"https://site{}.example/", History::kMaxEntries - 1)));
    EXPECT(html.ends_with(L"</script></body></html>"));
}

TEST_CASE(ZoomIsKeptBySite) {
    SiteZoom zoom;
    EXPECT_EQ(zoom.FactorFor(L"a.example"), 1.0);
    zoom.Set(L"a.example", 1.25);
    zoom.Set(L"b.example", 0.8);
    EXPECT_EQ(zoom.FactorFor(L"a.example"), 1.25);
    EXPECT_EQ(zoom.Serialize(), L"a.example\t1.2500\nb.example\t0.8000\n");

    // Back at the normal size a site has no entry.
    zoom.Set(L"a.example", 1.0);
    EXPECT_EQ(zoom.FactorFor(L"a.example"), 1.0);
    EXPECT_EQ(zoom.Serialize(), L"b.example\t0.8000\n");
}

TEST_CASE(ParsesZoomFilesAndDropsNonsense) {
    const SiteZoom zoom = SiteZoom::Parse(L"a.example\t1.2500\n"
                                          L"toobig.example\t9\n"
                                          L"toosmall.example\t0.1\n"
                                          L"\t2\n"
                                          L"no factor\n"
                                          L"junk.example\tabc\n");
    EXPECT_EQ(zoom.Serialize(), L"a.example\t1.2500\n");
    EXPECT(SameZoom(1.25, 1.2504));
    EXPECT(!SameZoom(1.25, 1.26));
}

TEST_CASE(ZoomGoesUpAndDownAStepAtATime) {
    EXPECT_EQ(NextZoomStep(1.0, true), 1.1);
    EXPECT_EQ(NextZoomStep(1.0, false), 0.9);
    EXPECT_EQ(NextZoomStep(1.1, false), 1.0);
    EXPECT_EQ(NextZoomStep(0.5, false), 1 / 3.0);

    // From between two steps, where the wheel can leave a page, to the nearer one that way;
    // and a factor that is a step for all that can be seen of it counts as that step.
    EXPECT_EQ(NextZoomStep(1.2, true), 1.25);
    EXPECT_EQ(NextZoomStep(1.2, false), 1.1);
    EXPECT_EQ(NextZoomStep(1.2504, true), 1.5);
    EXPECT_EQ(NextZoomStep(1.2504, false), 1.1);

    // The ends stay where they are.
    EXPECT_EQ(NextZoomStep(SiteZoom::kMax, true), SiteZoom::kMax);
    EXPECT_EQ(NextZoomStep(SiteZoom::kMin, false), SiteZoom::kMin);

    // All the way down from the largest: sixteen steps, with 100% among them, each of
    // which the file keeps and reads back.
    int steps = 0;
    bool passedNormal = false;
    for (double factor = SiteZoom::kMax; !SameZoom(factor, SiteZoom::kMin); ++steps) {
        factor = NextZoomStep(factor, false);
        passedNormal = passedNormal || SameZoom(factor, SiteZoom::kNormal);
        SiteZoom zoom;
        zoom.Set(L"a.example", factor);
        EXPECT(SameZoom(SiteZoom::Parse(zoom.Serialize()).FactorFor(L"a.example"), factor));
    }
    EXPECT_EQ(steps, 16);
    EXPECT(passedNormal);
}
