// Reads tags and covers on threads of its own, so that Winamp's window never waits for
// a file. Results pile up until the window takes them; it is told with one posted
// message whenever there are some and it hasn't been told yet.
#pragma once

#include "covers_model.h"
#include "tag_cache.h"
#include "wasabi.h"

#include <windows.h>

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace covers {

struct ArtRequest {
    std::wstring key;                // the cover's group
    std::vector<std::wstring> files; // tried in turn until one has a picture
    int size = 0;
};

struct TagResult {
    std::wstring file;
    Tags tags;
};

struct ArtResult {
    std::wstring key;
    int size = 0;
    std::vector<std::uint32_t> pixels; // size x size; empty if none of the files had a picture
};

struct Services {
    wasabi::Metadata* metadata = nullptr;
    wasabi::AlbumArt* albumArt = nullptr;
    wasabi::MemoryManager* memory = nullptr;
};

class Loader {
public:
    Loader() = default;
    Loader(const Loader&) = delete;
    Loader& operator=(const Loader&) = delete;
    ~Loader() { (void)Stop(); }

    void Start(HWND notify, UINT message, const Services& services, std::wstring tagCacheFile);
    // Waits for the threads, answering what they send Winamp's window meanwhile; the tag
    // thread saves the cache on its way out. Gives up on a thread after a few seconds,
    // and says so: one still running still uses this object, which must then be left.
    [[nodiscard]] bool Stop();

    // Added to what is waiting. `fresh` reads the files again even if the cache has them.
    void WantTags(const std::vector<std::wstring>& files, bool fresh = false);
    // Replaces what is waiting: the covers in sight, then the ones near them.
    void WantArt(std::vector<ArtRequest> requests);

    void Take(std::vector<TagResult>& tags, std::vector<ArtResult>& art);
    [[nodiscard]] std::size_t TagsWaiting() const;
    [[nodiscard]] std::size_t ArtWaiting() const;

private:
    static DWORD WINAPI TagThread(void* self);
    static DWORD WINAPI ArtThread(void* self);
    void RunTags();
    void RunArt();
    [[nodiscard]] Tags CachedOrRead(const std::wstring& file, bool fresh);
    [[nodiscard]] Tags ReadTags(const std::wstring& file);
    [[nodiscard]] std::vector<std::uint32_t> ReadArt(const ArtRequest& request);
    void LoadCache();
    void SaveCache();
    void Notify(); // with the lock held

    mutable std::mutex mutex_;
    std::condition_variable tagWake_, artWake_;
    std::deque<std::pair<std::wstring, bool>> tagQueue_;
    std::deque<ArtRequest> artQueue_;
    std::vector<TagResult> tagResults_;
    std::vector<ArtResult> artResults_;
    std::size_t tagsBusy_ = 0; // taken from the queue, not yet answered
    std::size_t artBusy_ = 0;
    bool stopping_ = false;
    bool notified_ = false;

    HANDLE tagThread_ = nullptr, artThread_ = nullptr;
    HWND notify_ = nullptr;
    UINT message_ = 0;
    Services services_;
    std::wstring cacheFile_;
    TagCache cache_; // the tag thread's alone
    bool cacheChanged_ = false;
};

} // namespace covers
