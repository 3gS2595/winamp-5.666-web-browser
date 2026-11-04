#include "url_util.h"

#include "search_engines.h"
#include "text_util.h"

#include <windows.h>

#include <shlwapi.h>

#include <algorithm>
#include <cwctype>
#include <iterator>

namespace exodium {

namespace {

constexpr std::wstring_view kHttps = L"https://";
constexpr std::wstring_view kHttp = L"http://";
constexpr std::wstring_view kFile = L"file://";
constexpr std::wstring_view kWww = L"www.";

constexpr bool IsUnreserved(char c) noexcept {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.' ||
           c == '_' || c == '~';
}

// The value of a hexadecimal digit, or -1.
constexpr int HexValue(wchar_t c) noexcept {
    if (c >= L'0' && c <= L'9') return c - L'0';
    if (c >= L'a' && c <= L'f') return c - L'a' + 10;
    if (c >= L'A' && c <= L'F') return c - L'A' + 10;
    return -1;
}

} // namespace

bool IsLikelyHttpUrl(std::wstring_view url) noexcept {
    return StartsWithIgnoreCase(url, kHttp) || StartsWithIgnoreCase(url, kHttps) || StartsWithIgnoreCase(url, kFile);
}

bool LooksLikeDomain(std::wstring_view text) noexcept {
    if (text.empty() || text.contains(L' ')) return false;
    const auto lastDot = text.rfind(L'.');
    if (lastDot == std::wstring_view::npos) return false;
    const std::wstring_view tld = text.substr(lastDot + 1);
    if (tld.size() < 2 || tld.size() > 24) return false;
    return std::ranges::all_of(tld, [](wchar_t c) { return std::iswalpha(c) != 0; });
}

std::wstring PercentEncode(std::wstring_view text) {
    std::wstring out;
    out.reserve(text.size());
    for (const char byte : ToUtf8(text)) {
        if (IsUnreserved(byte)) out += static_cast<wchar_t>(byte);
        else out.append(1, L'%').append(ToHex(static_cast<unsigned char>(byte), 2, true));
    }
    return out;
}

std::wstring PercentDecode(std::wstring_view text) {
    std::string bytes;
    bytes.reserve(text.size());
    std::size_t literal = 0; // where the text not yet copied over begins
    for (std::size_t i = 0; i + 2 < text.size();) {
        const int high = text[i] == L'%' ? HexValue(text[i + 1]) : -1;
        const int low = high < 0 ? -1 : HexValue(text[i + 2]);
        if (low < 0) {
            ++i;
            continue;
        }
        bytes += ToUtf8(text.substr(literal, i - literal));
        bytes += static_cast<char>(high * 16 + low);
        i += 3;
        literal = i;
    }
    bytes += ToUtf8(text.substr(literal));
    return FromUtf8(bytes);
}

std::wstring BuildSearchUrl(std::wstring_view query) {
    return FillSearchUrl(SearchUrlTemplate(), PercentEncode(query));
}

ResolvedInput ResolveAddressInput(std::wstring_view input) {
    if (EqualsIgnoreCase(input, kHistoryName)) return {kHistoryUrl, false};
    std::wstring text(input);
    if (::PathIsURLW(text.c_str())) return {std::move(text), false};
    if (LooksLikeDomain(text)) return {std::wstring(kHttps) + text, false};
    return {BuildSearchUrl(text), true};
}

std::optional<std::wstring> ResolveOutsideLink(std::wstring_view link) {
    if (link.empty() || IsLikelyHttpUrl(link)) return std::wstring(link);

    const std::wstring path(link);
    if (::PathIsRelativeW(path.c_str())) return std::nullopt; // a script address reads as a relative path
    // No more than every character escaped, and the scheme.
    DWORD size = static_cast<DWORD>(path.size() * 3 + kFile.size() + 2);
    std::wstring url(size, L'\0');
    if (FAILED(::UrlCreateFromPathW(path.c_str(), url.data(), &size, 0))) return std::nullopt;
    url.resize(size);
    return url;
}

std::optional<std::uint32_t> ToolSessionOf(std::wstring_view url) {
    const std::size_t page = url.find(kToolStartPage);
    if (page == std::wstring_view::npos) return std::nullopt;
    const std::size_t hash = url.find(L'#', page);
    if (hash == std::wstring_view::npos) return std::nullopt;
    for (const std::wstring_view part : SplitWords(url.substr(hash + 1), L"&")) {
        if (!part.starts_with(L"pid=")) continue;
        const std::wstring_view digits = part.substr(4);
        if (digits.empty() || !std::ranges::all_of(digits, [](wchar_t c) { return c >= L'0' && c <= L'9'; })) continue;
        if (const auto pid = ParseNumber<std::uint32_t>(digits); pid && *pid != 0) return pid;
    }
    return std::nullopt;
}

UrlSpan SpanOf(std::wstring_view url) noexcept {
    UrlSpan span;
    if (StartsWithIgnoreCase(url, kHttps)) span.www = kHttps.size();
    else if (StartsWithIgnoreCase(url, kHttp)) span.www = kHttp.size();
    span.web = span.www != 0;
    span.bare = span.www + (StartsWithIgnoreCase(url.substr(span.www), kWww) ? kWww.size() : 0);
    span.hostEnd = span.web ? std::min(url.find_first_of(L"/?#", span.bare), url.size()) : 0;
    return span;
}

std::wstring ShownUrl(std::wstring_view url, const UrlSpan& span) {
    std::wstring shown(url.substr(span.bare));
    if (span.web && url.size() == span.hostEnd + 1 && url.back() == L'/') shown.pop_back();
    return shown;
}

std::wstring HostOf(std::wstring_view url) {
    const UrlSpan span = SpanOf(url);
    return span.web ? ToLower(url.substr(span.www, span.hostEnd - span.www)) : std::wstring();
}

std::optional<std::wstring> SearchPageQuery(std::wstring_view url, const UrlSpan& span) {
    if (!span.web) return std::nullopt;
    const std::wstring_view bare = url.substr(span.bare);
    const std::span<const ResultsPage> pages = KnownResultsPages();
    const auto page =
        std::ranges::find_if(pages, [bare](const ResultsPage& p) { return StartsWithIgnoreCase(bare, p.prefix); });
    if (page == pages.end()) return std::nullopt;

    // Each turn of the loop `at` is on the '?' or '&' that starts a parameter.
    const std::wstring key = page->key + L'=';
    std::size_t at = span.bare + page->prefix.size() - 1;
    while (!url.substr(at + 1).starts_with(key)) {
        at = url.find(L'&', at + 1);
        if (at == std::wstring_view::npos) return std::wstring();
    }
    const std::size_t begin = at + 1 + key.size();
    const std::size_t end = std::min(url.find_first_of(L"&#", begin), url.size());

    std::wstring words(url.substr(begin, end - begin));
    std::ranges::replace(words, L'+', L' ');
    words = PercentDecode(words);
    if (const auto nul = words.find(L'\0'); nul != std::wstring::npos) words.resize(nul);
    return std::wstring(Trim(words));
}

} // namespace exodium
