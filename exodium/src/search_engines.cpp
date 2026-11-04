#include "search_engines.h"

#include "text_util.h"

#include <algorithm>
#include <cwctype>
#include <vector>

namespace exodium {

namespace {

constexpr std::wstring_view kWords = L"%s";
constexpr std::wstring_view kSchemeEnd = L"://";
constexpr std::wstring_view kWww = L"www.";

// Whether `text` begins with a scheme and "://", as in "https://".
[[nodiscard]] bool HasScheme(std::wstring_view text) noexcept {
    const std::size_t end = text.find(kSchemeEnd);
    if (end == std::wstring_view::npos || end == 0 || std::iswalpha(text[0]) == 0) return false;
    return std::ranges::all_of(text.substr(0, end),
                               [](wchar_t c) { return std::iswalnum(c) != 0 || c == L'+' || c == L'-' || c == L'.'; });
}

// Where an address goes on past its scheme and a "www.".
[[nodiscard]] std::size_t BareStart(std::wstring_view url) noexcept {
    std::size_t at = HasScheme(url) ? url.find(kSchemeEnd) + kSchemeEnd.size() : 0;
    if (StartsWithIgnoreCase(url.substr(at), kWww)) at += kWww.size();
    return at;
}

struct ActiveSearch {
    std::wstring url;
    std::wstring label;
    std::vector<ResultsPage> resultsPages;
};

[[nodiscard]] bool SamePage(const ResultsPage& a, const ResultsPage& b) noexcept {
    return EqualsIgnoreCase(a.prefix, b.prefix) && a.key == b.key;
}

[[nodiscard]] ActiveSearch MakeActiveSearch(std::wstring_view id, std::wstring_view customUrl) {
    ActiveSearch active;
    for (const SearchEngine& engine : kSearchEngines) {
        if (auto page = ResultsPageOf(engine.url)) active.resultsPages.push_back(std::move(*page));
    }

    std::wstring custom = EqualsIgnoreCase(id, kCustomSearchId) ? NormalizeSearchUrl(customUrl) : std::wstring();
    if (custom.empty()) {
        const SearchEngine* engine = FindSearchEngine(id);
        if (!engine) engine = &kSearchEngines.front();
        active.url = engine->url;
        active.label = engine->label;
        return active;
    }

    active.label = CustomSearchLabel(custom);
    // A custom engine is often one on offer with a parameter added, such as Google's
    // results without the AI overview. Its searches are still that engine's, and its
    // results pages are known already.
    if (std::optional<ResultsPage> page = ResultsPageOf(custom)) {
        for (const SearchEngine& engine : kSearchEngines) {
            if (const auto known = ResultsPageOf(engine.url); known && SamePage(*known, *page)) {
                active.label = engine.label;
                break;
            }
        }
        const auto same = [&page](const ResultsPage& known) { return SamePage(known, *page); };
        if (std::ranges::none_of(active.resultsPages, same)) active.resultsPages.push_back(std::move(*page));
    }
    active.url = std::move(custom);
    return active;
}

// Made on first use, so that it is there whenever it is asked for, tests included.
[[nodiscard]] ActiveSearch& Active() {
    static ActiveSearch active = MakeActiveSearch(kSearchEngines.front().id, {});
    return active;
}

} // namespace

const SearchEngine* FindSearchEngine(std::wstring_view id) noexcept {
    const auto found =
        std::ranges::find_if(kSearchEngines, [id](const SearchEngine& e) { return EqualsIgnoreCase(e.id, id); });
    return found == kSearchEngines.end() ? nullptr : &*found;
}

std::wstring NormalizeSearchUrl(std::wstring_view typed) {
    const std::wstring_view text = Trim(typed);
    if (text.empty() || HasScheme(text)) return std::wstring(text);
    return L"https://" + std::wstring(text);
}

std::wstring FillSearchUrl(std::wstring_view url, std::wstring_view encodedWords) {
    std::wstring filled;
    std::size_t from = 0; // where the address not yet copied begins
    for (std::size_t at = url.find(kWords); at != std::wstring_view::npos; at = url.find(kWords, from)) {
        filled.append(url.substr(from, at - from)).append(encodedWords);
        from = at + kWords.size();
    }
    if (from == 0) return std::wstring(url) + std::wstring(encodedWords);
    return filled.append(url.substr(from));
}

std::optional<ResultsPage> ResultsPageOf(std::wstring_view url) {
    // The words are the value of a parameter: "?key=%s" or "&key=%s".
    const std::size_t words = url.find(kWords);
    if (words == std::wstring_view::npos || words == 0 || url[words - 1] != L'=') return std::nullopt;
    const std::size_t equals = words - 1;
    const std::size_t query = url.find(L'?');
    const std::size_t separator = url.find_last_of(L"?&", equals);
    if (query == std::wstring_view::npos || separator == std::wstring_view::npos || separator < query ||
        separator + 1 == equals) {
        return std::nullopt;
    }
    const std::size_t bare = BareStart(url);
    if (bare > query) return std::nullopt;
    return ResultsPage{.prefix = std::wstring(url.substr(bare, query + 1 - bare)),
                       .key = std::wstring(url.substr(separator + 1, equals - separator - 1))};
}

std::wstring CustomSearchLabel(std::wstring_view url) {
    const std::size_t bare = BareStart(url);
    const std::size_t end = std::min(url.find_first_of(L"/?#:", bare), url.size());
    const std::wstring host = ToLower(url.substr(bare, end - bare));
    return host.empty() ? L"Search" : host + L" Search";
}

void UseSearchEngine(std::wstring_view id, std::wstring_view customUrl) {
    Active() = MakeActiveSearch(id, customUrl);
}

const std::wstring& SearchUrlTemplate() {
    return Active().url;
}

const std::wstring& SearchLabel() {
    return Active().label;
}

std::span<const ResultsPage> KnownResultsPages() {
    return Active().resultsPages;
}

} // namespace exodium
