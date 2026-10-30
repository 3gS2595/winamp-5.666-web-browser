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

TEST_CASE(RecognisesBareDomains) {
    EXPECT(LooksLikeDomain(L"example.com"));
    EXPECT(LooksLikeDomain(L"sub.example.museum"));
    EXPECT(!LooksLikeDomain(L"example"));
    EXPECT(!LooksLikeDomain(L"two words.com"));
    EXPECT(!LooksLikeDomain(L"example.c"));
    EXPECT(!LooksLikeDomain(L"example.c0m"));
    EXPECT(!LooksLikeDomain(L"example."));
    EXPECT(!LooksLikeDomain(L""));
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
