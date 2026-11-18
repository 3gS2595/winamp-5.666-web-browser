#include "art_scale.h"

#include <algorithm>
#include <cmath>

namespace covers {

namespace {

// How much of each source pixel goes into one new pixel along a line.
struct Weight {
    int source;
    float amount;
};

// For each of `to` new pixels, the source pixels in [offset, offset + from) it covers
// and how much of each. A new pixel narrower than a source pixel (scaling up) takes
// the one or two it lies over.
std::vector<std::vector<Weight>> Weights(int from, int to, int offset) {
    std::vector<std::vector<Weight>> weights(static_cast<size_t>(to));
    const double scale = static_cast<double>(from) / to;
    for (int i = 0; i < to; ++i) {
        double start = i * scale, end = (i + 1) * scale;
        if (end - start < 1.0) {
            // Scaling up: a pixel's worth round the middle of the span.
            const double middle = (start + end) / 2;
            start = std::clamp(middle - 0.5, 0.0, from - 1.0);
            end = start + 1.0;
        }
        auto& list = weights[static_cast<size_t>(i)];
        float total = 0;
        for (int s = static_cast<int>(std::floor(start)); s < from && s < end; ++s) {
            const float amount = static_cast<float>(std::min<double>(end, s + 1) - std::max<double>(start, s));
            if (amount <= 0) continue;
            list.push_back({offset + s, amount});
            total += amount;
        }
        for (Weight& weight : list) weight.amount /= total;
    }
    return weights;
}

struct Pixel {
    float a = 0, r = 0, g = 0, b = 0;
};

[[nodiscard]] Pixel Unpack(std::uint32_t argb) noexcept {
    return {static_cast<float>(argb >> 24), static_cast<float>((argb >> 16) & 0xFF),
            static_cast<float>((argb >> 8) & 0xFF), static_cast<float>(argb & 0xFF)};
}

[[nodiscard]] std::uint32_t Pack(const Pixel& p) noexcept {
    const auto channel = [](float value) {
        return static_cast<std::uint32_t>(std::clamp(std::lround(value), 0L, 255L));
    };
    const std::uint32_t a = channel(p.a);
    // Premultiplied: no colour can be brighter than its alpha.
    return a << 24 | std::min(channel(p.r), a) << 16 | std::min(channel(p.g), a) << 8 | std::min(channel(p.b), a);
}

} // namespace

std::vector<std::uint32_t> ScaleToSquare(const std::uint32_t* pixels, int width, int height, int size) {
    if (!pixels || width <= 0 || height <= 0 || size <= 0) return {};
    const int side = std::min(width, height);
    const auto across = Weights(side, size, (width - side) / 2);
    const auto down = Weights(side, size, (height - side) / 2);

    // Across first, for just the rows the square takes.
    const int firstRow = (height - side) / 2;
    std::vector<Pixel> rows(static_cast<size_t>(side) * size);
    for (int y = 0; y < side; ++y) {
        const std::uint32_t* source = pixels + static_cast<size_t>(firstRow + y) * width;
        Pixel* row = rows.data() + static_cast<size_t>(y) * size;
        for (int x = 0; x < size; ++x) {
            Pixel sum;
            for (const Weight& weight : across[static_cast<size_t>(x)]) {
                const Pixel p = Unpack(source[weight.source]);
                sum.a += p.a * weight.amount;
                sum.r += p.r * weight.amount;
                sum.g += p.g * weight.amount;
                sum.b += p.b * weight.amount;
            }
            row[x] = sum;
        }
    }

    std::vector<std::uint32_t> square(static_cast<size_t>(size) * size);
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            Pixel sum;
            for (const Weight& weight : down[static_cast<size_t>(y)]) {
                const Pixel& p = rows[static_cast<size_t>(weight.source - firstRow) * size + x];
                sum.a += p.a * weight.amount;
                sum.r += p.r * weight.amount;
                sum.g += p.g * weight.amount;
                sum.b += p.b * weight.amount;
            }
            square[static_cast<size_t>(y) * size + x] = Pack(sum);
        }
    }
    return square;
}

} // namespace covers
