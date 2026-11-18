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

// What the × at a row's end does to the history.
void Forget(History& history, const Suggestion& row) {
    for (const std::wstring& url : PagesBehind(history.entries(), row)) history.Remove(url);
}

// The addresses a history holds, latest first, with a space between.
std::wstring UrlsOf(const History& history) {
    std::wstring urls;
    for (const HistoryEntry& entry : history.entries()) urls.append(urls.empty() ? L"" : L" ").append(entry.url);
    return urls;
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

TEST_CASE(BookmarksAreSuggestedVisitedOrNot) {
    Bookmarks bookmarks;
    EXPECT(bookmarks.Add(L"https://threejs.org/docs/", L"three.js docs", kNow));
    const auto suggest = [&bookmarks](const History& history, std::wstring_view typed) {
        return BuildSuggestions(history.entries(), typed, true, kNow, bookmarks.entries());
    };

    // Never visited, it is filled in all the same: marking it says it is wanted.
    const SuggestionList typed = suggest(History(), L"thr");
    EXPECT_EQ(typed.inlineCompletion, L"eejs.org");
    EXPECT_EQ(typed.rows[0].url, L"https://threejs.org/");

    // By a word of its title, with its star.
    const SuggestionList worded = suggest(History(), L"docs");
    EXPECT_EQ(worded.rows.size(), std::size_t{2});
    EXPECT_EQ(worded.rows[1].url, L"https://threejs.org/docs/");
    EXPECT_EQ(worded.rows[1].title, L"three.js docs");
    EXPECT(worded.rows[1].bookmarked);
    EXPECT(!worded.rows[0].bookmarked);

    // In the history as well, it is listed once, and before a page seen as often that isn't one.
    History history;
    history.RecordVisit(L"https://other.example/docs", L"Other docs", false, kNow);
    history.RecordVisit(L"https://threejs.org/docs/", L"three.js docs", false, kNow);
    const SuggestionList both = suggest(history, L"docs");
    EXPECT_EQ(both.rows.size(), std::size_t{3});
    EXPECT_EQ(both.rows[1].url, L"https://threejs.org/docs/");
    EXPECT(both.rows[1].bookmarked);
    EXPECT_EQ(both.rows[2].url, L"https://other.example/docs");
    EXPECT(!both.rows[2].bookmarked);
}

TEST_CASE(SuggestionsFromALongHistoryAreTheSame) {
    // Enough pages that the words turn up in thousands, of which the best are listed.
    History history;
    std::wstring text;
    for (int i = 0; i < 5000; ++i)
        text += std::format(L"https://site{}.example/page/{}\t1\t0\t{}\tPage {}\n", i, i, kNow, i);
    text += std::format(L"https://best.example/page\t9\t2\t{}\tThe best page\n", kNow);
    history = History::Parse(text);
    const SuggestionList list = Suggest(history, L"page");
    EXPECT_EQ(list.rows.size(), kMaxSuggestions);
    EXPECT_EQ(list.rows[1].url, L"https://best.example/page");
    // Those worth the same stay in the history's order, latest first.
    EXPECT_EQ(list.rows[2].url, L"https://site0.example/page/0");
    EXPECT_EQ(list.rows[3].url, L"https://site1.example/page/1");
}

TEST_CASE(RowsSayWhatOfTheHistoryIsBehindThem) {
    const History history = Visited(
        {L"https://docs.rs/alpha", L"https://docs.rs/beta", L"https://www.google.com/search?q=doc+comments&hl=en"});
    const SuggestionList list = Suggest(history, L"doc");
    EXPECT_EQ(list.rows.size(), std::size_t{5});
    // The site its pages speak for, the typed text, the two pages, the search made before.
    EXPECT_EQ(list.rows[0].text, L"docs.rs");
    EXPECT(list.rows[0].behind == HistoryBehind::kSite);
    EXPECT_EQ(list.rows[1].text, L"doc");
    EXPECT(list.rows[1].behind == HistoryBehind::kNothing);
    EXPECT_EQ(list.rows[2].url, L"https://docs.rs/beta");
    EXPECT(list.rows[2].behind == HistoryBehind::kPage);
    EXPECT(list.rows[3].behind == HistoryBehind::kPage);
    EXPECT_EQ(list.rows[4].text, L"doc comments");
    EXPECT(list.rows[4].behind == HistoryBehind::kSearch);

    // An address that goes on past the site's name is a page, not the site.
    const SuggestionList page = Suggest(history, L"docs.rs/al");
    EXPECT_EQ(page.rows[1].url, L"https://docs.rs/alpha");
    EXPECT(page.rows[1].behind == HistoryBehind::kPage);

    // Nothing typed before has anything behind it.
    const SuggestionList fresh = Suggest(history, L"zebra");
    EXPECT_EQ(fresh.rows.size(), std::size_t{1});
    EXPECT(PagesBehind(history.entries(), fresh.rows[0]).empty());
}

TEST_CASE(ForgettingAPageLeavesTheOthers) {
    History history = Visited({L"https://docs.rs/alpha", L"https://docs.rs/beta"});
    Forget(history, Suggest(history, L"alp").rows[1]);
    EXPECT_EQ(UrlsOf(history), L"https://docs.rs/beta");
    EXPECT_EQ(Suggest(history, L"alp").rows.size(), std::size_t{1});

    // Visited again, it is suggested again.
    history.RecordVisit(L"https://docs.rs/alpha", L"", false, kNow);
    EXPECT_EQ(Suggest(history, L"alp").rows[1].url, L"https://docs.rs/alpha");
}

TEST_CASE(ForgettingASiteTakesEveryPageOnIt) {
    History history = Visited({L"https://docs.rs/alpha", L"https://www.docs.rs/beta", L"http://docs.rs/",
                               L"https://docs.rs.example/", L"https://other.example/docs.rs"});
    const SuggestionList list = Suggest(history, L"docs.r");
    EXPECT_EQ(list.rows[0].text, L"docs.rs");
    Forget(history, list.rows[0]);
    // Over http and https, with "www." and without; but no site that only begins the same,
    // and no page that only names it.
    EXPECT_EQ(UrlsOf(history), L"https://other.example/docs.rs https://docs.rs.example/");
    const SuggestionList after = Suggest(history, L"docs.r");
    EXPECT(after.inlineCompletion.empty());
    EXPECT_EQ(after.rows[1].text, L"docs.rs.example");

    // A page of the site visited again brings the site back.
    history.RecordVisit(L"https://docs.rs/gamma", L"", true, kNow);
    EXPECT_EQ(Suggest(history, L"docs.r").rows[0].text, L"docs.rs");
}

TEST_CASE(ASiteIsForgottenAsItWasTyped) {
    // Typed with its "www.", the row names the site with it, and the pages without aren't behind it.
    History history = Visited({L"https://www.docs.rs/alpha", L"https://docs.rs/beta"});
    const SuggestionList list = Suggest(history, L"www.do");
    EXPECT_EQ(list.rows[1].text, L"www.docs.rs");
    Forget(history, list.rows[1]);
    EXPECT_EQ(UrlsOf(history), L"https://docs.rs/beta");
}

TEST_CASE(ForgettingASearchTakesEveryResultsPageOfIt) {
    History history = Visited({L"https://www.google.com/search?q=modern+c%2B%2B&hl=en",
                               L"https://www.google.com/search?q=Modern+C%2B%2B&start=10",
                               L"https://www.google.com/search?q=modern+art", L"https://plain.example/modern"});
    // The same words in other letters are the same search, listed once: the latest of them.
    const SuggestionList list = Suggest(history, L"modern c");
    EXPECT_EQ(list.rows.size(), std::size_t{2});
    EXPECT_EQ(list.rows[1].text, L"Modern C++");
    Forget(history, list.rows[1]);
    EXPECT_EQ(UrlsOf(history), L"https://plain.example/modern https://www.google.com/search?q=modern+art");

    // Typed out in full, a search made before is the top row, and is forgotten from there.
    const SuggestionList whole = Suggest(history, L"Modern Art");
    EXPECT_EQ(whole.rows.size(), std::size_t{1});
    EXPECT(whole.rows[0].behind == HistoryBehind::kSearch);
    Forget(history, whole.rows[0]);
    EXPECT_EQ(UrlsOf(history), L"https://plain.example/modern");
    EXPECT(Suggest(history, L"Modern Art").rows[0].behind == HistoryBehind::kNothing);
}

TEST_CASE(ABookmarkIsNotForgotten) {
    Bookmarks bookmarks;
    EXPECT(bookmarks.Add(L"https://threejs.org/docs/", L"three.js docs", kNow));
    const auto suggest = [&bookmarks](const History& history, std::wstring_view typed) {
        return BuildSuggestions(history.entries(), typed, true, kNow, bookmarks.entries());
    };

    // On its own it has nothing behind it, as a page or as the site it speaks for.
    History history;
    EXPECT(suggest(history, L"docs").rows[1].behind == HistoryBehind::kNothing);
    EXPECT(suggest(history, L"thr").rows[0].behind == HistoryBehind::kNothing);

    // In the history as well, that much of it can be forgotten, and it stays for its star.
    history.RecordVisit(L"https://threejs.org/docs/", L"three.js docs", false, kNow);
    const SuggestionList both = suggest(history, L"docs");
    EXPECT(both.rows[1].bookmarked);
    EXPECT(both.rows[1].behind == HistoryBehind::kPage);
    EXPECT(suggest(history, L"thr").rows[0].behind == HistoryBehind::kSite);
    // So it is as the address that is filled in, whichever of the two counts for more.
    for (const bool typed : {false, true}) {
        history.RecordVisit(L"https://threejs.org/docs/", L"", typed, kNow);
        const SuggestionList filled = suggest(history, L"threejs.org/d");
        EXPECT_EQ(filled.inlineCompletion, L"ocs/");
        EXPECT(filled.rows[0].bookmarked);
        EXPECT(filled.rows[0].behind == HistoryBehind::kPage);
    }
    Forget(history, both.rows[1]);
    EXPECT(history.entries().empty());
    const SuggestionList left = suggest(history, L"docs");
    EXPECT_EQ(left.rows[1].url, L"https://threejs.org/docs/");
    EXPECT(left.rows[1].bookmarked);
    EXPECT(left.rows[1].behind == HistoryBehind::kNothing);
}

TEST_CASE(ListIsCapped) {
    History history;
    for (int i = 0; i < 20; ++i) history.RecordVisit(std::format(L"https://site{}.example/page", i), L"", false, kNow);
    EXPECT_EQ(Suggest(history, L"page").rows.size(), kMaxSuggestions);
}
