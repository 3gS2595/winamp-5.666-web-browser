#include "url_util.h"

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
constexpr std::wstring_view kSearchUrl = L"https://www.google.com/search?q=";
constexpr std::wstring_view kSearchPage = L"google.com/search?";

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
    return std::wstring(kSearchUrl) + PercentEncode(query);
}

ResolvedInput ResolveAddressInput(std::wstring_view input) {
    if (EqualsIgnoreCase(input, kHistoryName)) return {kHistoryUrl, false};
    std::wstring text(input);
    if (::PathIsURLW(text.c_str())) return {std::move(text), false};
    if (LooksLikeDomain(text)) return {std::wstring(kHttps) + text, false};
    return {BuildSearchUrl(text), true};
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
    if (!span.web || !StartsWithIgnoreCase(url.substr(span.bare), kSearchPage)) return std::nullopt;

    // Each turn of the loop `at` is on the '?' or '&' that starts a parameter.
    std::size_t at = span.bare + kSearchPage.size() - 1;
    while (!url.substr(at + 1).starts_with(L"q=")) {
        at = url.find(L'&', at + 1);
        if (at == std::wstring_view::npos) return std::wstring();
    }
    const std::size_t begin = at + 3;
    const std::size_t end = std::min(url.find_first_of(L"&#", begin), url.size());

    std::wstring words(url.substr(begin, end - begin));
    std::ranges::replace(words, L'+', L' ');
    words = PercentDecode(words);
    if (const auto nul = words.find(L'\0'); nul != std::wstring::npos) words.resize(nul);
    return std::wstring(Trim(words));
}

} // namespace exodium
