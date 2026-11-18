// The folders in the media library's tree, under Playlists, and ml_playlists' items moved
// into them.
//
// ml_playlists keeps making its items directly under Playlists: at start, for a new
// playlist, and when it moves one (it takes the item out and puts it back). Its messages
// to the library pass through Tree::BeforeIpc first, which puts each of its items where
// the model says, under a folder. An item keeps its id, which is all ml_playlists knows
// it by, wherever it is in the tree.
#pragma once

#include "library.h"
#include "model.h"
#include "wasabi.h"

#include <windows.h>

#include <functional>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace folders {

inline constexpr int kPlaylistsRootId = 3001;       // ml_playlists' "Playlists"
inline constexpr int kFirstPlaylistId = 3002;       // ml_playlists numbers its items from here at start
inline constexpr int kFolderIdBase = 0x46440000;    // the folders' ids: this plus the folder's key
inline constexpr int kFolderDragType = 0x4644;      // what a folder is, dragged (no other plugin takes it)

enum class Kind { Other, Root, Folder, Playlist };

struct PlaylistRecord {
    std::wstring file; // as the library has it
    std::wstring key;  // FileKey
    std::wstring name;
    std::size_t index = 0;
};

// A row of a folder's view: a folder in it, or a playlist.
struct Row {
    bool folder = false;
    int key = 0;      // the folder's
    int itemId = 0;   // the tree item's
    std::wstring name;
    std::wstring file;
    std::size_t songs = 0;
    std::size_t seconds = 0;
};

class Tree {
public:
    // `post` asks for a call back on Winamp's thread later (once the library's message is done).
    bool Start(HWND library, wasabi::Playlists* playlists, std::wstring settingsFile, std::wstring playlistsDir,
               std::function<void()> post);
    void Stop();
    // Puts in the folders and moves the playlists into them, once Playlists is there.
    bool Arrange();
    [[nodiscard]] bool Started() const { return arranged_; }

    [[nodiscard]] Library& Lib() { return library_; }
    [[nodiscard]] const Model& Folders() const { return model_; }

    // ---- What a tree item is
    [[nodiscard]] Kind KindOf(int id) const;
    [[nodiscard]] Kind KindOfItem(ml::HNAVITEM item) const { return KindOf(library_.IdOf(item)); }
    [[nodiscard]] static int FolderKeyOf(int id) { return id > kFolderIdBase ? id - kFolderIdBase : 0; }
    [[nodiscard]] static int FolderIdOf(int key) { return kFolderIdBase + key; }
    [[nodiscard]] ml::HNAVITEM ItemOfFolder(int key) const; // 0: Playlists itself
    // The folder an item is in (0: directly under Playlists), -1 for one outside Playlists.
    [[nodiscard]] int ContainerOf(int id) const;
    [[nodiscard]] std::wstring PlaylistKeyOf(int id) const;

    // ---- The library's messages, from ml_playlists or anyone (see the top)
    // True to stop the message there, with `result`.
    bool BeforeIpc(LPARAM command, WPARAM param, LRESULT& result);
    void AfterIpc(LPARAM command, WPARAM param, LRESULT result);
    // The work BeforeIpc leaves for later (`post`).
    void Deferred();

    // ---- What the user does
    int NewFolder(int parent, bool rename); // the new folder's key, or 0
    void BeginRename(int key);
    void FinishRename(int key, const std::wstring& name);
    // Asks first (`ask`) when there is something in it.
    void DeleteFolder(int key, HWND owner, bool ask = true);
    bool MoveFolder(int key, int parent);
    bool MovePlaylist(int itemId, int folder);
    // A playlist or folder dropped at a point of the tree (ML_MSG_TREE_ONDROP). True if
    // it was a move into or out of a folder, done here.
    bool Drop(int draggedId, POINT screen);
    // Whether a folder takes what's dragged over it: playlists and folders of the tree.
    [[nodiscard]] bool TakesDrop(int targetId, int type) const;
    void NoteDragStart(int draggedId) { dragging_ = draggedId; }

    // The next playlist ml_playlists makes goes into this folder (New playlist here).
    void SetNewPlaylistFolder(int key) { newPlaylistFolder_ = key; }

    // ---- For the folder views and menus
    [[nodiscard]] std::vector<Row> RowsOf(int folder);
    // The playlists' files in a folder and the folders in it, in the tree's order.
    [[nodiscard]] std::vector<std::wstring> FilesUnder(int folder);
    [[nodiscard]] std::vector<PlaylistRecord> Snapshot() const;
    // The picture ml_playlists gives its items (an index in the tree's image list), or -1.
    [[nodiscard]] int PlaylistImage() const;
    // The tree item of the playlist of that name (the first), or 0.
    [[nodiscard]] int ItemIdOfName(const std::wstring& name) const;
    // The tree item under Playlists with that text, folders included, or null.
    [[nodiscard]] ml::HNAVITEM FindByText(const std::wstring& text) const;
    void SetChanged(std::function<void(int folder)> changed) { changed_ = std::move(changed); }

    // The open or shut state of each folder, from the tree, saved with the rest.
    void SyncExpanded();
    void Save();
    // For the test script: the tree under Playlists, one line an item, indented.
    [[nodiscard]] std::wstring Dump() const;

private:
    struct Captured {
        ml::NavItem info{};
        std::wstring text, invariant;
        bool expanded = false;
        std::vector<Captured> children;
    };

    [[nodiscard]] ml::HNAVITEM Root() const;
    [[nodiscard]] std::size_t IndexOfKey(const std::vector<PlaylistRecord>& records, const std::wstring& key) const;
    // Playlist key -> its item's id, for the items in the tree.
    [[nodiscard]] std::unordered_map<std::wstring, int> ItemIdsByKey() const;
    [[nodiscard]] Entry EntryOf(ml::HNAVITEM item, const std::vector<PlaylistRecord>& records) const;
    // The child of `parent` that an item of `entry` goes after (kNciFirst: first).
    [[nodiscard]] ml::HNAVITEM PlaceIn(ml::HNAVITEM parent, const Entry& entry, ml::HNAVITEM except,
                                       const std::vector<PlaylistRecord>& records) const;
    [[nodiscard]] Entry EntryForFolder(int key) const;
    [[nodiscard]] Entry EntryForPlaylist(const std::wstring& key, const std::vector<PlaylistRecord>& records) const;

    ml::HNAVITEM InsertFolderItem(int key, const std::vector<PlaylistRecord>& records);
    // Puts an item where it belongs: its folder, its place there.
    void Place(ml::HNAVITEM item, const std::vector<PlaylistRecord>& records);
    Captured Capture(ml::HNAVITEM item) const;
    ml::HNAVITEM Reinsert(const Captured& captured, ml::HNAVITEM parent, ml::HNAVITEM after);
    void RestoreExpanded(const Captured& captured);
    // Which playlist an item ml_playlists makes is.
    std::wstring Identify(int id, const std::wstring& text, const std::vector<PlaylistRecord>& records) const;
    void IdentifyAll(const std::vector<PlaylistRecord>& records);
    void ArrangeEverything();
    void Changed(int folder);

    Library library_;
    wasabi::Playlists* playlists_ = nullptr;
    std::wstring settingsFile_, playlistsDir_;
    std::function<void()> post_;
    std::function<void(int)> changed_;
    Model model_;
    bool arranged_ = false;

    std::map<int, std::wstring> items_; // ml_playlists' item ids -> playlist key
    // Playlist key -> place in the library's list, as the last Snapshot found it (all that
    // one piece of work does uses one snapshot, so a lookup needn't go through the list).
    mutable std::unordered_map<std::wstring, std::size_t> indexOf_;
    struct Taken {
        int id = 0;
        std::wstring key;
        DWORD when = 0;
    } taken_; // the playlist item ml_playlists took out last (it may put it back)
    std::vector<int> relabelled_; // items ml_playlists gave other playlists' names (it sorted)
    std::wstring pendingKey_;     // the playlist of the item being made (BeforeIpc to AfterIpc)
    bool deferredPending_ = false;
    bool checkRemoved_ = false;
    int newPlaylistFolder_ = 0;
    int dragging_ = 0;
};

} // namespace folders
