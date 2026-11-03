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
    history.RecordVisit(L"example.com", L"", true, kNow);
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
    History history;
    for (int i = 0; i < static_cast<int>(History::kMaxEntries) + 5; ++i) {
        history.RecordVisit(std::format(L"https://site{}.example/", i), L"", false, kNow + i);
    }
    EXPECT_EQ(history.entries().size(), History::kMaxEntries);
    EXPECT_EQ(history.entries()[0].url, std::format(L"https://site{}.example/", History::kMaxEntries + 4));
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
