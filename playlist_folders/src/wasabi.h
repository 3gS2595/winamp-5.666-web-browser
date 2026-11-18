// The one Wasabi service the plugin uses, api_playlists, declared as Winamp's SDK declares
// it (Src/playlist/api_playlists.h). A Wasabi interface is an object with a single virtual
// function, _dispatch, given a method's number and pointers to its arguments.
#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>

namespace folders::wasabi {

class Dispatchable {
public:
    virtual int _dispatch(int msg, void* retval, void** params = nullptr, int nparam = 0) = 0;

protected:
    ~Dispatchable() = default;

    template <typename Result, typename... Params>
    Result Call(int msg, Result fallback, Params... params) {
        void* list[sizeof...(Params) + 1] = {static_cast<void*>(&params)...};
        Result result{};
        if (_dispatch(msg, &result, list, static_cast<int>(sizeof...(Params)))) return result;
        return fallback;
    }

    template <typename... Params>
    void VoidCall(int msg, Params... params) {
        void* list[sizeof...(Params) + 1] = {static_cast<void*>(&params)...};
        _dispatch(msg, nullptr, list, static_cast<int>(sizeof...(Params)));
    }
};

class ServiceFactory : public Dispatchable {
public:
    [[nodiscard]] void* GetInterface(int globalLock = TRUE) { return Call<void*>(300, nullptr, globalLock); }
    int ReleaseInterface(void* object) { return Call<int>(310, 0, object); }
};

class ServiceManager : public Dispatchable {
public:
    [[nodiscard]] ServiceFactory* ByGuid(GUID guid) { return Call<ServiceFactory*>(50, nullptr, guid); }
};

// api_playlists (System\playlist.w5s): the media library's saved playlists, in their order.
// Indices and the strings handed out hold only between Lock and Unlock.
class Playlists : public Dispatchable {
public:
    void Lock() { VoidCall(10); }
    void Unlock() { VoidCall(20); }
    [[nodiscard]] std::size_t Count() { return Call<std::size_t>(50, 0); }
    [[nodiscard]] const wchar_t* Filename(std::size_t index) { return Call<const wchar_t*>(60, nullptr, index); }
    [[nodiscard]] const wchar_t* Name(std::size_t index) { return Call<const wchar_t*>(70, nullptr, index); }
    // 0 on success. The library keeps item counts and lengths (seconds) as size_t.
    int GetInfo(std::size_t index, GUID info, void* data, std::size_t length) {
        return Call<int>(100, 1, index, info, data, length);
    }
    // Both tell whoever listens (ml_playlists), which then changes its tree item. Only the
    // test commands use them.
    int RenamePlaylist(std::size_t index, const wchar_t* name) { return Call<int>(140, 1, index, name); }
    int RemovePlaylist(std::size_t index) { return Call<int>(170, 1, index); }
};

inline constexpr GUID kPlaylistsGuid = {0x2dc3c390, 0xd9b8, 0x4a49, {0xb2, 0x30, 0xef, 0x24, 0x0a, 0xdd, 0xdc, 0xdb}};
inline constexpr GUID kItemCountInfo = {0xc4fad6ce, 0xda38, 0x47b0, {0xaa, 0xa9, 0xe9, 0x66, 0xd8, 0xe8, 0xe7, 0xc5}};
inline constexpr GUID kTotalTimeInfo = {0xd4e0e000, 0xa3f5, 0x4f18, {0xad, 0xa5, 0xf2, 0xba, 0x40, 0x68, 0x95, 0x93}};

// Lock()s for as long as it lives.
class PlaylistsLock {
public:
    explicit PlaylistsLock(Playlists* playlists) : playlists_(playlists) {
        if (playlists_) playlists_->Lock();
    }
    ~PlaylistsLock() {
        if (playlists_) playlists_->Unlock();
    }
    PlaylistsLock(const PlaylistsLock&) = delete;
    PlaylistsLock& operator=(const PlaylistsLock&) = delete;

private:
    Playlists* playlists_;
};

} // namespace folders::wasabi
