// What the address bar offers for the text typed into it.
//
// The address bar works the way Chrome's does. The top row of the list is always what
// Enter will do. When an address in the history begins with what has been typed, it
// takes that row and the rest of it is filled in after the caret, selected, so that
// typing on replaces it. Otherwise the top row is the typed text itself, as an
// address or a search. Below it come the pages the typed words turn up in, best
// first, and after them any past searches they turn up in. Up and Down try the
// other rows out in the box.
//
// Bookmarks are offered along with the history, whether they have been visited or not.
// A bookmark counts for as much as an address typed in once and lately, which is enough
// for it to be filled in.
//
// A row the history put in the list can be forgotten, by the × at its end: the pages
// behind it are taken out of the history, and it is no longer suggested until one of them
// is visited again. A row that is there for the typed text alone, or for a bookmark alone,
// has nothing to forget.
#pragma once

#include "bookmarks.h"
#include "history.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace exodium {

inline constexpr std::size_t kMaxSuggestions = 8;

// What of the history is behind a row of the list, and goes when the row is forgotten.
enum class HistoryBehind : unsigned char {
    kNothing, // the typed text itself, or a bookmark, which is suggested for as long as it is one
    kPage,    // the page the row goes to
    kSite,    // every page visited on the site the row names, each of which speaks for it
    kSearch,  // every results page of the search the row stands for
};

// One row of the list under the address bar.
struct Suggestion {
    std::wstring text;       // what the address bar shows for it
    std::wstring title;      // the page's title, when one is known
    std::wstring url;        // where it goes
    bool search = false;     // a search for text with the engine in use, not a page
    bool bookmarked = false; // the page is a bookmark, which the list marks with a star
    HistoryBehind behind = HistoryBehind::kNothing;
    double score = 0;
};

struct SuggestionList {
    std::vector<Suggestion> rows;  // empty when nothing was typed
    std::wstring inlineCompletion; // what to show selected after the typed text, if anything
};

// The list for `typed`. `allowCompletion` lets the best address be filled in after
// the caret; a deletion or a paste leaves the text as it stands.
[[nodiscard]] SuggestionList BuildSuggestions(std::span<const HistoryEntry> history, std::wstring_view typed,
                                              bool allowCompletion, std::int64_t now,
                                              std::span<const Bookmark> bookmarks = {});

// The addresses of the pages in the history that are behind a row, as Suggestion::behind
// says: what forgetting the row takes out of the history.
[[nodiscard]] std::vector<std::wstring> PagesBehind(std::span<const HistoryEntry> history, const Suggestion& row);

} // namespace exodium
