// The search engines the address bar can search with, and the one it searches with now.
//
// An engine is known by the address of its results page, with %s where the words go,
// as Chrome and Firefox write them. The preferences pick one of kSearchEngines or give
// an address of their own; UseSearchEngine makes that the one in use. Results pages
// are recognized for every engine offered, whichever is in use, so that a search made
// before a change of engine is still a search in the history.
#pragma once

#include <array>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace exodium {

struct SearchEngine {
    std::wstring_view id;    // its name in winamp.ini
    std::wstring_view name;  // its name in the preferences
    std::wstring_view label; // after a search in the address bar's suggestions: "words - Google Search"
    std::wstring_view url;   // its results page, %s standing for the words
};

// The engines on offer. The first is the one searched with until the preferences say otherwise.
inline constexpr std::array kSearchEngines{
    SearchEngine{L"google", L"Google", L"Google Search", L"https://www.google.com/search?q=%s"},
    SearchEngine{L"bing", L"Bing", L"Bing Search", L"https://www.bing.com/search?q=%s"},
    SearchEngine{L"duckduckgo", L"DuckDuckGo", L"DuckDuckGo Search", L"https://duckduckgo.com/?q=%s"},
    SearchEngine{L"brave", L"Brave Search", L"Brave Search", L"https://search.brave.com/search?q=%s"},
    SearchEngine{L"ecosia", L"Ecosia", L"Ecosia Search", L"https://www.ecosia.org/search?q=%s"},
    SearchEngine{L"startpage", L"Startpage", L"Startpage Search", L"https://www.startpage.com/sp/search?query=%s"},
    SearchEngine{L"yahoo", L"Yahoo", L"Yahoo Search", L"https://search.yahoo.com/search?p=%s"},
};
// The engine whose address the user gives.
inline constexpr std::wstring_view kCustomSearchId = L"custom";

// The engine on offer with this id, in any case. Nothing for kCustomSearchId or an unknown one.
[[nodiscard]] const SearchEngine* FindSearchEngine(std::wstring_view id) noexcept;

// A custom engine's address as typed, made one a page can be loaded from: trimmed, and
// over https if it names no scheme. Empty stays empty.
[[nodiscard]] std::wstring NormalizeSearchUrl(std::wstring_view typed);

// The results page `url` gives for `encodedWords`, which are already percent-encoded:
// every %s in it is the words, and an address with no %s has them added at the end.
[[nodiscard]] std::wstring FillSearchUrl(std::wstring_view url, std::wstring_view encodedWords);

// What tells a results page from other pages: its address from past the scheme and a
// "www." up to the '?' that starts its parameters, and the parameter holding the words.
struct ResultsPage {
    std::wstring prefix; // as in "google.com/search?"
    std::wstring key;    // as in "q"
};
// The results page an engine's address leads to. Nothing for one that doesn't give the
// words as a parameter of their own, such as "https://example.com/search/%s".
[[nodiscard]] std::optional<ResultsPage> ResultsPageOf(std::wstring_view url);

// What a search made with a custom engine says it is, from the host it goes to:
// "example.com Search".
[[nodiscard]] std::wstring CustomSearchLabel(std::wstring_view url);

// Makes an engine the one searched with: one on offer by its id, or for kCustomSearchId
// the address given, normalized. Anything else, and a custom engine with no address,
// is the first engine on offer.
void UseSearchEngine(std::wstring_view id, std::wstring_view customUrl = {});
// The engine in use: its results page, with %s for the words, and its label. A custom
// engine whose results pages are those of one on offer has that one's label.
[[nodiscard]] const std::wstring& SearchUrlTemplate();
[[nodiscard]] const std::wstring& SearchLabel();
// The results pages of every engine on offer, and of the one in use if that is custom.
[[nodiscard]] std::span<const ResultsPage> KnownResultsPages();

} // namespace exodium
