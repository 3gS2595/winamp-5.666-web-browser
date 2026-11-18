// Bookmarks: the pages the user has marked to come back to, shared by every window. The
// star on the toolbar and Ctrl+D mark the page in view or unmark it, the address bar
// suggests from them as it does from the history, and the bookmarks page lists them
// (exodium://bookmarks, Ctrl+Shift+O), where one can be removed, and another browser's can
// be brought in. They are kept in bookmarks.txt, which is written whenever they change.
//
// Nothing here knows a window or a webview; the star is layout.cpp's and tabs.cpp's, and
// the page is served by webview_events.cpp.
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace exodium {

struct Bookmark {
    std::wstring url;
    std::wstring title;
    std::int64_t added = 0; // when, in seconds since 1970; 0 if not known

    friend bool operator==(const Bookmark&, const Bookmark&) = default;
};

class Bookmarks {
public:
    static constexpr std::size_t kMaxEntries = 10000;
    static constexpr std::size_t kMaxTitleLength = 200;

    // The one added last first.
    [[nodiscard]] std::span<const Bookmark> entries() const noexcept { return entries_; }
    [[nodiscard]] const Bookmark* Find(std::wstring_view url) const noexcept;

    // Whether a page can be bookmarked at all: a web page or a file, as the history keeps.
    [[nodiscard]] static bool CanKeep(std::wstring_view url) noexcept;

    // Marks a page, at the front. Says whether it was: not one that is marked already,
    // which is left as it is, nor one that can't be kept.
    bool Add(std::wstring_view url, std::wstring_view title, std::int64_t now);
    // Unmarks a page. Says whether it was marked.
    bool Remove(std::wstring_view url);
    // Adds those of `others` that aren't there yet, after the ones that are and in the
    // order they come, as another browser's are brought in. Says how many.
    std::size_t Merge(std::span<const Bookmark> others);

    // One bookmark to a line: URL, when it was added, title, with tabs between.
    [[nodiscard]] static Bookmarks Parse(std::wstring_view text);
    [[nodiscard]] std::wstring Serialize() const;

    void Load(const std::filesystem::path& file);
    bool Save(const std::filesystem::path& file) const;

private:
    [[nodiscard]] std::vector<Bookmark>::iterator Locate(std::wstring_view url) noexcept;

    std::vector<Bookmark> entries_;
};

// The bookmarks in the file Chrome, Edge and the browsers made from them keep theirs in
// ("Bookmarks" in the profile's folder, which is JSON), in the order they come in it,
// those in folders included, without the folders. Only pages that can be kept here.
// Nothing if it isn't such a file.
[[nodiscard]] std::optional<std::vector<Bookmark>> ParseChromiumBookmarks(std::wstring_view json);

// A browser whose bookmarks can be brought in, as the bookmarks page offers it.
struct BookmarkSource {
    std::wstring_view id;   // what the page sends back to say which
    std::wstring_view name; // on its button: "Import from <name>"
};

// What the bookmarks page sends: one of these, a line end, and what it is about.
inline constexpr std::wstring_view kBookmarkPageRemove = L"remove"; // then the address
inline constexpr std::wstring_view kBookmarkPageImport = L"import"; // then a source's id

// The bookmarks page, made afresh for every visit: the bookmarks, a box to search them,
// a button on each to remove it, and one for each browser in `sources`. `notice` is said
// at the top, as after bookmarks were brought in.
[[nodiscard]] std::wstring BuildBookmarksPageHtml(std::span<const Bookmark> bookmarks,
                                                  std::span<const BookmarkSource> sources, std::wstring_view notice);

} // namespace exodium
