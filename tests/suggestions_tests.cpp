#include "test_framework.h"

#include "suggestions.h"
#include "url_util.h"

#include <format>

using namespace exodium;

namespace {

constexpr std::int64_t kNow = 1'760'000'000;

History Visited(std::initializer_list<std::wstring_view> urls, bool typed = false) {
    History history;
    for (const std::wstring_view url : urls) history.RecordVisit(url, L"", typed, kNow);
    return history;
}

SuggestionList Suggest(const History& history, std::wstring_view typed, bool allowCompletion = true) {
    return BuildSuggestions(history.entries(), typed, allowCompletion, kNow);
}

} // namespace

TEST_CASE(NothingTypedSuggestsNothing) {
    const History history = Visited({L"https://example.com/"});
    EXPECT(Suggest(history, L"").rows.empty());
    EXPECT(Suggest(history, L"   ").rows.empty());
}

TEST_CASE(TypedTextIsItsOwnTopRow) {
    const SuggestionList search = Suggest(History(), L"hello world");
    EXPECT_EQ(search.rows.size(), std::size_t{1});
    EXPECT(search.rows[0].search);
    EXPECT_EQ(search.rows[0].text, L"hello world");
    EXPECT_EQ(search.rows[0].url, BuildSearchUrl(L"hello world"));
    EXPECT(search.inlineCompletion.empty());

    const SuggestionList address = Suggest(History(), L"example.com");
    EXPECT_EQ(address.rows.size(), std::size_t{1});
    EXPECT(!address.rows[0].search);
    EXPECT_EQ(address.rows[0].url, L"https://example.com");
}

TEST_CASE(CompletesAnAddressTypedBefore) {
    const History history = Visited({L"https://www.github.com/"}, true);
    const SuggestionList list = Suggest(history, L"gi");

    // The site takes the top row and the rest of its name follows the caret.
    EXPECT_EQ(list.inlineCompletion, L"thub.com");
    EXPECT_EQ(list.rows.size(), std::size_t{2});
    EXPECT_EQ(list.rows[0].text, L"github.com");
    EXPECT_EQ(list.rows[0].url, L"https://www.github.com/");
    EXPECT(!list.rows[0].search);
    // The typed text is still on offer, as the search it would otherwise be.
    EXPECT_EQ(list.rows[1].text, L"gi");
    EXPECT(list.rows[1].search);
}

TEST_CASE(SchemeAndWwwCountOnceTypedPast) {
    const History history = Visited({L"https://www.github.com/"}, true);
    EXPECT_EQ(Suggest(history, L"www.gi").inlineCompletion, L"thub.com");
    EXPECT_EQ(Suggest(history, L"https://www.gi").inlineCompletion, L"thub.com");
    // Just the start of "www." is no reason to offer every site that has one.
    EXPECT(Suggest(history, L"ww").inlineCompletion.empty());
}

TEST_CASE(DoesNotCompleteAfterADeletionOrASpace) {
    const History history = Visited({L"https://www.github.com/"}, true);

    const SuggestionList deleted = Suggest(history, L"gi", false);
    EXPECT(deleted.inlineCompletion.empty());
    EXPECT_EQ(deleted.rows.size(), std::size_t{2});
    EXPECT_EQ(deleted.rows[0].text, L"gi");
    EXPECT_EQ(deleted.rows[1].url, L"https://www.github.com/");

    const SuggestionList spaced = Suggest(history, L"gi ");
    EXPECT(spaced.inlineCompletion.empty());
    EXPECT_EQ(spaced.rows[0].text, L"gi");
}

TEST_CASE(OneFollowedLinkIsNotEnoughToComplete) {
    const History history = Visited({L"https://www.github.com/"});
    const SuggestionList list = Suggest(history, L"gi");
    EXPECT(list.inlineCompletion.empty());
    EXPECT_EQ(list.rows[0].text, L"gi");
    EXPECT_EQ(list.rows[1].url, L"https://www.github.com/");
}

TEST_CASE(PagesOfASiteSpeakForIt) {
    // Neither page was visited enough to be completed, but the site was.
    const History history = Visited({L"https://docs.rs/alpha", L"https://docs.rs/beta"});
    const SuggestionList list = Suggest(history, L"do");
    EXPECT_EQ(list.inlineCompletion, L"cs.rs");
    EXPECT_EQ(list.rows[0].text, L"docs.rs");
    EXPECT_EQ(list.rows[0].url, L"https://docs.rs/");
}

TEST_CASE(AnAddressTypedInFullIsTheOneMeant) {
    const History history = Visited({L"https://example.com/docs/intro", L"https://example.com/docs"});
    const SuggestionList list = Suggest(history, L"example.com/docs");
    EXPECT(list.inlineCompletion.empty());
    EXPECT_EQ(list.rows[0].url, L"https://example.com/docs");
    EXPECT(!list.rows[0].search);
}

TEST_CASE(MatchesWordsInTitles) {
    History history;
    history.RecordVisit(L"https://news.example/article-1", L"Quantum computing breakthrough", false, kNow);
    history.RecordVisit(L"https://news.example/article-2", L"Gardening tips", false, kNow);

    const SuggestionList list = Suggest(history, L"computing quantum");
    EXPECT_EQ(list.rows.size(), std::size_t{2});
    EXPECT(list.rows[0].search);
    EXPECT_EQ(list.rows[1].url, L"https://news.example/article-1");
    EXPECT_EQ(list.rows[1].title, L"Quantum computing breakthrough");
    EXPECT_EQ(list.rows[1].text, L"news.example/article-1");
}

TEST_CASE(PastSearchesAreOfferedAsSearches) {
    const History history = Visited({L"https://www.google.com/search?q=modern+c%2B%2B&hl=en"});
    const SuggestionList list = Suggest(history, L"modern");
    EXPECT_EQ(list.rows.size(), std::size_t{2});
    EXPECT_EQ(list.rows[1].text, L"modern c++");
    EXPECT(list.rows[1].search);
    EXPECT_EQ(list.rows[1].url, BuildSearchUrl(L"modern c++"));

    // Typed out in full it is the same search as the top row, so it is listed once.
    EXPECT_EQ(Suggest(history, L"modern c++").rows.size(), std::size_t{1});
}

TEST_CASE(PagesComeBeforePastSearches) {
    History history;
    // Gone back to after each result looked at, the results page has far more visits
    // than the page that was found from it.
    for (int i = 0; i < 6; ++i) {
        history.RecordVisit(L"https://www.google.com/search?q=claude+code", L"", false, kNow);
    }
    history.RecordVisit(L"https://docs.example/agents", L"Claude Code overview", false, kNow);

    const SuggestionList list = Suggest(history, L"claude");
    EXPECT_EQ(list.rows.size(), std::size_t{3});
    EXPECT(list.rows[0].search);
    EXPECT_EQ(list.rows[0].text, L"claude");
    EXPECT_EQ(list.rows[1].url, L"https://docs.example/agents");
    EXPECT(list.rows[2].search);
    EXPECT_EQ(list.rows[2].text, L"claude code");
}

TEST_CASE(BetterMatchesComeFirst) {
    History history;
    history.RecordVisit(L"https://one.example/mypage", L"", false, kNow); // inside a word
    history.RecordVisit(L"https://two.example/a-page", L"", false, kNow); // at the start of a word
    const SuggestionList list = Suggest(history, L"page");
    EXPECT_EQ(list.rows.size(), std::size_t{3});
    EXPECT_EQ(list.rows[1].url, L"https://two.example/a-page");
    EXPECT_EQ(list.rows[2].url, L"https://one.example/mypage");
}

TEST_CASE(ListIsCapped) {
    History history;
    for (int i = 0; i < 20; ++i) history.RecordVisit(std::format(L"https://site{}.example/page", i), L"", false, kNow);
    EXPECT_EQ(Suggest(history, L"page").rows.size(), kMaxSuggestions);
}
