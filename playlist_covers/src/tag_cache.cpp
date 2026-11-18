#include "tag_cache.h"

#include <windows.h>

#include <charconv>
#include <vector>

namespace covers {

namespace {

constexpr std::string_view kHeader = "playlist_covers tags 1";

std::string ToUtf8(std::wstring_view text) {
    if (text.empty()) return {};
    const int length = ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0,
                                             nullptr, nullptr);
    std::string utf8(static_cast<size_t>(length), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), utf8.data(), length, nullptr,
                          nullptr);
    return utf8;
}

std::wstring FromUtf8(std::string_view text) {
    if (text.empty()) return {};
    const int length = ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring wide(static_cast<size_t>(length), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), length);
    return wide;
}

// A field can't hold the tab or the line end that separate them.
std::string Field(std::wstring_view text) {
    std::string field = ToUtf8(text);
    for (char& c : field) {
        if (c == '\t' || c == '\r' || c == '\n') c = ' ';
    }
    return field;
}

void AppendLine(std::string& text, const std::wstring& path, const CachedTags& entry) {
    char number[32];
    text += Field(path);
    for (const std::uint64_t value : {entry.written, entry.size}) {
        const auto end = std::to_chars(number, number + sizeof(number), value, 16).ptr;
        (text += '\t').append(number, end);
    }
    for (const std::wstring* tag : {&entry.tags.title, &entry.tags.artist, &entry.tags.album, &entry.tags.albumArtist}) {
        (text += '\t') += Field(*tag);
    }
    text += '\n';
}

std::vector<std::string_view> SplitTabs(std::string_view line) {
    std::vector<std::string_view> fields;
    size_t start = 0;
    while (true) {
        const size_t tab = line.find('\t', start);
        fields.push_back(line.substr(start, tab == std::string_view::npos ? std::string_view::npos : tab - start));
        if (tab == std::string_view::npos) return fields;
        start = tab + 1;
    }
}

bool ParseHex(std::string_view text, std::uint64_t& value) {
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value, 16);
    return error == std::errc() && end == text.data() + text.size();
}

} // namespace

std::string SerializeTagCache(const TagCache& cache, std::size_t limit) {
    std::string text(kHeader);
    text += '\n';
    // Over the limit, the files of this session are the ones kept.
    std::size_t written = 0;
    for (const bool used : {true, false}) {
        for (const auto& [path, entry] : cache) {
            if (entry.used != used || written >= limit) continue;
            AppendLine(text, path, entry);
            ++written;
        }
    }
    return text;
}

TagCache ParseTagCache(std::string_view text) {
    TagCache cache;
    if (text.starts_with("\xEF\xBB\xBF")) text.remove_prefix(3);
    if (!text.starts_with(kHeader)) return cache;
    size_t at = text.find('\n');
    while (at != std::string_view::npos && at + 1 < text.size()) {
        const size_t start = at + 1;
        at = text.find('\n', start);
        std::string_view line = text.substr(start, at == std::string_view::npos ? std::string_view::npos : at - start);
        if (line.ends_with('\r')) line.remove_suffix(1);
        const auto fields = SplitTabs(line);
        if (fields.size() != 7 || fields[0].empty()) continue;
        CachedTags entry;
        if (!ParseHex(fields[1], entry.written) || !ParseHex(fields[2], entry.size)) continue;
        entry.tags = {FromUtf8(fields[3]), FromUtf8(fields[4]), FromUtf8(fields[5]), FromUtf8(fields[6])};
        cache.insert_or_assign(FromUtf8(fields[0]), std::move(entry));
    }
    return cache;
}

} // namespace covers
