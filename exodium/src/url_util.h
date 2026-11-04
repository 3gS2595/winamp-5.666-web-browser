// What the plugin knows about addresses: its own, what typed text leads to, and the
// parts of a web address that are shown, matched and searched.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace exodium {

// The home page until the preferences name another. Where a new tab goes when it is
// given nowhere else to go is HomePage() (browser.h), which follows them.
inline constexpr wchar_t kDefaultUrl[] = L"https://google.com/";

// A closed tab's webview is put away on this page, which unloads the one it showed.
inline constexpr wchar_t kParkedUrl[] = L"about:blank#exodium-closed";

// The history page is made by the plugin and served to its own tabs at kHistoryUrl,
// an address that leads nowhere on the network. The address bar calls it kHistoryName.
inline constexpr wchar_t kHistoryUrl[] = L"https://exodium.invalid/history";
inline constexpr wchar_t kHistoryName[] = L"exodium://history";

// The page Claude Code's windows open on, tools\exodium_start.html, known by its name. It
// has a new address each time and there is nothing on it to go back to.
inline constexpr wchar_t kToolStartPage[] = L"exodium_start.html";

// The Claude Code session a window opened on kToolStartPage belongs to: the process id of
// its browser tools, which they give as "pid=" in the page's fragment, as in
// exodium_start.html#pid=1234&<uuid>. Nothing for any other address.
[[nodiscard]] std::optional<std::uint32_t> ToolSessionOf(std::wstring_view url);

// Whether `url` is the kind of address the history keeps: http, https or file.
[[nodiscard]] bool IsLikelyHttpUrl(std::wstring_view url) noexcept;
// Whether `text` reads as a bare domain: no spaces, and a dot followed by letters.
[[nodiscard]] bool LooksLikeDomain(std::wstring_view text) noexcept;

// `text` as a URL component: UTF-8, with everything but unreserved characters escaped.
[[nodiscard]] std::wstring PercentEncode(std::wstring_view text);
// The reverse. Escapes that aren't valid are left as they stand.
[[nodiscard]] std::wstring PercentDecode(std::wstring_view text);

// The results page for a search, from the search engine in use (search_engines.h).
[[nodiscard]] std::wstring BuildSearchUrl(std::wstring_view query);

struct ResolvedInput {
    std::wstring url;
    bool isSearch = false;
};
// Where text entered in the address bar leads: a URL as it stands, a bare domain
// over https, and anything else to a search for it.
[[nodiscard]] ResolvedInput ResolveAddressInput(std::wstring_view input);

// Where a link handed to the browser from outside leads: a web or file address as it
// stands, and the full path of a page on disk as a file address. Windows sends nothing
// else, so anything else is refused; a script address is not to be run on its say-so.
// No link stays no link.
[[nodiscard]] std::optional<std::wstring> ResolveOutsideLink(std::wstring_view link);

// Where the parts of an address begin. A web address is shown and matched from
// `bare` on: nobody types the scheme or the "www.", so neither should be needed.
struct UrlSpan {
    std::size_t www = 0;     // past the scheme of a web address; 0 for anything else
    std::size_t bare = 0;    // past a "www." as well
    std::size_t hostEnd = 0; // where a web address's host ends
    bool web = false;
};
[[nodiscard]] UrlSpan SpanOf(std::wstring_view url) noexcept;

// An address as the suggestion list shows it: from `bare` on, less the slash that
// ends a bare host.
[[nodiscard]] std::wstring ShownUrl(std::wstring_view url, const UrlSpan& span);

// The host of a web address in lower case, "www." included. Empty for anything else.
[[nodiscard]] std::wstring HostOf(std::wstring_view url);

// The words searched for if `url` is a results page of any search engine on offer, not
// only the one in use (see KnownResultsPages): empty when the page names no search, such
// as an image lookup, and nothing at all for any other address. Past searches are offered
// as searches, not as the long addresses they left behind.
[[nodiscard]] std::optional<std::wstring> SearchPageQuery(std::wstring_view url, const UrlSpan& span);

} // namespace exodium
