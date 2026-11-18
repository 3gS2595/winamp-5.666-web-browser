#include "loader.h"

#include "art_scale.h"

#include <objbase.h>

#include <iterator>
#include <utility>

namespace covers {

namespace {

constexpr DWORD kStopWaitMs = 4000;
constexpr std::size_t kCacheLimit = 50000;
constexpr std::size_t kTagLength = 1024;
constexpr int kLargestPicture = 16384;

// Streams aren't read: their tags and covers would come over the network.
[[nodiscard]] bool IsLocalFile(const std::wstring& file) {
    return !file.empty() && file.find(L"://") == std::wstring::npos;
}

[[nodiscard]] bool FileStamp(const std::wstring& file, std::uint64_t& written, std::uint64_t& size) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!::GetFileAttributesExW(file.c_str(), GetFileExInfoStandard, &data)) return false;
    written = static_cast<std::uint64_t>(data.ftLastWriteTime.dwHighDateTime) << 32 | data.ftLastWriteTime.dwLowDateTime;
    size = static_cast<std::uint64_t>(data.nFileSizeHigh) << 32 | data.nFileSizeLow;
    return true;
}

// Waits for a thread while still answering what other threads send this one: an input
// plug-in the thread is in could be sending Winamp's window a message.
[[nodiscard]] bool WaitAnswering(HANDLE thread, DWORD timeoutMs) {
    const ULONGLONG deadline = ::GetTickCount64() + timeoutMs;
    while (true) {
        const ULONGLONG now = ::GetTickCount64();
        if (now >= deadline) return ::WaitForSingleObject(thread, 0) == WAIT_OBJECT_0;
        const DWORD result =
            ::MsgWaitForMultipleObjects(1, &thread, FALSE, static_cast<DWORD>(deadline - now), QS_SENDMESSAGE);
        if (result == WAIT_OBJECT_0) return true;
        if (result != WAIT_OBJECT_0 + 1) return false;
        MSG message;
        ::PeekMessageW(&message, nullptr, 0, 0, PM_NOREMOVE | PM_QS_SENDMESSAGE);
    }
}

[[nodiscard]] std::string ReadWholeFile(const std::wstring& path) {
    std::string text;
    const HANDLE file = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                      FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return text;
    LARGE_INTEGER size{};
    if (::GetFileSizeEx(file, &size) && size.QuadPart > 0 && size.QuadPart < 256LL * 1024 * 1024) {
        text.resize(static_cast<size_t>(size.QuadPart));
        DWORD read = 0;
        if (!::ReadFile(file, text.data(), static_cast<DWORD>(text.size()), &read, nullptr)) read = 0;
        text.resize(read);
    }
    ::CloseHandle(file);
    return text;
}

// Written beside it first, so that a crash halfway leaves the old one.
void WriteWholeFile(const std::wstring& path, const std::string& text) {
    const std::wstring temporary = path + L".tmp";
    const HANDLE file =
        ::CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    const bool ok = ::WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) &&
                    written == text.size();
    ::CloseHandle(file);
    if (!ok || !::MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        ::DeleteFileW(temporary.c_str());
    }
}

} // namespace

void Loader::Start(HWND notify, UINT message, const Services& services, std::wstring tagCacheFile) {
    if (tagThread_ || artThread_) return;
    notify_ = notify;
    message_ = message;
    services_ = services;
    cacheFile_ = std::move(tagCacheFile);
    stopping_ = false;
    tagThread_ = ::CreateThread(nullptr, 0, TagThread, this, 0, nullptr);
    artThread_ = ::CreateThread(nullptr, 0, ArtThread, this, 0, nullptr);
}

bool Loader::Stop() {
    {
        std::lock_guard lock(mutex_);
        if (!tagThread_ && !artThread_) return true;
        stopping_ = true;
        tagQueue_.clear();
        artQueue_.clear();
    }
    tagWake_.notify_all();
    artWake_.notify_all();
    bool stopped = true;
    for (HANDLE* thread : {&tagThread_, &artThread_}) {
        if (!*thread) continue;
        if (WaitAnswering(*thread, kStopWaitMs)) ::CloseHandle(*thread);
        else stopped = false; // still in a plug-in; it is left to finish
        *thread = nullptr;
    }
    return stopped;
}

void Loader::WantTags(const std::vector<std::wstring>& files, bool fresh) {
    if (files.empty()) return;
    {
        std::lock_guard lock(mutex_);
        if (stopping_) return;
        for (const std::wstring& file : files) tagQueue_.emplace_back(file, fresh);
    }
    tagWake_.notify_one();
}

void Loader::WantArt(std::vector<ArtRequest> requests) {
    {
        std::lock_guard lock(mutex_);
        if (stopping_) return;
        artQueue_.assign(std::make_move_iterator(requests.begin()), std::make_move_iterator(requests.end()));
    }
    artWake_.notify_one();
}

void Loader::Take(std::vector<TagResult>& tags, std::vector<ArtResult>& art) {
    std::lock_guard lock(mutex_);
    tags = std::move(tagResults_);
    art = std::move(artResults_);
    tagResults_.clear();
    artResults_.clear();
    notified_ = false;
}

std::size_t Loader::TagsWaiting() const {
    std::lock_guard lock(mutex_);
    return tagQueue_.size() + tagsBusy_;
}

std::size_t Loader::ArtWaiting() const {
    std::lock_guard lock(mutex_);
    return artQueue_.size() + artBusy_;
}

void Loader::Notify() {
    if (notified_ || !notify_) return;
    notified_ = ::PostMessageW(notify_, message_, 0, 0) != FALSE;
}

DWORD WINAPI Loader::TagThread(void* self) {
    static_cast<Loader*>(self)->RunTags();
    return 0;
}

DWORD WINAPI Loader::ArtThread(void* self) {
    static_cast<Loader*>(self)->RunArt();
    return 0;
}

void Loader::RunTags() {
    // For input plug-ins that read tags through COM (Windows Media's).
    const HRESULT com = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    LoadCache();
    std::unique_lock lock(mutex_);
    while (true) {
        tagWake_.wait(lock, [this] { return stopping_ || !tagQueue_.empty(); });
        if (stopping_) break;
        auto [file, fresh] = std::move(tagQueue_.front());
        tagQueue_.pop_front();
        ++tagsBusy_;
        lock.unlock();

        Tags tags;
        try {
            tags = CachedOrRead(file, fresh);
        } catch (...) {
            tags = {};
        }

        lock.lock();
        --tagsBusy_;
        tagResults_.push_back({std::move(file), std::move(tags)});
        Notify();
    }
    lock.unlock();
    SaveCache();
    if (SUCCEEDED(com)) ::CoUninitialize();
}

void Loader::RunArt() {
    const HRESULT com = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    std::unique_lock lock(mutex_);
    while (true) {
        artWake_.wait(lock, [this] { return stopping_ || !artQueue_.empty(); });
        if (stopping_) break;
        ArtRequest request = std::move(artQueue_.front());
        artQueue_.pop_front();
        ++artBusy_;
        lock.unlock();

        std::vector<std::uint32_t> pixels;
        try {
            pixels = ReadArt(request);
        } catch (...) {
            pixels.clear();
        }

        lock.lock();
        --artBusy_;
        artResults_.push_back({std::move(request.key), request.size, std::move(pixels)});
        Notify();
    }
    lock.unlock();
    if (SUCCEEDED(com)) ::CoUninitialize();
}

Tags Loader::CachedOrRead(const std::wstring& file, bool fresh) {
    std::uint64_t written = 0, size = 0;
    if (!IsLocalFile(file) || !FileStamp(file, written, size)) return {};
    const std::wstring key = FoldCase(file);
    if (const auto found = cache_.find(key);
        !fresh && found != cache_.end() && found->second.written == written && found->second.size == size) {
        found->second.used = true;
        return found->second.tags;
    }
    Tags tags = ReadTags(file);
    cache_.insert_or_assign(key, CachedTags{written, size, tags, true});
    cacheChanged_ = true;
    return tags;
}

Tags Loader::ReadTags(const std::wstring& file) {
    Tags tags;
    if (!services_.metadata) return tags;
    wchar_t buffer[kTagLength];
    const auto read = [&](const wchar_t* field) -> std::wstring {
        buffer[0] = L'\0';
        if (!services_.metadata->GetExtendedFileInfo(file.c_str(), field, buffer, kTagLength)) return {};
        buffer[kTagLength - 1] = L'\0';
        return CleanTag(buffer);
    };
    tags.title = read(L"title");
    tags.artist = read(L"artist");
    tags.album = read(L"album");
    tags.albumArtist = read(L"albumartist");
    return tags;
}

std::vector<std::uint32_t> Loader::ReadArt(const ArtRequest& request) {
    if (!services_.albumArt) return {};
    for (const std::wstring& file : request.files) {
        if (!IsLocalFile(file)) continue;
        int width = 0, height = 0;
        wasabi::ARGB32* bits = nullptr;
        if (!services_.albumArt->Get(file.c_str(), L"cover", &width, &height, &bits)) continue;
        std::vector<std::uint32_t> pixels;
        if (bits && width > 0 && height > 0 && width <= kLargestPicture && height <= kLargestPicture) {
            try {
                pixels = ScaleToSquare(bits, width, height, request.size);
            } catch (...) {
                pixels.clear();
            }
        }
        if (bits && services_.memory) services_.memory->Free(bits);
        if (!pixels.empty()) return pixels;
    }
    return {};
}

void Loader::LoadCache() {
    if (cacheFile_.empty()) return;
    try {
        cache_ = ParseTagCache(ReadWholeFile(cacheFile_));
    } catch (...) {
        cache_.clear();
    }
}

void Loader::SaveCache() {
    if (cacheFile_.empty() || !cacheChanged_) return;
    try {
        WriteWholeFile(cacheFile_, SerializeTagCache(cache_, kCacheLimit));
        cacheChanged_ = false;
    } catch (...) {
    }
}

} // namespace covers
