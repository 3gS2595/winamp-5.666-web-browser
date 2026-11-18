#include "session_history.h"

#include "json_reader.h"
#include "text_util.h"
#include "url_util.h"

#include <utility>

namespace exodium {

namespace {

// The origin of kHistoryUrl, and of any other page the plugin makes itself.
constexpr std::wstring_view kPluginOrigin = L"https://exodium.invalid/";

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
