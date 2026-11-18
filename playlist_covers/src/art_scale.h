// Covers come in every size; the grid wants squares of one.
#pragma once

#include <cstdint>
#include <vector>

namespace covers {

// Cuts the middle square out of a picture (a wide video thumbnail loses its sides) and
// scales it to size x size, each new pixel the average of the ones it covers. Pixels
// are premultiplied ARGB, top row first, as Winamp's album art service gives them.
// Empty if the picture or the size is.
[[nodiscard]] std::vector<std::uint32_t> ScaleToSquare(const std::uint32_t* pixels, int width, int height,
                                                       int size);

} // namespace covers
