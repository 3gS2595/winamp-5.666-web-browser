// A saved playlist as the media library keeps it: an .m3u8 file (UTF-8), or an older
// .m3u (the system's code page unless it starts with a UTF-8 mark).
#pragma once

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace covers {

struct PlaylistEntry {
    std::wstring file;  // full path, or the address of a stream
    std::wstring title; // from #EXTINF, empty without one
    int seconds = -1;   // from #EXTINF, -1 without one
};

// `folder` is the playlist's own, which a relative path is taken from.
[[nodiscard]] std::vector<PlaylistEntry> ParsePlaylist(std::string_view bytes, std::wstring_view folder, bool utf8);

// Which entry each row of a list shows, given the rows' titles and the entries': the
// same order if they all match, else each row the first entry not yet taken with its
// title, or -1. (A playlist edited in the library and not yet saved differs from its file.)
[[nodiscard]] std::vector<int> MapRows(std::span<const std::wstring> rowTitles,
                                       std::span<const std::wstring> entryTitles);

// A row's title without the "12. " the library puts in front of it.
[[nodiscard]] std::wstring_view WithoutNumber(std::wstring_view title, int number);

} // namespace covers
