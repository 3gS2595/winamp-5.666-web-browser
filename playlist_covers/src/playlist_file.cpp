#include "playlist_file.h"

#include <windows.h>

#include <algorithm>
#include <deque>
#include <numeric>
#include <unordered_map>

namespace covers {

namespace {

std::wstring Decode(std::string_view bytes, UINT codePage) {
    if (bytes.empty()) return {};
    const int length = ::MultiByteToWideChar(codePage, 0, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
    std::wstring text(static_cast<size_t>(length), L'\0');
    ::MultiByteToWideChar(codePage, 0, bytes.data(), static_cast<int>(bytes.size()), text.data(), length);
    return text;
}

[[nodiscard]] bool IsAbsolute(std::wstring_view path) {
    if (path.size() >= 2 && path[1] == L':') return true;                         // C:\…
    if (path.starts_with(L"\\\\") || path.starts_with(L"//")) return true;         // \\server\…
    return path.find(L"://") != std::wstring_view::npos;                           // http://…
}

[[nodiscard]] std::wstring_view Trim(std::wstring_view line) {
    while (!line.empty() && (line.back() == L'\r' || line.back() == L' ' || line.back() == L'\t')) line.remove_suffix(1);
    while (!line.empty() && (line.front() == L' ' || line.front() == L'\t')) line.remove_prefix(1);
    return line;
}

} // namespace

std::vector<PlaylistEntry> ParsePlaylist(std::string_view bytes, std::wstring_view folder, bool utf8) {
    if (bytes.starts_with("\xEF\xBB\xBF")) {
        bytes.remove_prefix(3);
        utf8 = true;
    }
    const std::wstring text = Decode(bytes, utf8 ? CP_UTF8 : CP_ACP);

    std::vector<PlaylistEntry> entries;
    PlaylistEntry pending;
    size_t at = 0;
    while (at <= text.size()) {
        size_t end = text.find(L'\n', at);
        if (end == std::wstring::npos) end = text.size();
        const std::wstring_view line = Trim(std::wstring_view(text).substr(at, end - at));
        at = end + 1;
        if (line.empty()) continue;
        if (line.starts_with(L"#EXTINF:")) {
            const std::wstring_view info = line.substr(8);
            const size_t comma = info.find(L',');
            pending.seconds = static_cast<int>(std::wcstol(std::wstring(info.substr(0, comma)).c_str(), nullptr, 10));
            pending.title = comma == std::wstring_view::npos ? std::wstring() : std::wstring(info.substr(comma + 1));
            continue;
        }
        if (line.starts_with(L"#")) continue;

        std::wstring file(line);
        if (!IsAbsolute(file)) {
            // "\Music\x.mp3" is on the playlist's own drive; "x.mp3" is beside it.
            std::wstring base(folder);
            if (file.starts_with(L'\\') || file.starts_with(L'/')) base = base.substr(0, std::min<size_t>(2, base.size()));
            else if (!base.empty() && base.back() != L'\\' && base.back() != L'/') base += L'\\';
            file = base + file;
        }
        pending.file = std::move(file);
        entries.push_back(std::move(pending));
        pending = {};
    }
    return entries;
}

std::wstring_view WithoutNumber(std::wstring_view title, int number) {
    const std::wstring prefix = std::to_wstring(number) + L". ";
    if (title.starts_with(prefix)) title.remove_prefix(prefix.size());
    return title;
}

std::vector<int> MapRows(std::span<const std::wstring> rowTitles, std::span<const std::wstring> entryTitles) {
    std::vector<int> rows(rowTitles.size(), -1);
    if (std::ranges::equal(rowTitles, entryTitles)) {
        std::iota(rows.begin(), rows.end(), 0);
        return rows;
    }
    std::unordered_map<std::wstring_view, std::deque<int>> byTitle;
    for (int i = 0; i < static_cast<int>(entryTitles.size()); ++i) byTitle[entryTitles[static_cast<size_t>(i)]].push_back(i);
    for (size_t r = 0; r < rowTitles.size(); ++r) {
        const auto found = byTitle.find(rowTitles[r]);
        if (found == byTitle.end() || found->second.empty()) continue;
        rows[r] = found->second.front();
        found->second.pop_front();
    }
    return rows;
}

} // namespace covers
