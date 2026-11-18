#include "vis_latency.h"

#include <vis.h>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <cwchar>
#include <optional>
#include <utility>

namespace exodium {

namespace {

// The most latencyMs believed: more is no plug-in's, but memory that happens to look like one.
constexpr int kMostLatencyMs = 1000;
// No DLL's writable sections are this big; one that says so is no image of Winamp's.
constexpr std::size_t kMostSectionBytes = 64 * 1024 * 1024;
// How many times a plug-in's memory is looked through for its module before giving up on
// it until it is loaded again: Winamp fills in the module just after loading it, so the
// first look all but always finds it. MilkDrop 2's takes some 2 ms: it has 4 MB to go through.
constexpr int kMostSearches = 8;
// What a plug-in loaded but whose module isn't found is taken to ask for: some make theirs
// as they start, outside the DLL (the classic one, which asks for 10, does). More than any
// plug-in seen asks for, so such a plug-in is shown sound a little older than it could be,
// never none.
constexpr int kUnfoundLatencyMs = 25;

// A module's fields up to its sound data, which are all that is read of one.
constexpr std::size_t kHead = offsetof(winampVisModule, spectrumData);

// Copies `size` bytes from `address` in this process into `into`, or says it couldn't.
bool CopyOf(const void* address, void* into, std::size_t size) noexcept {
    SIZE_T copied = 0;
    return ::ReadProcessMemory(::GetCurrentProcess(), address, into, size, &copied) && copied == size;
}

template <typename T>
[[nodiscard]] T Field(const std::byte* head, std::size_t offset) noexcept {
    T value;
    std::memcpy(&value, head + offset, sizeof value);
    return value;
}

// The latencyMs of the module whose first kHead bytes are `head`, if Winamp has set it up
// in the plug-in `module` (and so it is a module at all).
[[nodiscard]] std::optional<int> LatencyOf(const std::byte* head, HMODULE module, HWND winamp) noexcept {
    if (Field<HWND>(head, offsetof(winampVisModule, hwndParent)) != winamp ||
        Field<HINSTANCE>(head, offsetof(winampVisModule, hDllInstance)) != module) {
        return std::nullopt;
    }
    const int latencyMs = Field<int>(head, offsetof(winampVisModule, latencyMs));
    const int spectrum = Field<int>(head, offsetof(winampVisModule, spectrumNch));
    const int waveform = Field<int>(head, offsetof(winampVisModule, waveformNch));
    if (latencyMs < 0 || latencyMs > kMostLatencyMs || spectrum < 0 || spectrum > 2 || waveform < 0 || waveform > 2) {
        return std::nullopt;
    }
    return latencyMs;
}

// Where in the plug-in `module` the module Winamp has set up is, or null where there is none.
const std::byte* Search(HMODULE module, HWND winamp) {
    const auto* base = reinterpret_cast<const std::byte*>(module);
    IMAGE_DOS_HEADER dos{};
    IMAGE_NT_HEADERS nt{};
    if (!CopyOf(base, &dos, sizeof dos) || dos.e_magic != IMAGE_DOS_SIGNATURE ||
        !CopyOf(base + dos.e_lfanew, &nt, sizeof nt) || nt.Signature != IMAGE_NT_SIGNATURE) {
        return nullptr;
    }
    std::vector<IMAGE_SECTION_HEADER> sections(nt.FileHeader.NumberOfSections);
    const std::byte* sectionsAt =
        base + dos.e_lfanew + offsetof(IMAGE_NT_HEADERS, OptionalHeader) + nt.FileHeader.SizeOfOptionalHeader;
    if (sections.empty() || !CopyOf(sectionsAt, sections.data(), sections.size() * sizeof sections[0])) return nullptr;

    std::vector<std::byte> bytes;
    for (const IMAGE_SECTION_HEADER& section : sections) {
        const std::size_t size = section.Misc.VirtualSize;
        if (!(section.Characteristics & IMAGE_SCN_MEM_WRITE) || size < kHead || size > kMostSectionBytes) continue;
        bytes.resize(size);
        if (!CopyOf(base + section.VirtualAddress, bytes.data(), size)) continue;
        // A module has pointers in it, so it starts where a pointer may.
        for (std::size_t from = 0; from + kHead <= size; from += alignof(winampVisModule)) {
            if (LatencyOf(bytes.data() + from, module, winamp)) return base + section.VirtualAddress + from;
        }
    }
    return nullptr;
}

} // namespace

void VisPlugins::Find(const wchar_t* folder) {
    std::vector<Plugin> found;
    if (folder && *folder) {
        std::wstring pattern = folder;
        if (pattern.back() != L'\\') pattern += L'\\';
        pattern += L"vis_*.dll";
        WIN32_FIND_DATAW file{};
        const HANDLE search = ::FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &file, FindExSearchNameMatch, nullptr, 0);
        if (search != INVALID_HANDLE_VALUE) {
            do {
                if (file.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                // By name alone, which finds it loaded however its path was spelled. What was
                // found of one already is kept: where its module is, while it stays loaded.
                const auto known = std::ranges::find_if(plugins_, [&](const Plugin& p) { return ::_wcsicmp(p.file.c_str(), file.cFileName) == 0; });
                found.push_back(known != plugins_.end() ? std::move(*known) : Plugin{.file = file.cFileName});
            } while (::FindNextFileW(search, &file));
            ::FindClose(search);
        }
    }
    plugins_ = std::move(found);
}

int VisPlugins::RunningLatencyMs(HWND winamp) noexcept {
    if (!winamp) return 0;
    int latency = 0;
    for (Plugin& plugin : plugins_) {
        // Loaded only while it runs. The handle isn't held, which is why it is only read by copying.
        const HMODULE module = ::GetModuleHandleW(plugin.file.c_str());
        if (module != plugin.module) { // loaded, unloaded or loaded again since
            plugin.module = module;
            plugin.at = nullptr;
            plugin.searches = 0;
        }
        if (!module) continue;
        std::byte head[kHead];
        if (plugin.at && CopyOf(plugin.at, head, kHead)) {
            if (const auto latencyMs = LatencyOf(head, module, winamp)) {
                latency = std::max(latency, *latencyMs);
                continue;
            }
        }
        plugin.at = nullptr;
        if (plugin.searches < kMostSearches) {
            ++plugin.searches;
            try {
                plugin.at = Search(module, winamp);
            } catch (...) {
                // out of memory: as if it wasn't found
            }
        }
        const auto latencyMs = plugin.at && CopyOf(plugin.at, head, kHead) ? LatencyOf(head, module, winamp) : std::nullopt;
        if (!latencyMs) plugin.at = nullptr;
        latency = std::max(latency, latencyMs.value_or(kUnfoundLatencyMs));
    }
    return latency;
}

} // namespace exodium
