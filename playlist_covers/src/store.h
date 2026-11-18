// What is known about the songs, shared by every playlist shown: their tags, and the
// covers made from their pictures, read on the loader's threads. Going from one
// playlist to another doesn't read again what has been read.
#pragma once

#include "covers_model.h"
#include "loader.h"

#include <windows.h>

#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace covers {

struct Art {
    HBITMAP bitmap = nullptr; // premultiplied, top-down, size x size
    int size = 0;
    bool missing = false; // none of the files had a picture
    ULONGLONG used = 0;
};

class Store {
public:
    Store() = default;
    Store(const Store&) = delete;
    Store& operator=(const Store&) = delete;
    ~Store();

    // Results are announced to `notify` with `message`; call Take then.
    void Start(HWND notify, UINT message, const Services& services, std::wstring tagCacheFile);
    void Stop();

    // By the file's path, case folded. Null while it hasn't been read.
    [[nodiscard]] const Tags* TagsOf(const std::wstring& foldedFile) const;
    // Asks for the files of these that haven't been asked for.
    void WantTags(const std::vector<std::wstring>& files);
    void ReadTagsAgain(const std::wstring& file);

    [[nodiscard]] Art* ArtOf(const std::wstring& key);
    // Replaces what is waiting.
    void WantArt(std::vector<ArtRequest> requests);

    struct Taken {
        std::vector<std::wstring> taggedFiles; // case folded, whose tags came in or changed
        bool art = false;
    };
    [[nodiscard]] Taken Take();

    [[nodiscard]] std::size_t TagsWaiting() const { return loader_ ? loader_->TagsWaiting() : 0; }
    [[nodiscard]] std::size_t ArtWaiting() const { return loader_ ? loader_->ArtWaiting() : 0; }

private:
    void StoreArt(ArtResult&& result);
    void EvictArt();

    std::unique_ptr<Loader> loader_;
    std::unordered_map<std::wstring, Tags> tags_;
    std::unordered_set<std::wstring> asked_;
    std::unordered_map<std::wstring, Art> art_;
};

} // namespace covers
