#include "test_framework.h"

#include "bookmarks.h"
#include "url_util.h"

#include <format>

using namespace exodium;

TEST_CASE(BookmarksAreKeptNewestFirst) {
    Bookmarks bookmarks;
    EXPECT(bookmarks.Add(L"https://a.example/", L"A", 100));
    EXPECT(bookmarks.Add(L"https://b.example/x?y=z", L"  B\tsite \n", 200));
    EXPECT_EQ(bookmarks.entries().size(), std::size_t{2});
    EXPECT_EQ(bookmarks.entries()[0].url, L"https://b.example/x?y=z");
    EXPECT_EQ(bookmarks.entries()[0].title, L"B site");
    EXPECT_EQ(bookmarks.entries()[0].added, std::int64_t{200});

    // One that is there already is left as it is.
    EXPECT(!bookmarks.Add(L"https://a.example/", L"Another title", 300));
    EXPECT_EQ(bookmarks.Find(L"https://a.example/")->title, L"A");
    EXPECT_EQ(bookmarks.Find(L"https://a.example/")->added, std::int64_t{100});
    // An address is a bookmark's as it is written: these are two pages to most sites.
    EXPECT(bookmarks.Find(L"https://a.example/Page") == nullptr);
    EXPECT(bookmarks.Add(L"https://a.example/Page", L"", 400));
    EXPECT(bookmarks.Add(L"https://a.example/page", L"", 500));

    EXPECT(bookmarks.Remove(L"https://a.example/"));
    EXPECT(!bookmarks.Remove(L"https://a.example/"));
    EXPECT(bookmarks.Find(L"https://a.example/") == nullptr);
    EXPECT_EQ(bookmarks.entries().size(), std::size_t{3});
}

TEST_CASE(OnlyPagesCanBeBookmarked) {
    Bookmarks bookmarks;
    EXPECT(bookmarks.Add(L"file:///C:/page.html", L"", 1));
    EXPECT(!bookmarks.Add(L"about:blank", L"", 1));
    EXPECT(!bookmarks.Add(L"", L"", 1));
    EXPECT(!bookmarks.Add(L"javascript:alert(1)", L"", 1));
    EXPECT(!bookmarks.Add(kHistoryUrl, L"History", 1));
    EXPECT(!bookmarks.Add(kBookmarksUrl, L"Bookmarks", 1));
    EXPECT(!bookmarks.Add(L"file:///C:/Winamp/Plugins/exodium_start.html#pid=1", L"", 1));
    EXPECT(!bookmarks.Add(L"https://a.example/a\tb", L"", 1));
    EXPECT_EQ(bookmarks.entries().size(), std::size_t{1});
}

TEST_CASE(BookmarksSurviveARoundTrip) {
    Bookmarks bookmarks;
    EXPECT(bookmarks.Add(L"https://a.example/", L"Title A", 1700000000));
    EXPECT(bookmarks.Add(L"https://b.example/x?y=z", L"", 1700000100));
    const std::wstring text = bookmarks.Serialize();
    EXPECT_EQ(text, L"https://b.example/x?y=z\t1700000100\t\n"
                    L"https://a.example/\t1700000000\tTitle A\n");
    EXPECT_EQ(Bookmarks::Parse(text).Serialize(), text);
}

TEST_CASE(ParsesBookmarkFilesAndDropsNonsense) {
    const Bookmarks bookmarks = Bookmarks::Parse(L"https://a.example/\t100\tA\n"
                                                 L"not a page\t200\tNo\n"
                                                 L"\n"
                                                 L"https://a.example/\t300\tA again\n"
                                                 L"https://b.example/\n"
                                                 L"https://c.example/\twhen\tC\n");
    EXPECT_EQ(bookmarks.Serialize(), L"https://a.example/\t100\tA\n"
                                     L"https://b.example/\t0\t\n"
                                     L"https://c.example/\t0\tC\n");
}

TEST_CASE(BookmarksBroughtInGoAfterTheOnesThereAre) {
    Bookmarks bookmarks;
    EXPECT(bookmarks.Add(L"https://a.example/", L"Mine", 100));
    const std::vector<Bookmark> others{
        {.url = L"https://b.example/", .title = L"B", .added = 5},
        {.url = L"https://a.example/", .title = L"Theirs", .added = 6},
        {.url = L"chrome://settings/", .title = L"Settings", .added = 7},
        {.url = L"https://b.example/", .title = L"B again", .added = 8},
        {.url = L"https://c.example/", .title = L"C", .added = 9},
    };
    EXPECT_EQ(bookmarks.Merge(others), std::size_t{2});
    EXPECT_EQ(bookmarks.Serialize(), L"https://a.example/\t100\tMine\n"
                                     L"https://b.example/\t5\tB\n"
                                     L"https://c.example/\t9\tC\n");
    EXPECT_EQ(bookmarks.Merge(others), std::size_t{0});
}

TEST_CASE(BookmarksAreCapped) {
    std::vector<Bookmark> many;
    for (std::size_t i = 0; i < Bookmarks::kMaxEntries + 5; ++i) {
        many.push_back({.url = std::format(L"https://site{}.example/", i)});
    }
    Bookmarks bookmarks;
    EXPECT_EQ(bookmarks.Merge(many), Bookmarks::kMaxEntries);
    EXPECT(!bookmarks.Add(L"https://one.more.example/", L"", 1));
    EXPECT_EQ(bookmarks.entries().size(), Bookmarks::kMaxEntries);
}

TEST_CASE(ReadsAnotherBrowsersBookmarks) {
    // As Chrome and Edge write the file: folders in folders, dates in microseconds since 1601.
    const std::wstring json = LR"json({
   "checksum": "0123456789abcdef",
   "roots": {
      "bookmark_bar": {
         "children": [ {
            "date_added": "13350000000000000",
            "guid": "a",
            "id": "5",
            "name": "Example \"one\"",
            "type": "url",
            "url": "https://one.example/"
         }, {
            "children": [ {
               "date_added": "13350000001000000",
               "name": "Two",
               "type": "url",
               "url": "https://two.example/a?b=c"
            }, {
               "children": [ { "name": "Three \u00e9", "type": "url", "url": "https://three.example/" } ],
               "name": "Deeper",
               "type": "folder"
            }, {
               "name": "A bookmarklet",
               "type": "url",
               "url": "javascript:void(0)"
            } ],
            "date_added": "13350000000000000",
            "name": "A folder",
            "type": "folder"
         } ],
         "name": "Bookmarks bar",
         "type": "folder"
      },
      "other": { "children": [ { "name": "", "type": "url", "url": "http://four.example/" } ], "name": "Other", "type": "folder" },
      "synced": { "children": [  ], "name": "Mobile", "type": "folder" },
      "sync_transaction_version": "12"
   },
   "version": 1
}
)json";
    const auto found = ParseChromiumBookmarks(json);
    EXPECT(found.has_value());
    if (!found) return;
    EXPECT_EQ(found->size(), std::size_t{4});
    if (found->size() != 4) return;
    EXPECT_EQ((*found)[0].url, L"https://one.example/");
    EXPECT_EQ((*found)[0].title, L"Example \"one\"");
    // 13350000000 seconds since 1601 is 1705526400 since 1970.
    EXPECT_EQ((*found)[0].added, std::int64_t{1705526400});
    EXPECT_EQ((*found)[1].url, L"https://two.example/a?b=c");
    EXPECT_EQ((*found)[1].added, std::int64_t{1705526401});
    EXPECT_EQ((*found)[2].title, L"Three \u00E9");
    EXPECT_EQ((*found)[2].added, std::int64_t{0});
    EXPECT_EQ((*found)[3].url, L"http://four.example/");
}

TEST_CASE(WhatIsNoBookmarksFileIsRefused) {
    EXPECT(!ParseChromiumBookmarks(L""));
    EXPECT(!ParseChromiumBookmarks(L"not json"));
    EXPECT(!ParseChromiumBookmarks(L"[]"));
    EXPECT(!ParseChromiumBookmarks(LR"({"version": 1})"));
    EXPECT(!ParseChromiumBookmarks(
        LR"({"roots": {"bookmark_bar": {"children": [ {"type": "url", "url": "https://a.example/" ] }}})"));
    EXPECT(!ParseChromiumBookmarks(LR"({"roots": {}} and more)"));
    // A file with no bookmarks in it is one all the same.
    const auto none = ParseChromiumBookmarks(LR"({"roots": {"bookmark_bar": {"children": [], "type": "folder"}}})");
    EXPECT(none.has_value());
    EXPECT(none && none->empty());
}

TEST_CASE(BookmarksPageEscapesWhatItShows) {
    Bookmarks bookmarks;
    EXPECT(bookmarks.Add(L"https://a.example/?x=<b>", L"He said \"hi\"", 1));
    const BookmarkSource sources[] = {{L"edge", L"Microsoft Edge"}};
    const std::wstring html = BuildBookmarksPageHtml(bookmarks.entries(), sources, L"3 </script> added");
    EXPECT(html.starts_with(L"<!doctype html>"));
    EXPECT(html.contains(L"[\"https://a.example/?x=\\u003cb>\",\"He said \\\"hi\\\"\"],\n"));
    EXPECT(html.contains(L"[\"edge\",\"Microsoft Edge\"],"));
    EXPECT(html.contains(L"const notice = \"3 \\u003c/script> added\";"));
    EXPECT(html.ends_with(L"</script></body></html>"));
    // What it sends back is what webview_events.cpp listens for.
    EXPECT(html.contains(L"postMessage('" + std::wstring(kBookmarkPageRemove) + L"\\n'"));
    EXPECT(html.contains(L"postMessage('" + std::wstring(kBookmarkPageImport) + L"\\n'"));
}
