#include "test_framework.h"

#include "url_util.h"

using namespace exodium;

TEST_CASE(RecognisesAddressesTheHistoryKeeps) {
    EXPECT(IsLikelyHttpUrl(L"HTTP://example.com"));
    EXPECT(IsLikelyHttpUrl(L"https://example.com"));
    EXPECT(IsLikelyHttpUrl(L"file:///C:/page.html"));
    EXPECT(!IsLikelyHttpUrl(L"about:blank"));
    EXPECT(!IsLikelyHttpUrl(L"example.com"));
    EXPECT(!IsLikelyHttpUrl(L""));
}

namespace {

// Where typed text leads, and "search" for text that is searched for.
std::wstring Typed(std::wstring_view text) {
    const ResolvedInput resolved = ResolveAddressInput(text);
    return resolved.isSearch ? L"search" : resolved.url;
}

} // namespace

TEST_CASE(SitesTypedWithoutASchemeAreAskedOverHttps) {
    EXPECT_EQ(Typed(L"example.com"), L"https://example.com");
    EXPECT_EQ(Typed(L"sub.example.museum"), L"https://sub.example.museum");
    EXPECT_EQ(Typed(L"www.example.com"), L"https://www.example.com");
    EXPECT_EQ(Typed(L"example.com."), L"https://example.com.");
    EXPECT_EQ(Typed(L"b\u00FCcher.de"), L"https://b\u00FCcher.de");
    EXPECT_EQ(Typed(L"xn--bcher-kva.xn--p1ai"), L"https://xn--bcher-kva.xn--p1ai");
    // With whatever follows the name: until 2026-11-09 a path made it a search.
    EXPECT_EQ(Typed(L"github.com/anthropics/claude-code"), L"https://github.com/anthropics/claude-code");
    EXPECT_EQ(Typed(L"en.wikipedia.org/wiki/Winamp"), L"https://en.wikipedia.org/wiki/Winamp");
    EXPECT_EQ(Typed(L"example.com/a.b/c?d=e.f#g"), L"https://example.com/a.b/c?d=e.f#g");
    EXPECT_EQ(Typed(L"example.com?q=1"), L"https://example.com?q=1");
    EXPECT_EQ(Typed(L"example.com:8443/app"), L"https://example.com:8443/app");
    // Spaces may come after the name, where they are part of the address.
    EXPECT_EQ(Typed(L"example.com/a page"), L"https://example.com/a page");
}

TEST_CASE(ThisComputerAndItsNeighboursAreAskedOverHttp) {
    EXPECT_EQ(Typed(L"localhost"), L"http://localhost");
    EXPECT_EQ(Typed(L"LocalHost:3000"), L"http://LocalHost:3000");
    EXPECT_EQ(Typed(L"localhost:5173/index.html"), L"http://localhost:5173/index.html");
    EXPECT_EQ(Typed(L"localhost/app?x=1"), L"http://localhost/app?x=1");
    EXPECT_EQ(Typed(L"127.0.0.1"), L"http://127.0.0.1");
    EXPECT_EQ(Typed(L"127.0.0.1:8000"), L"http://127.0.0.1:8000");
    EXPECT_EQ(Typed(L"192.168.1.1/admin"), L"http://192.168.1.1/admin");
    EXPECT_EQ(Typed(L"[::1]"), L"http://[::1]");
    EXPECT_EQ(Typed(L"[::1]:8080/x"), L"http://[::1]:8080/x");
    EXPECT_EQ(Typed(L"printer.local"), L"http://printer.local");
    EXPECT_EQ(Typed(L"app.localhost:3000"), L"http://app.localhost:3000");
    // A bare name is a word, unless a port says it is a computer's.
    EXPECT_EQ(Typed(L"nas:8080"), L"http://nas:8080");
    EXPECT_EQ(Typed(L"nas"), L"search");
    EXPECT_EQ(Typed(L"nas/files"), L"search");
}

TEST_CASE(AddressesWithTheirSchemeAreTakenAsTheyStand) {
    EXPECT_EQ(Typed(L"https://example.com/a?b=c"), L"https://example.com/a?b=c");
    EXPECT_EQ(Typed(L"HTTP://localhost:3000"), L"HTTP://localhost:3000");
    EXPECT_EQ(Typed(L"file:///C:/page.html"), L"file:///C:/page.html");
    EXPECT_EQ(Typed(L"edge://gpu"), L"edge://gpu");
    EXPECT_EQ(Typed(L"chrome-extension://abcdefgh/popup.html"), L"chrome-extension://abcdefgh/popup.html");
    EXPECT_EQ(Typed(L"about:blank"), L"about:blank");
    EXPECT_EQ(Typed(L"view-source:https://example.com"), L"view-source:https://example.com");
    EXPECT_EQ(Typed(L"EXODIUM://History"), kHistoryUrl);
    EXPECT_EQ(Typed(L"exodium://bookmarks"), kBookmarksUrl);
    // A file on this computer, by its path.
    EXPECT_EQ(Typed(LR"(C:\My Pages\page.html)"), L"file:///C:/My%20Pages/page.html");
    EXPECT_EQ(Typed(LR"(\\server\share\page.html)"), L"file://server/share/page.html");
}

TEST_CASE(WhatIsNoAddressIsSearchedFor) {
    EXPECT_EQ(Typed(L"hello world"), L"search");
    EXPECT_EQ(Typed(L"example"), L"search");
    EXPECT_EQ(Typed(L"example.com is down"), L"search");
    EXPECT_EQ(Typed(L"two words.com"), L"search");
    EXPECT_EQ(Typed(L"what is c++"), L"search");
    // Windows reads anything like "word:" as an address with a scheme, and until
    // 2026-11-09 so did the address bar: an error message pasted in went nowhere.
    EXPECT_EQ(Typed(L"error: cannot find module"), L"search");
    EXPECT_EQ(Typed(L"TypeError: x is not a function"), L"search");
    EXPECT_EQ(Typed(L"note:"), L"search");
    EXPECT_EQ(Typed(L"std::vector"), L"search");
    EXPECT_EQ(Typed(L"16:9"), L"search");
    EXPECT_EQ(Typed(L"localhost:port"), L"search");
    EXPECT_EQ(Typed(L"example.com:99999"), L"search");
    // Script addresses are not run on the address bar's say-so.
    EXPECT_EQ(Typed(L"javascript:alert(1)"), L"search");
    // Numbers, mail addresses, and names that end as files and no sites do.
    EXPECT_EQ(Typed(L"1.5"), L"search");
    EXPECT_EQ(Typed(L"v2.0"), L"search");
    EXPECT_EQ(Typed(L"1.2.3"), L"search");
    EXPECT_EQ(Typed(L"example.c"), L"search");
    EXPECT_EQ(Typed(L"example.c0m"), L"search");
    EXPECT_EQ(Typed(L"someone@example.com"), L"search");
    EXPECT_EQ(Typed(L"three.js"), L"search");
    EXPECT_EQ(Typed(L"notes.txt"), L"search");
    EXPECT_EQ(Typed(L"three.js docs"), L"search");

    const ResolvedInput search = ResolveAddressInput(L"hello world");
    EXPECT_EQ(search.url, L"https://www.google.com/search?q=hello%20world");
    EXPECT(search.isSearch);
}

TEST_CASE(ThePluginsOwnPagesHaveNames) {
    EXPECT_EQ(PluginPageName(kHistoryUrl), std::wstring_view(kHistoryName));
    EXPECT_EQ(PluginPageName(kBookmarksUrl), std::wstring_view(kBookmarksName));
    EXPECT(PluginPageName(L"https://example.com/").empty());
    EXPECT(PluginPageName(L"").empty());
}

TEST_CASE(PercentEncodesEverythingReserved) {
    EXPECT_EQ(PercentEncode(L"a-b_c.d~e9"), L"a-b_c.d~e9");
    EXPECT_EQ(PercentEncode(L"c++ & c#"), L"c%2B%2B%20%26%20c%23");
    EXPECT_EQ(PercentEncode(L"\u00E9"), L"%C3%A9");
    EXPECT_EQ(PercentEncode(L""), L"");
}

TEST_CASE(PercentDecodesWhatItCan) {
    EXPECT_EQ(PercentDecode(L"c%2B%2B%20%26"), L"c++ &");
    EXPECT_EQ(PercentDecode(L"%C3%A9t%c3%a9"), L"\u00E9t\u00E9");
    EXPECT_EQ(PercentDecode(L"100%"), L"100%");
    EXPECT_EQ(PercentDecode(L"%zz and %4"), L"%zz and %4");
    EXPECT_EQ(PercentDecode(PercentEncode(L"\u65E5\u672C \u2715?")), L"\u65E5\u672C \u2715?");
}

TEST_CASE(TypedTextLeadsSomewhere) {
    const ResolvedInput url = ResolveAddressInput(L"https://example.com/a?b=c");
    EXPECT_EQ(url.url, L"https://example.com/a?b=c");
    EXPECT(!url.isSearch);

    const ResolvedInput domain = ResolveAddressInput(L"example.com");
    EXPECT_EQ(domain.url, L"https://example.com");
    EXPECT(!domain.isSearch);

    const ResolvedInput search = ResolveAddressInput(L"hello world");
    EXPECT_EQ(search.url, L"https://www.google.com/search?q=hello%20world");
    EXPECT(search.isSearch);

    const ResolvedInput history = ResolveAddressInput(L"EXODIUM://History");
    EXPECT_EQ(history.url, kHistoryUrl);
    EXPECT(!history.isSearch);
}

TEST_CASE(OutsideLinksLeadToPages) {
    const auto resolved = [](std::wstring_view link) { return ResolveOutsideLink(link).value_or(L"(refused)"); };

    EXPECT_EQ(resolved(L"https://example.com/a?b=c#d"), L"https://example.com/a?b=c#d");
    EXPECT_EQ(resolved(L"HTTP://example.com"), L"HTTP://example.com");
    EXPECT_EQ(resolved(L"file:///C:/page.html"), L"file:///C:/page.html");

    // A file type Windows sends here comes as a path.
    EXPECT_EQ(resolved(LR"(C:\Pages\page.html)"), L"file:///C:/Pages/page.html");
    EXPECT_EQ(resolved(LR"(C:\My Pages\a#b.html)"), L"file:///C:/My%20Pages/a%23b.html");
    EXPECT_EQ(resolved(LR"(\\server\share\page.html)"), L"file://server/share/page.html");

    EXPECT_EQ(resolved(L""), L"");
}

TEST_CASE(OutsideLinksThatLeadToNoPageAreRefused) {
    EXPECT(!ResolveOutsideLink(L"javascript:alert(1)"));
    EXPECT(!ResolveOutsideLink(L"about:blank"));
    EXPECT(!ResolveOutsideLink(L"exodium://history"));
    EXPECT(!ResolveOutsideLink(L"--class=Evil https://example.com/"));
    EXPECT(!ResolveOutsideLink(L"page.html"));
}

TEST_CASE(ClaudeCodesWindowsSayWhichSessionTheyBelongTo) {
    const wchar_t start[] = L"file:///C:/Winamp/Plugins/exodium_start.html";
    EXPECT_EQ(ToolSessionOf(std::wstring(start) + L"#pid=1234&3f1c-uuid").value_or(0), 1234u);
    EXPECT_EQ(ToolSessionOf(std::wstring(start) + L"#3f1c-uuid&pid=77").value_or(0), 77u);
    // The start page of a wrapper from before sessions were named, and anything else.
    EXPECT(!ToolSessionOf(std::wstring(start) + L"#3f1c-uuid"));
    EXPECT(!ToolSessionOf(start));
    EXPECT(!ToolSessionOf(L"https://example.com/#pid=1234"));
    EXPECT(!ToolSessionOf(std::wstring(start) + L"#pid=12x4"));
    EXPECT(!ToolSessionOf(std::wstring(start) + L"#pid=0"));
    EXPECT(!ToolSessionOf(std::wstring(start) + L"#pid="));
}

TEST_CASE(FindsThePartsOfAnAddress) {
    const UrlSpan full = SpanOf(L"https://www.example.com/path");
    EXPECT(full.web);
    EXPECT_EQ(full.www, std::size_t{8});
    EXPECT_EQ(full.bare, std::size_t{12});
    EXPECT_EQ(full.hostEnd, std::size_t{23});

    const UrlSpan plain = SpanOf(L"HTTP://example.com");
    EXPECT(plain.web);
    EXPECT_EQ(plain.www, std::size_t{7});
    EXPECT_EQ(plain.bare, std::size_t{7});
    EXPECT_EQ(plain.hostEnd, std::size_t{18});

    const UrlSpan file = SpanOf(L"file:///C:/page.html");
    EXPECT(!file.web);
    EXPECT_EQ(file.bare, std::size_t{0});
    EXPECT_EQ(file.hostEnd, std::size_t{0});

    // As typed in the address bar, with no scheme.
    const UrlSpan typed = SpanOf(L"www.example.com");
    EXPECT(!typed.web);
    EXPECT_EQ(typed.bare, std::size_t{4});
}

TEST_CASE(ShowsAddressesWithoutSchemeOrWww) {
    const std::wstring_view front = L"https://www.example.com/";
    EXPECT_EQ(ShownUrl(front, SpanOf(front)), L"example.com");
    EXPECT_EQ(ShownPart(front, SpanOf(front)), std::wstring_view(L"example.com"));
    const std::wstring_view deep = L"https://example.com/a/";
    EXPECT_EQ(ShownUrl(deep, SpanOf(deep)), L"example.com/a/");
    const std::wstring_view local = L"file:///C:/page.html";
    EXPECT_EQ(ShownUrl(local, SpanOf(local)), L"file:///C:/page.html");
}

TEST_CASE(HostIsLowerCaseAndKeepsWww) {
    EXPECT_EQ(HostOf(L"https://WWW.Example.com/Path"), L"www.example.com");
    EXPECT_EQ(HostOf(L"http://example.com?x=1"), L"example.com");
    EXPECT_EQ(HostOf(L"file:///C:/page.html"), L"");
    EXPECT_EQ(HostOf(L""), L"");
}

TEST_CASE(ReadsTheSearchOutOfAResultsPage) {
    const auto query = [](std::wstring_view url) { return SearchPageQuery(url, SpanOf(url)); };

    EXPECT_EQ(query(L"https://www.google.com/search?q=hello+world&hl=en").value_or(L"?"), L"hello world");
    EXPECT_EQ(query(L"https://www.google.com/search?hl=en&q=c%2B%2B#top").value_or(L"?"), L"c++");
    EXPECT_EQ(query(L"https://google.com/search?q=%C3%A9t%C3%A9").value_or(L"?"), L"\u00E9t\u00E9");

    // A results page that names no search still is one.
    EXPECT_EQ(query(L"https://www.google.com/search?tbm=isch").value_or(L"?"), L"");
    EXPECT_EQ(query(L"https://www.google.com/search?q=").value_or(L"?"), L"");

    EXPECT(!query(L"https://example.com/search?q=x"));
    EXPECT(!query(L"https://www.google.com/maps"));
    EXPECT(!query(L"file:///google.com/search?q=x"));
}
