// The few Wasabi services the plugin uses, declared as Winamp's SDK declares them.
// A Wasabi interface is an object with a single virtual function, _dispatch, which is
// given a method's number and pointers to its arguments. The numbers and GUIDs are
// Winamp's own (Src/Agave and Src/Wasabi in its source); they have been the same since
// Winamp 5.5.
#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>

namespace covers::wasabi {

using ARGB32 = std::uint32_t;

class Dispatchable {
public:
    virtual int _dispatch(int msg, void* retval, void** params = nullptr, int nparam = 0) = 0;

protected:
    ~Dispatchable() = default;

    // Winamp's _call: the method fills `result` and says it did by returning nonzero.
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

// waServiceFactory: hands out the service's object.
class ServiceFactory : public Dispatchable {
public:
    [[nodiscard]] void* GetInterface(int globalLock = TRUE) { return Call<void*>(300, nullptr, globalLock); }
    int ReleaseInterface(void* object) { return Call<int>(310, 0, object); }
};

// api_service, which IPC_GET_API_SERVICE gives.
class ServiceManager : public Dispatchable {
public:
    [[nodiscard]] ServiceFactory* ByGuid(GUID guid) { return Call<ServiceFactory*>(50, nullptr, guid); }
};

// api_albumart (System\albumart.w5s): a file's cover, embedded or beside it in its
// folder, decoded. Winamp's media library calls it from a thread of its own.
class AlbumArt : public Dispatchable {
public:
    // On success the pixels are top-down, premultiplied ARGB, w * h of them, to be given
    // back with MemoryManager::Free.
    [[nodiscard]] bool Get(const wchar_t* file, const wchar_t* type, int* width, int* height, ARGB32** bits) {
        constexpr int kSuccess = 0, kFailure = 1;
        return Call<int>(10, kFailure, file, type, width, height, bits) == kSuccess;
    }
};

// api_metadata: what IPC_GET_EXTENDED_FILE_INFOW gives, callable from any thread
// (Winamp takes a lock of its own round the input plug-in's reading).
class Metadata : public Dispatchable {
public:
    int GetExtendedFileInfo(const wchar_t* file, const wchar_t* tag, wchar_t* data, std::size_t length) {
        return Call<int>(10, 0, file, tag, data, length);
    }
};

// api_memmgr.
class MemoryManager : public Dispatchable {
public:
    void Free(void* memory) { VoidCall(10, memory); }
};

// api_playlists (System\playlist.w5s): the media library's saved playlists. Indices and
// the strings handed out hold only between Lock and Unlock.
class Playlists : public Dispatchable {
public:
    void Lock() { VoidCall(10); }
    void Unlock() { VoidCall(20); }
    [[nodiscard]] std::size_t Count() { return Call<std::size_t>(50, 0); }
    [[nodiscard]] const wchar_t* Filename(std::size_t index) { return Call<const wchar_t*>(60, nullptr, index); }
    [[nodiscard]] const wchar_t* Name(std::size_t index) { return Call<const wchar_t*>(70, nullptr, index); }
};

inline constexpr GUID kAlbumArtGuid = {0xac4c4468, 0xf91f, 0x41f3, {0xa5, 0xfa, 0xe2, 0xb8, 0x1d, 0xc6, 0xeb, 0x3a}};
inline constexpr GUID kMetadataGuid = {0xdfa89f63, 0x995a, 0x407b, {0x8b, 0xc8, 0x82, 0x79, 0x00, 0x44, 0x07, 0x27}};
inline constexpr GUID kMemoryManagerGuid = {
    0x000cf46e, 0x4df6, 0x4a43, {0xbb, 0xe7, 0x40, 0xe7, 0xa3, 0xea, 0x02, 0xed}};
inline constexpr GUID kPlaylistsGuid = {0x2dc3c390, 0xd9b8, 0x4a49, {0xb2, 0x30, 0xef, 0x24, 0x0a, 0xdd, 0xdc, 0xdb}};

} // namespace covers::wasabi
