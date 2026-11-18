// The folders, and which playlist is in which: what the plugin keeps, without anything of
// Winamp's. A playlist is known by its file (as the media library's playlists.xml names
// it); one in no folder is directly under Playlists, as without the plugin.
#pragma once

#include <cstddef>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace folders {

struct Folder {
    int key = 0;    // 1 and up, never reused while the file lives
    int parent = 0; // 0: directly under Playlists
    std::wstring name;
    bool expanded = false;
};

// How a playlist file is known: lower case, backslashes, and without the playlists folder
// when it is in it (ml_playlists names its own files that way).
std::wstring FileKey(std::wstring_view filename, std::wstring_view playlistsDir);

// A folder's name as typed, tidied: no tabs or line breaks, no spaces round it. Empty if
// nothing is left.
std::wstring CleanName(std::wstring_view name);

// Explorer's order for names (numbers by value, case ignored).
bool NameBefore(std::wstring_view a, std::wstring_view b);

class Model {
public:
    [[nodiscard]] const Folder* Find(int key) const;
    [[nodiscard]] bool Exists(int key) const { return Find(key) != nullptr; }
    [[nodiscard]] const std::vector<Folder>& Folders() const { return folders_; }

    // A new folder in `parent` (0: top), its name made unique there. 0 if the name is empty
    // or the parent unknown.
    int Add(int parent, std::wstring_view name);
    // False for an empty name. The name is made unique among its siblings.
    bool Rename(int key, std::wstring_view name);
    // The folder goes; what was in it, folders and playlists, goes to its parent.
    void Remove(int key);
    // Into another folder (0: top). False for itself or one inside it.
    bool MoveFolder(int key, int parent);
    void SetExpanded(int key, bool expanded);

    // True when `key` is `ancestor` or inside it.
    [[nodiscard]] bool IsWithin(int key, int ancestor) const;
    // The folders in `parent`, in name order.
    [[nodiscard]] std::vector<int> ChildFolders(int parent) const;
    // Every folder, each after its parent, siblings in name order.
    [[nodiscard]] std::vector<int> TreeOrder() const;
    // "Rock", "Classic Rock" ...: its path from the top, for menus.
    [[nodiscard]] std::vector<std::wstring> PathOf(int key) const;
    // `base`, or `base (2)`, `base (3)`... whichever no other folder in `parent` has.
    [[nodiscard]] std::wstring UniqueName(int parent, std::wstring_view base, int except = 0) const;

    // The folder a playlist is in (0: none).
    [[nodiscard]] int FolderOf(std::wstring_view fileKey) const;
    // 0 takes it out of any folder. A folder that doesn't exist counts as 0.
    void Place(std::wstring_view fileKey, int folder);
    // The playlists in a folder (its keys), in no particular order.
    [[nodiscard]] std::vector<std::wstring> PlaylistsIn(int folder) const;
    [[nodiscard]] std::size_t PlacedCount() const { return placed_.size(); }
    // Forgets the playlists `exists` says are gone. True if any were.
    bool ForgetMissing(const std::function<bool(std::wstring_view)>& exists);

    // As kept in the file, UTF-8.
    [[nodiscard]] std::string Serialize() const;
    // Whatever of `text` makes sense; lines it doesn't understand are left out.
    static Model Parse(std::string_view text);

private:
    Folder* FindMutable(int key);

    std::vector<Folder> folders_;
    std::map<std::wstring, int, std::less<>> placed_; // file key -> folder
    int nextKey_ = 1;
};

// Where an item goes among the items already in a folder of the tree (or directly under
// Playlists): folders first, in name order, then playlists in the library's order.
struct Entry {
    bool folder = false;
    std::wstring name;     // a folder's
    std::size_t index = 0; // a playlist's place in the library's list
};
[[nodiscard]] bool EntryBefore(const Entry& a, const Entry& b);
// How many of `existing` (in their order in the tree) it goes after. Those out of order
// don't move it: it goes after the last one that belongs before it.
[[nodiscard]] std::size_t InsertPosition(const std::vector<Entry>& existing, const Entry& entry);

} // namespace folders
