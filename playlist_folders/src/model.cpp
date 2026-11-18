#include "model.h"

#include <windows.h>

#include <shlwapi.h>

#include <algorithm>
#include <set>

namespace folders {

namespace {

std::wstring Lower(std::wstring_view text) {
    std::wstring result(text);
    if (!result.empty()) {
        ::LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, result.data(), static_cast<int>(result.size()),
                        result.data(), static_cast<int>(result.size()), nullptr, nullptr, 0);
    }
    return result;
}

std::string Utf8(std::wstring_view text) {
    if (text.empty()) return {};
    const int size = ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0,
                                           nullptr, nullptr);
    std::string result(static_cast<std::size_t>(size), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size, nullptr,
                          nullptr);
    return result;
}

std::wstring Wide(std::string_view text) {
    if (text.empty()) return {};
    const int size = ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size);
    return result;
}

std::vector<std::string_view> Split(std::string_view line, char separator) {
    std::vector<std::string_view> parts;
    std::size_t start = 0;
    while (true) {
        const std::size_t end = line.find(separator, start);
        parts.push_back(line.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start));
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    return parts;
}

bool ToInt(std::string_view text, int& value) {
    if (text.empty() || text.size() > 9) return false;
    int result = 0;
    for (char c : text) {
        if (c < '0' || c > '9') return false;
        result = result * 10 + (c - '0');
    }
    value = result;
    return true;
}

} // namespace

std::wstring FileKey(std::wstring_view filename, std::wstring_view playlistsDir) {
    std::wstring file = Lower(filename);
    std::replace(file.begin(), file.end(), L'/', L'\\');
    std::wstring dir = Lower(playlistsDir);
    std::replace(dir.begin(), dir.end(), L'/', L'\\');
    while (!dir.empty() && dir.back() == L'\\') dir.pop_back();
    if (!dir.empty() && file.size() > dir.size() + 1 && file.compare(0, dir.size(), dir) == 0 &&
        file[dir.size()] == L'\\') {
        file.erase(0, dir.size() + 1);
    }
    return file;
}

std::wstring CleanName(std::wstring_view name) {
    std::wstring result;
    result.reserve(name.size());
    for (wchar_t c : name) result.push_back(c < L' ' ? L' ' : c);
    const auto first = result.find_first_not_of(L' ');
    if (first == std::wstring::npos) return {};
    const auto last = result.find_last_not_of(L' ');
    return result.substr(first, last - first + 1);
}

bool NameBefore(std::wstring_view a, std::wstring_view b) {
    const std::wstring left(a), right(b);
    const int order = ::StrCmpLogicalW(left.c_str(), right.c_str());
    if (order != 0) return order < 0;
    return left < right; // the same but for case: still one order
}

const Folder* Model::Find(int key) const {
    for (const Folder& folder : folders_) {
        if (folder.key == key) return &folder;
    }
    return nullptr;
}

Folder* Model::FindMutable(int key) {
    for (Folder& folder : folders_) {
        if (folder.key == key) return &folder;
    }
    return nullptr;
}

std::wstring Model::UniqueName(int parent, std::wstring_view base, int except) const {
    const std::wstring clean = CleanName(base);
    auto taken = [&](const std::wstring& name) {
        const std::wstring lower = Lower(name);
        for (const Folder& folder : folders_) {
            if (folder.parent == parent && folder.key != except && Lower(folder.name) == lower) return true;
        }
        return false;
    };
    if (!taken(clean)) return clean;
    for (int n = 2;; ++n) {
        std::wstring name = clean + L" (" + std::to_wstring(n) + L")";
        if (!taken(name)) return name;
    }
}

int Model::Add(int parent, std::wstring_view name) {
    if (parent != 0 && !Exists(parent)) return 0;
    if (CleanName(name).empty()) return 0;
    Folder folder;
    folder.key = nextKey_++;
    folder.parent = parent;
    folder.name = UniqueName(parent, name);
    folders_.push_back(folder);
    return folder.key;
}

bool Model::Rename(int key, std::wstring_view name) {
    Folder* folder = FindMutable(key);
    if (!folder || CleanName(name).empty()) return false;
    folder->name = UniqueName(folder->parent, name, key);
    return true;
}

void Model::Remove(int key) {
    const Folder* folder = Find(key);
    if (!folder) return;
    const int parent = folder->parent;
    std::erase_if(folders_, [key](const Folder& f) { return f.key == key; });
    for (Folder& child : folders_) {
        if (child.parent == key) {
            child.parent = parent;
            child.name = UniqueName(parent, child.name, child.key);
        }
    }
    for (auto& [file, in] : placed_) {
        if (in == key) in = parent;
    }
    std::erase_if(placed_, [](const auto& pair) { return pair.second == 0; });
}

bool Model::MoveFolder(int key, int parent) {
    Folder* folder = FindMutable(key);
    if (!folder) return false;
    if (parent != 0 && (!Exists(parent) || IsWithin(parent, key))) return false;
    if (folder->parent == parent) return true;
    folder->parent = parent;
    folder->name = UniqueName(parent, folder->name, key);
    return true;
}

void Model::SetExpanded(int key, bool expanded) {
    if (Folder* folder = FindMutable(key)) folder->expanded = expanded;
}

bool Model::IsWithin(int key, int ancestor) const {
    for (int guard = 0; key != 0 && guard <= static_cast<int>(folders_.size()); ++guard) {
        if (key == ancestor) return true;
        const Folder* folder = Find(key);
        if (!folder) return false;
        key = folder->parent;
    }
    return false;
}

std::vector<int> Model::ChildFolders(int parent) const {
    std::vector<const Folder*> children;
    for (const Folder& folder : folders_) {
        if (folder.parent == parent) children.push_back(&folder);
    }
    std::sort(children.begin(), children.end(),
              [](const Folder* a, const Folder* b) { return NameBefore(a->name, b->name); });
    std::vector<int> keys;
    for (const Folder* folder : children) keys.push_back(folder->key);
    return keys;
}

std::vector<int> Model::TreeOrder() const {
    std::vector<int> order;
    std::function<void(int)> walk = [&](int parent) {
        for (int key : ChildFolders(parent)) {
            order.push_back(key);
            walk(key);
        }
    };
    walk(0);
    return order;
}

std::vector<std::wstring> Model::PathOf(int key) const {
    std::vector<std::wstring> path;
    for (int guard = 0; key != 0 && guard <= static_cast<int>(folders_.size()); ++guard) {
        const Folder* folder = Find(key);
        if (!folder) break;
        path.insert(path.begin(), folder->name);
        key = folder->parent;
    }
    return path;
}

int Model::FolderOf(std::wstring_view fileKey) const {
    const auto found = placed_.find(fileKey);
    if (found == placed_.end()) return 0;
    return Exists(found->second) ? found->second : 0;
}

void Model::Place(std::wstring_view fileKey, int folder) {
    if (fileKey.empty()) return;
    if (folder == 0 || !Exists(folder)) {
        const auto found = placed_.find(fileKey);
        if (found != placed_.end()) placed_.erase(found);
        return;
    }
    placed_.insert_or_assign(std::wstring(fileKey), folder);
}

std::vector<std::wstring> Model::PlaylistsIn(int folder) const {
    std::vector<std::wstring> keys;
    for (const auto& [file, in] : placed_) {
        if (in == folder) keys.push_back(file);
    }
    return keys;
}

bool Model::ForgetMissing(const std::function<bool(std::wstring_view)>& exists) {
    const std::size_t before = placed_.size();
    std::erase_if(placed_, [&](const auto& pair) { return !exists(pair.first); });
    return placed_.size() != before;
}

std::string Model::Serialize() const {
    std::string text = "# Playlist folders in Winamp's media library, kept by ml_playlist_folders.dll.\r\n"
                       "# folder <tab> key <tab> parent key (0: top) <tab> open (1) or shut (0) <tab> name\r\n"
                       "# playlist <tab> folder key <tab> file\r\n"
                       "version\t1\r\n";
    text += "next\t" + std::to_string(nextKey_) + "\r\n";
    for (int key : TreeOrder()) {
        const Folder* folder = Find(key);
        text += "folder\t" + std::to_string(folder->key) + "\t" + std::to_string(folder->parent) + "\t" +
                (folder->expanded ? "1" : "0") + "\t" + Utf8(folder->name) + "\r\n";
    }
    for (const auto& [file, in] : placed_) {
        if (Exists(in)) text += "playlist\t" + std::to_string(in) + "\t" + Utf8(file) + "\r\n";
    }
    return text;
}

Model Model::Parse(std::string_view text) {
    if (text.size() >= 3 && text.substr(0, 3) == "\xEF\xBB\xBF") text.remove_prefix(3);
    Model model;
    std::vector<std::pair<std::wstring, int>> playlists;
    int next = 1;
    for (std::string_view line : Split(text, '\n')) {
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (line.empty() || line.front() == '#') continue;
        const auto parts = Split(line, '\t');
        if (parts[0] == "next" && parts.size() >= 2) {
            int value = 0;
            if (ToInt(parts[1], value)) next = std::max(next, value);
        } else if (parts[0] == "folder" && parts.size() >= 5) {
            Folder folder;
            int open = 0;
            if (!ToInt(parts[1], folder.key) || folder.key <= 0 || !ToInt(parts[2], folder.parent) ||
                !ToInt(parts[3], open)) {
                continue;
            }
            folder.expanded = open != 0;
            folder.name = CleanName(Wide(parts[4]));
            if (folder.name.empty() || model.Exists(folder.key)) continue;
            model.folders_.push_back(folder);
            next = std::max(next, folder.key + 1);
        } else if (parts[0] == "playlist" && parts.size() >= 3) {
            int in = 0;
            if (!ToInt(parts[1], in) || in <= 0 || parts[2].empty()) continue;
            playlists.emplace_back(Lower(Wide(parts[2])), in);
        }
    }
    // A folder whose parent is missing, or that would be inside itself, goes to the top.
    for (Folder& folder : model.folders_) {
        if (folder.parent == folder.key || (folder.parent != 0 && !model.Exists(folder.parent))) folder.parent = 0;
    }
    for (Folder& folder : model.folders_) {
        std::set<int> seen{folder.key};
        for (int up = folder.parent; up != 0;) {
            if (!seen.insert(up).second) {
                folder.parent = 0;
                break;
            }
            const Folder* parent = model.Find(up);
            up = parent ? parent->parent : 0;
        }
    }
    // Two folders of one name in one place: the second gets a number.
    for (Folder& folder : model.folders_) folder.name = model.UniqueName(folder.parent, folder.name, folder.key);
    model.nextKey_ = next;
    for (const auto& [file, in] : playlists) model.Place(file, in);
    return model;
}

bool EntryBefore(const Entry& a, const Entry& b) {
    if (a.folder != b.folder) return a.folder;
    if (a.folder) return NameBefore(a.name, b.name);
    return a.index < b.index;
}

std::size_t InsertPosition(const std::vector<Entry>& existing, const Entry& entry) {
    std::size_t position = 0;
    for (std::size_t i = 0; i < existing.size(); ++i) {
        if (EntryBefore(existing[i], entry)) position = i + 1;
    }
    return position;
}

} // namespace folders
