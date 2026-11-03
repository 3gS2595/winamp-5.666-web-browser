#include "suggestions.h"

#include "text_util.h"
#include "url_util.h"

#include <algorithm>
#include <cwctype>
#include <functional>
#include <iterator>
#include <optional>

namespace exodium {

namespace {

// In Frecency()'s terms: two fresh visits, or four older ones.
constexpr double kCompleteMinWorth = 2.0;
// An address typed out in full is the one that was meant, whatever longer ones begin with it.
constexpr double kExactAddressScore = 1e9;

struct Candidates {
    Suggestion top;                // the best address that begins with the typed text, if any does
    std::vector<Suggestion> hosts; // the sites whose names begin with it
    std::vector<Suggestion> rows;  // everything the typed words turn up in
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

// Does the entry's address begin with `typed`? Its scheme and "www." may be left out,
// and only count as typed once the typing has gone past them.
void ConsiderAsCompletion(Candidates& found, const HistoryEntry& entry, const UrlSpan& span, bool searched,
                          double worth, std::wstring_view typed) {
    const std::wstring_view url = entry.url;
    for (const std::size_t from : {span.bare, span.www, std::size_t{0}}) {
        if (typed.size() <= span.bare - from || !StartsWithIgnoreCase(url.substr(from), typed)) continue;

        if (span.web && from + typed.size() <= span.hostEnd) {
            // Still inside the site's name: the site is what's offered, and every page
            // visited on it speaks for it.
            const std::wstring_view site = url.substr(from, span.hostEnd - from);
            auto host = std::ranges::find_if(found.hosts,
                                             [site](const Suggestion& s) { return EqualsIgnoreCase(s.text, site); });
            if (host == found.hosts.end()) {
                found.hosts.push_back(
                    {.text = std::wstring(site), .url = std::wstring(url.substr(0, span.hostEnd)) + L"/"});
                host = std::prev(found.hosts.end());
            }
            host->score += worth;
            if (url.size() <= span.hostEnd + 1) { // its front page itself
                host->url = entry.url;
                host->title = entry.title;
            }
        } else if (!searched) {
            const double score = url.size() - from == typed.size() ? kExactAddressScore : worth;
            if (score > found.top.score) {
                found.top = {.text = std::wstring(url.substr(from)),
                             .title = entry.title,
                             .url = entry.url,
                             .search = false,
                             .score = score};
            }
        }
        return;
    }
}

// Do all the typed words turn up somewhere in the entry? `query` is the search it
// stands for, if it is a results page.
void ConsiderAsWordMatch(Candidates& found, const HistoryEntry& entry, const UrlSpan& span,
                         const std::optional<std::wstring>& query, double worth,
                         std::span<const std::wstring_view> terms) {
    // A results page that names no search, such as an image lookup, has nothing to offer.
    if (terms.empty() || (query && query->empty())) return;

    std::wstring shown = query ? *query : ShownUrl(entry.url, span);
    int fit = 0;
    for (const std::wstring_view term : terms) {
        int weight = MatchWeight(shown, term);
        if (!query) weight = std::max(weight, MatchWeight(entry.title, term));
        if (!weight) return;
        fit += weight;
    }
    const double score = worth * fit / static_cast<double>(terms.size());
    if (query) {
        std::wstring url = BuildSearchUrl(shown);
        found.rows.push_back({.text = std::move(shown), .url = std::move(url), .search = true, .score = score});
    } else {
        found.rows.push_back({.text = std::move(shown), .title = entry.title, .url = entry.url, .score = score});
    }
}

} // namespace

SuggestionList BuildSuggestions(std::span<const HistoryEntry> history, std::wstring_view typed, bool allowCompletion,
                                std::int64_t now) {
    SuggestionList list;
    const std::wstring_view text = Trim(typed);
    if (text.empty()) return list;
    if (text.size() != typed.size()) allowCompletion = false; // nothing can follow a typed space

    // The words that can match anywhere in an address or a title.
    const std::vector<std::wstring_view> terms = SplitWords(text.substr(SpanOf(text).bare));

    Candidates found;
    for (const HistoryEntry& entry : history) {
        const UrlSpan span = SpanOf(entry.url);
        const std::optional<std::wstring> query = SearchPageQuery(entry.url, span);
        const double worth = Frecency(entry, now);
        ConsiderAsCompletion(found, entry, span, query.has_value(), worth, text);
        ConsiderAsWordMatch(found, entry, span, query, worth, terms);
    }
    for (const Suggestion& host : found.hosts) {
        if (found.top.url.empty() || host.score > found.top.score) found.top = host;
    }
    // Past searches only fill what room the pages leave. Their visits say little about
    // how much they are wanted: a results page is gone back to after every result looked
    // at, while each result gets one visit.
    std::ranges::stable_sort(found.rows, std::ranges::greater{}, &Suggestion::score);
    std::ranges::stable_partition(found.rows, std::logical_not{}, &Suggestion::search);

    ResolvedInput resolved = ResolveAddressInput(text);
    Suggestion own{.text = std::wstring(text), .url = std::move(resolved.url), .search = resolved.isSearch};

    // An address is only filled in on some evidence that it is wanted: typed in once
    // before, or a couple of recent visits. One followed link doesn't change what Enter does.
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

    for (Suggestion& row : found.rows) {
        if (list.rows.size() >= kMaxSuggestions) break;
        const bool listed = std::ranges::any_of(list.rows, [&row](const Suggestion& s) {
            return EqualsIgnoreCase(s.url, row.url) || (s.search == row.search && EqualsIgnoreCase(s.text, row.text));
        });
        if (!listed) list.rows.push_back(std::move(row));
    }
    return list;
}

} // namespace exodium
