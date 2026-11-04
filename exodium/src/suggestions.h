// What the address bar offers for the text typed into it.
//
// The address bar works the way Chrome's does. The top row of the list is always what
// Enter will do. When an address in the history begins with what has been typed, it
// takes that row and the rest of it is filled in after the caret, selected, so that
// typing on replaces it. Otherwise the top row is the typed text itself, as an
// address or a search. Below it come the pages the typed words turn up in, best
// first, and after them any past searches they turn up in. Up and Down try the
// other rows out in the box.
#pragma once

#include "history.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace exodium {

inline constexpr std::size_t kMaxSuggestions = 8;

// One row of the list under the address bar.
struct Suggestion {
    std::wstring text;   // what the address bar shows for it
    std::wstring title;  // the page's title, when one is known
    std::wstring url;    // where it goes
    bool search = false; // a search for text with the engine in use, not a page
    double score = 0;
};

struct SuggestionList {
    std::vector<Suggestion> rows;  // empty when nothing was typed
    std::wstring inlineCompletion; // what to show selected after the typed text, if anything
};

// The list for `typed`. `allowCompletion` lets the best address be filled in after
// the caret; a deletion or a paste leaves the text as it stands.
[[nodiscard]] SuggestionList BuildSuggestions(std::span<const HistoryEntry> history, std::wstring_view typed,
                                              bool allowCompletion, std::int64_t now);

} // namespace exodium
