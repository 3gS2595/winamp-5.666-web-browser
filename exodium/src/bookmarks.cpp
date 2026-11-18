#include "bookmarks.h"

#include "json_reader.h"
#include "text_util.h"
#include "url_util.h"

#include <algorithm>
#include <iterator>
#include <memory>
#include <ranges>
#include <unordered_set>
#include <utility>

namespace exodium {

namespace {

// Chrome dates a bookmark in microseconds since 1601, which is that many seconds before 1970.
constexpr std::int64_t kSecondsFrom1601To1970 = 11'644'473'600;
constexpr std::int64_t kMicrosecondsPerSecond = 1'000'000;

[[nodiscard]] std::wstring CleanTitle(std::wstring_view title) {
    return SingleLine(Trim(title), Bookmarks::kMaxTitleLength);
}

// One node of another browser's file: a bookmark, or a folder with nodes of its own. The
// bookmarks in it are added to `found`.
[[nodiscard]] bool ReadNode(JsonReader& reader, std::vector<Bookmark>& found, int depth) {
    if (depth > JsonReader::kMaxDepth) return false;
    std::wstring type, name, url, added;
    const bool read = reader.Object([&](const std::wstring& key) {
        std::wstring* const wanted = key == L"type"         ? &type
                                     : key == L"name"       ? &name
                                     : key == L"url"        ? &url
                                     : key == L"date_added" ? &added
                                                            : nullptr;
        if (wanted) {
            // Each is a string in every file seen; one that isn't is passed over.
            if (std::optional<std::wstring> text = reader.String()) {
                *wanted = std::move(*text);
                return true;
            }
            return reader.Skip();
        }
        if (key == L"children") {
            return reader.Array([&] { return reader.AtObject() ? ReadNode(reader, found, depth + 1) : reader.Skip(); });
        }
        return reader.Skip();
    });
    if (!read) return false;
    if (type == L"url" && Bookmarks::CanKeep(url)) {
        const std::int64_t since1601 = ParseNumber<std::int64_t>(added).value_or(0) / kMicrosecondsPerSecond;
        found.push_back({.url = std::move(url),
                         .title = CleanTitle(name),
                         .added = std::max<std::int64_t>(0, since1601 - kSecondsFrom1601To1970)});
    }
    return true;
}

} // namespace

std::vector<Bookmark>::iterator Bookmarks::Locate(std::wstring_view url) noexcept {
    return std::ranges::find(entries_, url, &Bookmark::url);
}

const Bookmark* Bookmarks::Find(std::wstring_view url) const noexcept {
    const auto found = std::ranges::find(entries_, url, &Bookmark::url);
    return found == entries_.end() ? nullptr : std::to_address(found);
}

bool Bookmarks::CanKeep(std::wstring_view url) noexcept {
    return IsLikelyHttpUrl(url) && PluginPageName(url).empty() && !url.contains(kToolStartPage) &&
           url.find_first_of(L"\t\r\n") == std::wstring_view::npos;
}

bool Bookmarks::Add(std::wstring_view url, std::wstring_view title, std::int64_t now) {
    if (!CanKeep(url) || Locate(url) != entries_.end() || entries_.size() >= kMaxEntries) return false;
    entries_.insert(entries_.begin(), Bookmark{.url = std::wstring(url), .title = CleanTitle(title), .added = now});
    return true;
}

bool Bookmarks::Remove(std::wstring_view url) {
    const auto known = Locate(url);
    if (known == entries_.end()) return false;
    entries_.erase(known);
    return true;
}

std::size_t Bookmarks::Merge(std::span<const Bookmark> others) {
    std::unordered_set<std::wstring_view> have;
    for (const Bookmark& bookmark : entries_) have.insert(bookmark.url);
    // Kept apart until they are all in: the views above are into `entries_`.
    std::vector<Bookmark> added;
    for (const Bookmark& other : others) {
        if (entries_.size() + added.size() >= kMaxEntries) break;
        if (!CanKeep(other.url) || have.contains(other.url)) continue;
        const bool again = std::ranges::any_of(added, [&other](const Bookmark& b) { return b.url == other.url; });
        if (again) continue;
        added.push_back({.url = other.url, .title = CleanTitle(other.title), .added = other.added});
    }
    const std::size_t count = added.size();
    entries_.insert(entries_.end(), std::make_move_iterator(added.begin()), std::make_move_iterator(added.end()));
    return count;
}

Bookmarks Bookmarks::Parse(std::wstring_view text) {
    Bookmarks bookmarks;
    std::unordered_set<std::wstring> seen;
    for (const auto lineRange : text | std::views::split(L'\n')) {
        if (bookmarks.entries_.size() >= kMaxEntries) break;
        const auto columns = SplitColumns(std::wstring_view(lineRange));
        if (!CanKeep(columns[0]) || !seen.insert(std::wstring(columns[0])).second) continue;
        bookmarks.entries_.push_back({
            .url = std::wstring(columns[0]),
            .title = columns.size() > 2 ? CleanTitle(columns[2]) : std::wstring(),
            .added =
                columns.size() > 1 ? std::max<std::int64_t>(0, ParseNumber<std::int64_t>(columns[1]).value_or(0)) : 0,
        });
    }
    return bookmarks;
}

std::wstring Bookmarks::Serialize() const {
    std::wstring text;
    for (const Bookmark& bookmark : entries_) {
        text.append(bookmark.url).append(1, L'\t');
        text.append(std::to_wstring(bookmark.added)).append(1, L'\t');
        text.append(bookmark.title).append(1, L'\n');
    }
    return text;
}

void Bookmarks::Load(const std::filesystem::path& file) {
    const auto text = ReadTextFile(file);
    *this = text ? Parse(*text) : Bookmarks();
}

bool Bookmarks::Save(const std::filesystem::path& file) const {
    return WriteTextFile(file, Serialize());
}

std::optional<std::vector<Bookmark>> ParseChromiumBookmarks(std::wstring_view json) {
    JsonReader reader(json);
    std::vector<Bookmark> found;
    bool hasRoots = false;
    // Under "roots" there is a folder for the bookmarks bar, one for the others, and so
    // on, along with whatever else a version of the browser keeps there.
    const bool read = reader.Object([&](const std::wstring& key) {
        if (key != L"roots") return reader.Skip();
        hasRoots = true;
        return reader.Object(
            [&](const std::wstring&) { return reader.AtObject() ? ReadNode(reader, found, 0) : reader.Skip(); });
    });
    if (!read || !hasRoots || !reader.AtEnd()) return std::nullopt;
    return found;
}

} // namespace exodium
