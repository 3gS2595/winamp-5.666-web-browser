#include "suggestions.h"

#include "text_util.h"
#include "url_util.h"

#include <algorithm>
#include <cwctype>
#include <functional>
#include <iterator>
#include <optional>
#include <unordered_map>
#include <utility>

namespace exodium {

namespace {

// In Frecency()'s terms: two fresh visits, or four older ones.
constexpr double kCompleteMinWorth = 2.0;
// An address typed out in full is the one that was meant, whatever longer ones begin with it.
constexpr double kExactAddressScore = 1e9;
// What a bookmark is worth, visited or not: as much as an address typed in once, lately.
constexpr double kBookmarkWorth = 5.0;

// A page that may be suggested: one in the history, or a bookmark.
struct Page {
    std::wstring_view url;
    std::wstring_view title;
    double worth = 0;
    bool bookmarked = false;
};

// Something the typed words turned up in. It is only made a row of the list, which
// takes copying what it shows, if it is among the few that get into the list: a letter
// or two turn up in most of a history of thousands of pages. So it is small, and cheap to
// put in order.
struct Match {
    double score = 0;
    std::size_t found = 0;   // how many were found before it
    std::wstring_view url;   // a page's
    std::wstring_view title; // and its title
    std::wstring_view shown; // and its address as the list shows it
    int query = -1;          // or the words of a past search, by their place in Candidates::queries
    bool bookmarked = false;

    [[nodiscard]] bool search() const noexcept { return query >= 0; }
};

// The order the list has them in: pages before past searches, the better first, and
// those worth the same as they were found, which is latest first. Past searches only fill
// what room the pages leave: their visits say little about how much they are wanted, as a
// results page is gone back to after every result looked at, while each result gets one
// visit.
[[nodiscard]] bool ComesBefore(const Match& a, const Match& b) noexcept {
    if (a.search() != b.search()) return b.search();
    if (a.score != b.score) return a.score > b.score;
    return a.found < b.found;
}

struct Candidates {
    Suggestion top;                // the best address that begins with the typed text, if any does
    std::vector<Suggestion> hosts; // the sites whose names begin with it
    std::unordered_map<std::wstring, std::size_t> hostAt; // where each is in `hosts`, by its name in lower case
    std::vector<Match> rows;                              // everything the typed words turn up in
    std::vector<std::wstring> queries;                    // the words of the past searches among them
};

// How well `term` is found in `text`: best at the very start, then at the start of a
// word, least inside one. 0 when it isn't there at all.
int MatchWeight(std::wstring_view text, std::wstring_view term) noexcept {
    int best = 0;
    for (auto at = FindIgnoreCase(text, term); at != std::wstring_view::npos && best < 2;
         at = FindIgnoreCase(text, term, at + 1)) {
        if (at == 0) return 3;
        if (!std::iswalnum(text[at - 1])) best = 2;
        else if (!best) best = 1;
    }
    return best;
}

// Does the page's address begin with `typed`? Its scheme and "www." may be left out,
// and only count as typed once the typing has gone past them.
void ConsiderAsCompletion(Candidates& found, const Page& page, const UrlSpan& span, bool searched,
                          std::wstring_view typed) {
    const std::wstring_view url = page.url;
    for (const std::size_t from : {span.bare, span.www, std::size_t{0}}) {
        if (typed.size() <= span.bare - from || !StartsWithIgnoreCase(url.substr(from), typed)) continue;

        if (span.web && from + typed.size() <= span.hostEnd) {
            // Still inside the site's name: the site is what's offered, and every page
            // visited on it speaks for it.
            const std::wstring_view site = url.substr(from, span.hostEnd - from);
            const auto [at, isNew] = found.hostAt.try_emplace(ToLower(site), found.hosts.size());
            if (isNew) {
                found.hosts.push_back(
                    {.text = std::wstring(site), .url = std::wstring(url.substr(0, span.hostEnd)) + L"/"});
            }
            Suggestion& host = found.hosts[at->second];
            host.score += page.worth;
            if (url.size() <= span.hostEnd + 1) { // its front page itself
                host.url = url;
                host.title = page.title;
                host.bookmarked = host.bookmarked || page.bookmarked;
            }
        } else if (!searched) {
            const double score = url.size() - from == typed.size() ? kExactAddressScore : page.worth;
            if (score > found.top.score) {
                found.top = {.text = std::wstring(url.substr(from)),
                             .title = std::wstring(page.title),
                             .url = std::wstring(url),
                             .search = false,
                             .bookmarked = page.bookmarked,
                             .score = score};
            }
        }
        return;
    }
}

// Do all the typed words turn up somewhere in the page? `query` is the search it
// stands for, if it is a results page.
void ConsiderAsWordMatch(Candidates& found, const Page& page, const UrlSpan& span, std::optional<std::wstring>&& query,
                         std::span<const std::wstring_view> terms) {
    // A results page that names no search, such as an image lookup, has nothing to offer.
    if (terms.empty() || (query && query->empty())) return;

    const std::wstring_view shown = query ? std::wstring_view(*query) : ShownPart(page.url, span);
    int fit = 0;
    for (const std::wstring_view term : terms) {
        int weight = MatchWeight(shown, term);
        if (!query) weight = std::max(weight, MatchWeight(page.title, term));
        if (!weight) return;
        fit += weight;
    }
    const double score = page.worth * fit / static_cast<double>(terms.size());
    const std::size_t place = found.rows.size();
    if (query) {
        found.rows.push_back({.score = score, .found = place, .query = static_cast<int>(found.queries.size())});
        found.queries.push_back(std::move(*query));
    } else {
        found.rows.push_back({.score = score,
                              .found = place,
                              .url = page.url,
                              .title = page.title,
                              .shown = shown,
                              .bookmarked = page.bookmarked});
    }
}

void Consider(Candidates& found, const Page& page, std::wstring_view typed, std::span<const std::wstring_view> terms) {
    const UrlSpan span = SpanOf(page.url);
    std::optional<std::wstring> query = SearchPageQuery(page.url, span);
    ConsiderAsCompletion(found, page, span, query.has_value(), typed);
    ConsiderAsWordMatch(found, page, span, std::move(query), terms);
}

// Puts one thing the typed words turned up in the list, unless it is there already, by
// its address or by what it shows.
void ListMatch(SuggestionList& list, const Match& match, const std::vector<std::wstring>& queries) {
    if (match.search()) {
        const std::wstring& words = queries[static_cast<std::size_t>(match.query)];
        const auto sameWords = [&words](const Suggestion& s) { return s.search && EqualsIgnoreCase(s.text, words); };
        if (std::ranges::any_of(list.rows, sameWords)) return;
        std::wstring url = BuildSearchUrl(words);
        const auto sameUrl = [&url](const Suggestion& s) { return EqualsIgnoreCase(s.url, url); };
        if (std::ranges::any_of(list.rows, sameUrl)) return;
        list.rows.push_back({.text = words, .url = std::move(url), .search = true, .score = match.score});
        return;
    }
    const auto samePage = [&match](const Suggestion& s) {
        return EqualsIgnoreCase(s.url, match.url) || (!s.search && EqualsIgnoreCase(s.text, match.shown));
    };
    if (const auto listed = std::ranges::find_if(list.rows, samePage); listed != list.rows.end()) {
        // A page that is in the history and a bookmark is listed once, with its star.
        if (match.bookmarked && EqualsIgnoreCase(listed->url, match.url)) listed->bookmarked = true;
        return;
    }
    list.rows.push_back({.text = std::wstring(match.shown),
                         .title = std::wstring(match.title),
                         .url = std::wstring(match.url),
                         .bookmarked = match.bookmarked,
                         .score = match.score});
}

// Puts what the typed words turned up in the list, best first, until it is full. They are
// only put in order as far as the list needs them: the best few dozen to begin with, which
// nearly always fill it, and the rest only if those turn out to be the same few pages and
// searches over and over.
void ListMatches(SuggestionList& list, Candidates& found) {
    constexpr std::size_t kFirstLook = 8 * kMaxSuggestions;
    std::vector<Match>& matches = found.rows;
    const auto firstEnd = matches.begin() + static_cast<std::ptrdiff_t>(std::min(kFirstLook, matches.size()));
    std::partial_sort(matches.begin(), firstEnd, matches.end(), ComesBefore);
    for (auto match = matches.begin(); match != matches.end(); ++match) {
        if (list.rows.size() >= kMaxSuggestions) return;
        if (match == firstEnd) std::sort(firstEnd, matches.end(), ComesBefore);
        ListMatch(list, *match, found.queries);
    }
}

} // namespace

SuggestionList BuildSuggestions(std::span<const HistoryEntry> history, std::wstring_view typed, bool allowCompletion,
                                std::int64_t now, std::span<const Bookmark> bookmarks) {
    SuggestionList list;
    const std::wstring_view text = Trim(typed);
    if (text.empty()) return list;
    if (text.size() != typed.size()) allowCompletion = false; // nothing can follow a typed space

    // The words that can match anywhere in an address or a title.
    const std::vector<std::wstring_view> terms = SplitWords(text.substr(SpanOf(text).bare));

    Candidates found;
    for (const HistoryEntry& entry : history) {
        Consider(found, {.url = entry.url, .title = entry.title, .worth = Frecency(entry, now)}, text, terms);
    }
    for (const Bookmark& bookmark : bookmarks) {
        Consider(found, {.url = bookmark.url, .title = bookmark.title, .worth = kBookmarkWorth, .bookmarked = true},
                 text, terms);
    }
    for (const Suggestion& host : found.hosts) {
        if (found.top.url.empty() || host.score > found.top.score) found.top = host;
    }

    ResolvedInput resolved = ResolveAddressInput(text);
    Suggestion own{.text = std::wstring(text), .url = std::move(resolved.url), .search = resolved.isSearch};

    // An address is only filled in on some evidence that it is wanted: typed in once
    // before, bookmarked, or a couple of recent visits. One followed link doesn't change
    // what Enter does.
    const Suggestion& top = found.top;
    const bool hasTop = !top.url.empty();
    const bool whole = hasTop && top.text.size() <= text.size();
    if (hasTop && (whole || (allowCompletion && top.score >= kCompleteMinWorth))) {
        list.rows.push_back(top);
        if (!whole) list.inlineCompletion = top.text.substr(text.size());
        if (!whole || own.search) list.rows.push_back(std::move(own));
    } else {
        list.rows.push_back(std::move(own));
        if (hasTop) list.rows.push_back(top);
    }

    ListMatches(list, found);
    return list;
}

} // namespace exodium
