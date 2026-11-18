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

// ---- what typed text is: see ResolveAddressInput ----

constexpr std::wstring_view kSchemeEnd = L"://";
// Schemes that lead somewhere without a "//" after them. Script addresses are left out:
// typed or pasted into the address bar, they are searched for, not run.
constexpr std::wstring_view kBareSchemes[] = {L"about:", L"view-source:", L"data:", L"blob:", L"mailto:"};
// Names that are only ever a computer on the network at hand, which serve http as a rule.
constexpr std::wstring_view kLocalEndings[] = {L".localhost", L".local", L".test", L".internal", L".lan"};
// What a name ends in when it is a file's, and no site's: none of these is a top-level
// domain. (Some endings of files are, "zip" and "md" among them, and stay addresses.)
constexpr std::wstring_view kFileEndings[] = {
    L"js",  L"jsx", L"ts",   L"tsx", L"json", L"txt", L"html", L"htm",  L"css",  L"xml",  L"yml", L"yaml", L"toml",
    L"ini", L"log", L"csv",  L"cpp", L"hpp",  L"exe", L"dll",  L"lib",  L"bat",  L"php",  L"asp", L"aspx", L"jsp",
    L"png", L"jpg", L"jpeg", L"gif", L"svg",  L"pdf", L"doc",  L"docx", L"xls",  L"xlsx", L"ppt", L"pptx", L"wav",
    L"avi", L"mkv", L"iso",  L"rar", L"tar",  L"bak", L"tmp",  L"wasm", L"glsl", L"gltf", L"glb", L"fbx",  L"sln",
};
constexpr std::size_t kMaxLabelLength = 63;
constexpr std::size_t kMaxTopLevelLength = 24;

constexpr bool IsAsciiLetter(wchar_t c) noexcept {
    return (c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z');
}
constexpr bool IsAsciiDigit(wchar_t c) noexcept {
    return c >= L'0' && c <= L'9';
}

// Whether `text` begins with a scheme and "://", as "https://x" and "edge://gpu" do.
[[nodiscard]] bool HasSchemeAndSlashes(std::wstring_view text) noexcept {
    const std::size_t end = text.find(kSchemeEnd);
    if (end == std::wstring_view::npos || end == 0 || !IsAsciiLetter(text[0])) return false;
    return std::ranges::all_of(text.substr(0, end), [](wchar_t c) {
        return IsAsciiLetter(c) || IsAsciiDigit(c) || c == L'+' || c == L'-' || c == L'.';
    });
}

// The full path of a file or folder on this computer or another: "C:\a", "C:/a", "\\pc\share".
[[nodiscard]] bool IsLocalPath(std::wstring_view text) noexcept {
    if (text.starts_with(L"\\\\")) return text.size() > 2;
    return text.size() >= 3 && IsAsciiLetter(text[0]) && text[1] == L':' && (text[2] == L'\\' || text[2] == L'/');
}

[[nodiscard]] bool IsPort(std::wstring_view text) noexcept {
    if (text.empty() || text.size() > 5 || !std::ranges::all_of(text, IsAsciiDigit)) return false;
    return ParseNumber<int>(text).value_or(65536) <= 65535;
}

// Four numbers up to 255 with dots between them.
[[nodiscard]] bool IsIpv4(std::wstring_view host) noexcept {
    int parts = 0;
    for (std::size_t begin = 0;; ++parts) {
        const std::size_t end = std::min(host.find(L'.', begin), host.size());
        const std::wstring_view part = host.substr(begin, end - begin);
        if (part.empty() || part.size() > 3 || !std::ranges::all_of(part, IsAsciiDigit)) return false;
        if (ParseNumber<int>(part).value_or(256) > 255) return false;
        if (end == host.size()) return parts == 3;
        begin = end + 1;
    }
}

// "[::1]" and the like: hexadecimal digits, colons, and the dots of an IPv4 tail.
[[nodiscard]] bool IsIpv6Literal(std::wstring_view host) noexcept {
    if (host.size() < 4 || host.front() != L'[' || host.back() != L']') return false;
    const std::wstring_view inner = host.substr(1, host.size() - 2);
    return inner.contains(L':') &&
           std::ranges::all_of(inner, [](wchar_t c) { return HexDigitValue(c) >= 0 || c == L':' || c == L'.'; });
}

// One part of a name, between its dots: letters and digits of any alphabet, hyphens, and
// the underscores some networks' names have.
[[nodiscard]] bool IsHostLabel(std::wstring_view label) noexcept {
    return !label.empty() && label.size() <= kMaxLabelLength &&
           std::ranges::all_of(label, [](wchar_t c) { return std::iswalnum(c) != 0 || c == L'-' || c == L'_'; });
}

enum class HostKind {
    kNone,  // not a host's name at all
    kLocal, // this computer or one beside it, which http is asked of
    kWeb,   // a site, which https is asked of
};

[[nodiscard]] HostKind KindOfHost(std::wstring_view host, bool hasPort) noexcept {
    if (host.empty()) return HostKind::kNone;
    if (host.front() == L'[') return IsIpv6Literal(host) ? HostKind::kLocal : HostKind::kNone;
    if (EqualsIgnoreCase(host, L"localhost") || IsIpv4(host)) return HostKind::kLocal;
    // A name written out in full may end in the root's dot.
    if (host.back() == L'.') host.remove_suffix(1);

    std::size_t labels = 0;
    std::wstring_view last;
    for (std::size_t begin = 0;; ++labels) {
        const std::size_t end = std::min(host.find(L'.', begin), host.size());
        last = host.substr(begin, end - begin);
        if (!IsHostLabel(last)) return HostKind::kNone;
        if (end == host.size()) break;
        begin = end + 1;
    }
    // A name on its own is a word, unless a port says it is a computer's.
    if (labels == 0) return hasPort && std::iswalpha(last.front()) != 0 ? HostKind::kLocal : HostKind::kNone;

    // What it ends in has to be able to be a top-level domain: letters, or one written out
    // in ASCII ("xn--..."), and not the ending of a file.
    const bool encoded = StartsWithIgnoreCase(last, L"xn--");
    if (!encoded) {
        if (last.size() < 2 || last.size() > kMaxTopLevelLength) return HostKind::kNone;
        if (!std::ranges::all_of(last, [](wchar_t c) { return std::iswalpha(c) != 0; })) return HostKind::kNone;
        const auto isIt = [last](std::wstring_view ending) { return EqualsIgnoreCase(last, ending); };
        if (std::ranges::any_of(kFileEndings, isIt)) return HostKind::kNone;
    }
    for (const std::wstring_view ending : kLocalEndings) {
        if (host.size() > ending.size() && EqualsIgnoreCase(host.substr(host.size() - ending.size()), ending)) {
            return HostKind::kLocal;
        }
    }
    return HostKind::kWeb;
}

// `text` as an address, if it is one with its scheme left out: a host, a port if it has
// one, and whatever follows them.
[[nodiscard]] std::optional<std::wstring> AsBareAddress(std::wstring_view text) {
    const std::wstring_view authority = text.substr(0, std::min(text.find_first_of(L"/?#"), text.size()));
    // A space in it, and it is words; an '@', and it is somebody's mail address.
    if (authority.empty() || authority.find_first_of(L" \t\r\n@") != std::wstring_view::npos) return std::nullopt;

    std::wstring_view host = authority;
    std::wstring_view port;
    bool hasPort = false;
    if (host.front() == L'[') {
        const std::size_t close = host.find(L']');
        if (close == std::wstring_view::npos) return std::nullopt;
        const std::wstring_view rest = host.substr(close + 1);
        if (!rest.empty() && rest.front() != L':') return std::nullopt;
        hasPort = !rest.empty();
        if (hasPort) port = rest.substr(1);
        host = host.substr(0, close + 1);
    } else if (const std::size_t colon = host.rfind(L':'); colon != std::wstring_view::npos) {
        hasPort = true;
        port = host.substr(colon + 1);
        host = host.substr(0, colon);
    }
    if (hasPort && !IsPort(port)) return std::nullopt;

    switch (KindOfHost(host, hasPort)) {
    case HostKind::kLocal:
        return std::wstring(kHttp).append(text);
    case HostKind::kWeb:
        return std::wstring(kHttps).append(text);
    case HostKind::kNone:
        break;
    }
    return std::nullopt;
}

} // namespace

std::wstring_view PluginPageName(std::wstring_view url) noexcept {
    if (url == kHistoryUrl) return kHistoryName;
    if (url == kBookmarksUrl) return kBookmarksName;
    return {};
}

bool IsLikelyHttpUrl(std::wstring_view url) noexcept {
    return StartsWithIgnoreCase(url, kHttp) || StartsWithIgnoreCase(url, kHttps) || StartsWithIgnoreCase(url, kFile);
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
        const int high = text[i] == L'%' ? HexDigitValue(text[i + 1]) : -1;
        const int low = high < 0 ? -1 : HexDigitValue(text[i + 2]);
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
    if (EqualsIgnoreCase(input, kBookmarksName)) return {kBookmarksUrl, false};
    // Before anything is taken for a scheme: a drive's letter and colon read as one.
    if (IsLocalPath(input)) {
        if (std::optional<std::wstring> file = ResolveOutsideLink(input)) return {std::move(*file), false};
    }
    const auto begins = [input](std::wstring_view scheme) { return StartsWithIgnoreCase(input, scheme); };
    if (HasSchemeAndSlashes(input) || std::ranges::any_of(kBareSchemes, begins)) return {std::wstring(input), false};
    if (std::optional<std::wstring> address = AsBareAddress(input)) return {std::move(*address), false};
    return {BuildSearchUrl(input), true};
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

std::wstring_view ShownPart(std::wstring_view url, const UrlSpan& span) noexcept {
    std::wstring_view shown = url.substr(span.bare);
    if (span.web && url.size() == span.hostEnd + 1 && url.back() == L'/') shown.remove_suffix(1);
    return shown;
}

std::wstring ShownUrl(std::wstring_view url, const UrlSpan& span) {
    return std::wstring(ShownPart(url, span));
}

std::wstring HostOf(std::wstring_view url) {
    const UrlSpan span = SpanOf(url);
    return span.web ? ToLower(url.substr(span.www, span.hostEnd - span.www)) : std::wstring();
}

std::optional<std::wstring> SearchPageQuery(std::wstring_view url, const UrlSpan& span) {
    // Every results page is known by its address up to the '?' its parameters start with.
    if (!span.web || url.find(L'?', span.hostEnd) == std::wstring_view::npos) return std::nullopt;
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
