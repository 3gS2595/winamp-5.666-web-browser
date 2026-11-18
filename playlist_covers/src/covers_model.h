// The playlist as the window shows it: its songs, and the covers they are grouped
// under. Nothing here needs Winamp or a window, so the tests can run it.
#pragma once

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace covers {

// What a file's tags say, as Winamp's input plug-ins read them.
struct Tags {
    std::wstring title, artist, album, albumArtist;
    bool operator==(const Tags&) const = default;
};

// One entry of Winamp's playlist.
struct Track {
    std::wstring file;
    std::wstring listTitle; // the playlist's own title for it, usually "Artist - Title"
    std::wstring length;    // as the playlist shows it ("3:45"); empty while it doesn't know
    std::optional<Tags> tags; // once they have been read
};

// A cover: an album's songs, or a song with no album of its own.
struct Group {
    std::wstring key;        // the same for the same album however the playlist changes
    std::wstring name;       // the album, or the song's title
    std::wstring artist;     // the album artist, the artist all its songs share, or "Various artists"
    std::vector<int> tracks; // playlist positions, in playlist order
    bool single = false;     // a song of its own rather than an album
};

enum class GroupOrder { kPlaylist = 0, kArtist = 1, kAlbum = 2 };

inline constexpr wchar_t kVariousArtists[] = L"Various artists";

[[nodiscard]] std::wstring FoldCase(std::wstring_view text);
// The folder a file is in, without the last backslash; empty if there is none.
[[nodiscard]] std::wstring_view FolderOf(std::wstring_view file) noexcept;
// The file's name without its folder or extension.
[[nodiscard]] std::wstring_view FileStem(std::wstring_view file) noexcept;
// A tag as it is shown and stored: control characters as spaces, trimmed.
[[nodiscard]] std::wstring CleanTag(std::wstring_view text);

// What the list shows for a song. While its tags aren't known, and for a file that
// has none, the playlist's "Artist - Title" is taken apart.
[[nodiscard]] std::wstring TitleOf(const Track& track);
[[nodiscard]] std::wstring ArtistOf(const Track& track);
[[nodiscard]] std::wstring AlbumOf(const Track& track);

// The songs of an album go under one cover: the same album tag and album artist, or,
// without an album artist, the same album tag in the same folder. Any other song has
// a cover of its own.
[[nodiscard]] std::wstring GroupKey(const Track& track);
[[nodiscard]] std::vector<Group> GroupTracks(std::span<const Track> tracks, GroupOrder order);

// A search is words, each of which a song has to have somewhere in its title, artist,
// album or file name, in any case.
[[nodiscard]] std::vector<std::wstring> SearchWords(std::wstring_view query);
[[nodiscard]] std::wstring SearchText(const Track& track);
[[nodiscard]] bool MatchesAll(std::wstring_view searchText, std::span<const std::wstring> words) noexcept;

} // namespace covers
