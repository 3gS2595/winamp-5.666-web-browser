#include "tree.h"

#include <algorithm>
#include <iterator>

namespace folders {

using namespace ml;

namespace {

constexpr wchar_t kNewFolderName[] = L"New folder";
constexpr DWORD kTakenForMs = 5000; // how long a playlist item taken out may come back as the same

std::string ReadWholeFile(const std::wstring& path) {
    std::string data;
    const HANDLE file = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return data;
    LARGE_INTEGER size{};
    if (::GetFileSizeEx(file, &size) && size.QuadPart > 0 && size.QuadPart < 16 * 1024 * 1024) {
        data.resize(static_cast<std::size_t>(size.QuadPart));
        DWORD read = 0;
        if (!::ReadFile(file, data.data(), static_cast<DWORD>(data.size()), &read, nullptr)) read = 0;
        data.resize(read);
    }
    ::CloseHandle(file);
    return data;
}

// Written beside it, then put in its place, so a crash never leaves half a file.
bool WriteWholeFile(const std::wstring& path, const std::string& data) {
    const std::wstring temp = path + L".new";
    const HANDLE file =
        ::CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool ok = ::WriteFile(file, data.data(), static_cast<DWORD>(data.size()), &written, nullptr) &&
                    written == data.size();
    ::CloseHandle(file);
    if (!ok) {
        ::DeleteFileW(temp.c_str());
        return false;
    }
    return ::MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}

} // namespace

// ---- Start and stop

bool Tree::Start(HWND library, wasabi::Playlists* playlists, std::wstring settingsFile, std::wstring playlistsDir,
                 std::function<void()> post) {
    library_.Attach(library);
    playlists_ = playlists;
    settingsFile_ = std::move(settingsFile);
    playlistsDir_ = std::move(playlistsDir);
    post_ = std::move(post);
    model_ = Model::Parse(ReadWholeFile(settingsFile_));
    return Arrange();
}

void Tree::Stop() {
    if (!arranged_) return;
    SyncExpanded();
    Save();
}

bool Tree::Arrange() {
    if (arranged_) return true;
    const HNAVITEM root = Root();
    if (!root) return false;
    const auto records = Snapshot();
    const bool forgot = model_.ForgetMissing([&](std::wstring_view key) {
        return std::any_of(records.begin(), records.end(), [&](const PlaylistRecord& r) { return r.key == key; });
    });
    IdentifyAll(records);

    library_.BeginUpdate();
    for (int key : model_.TreeOrder()) InsertFolderItem(key, records);
    const auto ids = ItemIdsByKey();
    for (const PlaylistRecord& record : records) {
        const auto found = ids.find(record.key);
        if (found != ids.end() && model_.FolderOf(record.key) != 0) Place(library_.FindById(found->second), records);
    }
    for (int key : model_.TreeOrder()) {
        if (model_.Find(key)->expanded) library_.Expand(ItemOfFolder(key), kNavItemExpand);
    }
    library_.EndUpdate();
    arranged_ = true;
    if (forgot) Save();
    return true;
}

// ---- What's what

HNAVITEM Tree::Root() const {
    return library_.FindById(kPlaylistsRootId);
}

HNAVITEM Tree::ItemOfFolder(int key) const {
    if (key == 0) return Root();
    const HNAVITEM item = library_.FindById(FolderIdOf(key));
    return item ? item : Root();
}

Kind Tree::KindOf(int id) const {
    if (id == 0) return Kind::Other;
    if (id == kPlaylistsRootId) return Kind::Root;
    if (id > kFolderIdBase && model_.Exists(id - kFolderIdBase)) return Kind::Folder;
    if (items_.contains(id)) return Kind::Playlist;
    return Kind::Other;
}

int Tree::ContainerOf(int id) const {
    const int parent = library_.IdOf(library_.Parent(library_.FindById(id)));
    if (parent == kPlaylistsRootId) return 0;
    if (KindOf(parent) == Kind::Folder) return FolderKeyOf(parent);
    return -1;
}

std::wstring Tree::PlaylistKeyOf(int id) const {
    const auto found = items_.find(id);
    return found == items_.end() ? std::wstring() : found->second;
}

std::vector<PlaylistRecord> Tree::Snapshot() const {
    std::vector<PlaylistRecord> records;
    if (!playlists_) return records;
    wasabi::PlaylistsLock lock(playlists_);
    const std::size_t count = playlists_->Count();
    records.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        PlaylistRecord record;
        const wchar_t* file = playlists_->Filename(i);
        const wchar_t* name = playlists_->Name(i);
        record.file = file ? file : L"";
        record.name = name ? name : L"";
        record.key = FileKey(record.file, playlistsDir_);
        record.index = i;
        records.push_back(std::move(record));
    }
    indexOf_.clear();
    for (const PlaylistRecord& record : records) indexOf_.emplace(record.key, record.index);
    return records;
}

std::size_t Tree::IndexOfKey(const std::vector<PlaylistRecord>& records, const std::wstring& key) const {
    const auto found = indexOf_.find(key);
    if (found != indexOf_.end() && found->second < records.size() && records[found->second].key == key) {
        return found->second;
    }
    for (const PlaylistRecord& record : records) {
        if (record.key == key) return record.index;
    }
    return SIZE_MAX;
}

std::unordered_map<std::wstring, int> Tree::ItemIdsByKey() const {
    std::unordered_map<std::wstring, int> ids;
    for (const auto& [id, key] : items_) ids.emplace(key, id);
    return ids;
}

Entry Tree::EntryForFolder(int key) const {
    Entry entry;
    entry.folder = true;
    if (const Folder* folder = model_.Find(key)) entry.name = folder->name;
    return entry;
}

Entry Tree::EntryForPlaylist(const std::wstring& key, const std::vector<PlaylistRecord>& records) const {
    Entry entry;
    entry.index = IndexOfKey(records, key);
    return entry;
}

Entry Tree::EntryOf(HNAVITEM item, const std::vector<PlaylistRecord>& records) const {
    const int id = library_.IdOf(item);
    switch (KindOf(id)) {
    case Kind::Folder: return EntryForFolder(FolderKeyOf(id));
    case Kind::Playlist: return EntryForPlaylist(items_.at(id), records);
    default: break;
    }
    // Not known: by its name, if a playlist has it, else last.
    Entry entry;
    entry.index = SIZE_MAX;
    const std::wstring text = library_.TextOf(item);
    for (const PlaylistRecord& record : records) {
        if (record.name == text) {
            entry.index = record.index;
            break;
        }
    }
    return entry;
}

HNAVITEM Tree::PlaceIn(HNAVITEM parent, const Entry& entry, HNAVITEM except,
                       const std::vector<PlaylistRecord>& records) const {
    std::vector<Entry> existing;
    std::vector<HNAVITEM> items;
    for (HNAVITEM child = library_.Child(parent); child; child = library_.Next(child)) {
        if (child == except) continue;
        existing.push_back(EntryOf(child, records));
        items.push_back(child);
    }
    const std::size_t position = InsertPosition(existing, entry);
    return position == 0 ? kNciFirst : items[position - 1];
}

std::wstring Tree::Identify(int id, const std::wstring& text, const std::vector<PlaylistRecord>& records) const {
    const auto ids = ItemIdsByKey();
    auto inTree = [&](const std::wstring& key) { return ids.contains(key); };
    // The one ml_playlists just took out, coming back (it moves an item that way).
    if (taken_.id && ::GetTickCount() - taken_.when < kTakenForMs) {
        const std::size_t index = IndexOfKey(records, taken_.key);
        if (index < records.size() && records[index].name == text) return taken_.key;
    }
    // ml_playlists' own numbering, its id being 3002 + its place in the list.
    if (id >= kFirstPlaylistId) {
        const auto index = static_cast<std::size_t>(id - kFirstPlaylistId);
        if (index < records.size() && records[index].name == text && !inTree(records[index].key)) {
            return records[index].key;
        }
    }
    // One of that name not in the tree yet: the last (a new one is added at the end).
    for (std::size_t i = records.size(); i-- > 0;) {
        if (records[i].name == text && !inTree(records[i].key)) return records[i].key;
    }
    for (const PlaylistRecord& record : records) {
        if (record.name == text) return record.key;
    }
    return {};
}

void Tree::IdentifyAll(const std::vector<PlaylistRecord>& records) {
    items_.clear();
    std::size_t position = 0;
    for (HNAVITEM child = library_.Child(Root()); child; child = library_.Next(child)) {
        const int id = library_.IdOf(child);
        if (id > kFolderIdBase) continue;
        const std::wstring text = library_.TextOf(child);
        std::wstring key;
        // ml_playlists makes them in its list's order.
        if (position < records.size() && records[position].name == text) key = records[position].key;
        else key = Identify(id, text, records);
        ++position;
        if (!key.empty()) items_[id] = key;
    }
}

// ---- Putting items in their place

HNAVITEM Tree::InsertFolderItem(int key, const std::vector<PlaylistRecord>& records) {
    const Folder* folder = model_.Find(key);
    if (!folder) return nullptr;
    const HNAVITEM parent = ItemOfFolder(folder->parent);
    std::wstring name = folder->name;
    std::wstring invariant = L"PlaylistFolder" + std::to_wstring(key);
    NavInsert insert{};
    insert.hParent = parent;
    insert.hInsertAfter = PlaceIn(parent, EntryForFolder(key), nullptr, records);
    insert.item.mask = kNimfItemId | kNimfText | kNimfTextInvariant | kNimfStyle;
    insert.item.id = FolderIdOf(key);
    insert.item.pszText = name.data();
    insert.item.pszInvariant = invariant.data();
    insert.item.style = kNisHasChildren | kNisAllowEdit;
    insert.item.styleMask = kNisHasChildren | kNisAllowEdit;
    return library_.Insert(insert);
}

Tree::Captured Tree::Capture(HNAVITEM item) const {
    Captured captured;
    wchar_t text[1024] = {}, invariant[256] = {};
    captured.info.cbSize = sizeof(NavItem);
    captured.info.hItem = item;
    captured.info.mask = kNimfItemId | kNimfText | kNimfTextInvariant | kNimfImage | kNimfImageSel | kNimfStyle |
                         kNimfFont | kNimfParam;
    captured.info.pszText = text;
    captured.info.cchTextMax = static_cast<int>(std::size(text));
    captured.info.pszInvariant = invariant;
    captured.info.cchInvariantMax = static_cast<int>(std::size(invariant));
    captured.info.styleMask = 0xFFFFFFFF;
    library_.GetInfo(captured.info);
    captured.text = text;
    captured.invariant = invariant;
    captured.expanded = library_.IsExpanded(item);
    for (HNAVITEM child = library_.Child(item); child; child = library_.Next(child)) {
        captured.children.push_back(Capture(child));
    }
    return captured;
}

HNAVITEM Tree::Reinsert(const Captured& captured, HNAVITEM parent, HNAVITEM after) {
    std::wstring text = captured.text, invariant = captured.invariant;
    NavInsert insert{};
    insert.hParent = parent;
    insert.hInsertAfter = after;
    insert.item = captured.info;
    insert.item.hItem = nullptr;
    insert.item.pszText = text.data();
    insert.item.pszInvariant = invariant.empty() ? nullptr : invariant.data();
    if (invariant.empty()) insert.item.mask &= ~kNimfTextInvariant;
    if (insert.item.iImage == -1) insert.item.mask &= ~kNimfImage;
    if (insert.item.iSelectedImage == -1) insert.item.mask &= ~kNimfImageSel;
    if (!insert.item.hFont) insert.item.mask &= ~kNimfFont;
    const HNAVITEM fresh = library_.Insert(insert);
    if (!fresh) return nullptr;
    for (const Captured& child : captured.children) Reinsert(child, fresh, kNciLast);
    return fresh;
}

void Tree::RestoreExpanded(const Captured& captured) {
    if (captured.expanded) library_.Expand(library_.FindById(captured.info.id), kNavItemExpand);
    for (const Captured& child : captured.children) RestoreExpanded(child);
}

void Tree::Place(HNAVITEM item, const std::vector<PlaylistRecord>& records) {
    if (!item) return;
    const int id = library_.IdOf(item);
    HNAVITEM parent = nullptr;
    Entry entry;
    switch (KindOf(id)) {
    case Kind::Folder: {
        const Folder* folder = model_.Find(FolderKeyOf(id));
        parent = ItemOfFolder(folder->parent);
        entry = EntryForFolder(folder->key);
        break;
    }
    case Kind::Playlist: {
        const std::wstring& key = items_.at(id);
        parent = ItemOfFolder(model_.FolderOf(key));
        entry = EntryForPlaylist(key, records);
        break;
    }
    default: return;
    }
    if (!parent) return;
    const HNAVITEM after = PlaceIn(parent, entry, item, records);
    if (library_.Parent(item) == parent) {
        const HNAVITEM previous = library_.Previous(item);
        if (after == (previous ? previous : kNciFirst)) return;
        library_.MoveAmongSiblings(item, after == kNciFirst ? nullptr : after);
        return;
    }
    // Another parent: the item, and all in it, taken out and put back there, ids and all.
    int selected = 0;
    for (HNAVITEM up = library_.Selection(); up; up = library_.Parent(up)) {
        if (up == item) {
            selected = library_.IdOf(library_.Selection());
            break;
        }
    }
    const Captured captured = Capture(item);
    library_.Delete(item);
    Reinsert(captured, parent, after);
    RestoreExpanded(captured);
    if (selected) library_.Select(library_.FindById(selected));
}

void Tree::ArrangeEverything() {
    const auto records = Snapshot();
    const auto ids = ItemIdsByKey();
    library_.BeginUpdate();
    for (const PlaylistRecord& record : records) {
        const auto found = ids.find(record.key);
        if (found != ids.end()) Place(library_.FindById(found->second), records);
    }
    library_.EndUpdate();
}

// ---- ml_playlists' messages to the library

bool Tree::BeforeIpc(LPARAM command, WPARAM param, LRESULT& result) {
    if (Library::Own::Active() || !arranged_ || !param) return false;
    switch (command) {
    case kIpcNavInsertItem: {
        auto* insert = reinterpret_cast<NavInsert*>(param);
        const HNAVITEM root = Root();
        if (!root || insert->hParent != root || !(insert->item.mask & kNimfText) || !insert->item.pszText) {
            return false;
        }
        const auto records = Snapshot();
        const int id = (insert->item.mask & kNimfItemId) ? insert->item.id : 0;
        const std::wstring key = Identify(id, insert->item.pszText, records);
        if (key.empty()) return false;
        if (newPlaylistFolder_ && key != taken_.key) {
            model_.Place(key, newPlaylistFolder_);
            newPlaylistFolder_ = 0;
            Save();
        }
        const HNAVITEM parent = ItemOfFolder(model_.FolderOf(key));
        insert->hParent = parent;
        insert->hInsertAfter = PlaceIn(parent, EntryForPlaylist(key, records), nullptr, records);
        pendingKey_ = key;
        return false;
    }
    case kIpcTreeItemInsertW:
    case kIpcTreeItemAddW: {
        auto* item = reinterpret_cast<TreeItemW*>(param);
        if (item->parentId != static_cast<UINT_PTR>(kPlaylistsRootId) || !item->title) return false;
        const auto records = Snapshot();
        const int id = command == kIpcTreeItemAddW ? static_cast<int>(item->id) : 0;
        const std::wstring key = Identify(id, item->title, records);
        if (key.empty()) return false;
        const HNAVITEM parent = ItemOfFolder(model_.FolderOf(key));
        const int parentId = library_.IdOf(parent);
        item->parentId = static_cast<UINT_PTR>(parentId);
        if (command == kIpcTreeItemInsertW) {
            // "After" its parent is gen_ml's way of saying first.
            const HNAVITEM after = PlaceIn(parent, EntryForPlaylist(key, records), nullptr, records);
            item->id = static_cast<UINT_PTR>(after == kNciFirst ? parentId : library_.IdOf(after));
        }
        pendingKey_ = key;
        return false;
    }
    case kIpcDelTreeItem:
    case kIpcTreeItemDelete:
    case kIpcNavDeleteItem: {
        const int id = command == kIpcDelTreeItem ? static_cast<int>(param)
                                                  : library_.IdOf(reinterpret_cast<HNAVITEM>(param));
        if (KindOf(id) == Kind::Folder) {
            // Not ml_playlists' to take out (it finds an item by its place under Playlists
            // when it has lost track of one, which folders would make wrong).
            result = 0;
            return true;
        }
        const auto found = items_.find(id);
        if (found != items_.end()) {
            taken_ = {id, found->second, ::GetTickCount()};
            items_.erase(found);
            checkRemoved_ = true;
            if (!deferredPending_ && post_) {
                deferredPending_ = true;
                post_();
            }
        }
        return false;
    }
    case kIpcTreeItemSetInfoW: {
        // ml_playlists, after sorting, gives each item the name of the playlist now in its
        // place: the items are then other playlists'.
        auto* info = reinterpret_cast<TreeItemInfoW*>(param);
        const int id = static_cast<int>(info->item.id);
        if (!info->handle && (info->mask & kMltiId) && (info->mask & kMltiText) && items_.contains(id)) {
            relabelled_.push_back(id);
            if (!deferredPending_ && post_) {
                deferredPending_ = true;
                post_();
            }
        }
        return false;
    }
    default: return false;
    }
}

void Tree::AfterIpc(LPARAM command, WPARAM param, LRESULT result) {
    if (pendingKey_.empty()) return;
    if (command != kIpcNavInsertItem && command != kIpcTreeItemInsertW && command != kIpcTreeItemAddW) return;
    const std::wstring key = std::move(pendingKey_);
    pendingKey_.clear();
    if (!result) return;
    const int id = library_.IdOf(reinterpret_cast<HNAVITEM>(result));
    if (!id) return;
    items_[id] = key;
    if (key == taken_.key) taken_ = {};
    Changed(model_.FolderOf(key));
    (void)param;
}

void Tree::Deferred() {
    deferredPending_ = false;
    if (!arranged_) return;
    const auto records = Snapshot();
    if (!relabelled_.empty()) {
        std::vector<int> ids = std::move(relabelled_);
        relabelled_.clear();
        if (ids.size() == records.size()) {
            // The whole list, in order: the n-th item is now the n-th playlist.
            for (std::size_t i = 0; i < ids.size(); ++i) items_[ids[i]] = records[i].key;
        } else {
            for (int id : ids) {
                const std::wstring text = library_.TextOf(library_.FindById(id));
                const std::size_t index = IndexOfKey(records, items_[id]);
                if (index < records.size() && records[index].name == text) continue;
                items_.erase(id);
                const std::wstring key = Identify(0, text, records);
                if (!key.empty()) items_[id] = key;
            }
        }
        ArrangeEverything();
        Changed(-1);
    }
    if (checkRemoved_) {
        checkRemoved_ = false;
        auto exists = [&](std::wstring_view key) {
            return std::any_of(records.begin(), records.end(), [&](const PlaylistRecord& r) { return r.key == key; });
        };
        std::erase_if(items_, [&](const auto& pair) { return !exists(pair.second); });
        if (model_.ForgetMissing(exists)) Save();
        if (taken_.id && ::GetTickCount() - taken_.when >= kTakenForMs) taken_ = {};
        Changed(-1);
    }
}

// ---- What the user does

int Tree::NewFolder(int parent, bool rename) {
    if (!arranged_ || (parent != 0 && !model_.Exists(parent))) return 0;
    const int key = model_.Add(parent, kNewFolderName);
    if (!key) return 0;
    const HNAVITEM item = InsertFolderItem(key, Snapshot());
    library_.Expand(ItemOfFolder(parent), kNavItemExpand);
    library_.EnsureVisible(item);
    Save();
    Changed(parent);
    if (rename) BeginRename(key);
    return key;
}

void Tree::BeginRename(int key) {
    const HNAVITEM item = ItemOfFolder(key);
    if (!item || key == 0) return;
    library_.EnsureVisible(item);
    if (const HWND tree = library_.TreeWindow()) ::SetFocus(tree);
    library_.EditTitle(item);
}

void Tree::FinishRename(int key, const std::wstring& name) {
    if (!model_.Rename(key, name)) return;
    const HNAVITEM item = ItemOfFolder(key);
    library_.SetText(item, model_.Find(key)->name);
    Place(item, Snapshot());
    Save();
    Changed(model_.Find(key)->parent);
    Changed(key);
}

void Tree::DeleteFolder(int key, HWND owner, bool ask) {
    const Folder* folder = model_.Find(key);
    if (!folder) return;
    const int parent = folder->parent;
    std::size_t playlists = 0, folders = 0;
    for (const Folder& f : model_.Folders()) {
        if (f.key != key && model_.IsWithin(f.key, key)) ++folders;
    }
    for (const Folder& f : model_.Folders()) {
        if (model_.IsWithin(f.key, key)) playlists += model_.PlaylistsIn(f.key).size();
    }
    if (ask && (playlists || folders)) {
        const std::wstring where = parent ? L"\"" + model_.Find(parent)->name + L"\"" : std::wstring(L"Playlists");
        std::wstring what;
        if (playlists) what += std::to_wstring(playlists) + (playlists == 1 ? L" playlist" : L" playlists");
        if (playlists && folders) what += L" and ";
        if (folders) what += std::to_wstring(folders) + (folders == 1 ? L" folder" : L" folders");
        const std::wstring text = L"Delete the folder \"" + folder->name + L"\"?\n\nWhat is in it (" + what +
                                  L") goes to " + where + L". No playlist is deleted.";
        if (::MessageBoxW(owner, text.c_str(), L"Delete Folder", MB_YESNO | MB_ICONQUESTION) != IDYES) return;
    }
    const HNAVITEM item = ItemOfFolder(key);
    std::vector<int> inside;
    for (HNAVITEM child = library_.Child(item); child; child = library_.Next(child)) {
        inside.push_back(library_.IdOf(child));
    }
    model_.Remove(key);
    const auto records = Snapshot();
    library_.BeginUpdate();
    for (int id : inside) {
        const HNAVITEM child = library_.FindById(id);
        if (KindOf(id) == Kind::Folder) library_.SetText(child, model_.Find(FolderKeyOf(id))->name);
        Place(child, records);
    }
    library_.Delete(item);
    library_.EndUpdate();
    Save();
    Changed(parent);
}

bool Tree::MoveFolder(int key, int parent) {
    const Folder* folder = model_.Find(key);
    if (!folder) return false;
    const int from = folder->parent;
    if (from == parent) return true;
    if (!model_.MoveFolder(key, parent)) return false;
    const HNAVITEM item = ItemOfFolder(key);
    library_.SetText(item, model_.Find(key)->name);
    Place(item, Snapshot());
    library_.Expand(ItemOfFolder(parent), kNavItemExpand);
    library_.EnsureVisible(ItemOfFolder(key));
    Save();
    Changed(from);
    Changed(parent);
    return true;
}

bool Tree::MovePlaylist(int itemId, int folder) {
    const std::wstring key = PlaylistKeyOf(itemId);
    if (key.empty() || (folder != 0 && !model_.Exists(folder))) return false;
    const int from = model_.FolderOf(key);
    if (from == folder) return true;
    model_.Place(key, folder);
    Place(library_.FindById(itemId), Snapshot());
    library_.Expand(ItemOfFolder(folder), kNavItemExpand);
    library_.EnsureVisible(library_.FindById(itemId));
    Save();
    Changed(from);
    Changed(folder);
    return true;
}

bool Tree::Drop(int draggedId, POINT screen) {
    dragging_ = 0;
    const HNAVITEM target = library_.HitTest(screen);
    if (!target) return false;
    const int targetId = library_.IdOf(target);
    const Kind targetKind = KindOf(targetId);
    int into = -1;
    switch (targetKind) {
    case Kind::Root: into = 0; break;
    case Kind::Folder: into = FolderKeyOf(targetId); break;
    case Kind::Playlist: into = ContainerOf(targetId); break;
    default: return false;
    }
    if (into < 0) return false;
    switch (KindOf(draggedId)) {
    case Kind::Playlist: {
        const int from = model_.FolderOf(PlaylistKeyOf(draggedId));
        // Among its neighbours, or onto Playlists from directly under it: ml_playlists'
        // own moves in its list.
        if (from == into && targetKind != Kind::Folder) return false;
        MovePlaylist(draggedId, into);
        return true;
    }
    case Kind::Folder: {
        const int key = FolderKeyOf(draggedId);
        if (into != 0 && model_.IsWithin(into, key)) return true; // into itself: nothing
        MoveFolder(key, into);
        return true;
    }
    default: return false;
    }
}

bool Tree::TakesDrop(int targetId, int type) const {
    const Kind kind = KindOf(targetId);
    if (kind == Kind::Other) return false;
    const Kind dragged = KindOf(dragging_);
    if (dragged == Kind::Folder) {
        const int key = FolderKeyOf(dragging_);
        const int into =
            kind == Kind::Root ? 0 : kind == Kind::Folder ? FolderKeyOf(targetId) : ContainerOf(targetId);
        return into >= 0 && !(into != 0 && model_.IsWithin(into, key));
    }
    return kind == Kind::Folder && dragged == Kind::Playlist && type == kTypeFileNames;
}

// ---- Views and menus

std::vector<Row> Tree::RowsOf(int folder) {
    std::vector<Row> rows;
    for (int key : model_.ChildFolders(folder)) {
        Row row;
        row.folder = true;
        row.key = key;
        row.itemId = FolderIdOf(key);
        row.name = model_.Find(key)->name;
        rows.push_back(std::move(row));
    }
    const auto records = Snapshot();
    const auto ids = ItemIdsByKey();
    for (const PlaylistRecord& record : records) {
        if (model_.FolderOf(record.key) != folder) continue;
        Row row;
        row.name = record.name;
        row.file = record.file;
        if (const auto found = ids.find(record.key); found != ids.end()) row.itemId = found->second;
        if (playlists_) {
            wasabi::PlaylistsLock lock(playlists_);
            std::size_t songs = 0, seconds = 0;
            if (playlists_->GetInfo(record.index, wasabi::kItemCountInfo, &songs, sizeof(songs)) == 0) {
                row.songs = songs;
            }
            if (playlists_->GetInfo(record.index, wasabi::kTotalTimeInfo, &seconds, sizeof(seconds)) == 0) {
                row.seconds = seconds;
            }
        }
        rows.push_back(std::move(row));
    }
    return rows;
}

std::vector<std::wstring> Tree::FilesUnder(int folder) {
    std::vector<std::wstring> files;
    for (const Row& row : RowsOf(folder)) {
        if (row.folder) {
            const auto inner = FilesUnder(row.key);
            files.insert(files.end(), inner.begin(), inner.end());
        } else if (!row.file.empty()) {
            files.push_back(row.file);
        }
    }
    return files;
}

int Tree::PlaylistImage() const {
    for (const auto& [id, key] : items_) {
        NavItem info{};
        info.cbSize = sizeof(info);
        info.hItem = library_.FindById(id);
        info.mask = kNimfImage;
        if (info.hItem && library_.GetInfo(info) && info.iImage >= 0) return info.iImage;
    }
    return -1;
}

int Tree::ItemIdOfName(const std::wstring& name) const {
    const auto ids = ItemIdsByKey();
    for (const PlaylistRecord& record : Snapshot()) {
        if (record.name != name) continue;
        if (const auto found = ids.find(record.key); found != ids.end()) return found->second;
    }
    return 0;
}

HNAVITEM Tree::FindByText(const std::wstring& text) const {
    std::function<HNAVITEM(HNAVITEM)> walk = [&](HNAVITEM item) -> HNAVITEM {
        for (; item; item = library_.Next(item)) {
            if (library_.TextOf(item) == text) return item;
            if (const HNAVITEM inner = walk(library_.Child(item))) return inner;
        }
        return nullptr;
    };
    const HNAVITEM root = Root();
    return root ? walk(library_.Child(root)) : nullptr;
}

void Tree::Changed(int folder) {
    if (changed_) changed_(folder);
}

void Tree::SyncExpanded() {
    if (!arranged_ || !Root()) return;
    for (const Folder& folder : model_.Folders()) {
        const HNAVITEM item = library_.FindById(FolderIdOf(folder.key));
        if (item) model_.SetExpanded(folder.key, library_.IsExpanded(item));
    }
}

void Tree::Save() {
    if (settingsFile_.empty()) return;
    WriteWholeFile(settingsFile_, model_.Serialize());
}

std::wstring Tree::Dump() const {
    std::wstring text;
    std::function<void(HNAVITEM, int)> walk = [&](HNAVITEM item, int depth) {
        for (; item; item = library_.Next(item)) {
            const int id = library_.IdOf(item);
            text += std::wstring(static_cast<std::size_t>(depth) * 2, L' ') + library_.TextOf(item);
            if (KindOf(id) == Kind::Folder) text += library_.IsExpanded(item) ? L" [folder, open]" : L" [folder]";
            text += L"\r\n";
            walk(library_.Child(item), depth + 1);
        }
    };
    const HNAVITEM root = Root();
    if (!root) return text;
    text += library_.TextOf(root) + L"\r\n";
    walk(library_.Child(root), 1);
    return text;
}

} // namespace folders
