#include "test_framework.h"

#include "history.h"
#include "search_engines.h"
#include "suggestions.h"
#include "text_util.h"
#include "url_util.h"

#include <algorithm>
#include <set>

using namespace exodium;

namespace {

// Uses an engine for the length of a test, and the first one on offer again after it,
// which is what every other test expects.
struct EngineInUse {
    explicit EngineInUse(std::wstring_view id, std::wstring_view customUrl = {}) { UseSearchEngine(id, customUrl); }
    ~EngineInUse() { UseSearchEngine(kSearchEngines.front().id); }
    EngineInUse(const EngineInUse&) = delete;
    EngineInUse& operator=(const EngineInUse&) = delete;
};

std::optional<std::wstring> QueryOf(std::wstring_view url) {
    return SearchPageQuery(url, SpanOf(url));
}

} // namespace

TEST_CASE(EnginesOnOfferAreFoundByTheirIds) {
    EXPECT(FindSearchEngine(L"google") == &kSearchEngines[0]);
    EXPECT_EQ(std::wstring(FindSearchEngine(L"DuckDuckGo")->name), L"DuckDuckGo");
    EXPECT(FindSearchEngine(kCustomSearchId) == nullptr);
    EXPECT(FindSearchEngine(L"altavista") == nullptr);
    EXPECT(FindSearchEngine(L"") == nullptr);
}

TEST_CASE(EveryEngineOnOfferHasAResultsPageAndAnIdOfItsOwn) {
    std::set<std::wstring> ids;
    for (const SearchEngine& engine : kSearchEngines) {
        EXPECT(engine.url.contains(L"%s"));
        EXPECT(ResultsPageOf(engine.url).has_value());
        EXPECT(!EqualsIgnoreCase(engine.id, kCustomSearchId));
        ids.insert(std::wstring(engine.id));
    }
    EXPECT_EQ(ids.size(), kSearchEngines.size());
}

TEST_CASE(CustomEngineAddressesGetAScheme) {
    EXPECT_EQ(NormalizeSearchUrl(L"kagi.com/search?q=%s"), L"https://kagi.com/search?q=%s");
    EXPECT_EQ(NormalizeSearchUrl(L"  https://example.com/?q=%s \t"), L"https://example.com/?q=%s");
    EXPECT_EQ(NormalizeSearchUrl(L"http://intranet/find?w=%s"), L"http://intranet/find?w=%s");
    EXPECT_EQ(NormalizeSearchUrl(L"file:///C:/search.html?q=%s"), L"file:///C:/search.html?q=%s");
    // An address in a parameter is not the scheme of the whole.
    EXPECT_EQ(NormalizeSearchUrl(L"example.com/go?to=https://x/%s"), L"https://example.com/go?to=https://x/%s");
    EXPECT_EQ(NormalizeSearchUrl(L"   "), L"");
}

TEST_CASE(TheWordsGoWhereverTheAddressSays) {
    EXPECT_EQ(FillSearchUrl(L"https://example.com/?q=%s", L"a%20b"), L"https://example.com/?q=a%20b");
    EXPECT_EQ(FillSearchUrl(L"https://example.com/%s/?again=%s", L"x"), L"https://example.com/x/?again=x");
    EXPECT_EQ(FillSearchUrl(L"%s", L"x"), L"x");
    // Forgotten, the %s is taken to be at the end.
    EXPECT_EQ(FillSearchUrl(L"https://example.com/search?q=", L"x"), L"https://example.com/search?q=x");
}

TEST_CASE(ResultsPagesAreKnownByAddressAndParameter) {
    const auto page = [](std::wstring_view url) {
        const auto found = ResultsPageOf(url);
        return found ? found->prefix + L" " + found->key : L"(none)";
    };
    EXPECT_EQ(page(L"https://www.google.com/search?q=%s"), L"google.com/search? q");
    EXPECT_EQ(page(L"https://duckduckgo.com/?q=%s"), L"duckduckgo.com/? q");
    EXPECT_EQ(page(L"https://search.yahoo.com/search?p=%s"), L"search.yahoo.com/search? p");
    EXPECT_EQ(page(L"https://www.startpage.com/sp/search?query=%s"), L"startpage.com/sp/search? query");
    EXPECT_EQ(page(L"https://example.com/find?type=web&q=%s&lang=en"), L"example.com/find? q");

    EXPECT_EQ(page(L"https://example.com/search/%s"), L"(none)");
    EXPECT_EQ(page(L"https://example.com/search?q="), L"(none)");
    EXPECT_EQ(page(L"https://example.com/search?=%s"), L"(none)");
    EXPECT_EQ(page(L"https://example.com/search?q%s"), L"(none)");
}

TEST_CASE(CustomSearchesAreLabelledWithTheirHost) {
    EXPECT_EQ(CustomSearchLabel(L"https://www.Kagi.com/search?q=%s"), L"kagi.com Search");
    EXPECT_EQ(CustomSearchLabel(L"http://intranet:8080/find?w=%s"), L"intranet Search");
    EXPECT_EQ(CustomSearchLabel(L"file:///C:/search.html?q=%s"), L"Search");
}

TEST_CASE(GoogleIsTheEngineUntilAnotherIsChosen) {
    EXPECT_EQ(SearchUrlTemplate(), L"https://www.google.com/search?q=%s");
    EXPECT_EQ(SearchLabel(), L"Google Search");
    EXPECT_EQ(BuildSearchUrl(L"c++ & c#"), L"https://www.google.com/search?q=c%2B%2B%20%26%20c%23");
}

TEST_CASE(SearchesGoToTheEngineInUse) {
    {
        const EngineInUse bing(L"bing");
        EXPECT_EQ(SearchLabel(), L"Bing Search");
        EXPECT_EQ(BuildSearchUrl(L"hello world"), L"https://www.bing.com/search?q=hello%20world");
        const ResolvedInput typed = ResolveAddressInput(L"hello world");
        EXPECT_EQ(typed.url, L"https://www.bing.com/search?q=hello%20world");
        EXPECT(typed.isSearch);
        // An address is still an address.
        EXPECT_EQ(ResolveAddressInput(L"example.com").url, L"https://example.com");
    }
    {
        const EngineInUse yahoo(L"YAHOO");
        EXPECT_EQ(BuildSearchUrl(L"x"), L"https://search.yahoo.com/search?p=x");
    }
    {
        const EngineInUse custom(kCustomSearchId, L"kagi.com/search?q=%s");
        EXPECT_EQ(SearchUrlTemplate(), L"https://kagi.com/search?q=%s");
        EXPECT_EQ(SearchLabel(), L"kagi.com Search");
        EXPECT_EQ(BuildSearchUrl(L"a b"), L"https://kagi.com/search?q=a%20b");
    }
}

TEST_CASE(AnEngineThatCantBeUsedIsGoogle) {
    {
        const EngineInUse unknown(L"altavista");
        EXPECT_EQ(SearchLabel(), L"Google Search");
    }
    {
        // Custom, with no address of its own.
        const EngineInUse custom(kCustomSearchId, L"  ");
        EXPECT_EQ(SearchUrlTemplate(), L"https://www.google.com/search?q=%s");
    }
    {
        // An address for a custom engine counts for nothing while another is in use.
        const EngineInUse bing(L"bing", L"https://kagi.com/search?q=%s");
        EXPECT_EQ(SearchLabel(), L"Bing Search");
    }
}

// The engine may change, but a search in the history stays a search: the results pages
// of every engine on offer are read, whichever is in use.
TEST_CASE(ResultsPagesOfEveryEngineOnOfferAreRead) {
    const EngineInUse duckDuckGo(L"duckduckgo");
    EXPECT_EQ(QueryOf(L"https://www.google.com/search?q=hello+world").value_or(L"?"), L"hello world");
    EXPECT_EQ(QueryOf(L"https://www.bing.com/search?q=a%20b&form=QBLH").value_or(L"?"), L"a b");
    EXPECT_EQ(QueryOf(L"https://duckduckgo.com/?t=h_&q=ducks&ia=web").value_or(L"?"), L"ducks");
    EXPECT_EQ(QueryOf(L"https://search.brave.com/search?q=brave").value_or(L"?"), L"brave");
    EXPECT_EQ(QueryOf(L"https://www.ecosia.org/search?method=index&q=trees").value_or(L"?"), L"trees");
    EXPECT_EQ(QueryOf(L"https://www.startpage.com/sp/search?query=private").value_or(L"?"), L"private");
    EXPECT_EQ(QueryOf(L"https://search.yahoo.com/search?p=yodel&fr=yfp").value_or(L"?"), L"yodel");
    // Yahoo's words are in p, not q.
    EXPECT_EQ(QueryOf(L"https://search.yahoo.com/search?q=not+these").value_or(L"?"), L"");
    // Startpage's own form posts its words, which leaves an address with no search in it.
    EXPECT_EQ(QueryOf(L"https://www.startpage.com/sp/search?").value_or(L"?"), L"");
    EXPECT(!QueryOf(L"https://www.startpage.com/sp/search"));

    EXPECT(!QueryOf(L"https://duckduckgo.com/"));
    EXPECT(!QueryOf(L"https://www.bing.com/maps?q=x"));
}

TEST_CASE(ACustomEnginesResultsPagesAreReadWhileItIsInUse) {
    const std::wstring_view page = L"https://kagi.com/search?q=kagi+words";
    {
        const EngineInUse custom(kCustomSearchId, L"https://kagi.com/search?q=%s");
        EXPECT_EQ(QueryOf(page).value_or(L"?"), L"kagi words");
        EXPECT_EQ(KnownResultsPages().size(), kSearchEngines.size() + 1);
    }
    EXPECT(!QueryOf(page));

    // One on offer with something added is known already, and its searches are its.
    const EngineInUse webOnly(kCustomSearchId, L"https://www.google.com/search?q=%s&udm=14");
    EXPECT_EQ(KnownResultsPages().size(), kSearchEngines.size());
    EXPECT_EQ(SearchLabel(), L"Google Search");
    EXPECT_EQ(BuildSearchUrl(L"x"), L"https://www.google.com/search?q=x&udm=14");
}

TEST_CASE(PastSearchesAreOfferedWithTheEngineInUse) {
    const EngineInUse duckDuckGo(L"duckduckgo");
    History history;
    history.RecordVisit(L"https://www.google.com/search?q=modern+c%2B%2B&hl=en", L"", false, 1'760'000'000);
    const SuggestionList list = BuildSuggestions(history.entries(), L"modern", true, 1'760'000'000);
    const auto past = std::ranges::find_if(list.rows, [](const Suggestion& row) { return row.text == L"modern c++"; });
    EXPECT(past != list.rows.end());
    if (past != list.rows.end()) {
        EXPECT(past->search);
        EXPECT_EQ(past->url, L"https://duckduckgo.com/?q=modern%20c%2B%2B");
    }
}
