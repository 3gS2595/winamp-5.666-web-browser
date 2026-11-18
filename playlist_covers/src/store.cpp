#include "store.h"

#include <algorithm>
#include <utility>

namespace covers {

namespace {

constexpr std::size_t kArtBudgetBytes = std::size_t{40} << 20;

// So that the code of a thread that is still running stays loaded after Winamp lets the
// plug-in go.
void PinModule() {
    HMODULE module = nullptr;
    ::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                         reinterpret_cast<LPCWSTR>(&PinModule), &module);
}

} // namespace

Store::~Store() {
    Stop();
}

void Store::Start(HWND notify, UINT message, const Services& services, std::wstring tagCacheFile) {
    if (loader_) return;
    loader_ = std::make_unique<Loader>();
    loader_->Start(notify, message, services, std::move(tagCacheFile));
}

void Store::Stop() {
    if (loader_ && !loader_->Stop()) {
        // A thread is still in an input plug-in and still uses the loader: it is left to
        // it, and the plug-in's code stays loaded for it.
        (void)loader_.release();
        PinModule();
    }
    loader_.reset();
    for (auto& [key, art] : art_) {
        if (art.bitmap) ::DeleteObject(art.bitmap);
    }
    art_.clear();
}

const Tags* Store::TagsOf(const std::wstring& foldedFile) const {
    const auto found = tags_.find(foldedFile);
    return found == tags_.end() ? nullptr : &found->second;
}

void Store::WantTags(const std::vector<std::wstring>& files) {
    std::vector<std::wstring> wanted;
    for (const std::wstring& file : files) {
        std::wstring key = FoldCase(file);
        if (tags_.contains(key) || !asked_.insert(std::move(key)).second) continue;
        wanted.push_back(file);
    }
    if (loader_) loader_->WantTags(wanted);
}

void Store::ReadTagsAgain(const std::wstring& file) {
    if (loader_) loader_->WantTags({file}, true);
}

Art* Store::ArtOf(const std::wstring& key) {
    const auto found = art_.find(key);
    return found == art_.end() ? nullptr : &found->second;
}

void Store::WantArt(std::vector<ArtRequest> requests) {
    if (loader_) loader_->WantArt(std::move(requests));
}

Store::Taken Store::Take() {
    Taken taken;
    if (!loader_) return taken;
    std::vector<TagResult> tags;
    std::vector<ArtResult> art;
    loader_->Take(tags, art);
    for (TagResult& result : tags) {
        std::wstring key = FoldCase(result.file);
        const auto found = tags_.find(key);
        if (found != tags_.end() && found->second == result.tags) continue;
        taken.taggedFiles.push_back(key);
        tags_.insert_or_assign(std::move(key), std::move(result.tags));
    }
    for (ArtResult& result : art) StoreArt(std::move(result));
    taken.art = !art.empty();
    if (taken.art) EvictArt();
    return taken;
}

void Store::StoreArt(ArtResult&& result) {
    Art& art = art_[result.key];
    if (art.bitmap) ::DeleteObject(art.bitmap);
    art = {};
    art.used = ::GetTickCount64();
    if (result.pixels.empty()) {
        art.missing = true;
        return;
    }
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = result.size;
    info.bmiHeader.biHeight = -result.size; // top-down, as the pixels are
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    const HBITMAP bitmap = ::CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bitmap || !bits) {
        if (bitmap) ::DeleteObject(bitmap);
        art_.erase(result.key);
        return;
    }
    std::copy(result.pixels.begin(), result.pixels.end(), static_cast<std::uint32_t*>(bits));
    art.bitmap = bitmap;
    art.size = result.size;
}

// Keeps the covers within a budget, letting go of the ones longest out of sight.
void Store::EvictArt() {
    std::size_t bytes = 0;
    std::vector<std::pair<ULONGLONG, std::wstring>> byAge;
    for (const auto& [key, art] : art_) {
        if (!art.bitmap) continue;
        bytes += static_cast<std::size_t>(art.size) * art.size * 4;
        byAge.emplace_back(art.used, key);
    }
    if (bytes <= kArtBudgetBytes) return;
    std::ranges::sort(byAge);
    for (const auto& [used, key] : byAge) {
        if (bytes <= kArtBudgetBytes * 3 / 4) break;
        const auto found = art_.find(key);
        bytes -= static_cast<std::size_t>(found->second.size) * found->second.size * 4;
        ::DeleteObject(found->second.bitmap);
        art_.erase(found);
    }
}

} // namespace covers
