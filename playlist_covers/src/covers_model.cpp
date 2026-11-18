#include "covers_model.h"

#include <windows.h>

#include <algorithm>
#include <unordered_map>
#include <utility>

namespace covers {

namespace {

constexpr std::wstring_view kSeparator = L" - ";

[[nodiscard]] bool IsSlash(wchar_t c) noexcept {
    return c == L'\\' || c == L'/';
}

// "Artist - Title" as two parts, or nothing if the title isn't one.
[[nodiscard]] std::optional<std::pair<std::wstring_view, std::wstring_view>> SplitListTitle(std::wstring_view title) {
    const size_t at = title.find(kSeparator);
    if (at == std::wstring_view::npos || at == 0 || at + kSeparator.size() >= title.size()) return std::nullopt;
    return std::pair{title.substr(0, at), title.substr(at + kSeparator.size())};
}

[[nodiscard]] bool HasTitleTag(const Track& track) noexcept {
    return track.tags && !track.tags->title.empty();
}

// Natural order, as Explorer sorts: case ignored, "Disc 2" before "Disc 10". Empty last.
[[nodiscard]] int CompareText(std::wstring_view a, std::wstring_view b) noexcept {
    if (a.empty() || b.empty()) return a.empty() == b.empty() ? 0 : (a.empty() ? 1 : -1);
    const int result =
        ::CompareStringEx(LOCALE_NAME_USER_DEFAULT, LINGUISTIC_IGNORECASE | SORT_DIGITSASNUMBERS, a.data(),
                          static_cast<int>(a.size()), b.data(), static_cast<int>(b.size()), nullptr, nullptr, 0);
    return result == 0 ? a.compare(b) : result - CSTR_EQUAL;
}

// The album's artist: its album artist tag, else the artist all its songs share.
[[nodiscard]] std::wstring AlbumArtistOf(std::span<const Track> tracks, const std::vector<int>& members) {
    for (const int index : members) {
        const auto& tags = tracks[index].tags;
        if (tags && !tags->albumArtist.empty()) return tags->albumArtist;
    }
    std::wstring shared;
    for (const int index : members) {
        const std::wstring artist = ArtistOf(tracks[index]);
        if (artist.empty()) continue;
        if (shared.empty()) shared = artist;
        else if (FoldCase(shared) != FoldCase(artist)) return kVariousArtists;
    }
    return shared;
}

} // namespace

std::wstring FoldCase(std::wstring_view text) {
    std::wstring folded(text);
    if (!folded.empty()) ::CharLowerBuffW(folded.data(), static_cast<DWORD>(folded.size()));
    return folded;
}

std::wstring_view FolderOf(std::wstring_view file) noexcept {
    const auto slash = std::find_if(file.rbegin(), file.rend(), IsSlash);
    if (slash == file.rend()) return {};
    return file.substr(0, static_cast<size_t>(file.rend() - slash) - 1);
}

std::wstring_view FileStem(std::wstring_view file) noexcept {
    const auto slash = std::find_if(file.rbegin(), file.rend(), IsSlash);
    std::wstring_view name = file.substr(static_cast<size_t>(file.rend() - slash));
    const size_t dot = name.rfind(L'.');
    if (dot != std::wstring_view::npos && dot > 0) name = name.substr(0, dot);
    return name;
}

std::wstring CleanTag(std::wstring_view text) {
    std::wstring clean(text);
    for (wchar_t& c : clean) {
        if (c < L' ') c = L' ';
    }
    const size_t first = clean.find_first_not_of(L' ');
    if (first == std::wstring::npos) return {};
    clean.erase(clean.find_last_not_of(L' ') + 1);
    clean.erase(0, first);
    return clean;
}

std::wstring TitleOf(const Track& track) {
    if (HasTitleTag(track)) return track.tags->title;
    if (const auto parts = SplitListTitle(track.listTitle)) return std::wstring(parts->second);
    if (!track.listTitle.empty()) return track.listTitle;
    return std::wstring(FileStem(track.file));
}

std::wstring ArtistOf(const Track& track) {
    if (track.tags && !track.tags->artist.empty()) return track.tags->artist;
    // With a title tag and no artist, the playlist shows the title alone, and a " - "
    // in it is part of the title.
    if (HasTitleTag(track)) return {};
    if (const auto parts = SplitListTitle(track.listTitle)) return std::wstring(parts->first);
    return {};
}

std::wstring AlbumOf(const Track& track) {
    return track.tags ? track.tags->album : std::wstring();
}

std::wstring GroupKey(const Track& track) {
    if (track.tags && !track.tags->album.empty()) {
        const Tags& tags = *track.tags;
        const std::wstring_view by = tags.albumArtist.empty() ? FolderOf(track.file) : tags.albumArtist;
        return L"a|" + FoldCase(tags.album) + L"|" + FoldCase(by);
    }
    return L"s|" + FoldCase(track.file);
}

std::vector<Group> GroupTracks(std::span<const Track> tracks, GroupOrder order) {
    std::vector<Group> groups;
    std::unordered_map<std::wstring, size_t> byKey;
    for (int index = 0; index < static_cast<int>(tracks.size()); ++index) {
        std::wstring key = GroupKey(tracks[index]);
        auto [found, added] = byKey.try_emplace(key, groups.size());
        if (added) {
            Group& group = groups.emplace_back();
            group.key = std::move(key);
            group.single = group.key.starts_with(L"s|");
        }
        groups[found->second].tracks.push_back(index);
    }

    for (Group& group : groups) {
        const Track& first = tracks[group.tracks.front()];
        if (group.single) {
            group.name = TitleOf(first);
            group.artist = ArtistOf(first);
        } else {
            group.name = first.tags->album;
            group.artist = AlbumArtistOf(tracks, group.tracks);
        }
    }

    // In playlist order they already are: each where its first song is.
    if (order == GroupOrder::kArtist) {
        std::ranges::stable_sort(groups, [](const Group& a, const Group& b) {
            if (const int byArtist = CompareText(a.artist, b.artist)) return byArtist < 0;
            return CompareText(a.name, b.name) < 0;
        });
    } else if (order == GroupOrder::kAlbum) {
        std::ranges::stable_sort(groups, [](const Group& a, const Group& b) {
            if (const int byName = CompareText(a.name, b.name)) return byName < 0;
            return CompareText(a.artist, b.artist) < 0;
        });
    }
    return groups;
}

std::vector<std::wstring> SearchWords(std::wstring_view query) {
    std::vector<std::wstring> words;
    size_t at = 0;
    while (at < query.size()) {
        const size_t start = query.find_first_not_of(L" \t", at);
        if (start == std::wstring_view::npos) break;
        size_t end = query.find_first_of(L" \t", start);
        if (end == std::wstring_view::npos) end = query.size();
        words.push_back(FoldCase(query.substr(start, end - start)));
        at = end;
    }
    return words;
}

std::wstring SearchText(const Track& track) {
    std::wstring text = TitleOf(track);
    for (const std::wstring& part : {ArtistOf(track), AlbumOf(track), track.listTitle}) {
        text += L'\n';
        text += part;
    }
    if (track.tags) (text += L'\n') += track.tags->albumArtist;
    (text += L'\n') += FileStem(track.file);
    return FoldCase(text);
}

bool MatchesAll(std::wstring_view searchText, std::span<const std::wstring> words) noexcept {
    return std::ranges::all_of(words, [&](const std::wstring& word) {
        return searchText.find(word) != std::wstring_view::npos;
    });
}

} // namespace covers
