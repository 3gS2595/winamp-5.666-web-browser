#include "session_history.h"

#include "text_util.h"
#include "url_util.h"

#include <utility>

namespace exodium {

namespace {

// The origin of kHistoryUrl, and of any other page the plugin makes itself.
constexpr std::wstring_view kPluginOrigin = L"https://exodium.invalid/";

// Deeper than any answer of the protocol's; past it a value isn't skipped but refused.
constexpr int kMaxJsonDepth = 64;

[[nodiscard]] constexpr bool IsDigit(wchar_t c) noexcept {
    return c >= L'0' && c <= L'9';
}

// The value of a hexadecimal digit, or -1.
[[nodiscard]] constexpr int HexDigit(wchar_t c) noexcept {
    if (IsDigit(c)) return c - L'0';
    if (c >= L'a' && c <= L'f') return c - L'a' + 10;
    if (c >= L'A' && c <= L'F') return c - L'A' + 10;
    return -1;
}

// Just enough of a JSON reader for the protocol's answers. Values are read in the order
// they come, and the ones that aren't wanted are skipped.
class JsonReader {
public:
    explicit JsonReader(std::wstring_view text) noexcept : text_(text) {}

    [[nodiscard]] bool AtEnd() noexcept {
        SkipSpace();
        return at_ == text_.size();
    }

    // An object: `member` is called with each key, reads its value and says whether it could.
    template <typename Member>
    [[nodiscard]] bool Object(Member&& member) {
        if (!Take(L'{')) return false;
        if (Take(L'}')) return true;
        do {
            const std::optional<std::wstring> key = String();
            if (!key || !Take(L':') || !member(*key)) return false;
        } while (Take(L','));
        return Take(L'}');
    }

    // An array: `element` reads each value and says whether it could.
    template <typename Element>
    [[nodiscard]] bool Array(Element&& element) {
        if (!Take(L'[')) return false;
        if (Take(L']')) return true;
        do {
            if (!element()) return false;
        } while (Take(L','));
        return Take(L']');
    }

    [[nodiscard]] std::optional<std::wstring> String() {
        if (!Take(L'"')) return std::nullopt;
        std::wstring text;
        while (at_ < text_.size()) {
            const wchar_t c = text_[at_++];
            if (c == L'"') return text;
            if (c != L'\\') {
                text += c;
                continue;
            }
            if (at_ == text_.size()) break;
            switch (const wchar_t escaped = text_[at_++]) {
            case L'"':
            case L'\\':
            case L'/':
                text += escaped;
                break;
            case L'b':
                text += L'\b';
                break;
            case L'f':
                text += L'\f';
                break;
            case L'n':
                text += L'\n';
                break;
            case L'r':
                text += L'\r';
                break;
            case L't':
                text += L'\t';
                break;
            case L'u': {
                // One UTF-16 unit, which is what a wstring holds: a character outside the
                // basic plane comes as two escapes, one for each half of its pair.
                unsigned unit = 0;
                for (int i = 0; i < 4; ++i) {
                    const int digit = at_ < text_.size() ? HexDigit(text_[at_++]) : -1;
                    if (digit < 0) return std::nullopt;
                    unit = unit * 16 + static_cast<unsigned>(digit);
                }
                text += static_cast<wchar_t>(unit);
                break;
            }
            default:
                return std::nullopt;
            }
        }
        return std::nullopt; // never closed
    }

    [[nodiscard]] std::optional<int> Integer() {
        SkipSpace();
        const std::size_t begin = at_;
        if (at_ < text_.size() && text_[at_] == L'-') ++at_;
        while (at_ < text_.size() && IsDigit(text_[at_])) ++at_;
        // A fraction or an exponent makes it no integer.
        if (at_ < text_.size() && (text_[at_] == L'.' || text_[at_] == L'e' || text_[at_] == L'E')) return std::nullopt;
        return ParseNumber<int>(text_.substr(begin, at_ - begin));
    }

    [[nodiscard]] bool Skip(int depth = 0) {
        SkipSpace();
        if (at_ == text_.size() || depth > kMaxJsonDepth) return false;
        switch (text_[at_]) {
        case L'{':
            return Object([&](const std::wstring&) { return Skip(depth + 1); });
        case L'[':
            return Array([&] { return Skip(depth + 1); });
        case L'"':
            return String().has_value();
        case L't':
            return TakeWord(L"true");
        case L'f':
            return TakeWord(L"false");
        case L'n':
            return TakeWord(L"null");
        default: {
            const std::size_t begin = at_;
            while (at_ < text_.size() && (IsDigit(text_[at_]) || std::wstring_view(L"+-.eE").contains(text_[at_]))) {
                ++at_;
            }
            return at_ > begin;
        }
        }
    }

private:
    void SkipSpace() noexcept {
        while (at_ < text_.size() && std::wstring_view(L" \t\r\n").contains(text_[at_])) ++at_;
    }

    [[nodiscard]] bool Take(wchar_t c) noexcept {
        SkipSpace();
        if (at_ == text_.size() || text_[at_] != c) return false;
        ++at_;
        return true;
    }

    [[nodiscard]] bool TakeWord(std::wstring_view word) noexcept {
        if (!text_.substr(at_).starts_with(word)) return false;
        at_ += word.size();
        return true;
    }

    std::wstring_view text_;
    std::size_t at_ = 0;
};

[[nodiscard]] std::wstring EscapeHtml(std::wstring_view text) {
    std::wstring escaped;
    escaped.reserve(text.size());
    for (const wchar_t c : text) {
        switch (c) {
        case L'&':
            escaped += L"&amp;";
            break;
        case L'<':
            escaped += L"&lt;";
            break;
        case L'>':
            escaped += L"&gt;";
            break;
        case L'"':
            escaped += L"&quot;";
            break;
        default:
            escaped += c;
            break;
        }
    }
    return escaped;
}

} // namespace

std::optional<SessionHistory> ParseSessionHistory(std::wstring_view json) {
    JsonReader reader(json);
    SessionHistory history;
    std::optional<int> current;
    bool hasEntries = false;

    const auto readEntry = [&] {
        SessionEntry entry;
        const bool read = reader.Object([&](const std::wstring& key) {
            if (key == L"id") {
                const std::optional<int> id = reader.Integer();
                entry.id = id.value_or(0);
                return id.has_value();
            }
            if (key == L"url" || key == L"title") {
                std::optional<std::wstring> text = reader.String();
                if (!text) return false;
                (key == L"url" ? entry.url : entry.title) = std::move(*text);
                return true;
            }
            return reader.Skip();
        });
        history.entries.push_back(std::move(entry));
        return read;
    };
    const bool read = reader.Object([&](const std::wstring& key) {
        if (key == L"currentIndex") {
            current = reader.Integer();
            return current.has_value();
        }
        if (key == L"entries") {
            hasEntries = true;
            return reader.Array(readEntry);
        }
        return reader.Skip();
    });

    if (!read || !reader.AtEnd() || !hasEntries || !current || *current < 0 ||
        *current >= static_cast<int>(history.entries.size())) {
        return std::nullopt;
    }
    history.current = *current;
    return history;
}

bool CanCopyEntry(std::wstring_view url) noexcept {
    return !url.empty() && url != kParkedUrl && !StartsWithIgnoreCase(url, L"javascript:") &&
           !StartsWithIgnoreCase(url, L"blob:");
}

std::optional<HistoryCopy> PlanHistoryCopy(const SessionHistory& history, bool forward) {
    const int count = static_cast<int>(history.entries.size());
    if (history.current < 0 || history.current >= count) return std::nullopt;

    int target = history.current;
    do {
        target += forward ? 1 : -1;
    } while (target >= 0 && target < count && !CanCopyEntry(history.entries[target].url));
    if (target < 0 || target >= count) return std::nullopt;

    HistoryCopy copy;
    for (int i = 0; i < count; ++i) {
        const SessionEntry& entry = history.entries[i];
        if (!CanCopyEntry(entry.url)) continue;
        if (copy.entries.empty() || copy.entries.back().url != entry.url) copy.entries.push_back(entry);
        if (i == target) copy.shown = copy.entries.size() - 1;
    }
    return copy;
}

std::optional<std::size_t> FindShownEntry(const SessionHistory& made, const HistoryCopy& copy) {
    if (copy.shown >= copy.entries.size()) return std::nullopt;
    const std::wstring& url = copy.entries[copy.shown].url;
    const auto distance = [&](std::size_t at) { return at > copy.shown ? at - copy.shown : copy.shown - at; };
    std::optional<std::size_t> found;
    for (std::size_t i = 0; i < made.entries.size(); ++i) {
        if (made.entries[i].url == url && (!found || distance(i) < distance(*found))) found = i;
    }
    // An address the webview wrote differently is still in the place it was made in.
    if (!found && made.entries.size() == copy.entries.size()) found = copy.shown;
    return found;
}

OpenIn WhereClickOpens(bool middleButton, bool ctrl, bool shift) noexcept {
    if (middleButton || ctrl) return shift ? OpenIn::kForegroundTab : OpenIn::kBackgroundTab;
    return shift ? OpenIn::kNewWindow : OpenIn::kSameTab;
}

bool IsPluginPage(std::wstring_view url) noexcept {
    return StartsWithIgnoreCase(url, kPluginOrigin);
}

std::wstring StandInPageHtml(std::wstring_view title) {
    std::wstring html = L"<!doctype html><meta charset=\"utf-8\"><meta name=\"color-scheme\" content=\"dark\">"
                        L"<link rel=\"icon\" href=\"data:,\"><title>";
    html += EscapeHtml(title);
    html += L"</title><script>addEventListener('pageshow', e => { if (e.persisted) location.reload(); });</script>";
    return html;
}

} // namespace exodium
