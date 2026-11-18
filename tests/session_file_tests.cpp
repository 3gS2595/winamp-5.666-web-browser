#include "test_framework.h"

#include "session_file.h"
#include "url_util.h"

#include <string>
#include <vector>

using namespace exodium;

namespace {

// Pages at the given addresses, each titled after its own, with `shown` shown.
[[nodiscard]] HistoryCopy Pages(const std::vector<std::wstring>& urls, std::size_t shown) {
    HistoryCopy pages;
    for (const std::wstring& url : urls) pages.entries.push_back({.url = url, .title = L"T " + url});
    pages.shown = shown;
    return pages;
}

// Pages written as their addresses, with the shown one in brackets, so that a failure
// shows what differs.
[[nodiscard]] std::wstring Show(const HistoryCopy& pages) {
    std::wstring text;
    for (std::size_t i = 0; i < pages.entries.size(); ++i) {
        if (i) text += L' ';
        text += i == pages.shown ? L"[" + pages.entries[i].url + L"]" : pages.entries[i].url;
    }
    return text;
}

[[nodiscard]] SessionHistory Of(const std::vector<std::wstring>& urls, int current) {
    SessionHistory history;
    for (const std::wstring& url : urls) {
        history.entries.push_back({.id = static_cast<int>(history.entries.size()) + 1, .url = url, .title = L"T " + url});
    }
    history.current = current;
    return history;
}

[[nodiscard]] SavedSession Sample() {
    SavedSession session;
    session.windows.push_back({.show = WindowShow::kHidden,
                               .place = {100, 90, 1000, 728},
                               .stacking = 2,
                               .tabs = {Pages({L"https://a.example/", L"https://b.example/"}, 1), HistoryCopy{}},
                               .active = 1});
    session.windows.push_back({.show = WindowShow::kMaximized,
                               .place = {-1900, 10, -100, 900},
                               .stacking = 0,
                               .tabs = {Pages({L"https://c.example/"}, 0)},
                               .active = 0});
    session.closed.push_back({.wholeWindow = true,
                              .window = {.show = WindowShow::kNormal,
                                         .place = {5, 6, 700, 800},
                                         .tabs = {Pages({L"https://d.example/", L"https://e.example/#x"}, 0),
                                                  Pages({L"file:///C:/f.html"}, 0)},
                                         .active = 1}});
    session.closed.push_back(
        {.wholeWindow = false, .window = {.tabs = {Pages({L"https://g.example/", L"https://h.example/"}, 1)}}});
    return session;
}

} // namespace

TEST_CASE(ASessionIsReadBackAsItWasWritten) {
    const SavedSession session = Sample();
    const SavedSession read = ParseSession(SerializeSession(session));
    EXPECT_EQ(read.windows.size(), 2u);
    EXPECT_EQ(read.closed.size(), 2u);
    if (read.windows.size() != 2 || read.closed.size() != 2) return;

    const SavedWindow& first = read.windows[0];
    EXPECT(first.show == WindowShow::kHidden);
    EXPECT(first.place == (SavedPlace{100, 90, 1000, 728}));
    EXPECT_EQ(first.stacking, 2);
    EXPECT_EQ(first.active, 1u);
    EXPECT_EQ(first.tabs.size(), 2u);
    if (first.tabs.size() == 2) {
        EXPECT_EQ(Show(first.tabs[0]), L"https://a.example/ [https://b.example/]");
        EXPECT_EQ(first.tabs[0].entries[0].title, L"T https://a.example/");
        EXPECT(first.tabs[1].entries.empty()); // a new tab
    }

    const SavedWindow& second = read.windows[1];
    EXPECT(second.show == WindowShow::kMaximized);
    EXPECT(second.place == (SavedPlace{-1900, 10, -100, 900}));
    EXPECT_EQ(second.stacking, 0);
    EXPECT_EQ(second.tabs.size(), 1u);

    EXPECT(read.closed[0].wholeWindow);
    EXPECT(read.closed[0].window.place == (SavedPlace{5, 6, 700, 800}));
    EXPECT_EQ(read.closed[0].window.active, 1u);
    EXPECT_EQ(read.closed[0].window.tabs.size(), 2u);
    if (read.closed[0].window.tabs.size() == 2) {
        EXPECT_EQ(Show(read.closed[0].window.tabs[0]), L"[https://d.example/] https://e.example/#x");
        EXPECT_EQ(Show(read.closed[0].window.tabs[1]), L"[file:///C:/f.html]");
    }
    EXPECT(!read.closed[1].wholeWindow);
    EXPECT_EQ(read.closed[1].window.tabs.size(), 1u);
    if (!read.closed[1].window.tabs.empty()) {
        EXPECT_EQ(Show(read.closed[1].window.tabs[0]), L"https://g.example/ [https://h.example/]");
    }

    // Written again, it is the same file.
    EXPECT_EQ(SerializeSession(read), SerializeSession(session));
}

TEST_CASE(SessionFileLooksAsDocumented) {
    SavedSession session;
    session.windows.push_back({.show = WindowShow::kNormal,
                               .place = {100, 90, 1000, 728},
                               .tabs = {Pages({L"https://example.com/"}, 0)}});
    EXPECT_EQ(SerializeSession(session), L"exodium-session\t1\n"
                                         L"window\tnormal\t100\t90\t1000\t728\t0\t0\n"
                                         L"tab\t0\n"
                                         L"page\thttps://example.com/\tT https://example.com/\n");
}

TEST_CASE(NotASessionFileReadsAsNothing) {
    EXPECT(ParseSession(L"").windows.empty());
    EXPECT(ParseSession(L"window\tnormal\t1\t2\t3\t4\t0\t0\ntab\t0\npage\thttps://a.example/\tA\n").windows.empty());
    // A later version may say what this one can't read.
    EXPECT(ParseSession(L"exodium-session\t2\nwindow\tnormal\t1\t2\t3\t4\t0\t0\n").windows.empty());
    EXPECT_EQ(ParseSession(L"exodium-session\t1\r\nwindow\tnormal\t1\t2\t3\t4\t0\t0\r\n").windows.size(), 1u);
}

TEST_CASE(SessionFileGoneWrongIsReadAsFarAsItMakesSense) {
    const SavedSession read = ParseSession(L"exodium-session\t1\n"
                                           L"page\thttps://nowhere.example/\tno tab before it\n"
                                           L"tab\t0\n" // no window before it
                                           L"something-new\t1\t2\n"
                                           L"window\tsideways\t1\t2\t3\n" // a show it doesn't know, a place cut short
                                           L"tab\t7\n"                    // shows past its pages
                                           L"page\thttps://a.example/\n"
                                           L"page\thttps://b.example/\tB\n"
                                           L"tab\tx\n"
                                           L"page\t\tan empty address\n"
                                           L"window\tnormal\t0\t0\t10\t10\t9\t0\n" // the tab in view past its tabs
                                           L"tab\t0\n"
                                           L"page\thttps://c.example/\tC\n"
                                           L"closed-tab\t0\n" // nothing in it
                                           L"closed-window\tnormal\t0\t0\t1\t1\t0\n");
    EXPECT_EQ(read.windows.size(), 2u);
    if (read.windows.size() != 2) return;
    EXPECT(read.windows[0].show == WindowShow::kNormal);
    EXPECT(read.windows[0].place == (SavedPlace{1, 2, 3, 0}));
    EXPECT_EQ(read.windows[0].tabs.size(), 2u);
    if (read.windows[0].tabs.size() == 2) {
        EXPECT_EQ(Show(read.windows[0].tabs[0]), L"https://a.example/ [https://b.example/]");
        EXPECT_EQ(read.windows[0].tabs[0].entries[0].title, L"");
        EXPECT(read.windows[0].tabs[1].entries.empty());
    }
    EXPECT_EQ(read.windows[1].active, 0u);
    EXPECT(read.closed.empty());
}

TEST_CASE(PagesThatCantBeKeptAreLeftOut) {
    const std::wstring tooLong = L"data:text/plain," + std::wstring(kMaxSavedUrl, L'x');
    HistoryCopy pages = Pages({L"https://a.example/", tooLong, L"https://b.example/\tx", L"", L"https://c.example/"}, 2);
    pages.entries[0].title = L"Tab\there\r\nand there" + std::wstring(300, L'.');
    const HistoryCopy kept = KeepablePages(pages);
    // The shown page couldn't be kept: the nearest kept one before it is shown.
    EXPECT_EQ(Show(kept), L"[https://a.example/] https://c.example/");
    if (!kept.entries.empty()) {
        EXPECT_EQ(kept.entries[0].title.size(), kMaxSavedTitle);
        EXPECT(kept.entries[0].title.starts_with(L"Tab here  and there"));
    }
    // None kept before it: the nearest after it.
    EXPECT_EQ(Show(KeepablePages(Pages({L"", L"https://a.example/"}, 0))), L"[https://a.example/]");
    EXPECT(KeepablePages(Pages({L"", tooLong}, 1)).entries.empty());
    EXPECT(KeepablePages({}).entries.empty());
}

TEST_CASE(OfTooManyPagesThoseRoundTheShownOneAreKept) {
    std::vector<std::wstring> urls;
    for (int i = 0; i < 60; ++i) urls.push_back(L"https://example.com/" + std::to_wstring(i));
    const auto keptFor = [&](std::size_t shown) { return KeepablePages(Pages(urls, shown)); };

    const HistoryCopy atEnd = keptFor(59);
    EXPECT_EQ(atEnd.entries.size(), kMaxSavedPages);
    EXPECT_EQ(atEnd.entries.front().url, L"https://example.com/10");
    EXPECT_EQ(atEnd.entries[atEnd.shown].url, L"https://example.com/59");

    const HistoryCopy atStart = keptFor(0);
    EXPECT_EQ(atStart.entries.size(), kMaxSavedPages);
    EXPECT_EQ(atStart.shown, 0u);
    EXPECT_EQ(atStart.entries.back().url, L"https://example.com/49");

    const HistoryCopy between = keptFor(30);
    EXPECT_EQ(between.entries.size(), kMaxSavedPages);
    EXPECT_EQ(between.entries.front().url, L"https://example.com/5");
    EXPECT_EQ(between.entries[between.shown].url, L"https://example.com/30");
}

TEST_CASE(KeptHistoryLeavesOutWhatCantBeMadeAgain) {
    // The page a closed tab was put away on, which a reopened one has ahead of it, and runs
    // of one address, which pushState leaves.
    EXPECT_EQ(Show(KeptHistory(Of({L"https://a.example/", L"https://b.example/", L"https://b.example/",
                                   L"https://c.example/", kParkedUrl},
                                  3))),
              L"https://a.example/ https://b.example/ [https://c.example/]");
    EXPECT_EQ(Show(KeptHistory(Of({L"https://a.example/", L"https://b.example/", L"https://b.example/"}, 2))),
              L"https://a.example/ [https://b.example/]");
    // The page in view can't be made again: the nearest before it is shown.
    EXPECT_EQ(Show(KeptHistory(Of({L"https://a.example/", L"blob:https://a.example/1", L"https://c.example/"}, 1))),
              L"[https://a.example/] https://c.example/");
    EXPECT_EQ(Show(KeptHistory(Of({L"javascript:void(0)", L"https://c.example/"}, 0))), L"[https://c.example/]");
    EXPECT(KeptHistory(Of({kParkedUrl}, 0)).entries.empty());
    // The titles go with them.
    const HistoryCopy kept = KeptHistory(Of({L"https://a.example/"}, 0));
    if (!kept.entries.empty()) EXPECT_EQ(kept.entries[0].title, L"T https://a.example/");
}

TEST_CASE(OnlyTheLatestClosedAreKept) {
    SavedSession session;
    for (std::size_t i = 0; i < kMaxSavedClosed + 5; ++i) {
        session.closed.push_back(
            {.window = {.tabs = {Pages({L"https://example.com/" + std::to_wstring(i)}, 0)}}});
    }
    const SavedSession read = ParseSession(SerializeSession(session));
    EXPECT_EQ(read.closed.size(), kMaxSavedClosed);
    if (!read.closed.empty()) {
        EXPECT_EQ(read.closed.front().window.tabs.front().entries.front().url, L"https://example.com/5");
        EXPECT_EQ(read.closed.back().window.tabs.front().entries.front().url,
                  L"https://example.com/" + std::to_wstring(kMaxSavedClosed + 4));
    }
}
