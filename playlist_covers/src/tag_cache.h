// The tags read in earlier sessions, so that a long playlist is grouped at once the
// next time rather than after every file has been read again. An entry holds for as
// long as its file keeps the size and the time it was last written.
#pragma once

#include "covers_model.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>

namespace covers {

struct CachedTags {
    std::uint64_t written = 0; // the file's last write time
    std::uint64_t size = 0;
    Tags tags;
    bool used = false; // looked up this session (not stored)
};

// By the file's path, case folded.
using TagCache = std::unordered_map<std::wstring, CachedTags>;

// UTF-8 text, a line per file: path, write time, size, then the four tags, between tabs.
[[nodiscard]] std::string SerializeTagCache(const TagCache& cache, std::size_t limit);
[[nodiscard]] TagCache ParseTagCache(std::string_view text);

} // namespace covers
