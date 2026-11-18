#include "glow_shape.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <latch>
#include <thread>

namespace exodium {

namespace {

[[nodiscard]] constexpr int WidthOf(const RECT& rect) noexcept {
    return rect.right - rect.left;
}
[[nodiscard]] constexpr int HeightOf(const RECT& rect) noexcept {
    return rect.bottom - rect.top;
}
[[nodiscard]] constexpr bool IsEmpty(const RECT& rect) noexcept {
    return rect.right <= rect.left || rect.bottom <= rect.top;
}
[[nodiscard]] constexpr RECT Grown(const RECT& rect, int by) noexcept {
    return {rect.left - by, rect.top - by, rect.right + by, rect.bottom + by};
}
// Empty when they don't meet.
[[nodiscard]] constexpr RECT Common(const RECT& a, const RECT& b) noexcept {
    const RECT common{std::max(a.left, b.left), std::max(a.top, b.top), std::min(a.right, b.right),
                      std::min(a.bottom, b.bottom)};
    return IsEmpty(common) ? RECT{} : common;
}
[[nodiscard]] constexpr bool Meet(const RECT& a, const RECT& b) noexcept {
    return !IsEmpty(Common(a, b));
}

// A band of InBands' work, done on a thread of Windows' thread pool.
struct Band {
    const std::function<void(int, int)>* work;
    int from;
    int to;
    std::latch* done;
};
void CALLBACK DoBand(PTP_CALLBACK_INSTANCE, void* context) {
    const Band& band = *static_cast<const Band*>(context);
    (*band.work)(band.from, band.to);
    band.done->count_down();
}

// Runs work(from, to) over 0 to `count` cut into bands, side by side on as many of the
// computer's cores as are worth it: the rows of a picture don't depend on each other. The
// bands but the first go to Windows' thread pool, whose threads are there already.
void InBands(int count, const std::function<void(int, int)>& work) {
    static const int kMostBands = static_cast<int>(std::clamp(std::thread::hardware_concurrency() / 2, 1U, 8U));
    const int bands = std::min(kMostBands, std::max(1, count / 32));
    const int each = (count + bands - 1) / bands;
    std::vector<Band> others;
    for (int from = each; from < count; from += each) others.push_back({&work, from, std::min(count, from + each), nullptr});
    std::latch done(static_cast<std::ptrdiff_t>(others.size()));
    for (Band& band : others) {
        band.done = &done;
        if (!::TrySubmitThreadpoolCallback(DoBand, &band, nullptr)) DoBand(nullptr, &band);
    }
    work(0, std::min(count, each));
    done.wait();
}

// Marks what a shape covers in a mask of `area`'s pixels, as solid as the shape is there: in the
// mask's rows from `fromRow` to `toRow`.
void Stamp(std::vector<std::uint8_t>& mask, const RECT& area, const GlowShape& shape, int fromRow, int toRow) {
    const int width = WidthOf(area);
    const int shapeWidth = WidthOf(shape.rect);
    const bool solid = shape.alpha.size() != static_cast<std::size_t>(shapeWidth) * std::max(HeightOf(shape.rect), 0);
    const RECT whole[] = {shape.rect};
    const std::span<const RECT> parts = shape.parts.empty() ? std::span<const RECT>(whole) : std::span(shape.parts);
    const RECT band{area.left, area.top + fromRow, area.right, area.top + toRow};
    for (const RECT& part : parts) {
        const RECT clip = Common(Common(part, shape.rect), band);
        if (IsEmpty(clip)) continue;
        for (int y = clip.top; y < clip.bottom; ++y) {
            std::uint8_t* const row = mask.data() + static_cast<std::ptrdiff_t>(y - area.top) * width + (clip.left - area.left);
            if (solid) {
                std::memset(row, 0xFF, static_cast<std::size_t>(WidthOf(clip)));
                continue;
            }
            const std::uint8_t* const alpha =
                shape.alpha.data() + static_cast<std::ptrdiff_t>(y - shape.rect.top) * shapeWidth + (clip.left - shape.rect.left);
            for (int x = 0; x < WidthOf(clip); ++x) row[x] = std::max(row[x], alpha[x]);
        }
    }
}

// One pass of a box blur along `count` values `stride` apart: each the average of those
// within `radius` of it, with nothing beyond the ends.
void BoxPass(const float* from, float* to, int count, std::ptrdiff_t stride, int radius) noexcept {
    const float share = 1.0F / static_cast<float>(2 * radius + 1);
    float sum = 0;
    for (int i = 0; i <= std::min(radius, count - 1); ++i) sum += from[i * stride];
    for (int i = 0; i < count; ++i) {
        to[i * stride] = sum * share;
        if (const int coming = i + radius + 1; coming < count) sum += from[coming * stride];
        if (const int going = i - radius; going >= 0) sum -= from[going * stride];
    }
}

// The same down the grid's columns from `fromColumn` to `toColumn`, a row at a time, keeping
// a running sum for each column: much quicker than going down each column on its own.
void BoxPassDown(const float* from, float* to, int columns, int rows, int fromColumn, int toColumn, int radius) {
    const float share = 1.0F / static_cast<float>(2 * radius + 1);
    const int width = toColumn - fromColumn;
    std::vector<float> sum(static_cast<std::size_t>(width), 0.0F);
    const auto rowOf = [&](const float* grid, int row) { return grid + static_cast<std::ptrdiff_t>(row) * columns + fromColumn; };
    for (int row = 0; row <= std::min(radius, rows - 1); ++row) {
        const float* const in = rowOf(from, row);
        for (int i = 0; i < width; ++i) sum[static_cast<std::size_t>(i)] += in[i];
    }
    for (int row = 0; row < rows; ++row) {
        float* const out = to + static_cast<std::ptrdiff_t>(row) * columns + fromColumn;
        for (int i = 0; i < width; ++i) out[i] = sum[static_cast<std::size_t>(i)] * share;
        if (const int coming = row + radius + 1; coming < rows) {
            const float* const in = rowOf(from, coming);
            for (int i = 0; i < width; ++i) sum[static_cast<std::size_t>(i)] += in[i];
        }
        if (const int going = row - radius; going >= 0) {
            const float* const in = rowOf(from, going);
            for (int i = 0; i < width; ++i) sum[static_cast<std::size_t>(i)] -= in[i];
        }
    }
}

// Where a pixel falls between the squares of the grid: the square whose middle is at or
// before it, and how far on to the next one's it is. Past either end, it is the end's own.
struct GridPlace {
    int cell;
    float on;
};
[[nodiscard]] GridPlace PlaceOnGrid(int pixel, int cells, int step) noexcept {
    const float place = (static_cast<float>(pixel) + 0.5F) / static_cast<float>(step) - 0.5F;
    if (place <= 0) return {0, 0};
    const int cell = static_cast<int>(place);
    if (cell >= cells - 1) return {cells - 1, 0};
    return {cell, place - static_cast<float>(cell)};
}

// How solid the glow is, out of 255, by the cover round a pixel: twice the cover, from 0 to 1,
// in kShadeSteps steps. Spread keeps it at its most for the first part of that.
constexpr int kShadeSteps = 1024;
void MakeShades(const GlowLook& look, std::vector<float>& shades) {
    shades.resize(kShadeSteps + 1);
    const double most = 255.0 * std::clamp(look.opacity, 0, 100) / 100.0;
    const double kept = std::clamp(look.spread, 0, kGlowSpreadMost) / 100.0;
    for (int step = 0; step <= kShadeSteps; ++step) {
        const double cover = std::min(1.0, static_cast<double>(step) / kShadeSteps / (1.0 - kept));
        const double curved = std::pow(cover, kGlowCurve);
        shades[static_cast<std::size_t>(step)] =
            static_cast<float>(std::max(0.0, (curved - kGlowFloor) / (1.0 - kGlowFloor)) * most);
    }
}
[[nodiscard]] float ShadeOf(const std::vector<float>& shades, float cover) noexcept {
    const float step = std::clamp(cover * (2.0F * kShadeSteps) + 0.5F, 0.0F, static_cast<float>(kShadeSteps));
    return shades[static_cast<std::size_t>(step)];
}

} // namespace

GlowGrid GlowGridFor(int size) noexcept {
    // Coarser squares for a larger glow: the grid stays as fine as its smoothing needs.
    const int wanted = std::clamp(size, kGlowSizeLeast, kGlowSizeMost);
    const int step = std::clamp(wanted / 15, 2, 8);
    return {.step = step, .smoothing = std::max(1, (wanted + 3 * step / 2) / (3 * step))};
}

std::vector<GlowRun> FindGlowRuns(std::span<const StackedWindow> stack, int reach) {
    std::vector<GlowRun> runs;
    std::vector<RECT> reaches; // how far the glow of the run being gathered reaches
    bool gathering = false;
    for (std::size_t i = 0; i < stack.size(); ++i) {
        const StackedWindow& window = stack[i];
        const bool ofTheRun = gathering && window.role != GlowRole::Other && window.group == runs.back().group;
        if (window.role == GlowRole::Casts) {
            if (!ofTheRun) {
                runs.push_back({.group = window.group});
                reaches.clear();
                gathering = true;
            }
            runs.back().casting.push_back(i);
            runs.back().bottom = i;
            reaches.push_back(Grown(window.rect, reach));
        } else if (gathering && !ofTheRun) {
            gathering = std::ranges::none_of(reaches, [&window](const RECT& r) { return Meet(r, window.rect); });
        }
    }
    return runs;
}

std::vector<int> DockedGroups(std::span<const RECT> windows, int slack) {
    // Each window starts as a group of its own, and docked ones are joined.
    std::vector<std::size_t> joinedTo(windows.size());
    for (std::size_t i = 0; i < windows.size(); ++i) joinedTo[i] = i;
    const auto root = [&joinedTo](std::size_t i) {
        while (joinedTo[i] != i) i = joinedTo[i] = joinedTo[joinedTo[i]];
        return i;
    };
    const auto along = [slack](LONG edge, LONG other) { return std::abs(edge - other) <= slack; };
    const auto docked = [&along](const RECT& a, const RECT& b) {
        const bool sideBySide = std::min(a.bottom, b.bottom) > std::max(a.top, b.top);
        const bool aboveAndBelow = std::min(a.right, b.right) > std::max(a.left, b.left);
        return (sideBySide && (along(a.right, b.left) || along(b.right, a.left))) ||
               (aboveAndBelow && (along(a.bottom, b.top) || along(b.bottom, a.top)));
    };
    for (std::size_t i = 0; i < windows.size(); ++i) {
        for (std::size_t j = i + 1; j < windows.size(); ++j) {
            if (docked(windows[i], windows[j])) joinedTo[root(i)] = root(j);
        }
    }
    // Numbered from 0, in the order the groups first come in the list.
    std::vector<int> groups(windows.size());
    std::vector<int> numberOf(windows.size(), -1);
    int next = 0;
    for (std::size_t i = 0; i < windows.size(); ++i) {
        int& number = numberOf[root(i)];
        if (number < 0) number = next++;
        groups[i] = number;
    }
    return groups;
}

RECT GlowArea(std::span<const GlowShape> casting, int reach, const RECT& screen) {
    RECT area{};
    bool any = false;
    for (const GlowShape& shape : casting) {
        if (IsEmpty(shape.rect)) continue;
        const RECT around = Grown(shape.rect, reach);
        area = any ? RECT{std::min(area.left, around.left), std::min(area.top, around.top),
                          std::max(area.right, around.right), std::max(area.bottom, around.bottom)}
                   : around;
        any = true;
    }
    return any ? Common(area, screen) : RECT{};
}

void PaintGlow(const RECT& area, std::span<const GlowShape> casting, std::span<const GlowShape> keptClear,
               const GlowLook& look, std::span<std::uint32_t> pixels, GlowScratch& scratch) {
    const int width = WidthOf(area), height = HeightOf(area);
    if (width <= 0 || height <= 0) return;
    const std::size_t count = static_cast<std::size_t>(width) * height;
    if (pixels.size() < count) return;

    scratch.casting.resize(count);
    scratch.clear.resize(count);
    InBands(height, [&](int from, int to) {
        const auto first = static_cast<std::ptrdiff_t>(from) * width, last = static_cast<std::ptrdiff_t>(to) * width;
        std::fill(scratch.casting.begin() + first, scratch.casting.begin() + last, std::uint8_t{0});
        std::fill(scratch.clear.begin() + first, scratch.clear.begin() + last, std::uint8_t{0});
        for (const GlowShape& shape : casting) Stamp(scratch.casting, area, shape, from, to);
        for (const GlowShape& shape : keptClear) Stamp(scratch.clear, area, shape, from, to);
    });

    // How much of each square of the grid the windows cover, from 0 to 1.
    const GlowGrid grid = GlowGridFor(look.size);
    const int step = grid.step;
    const int columns = (width + step - 1) / step, rows = (height + step - 1) / step;
    const std::size_t cells = static_cast<std::size_t>(columns) * rows;
    scratch.grid.assign(cells, 0);
    scratch.smoothed.assign(cells, 0);
    const float full = 255.0F * static_cast<float>(step * step);
    InBands(rows, [&](int from, int to) {
        for (int row = from; row < to; ++row) {
            float* const into = scratch.grid.data() + static_cast<std::ptrdiff_t>(row) * columns;
            for (int y = row * step; y < std::min(height, (row + 1) * step); ++y) {
                const std::uint8_t* const mask = scratch.casting.data() + static_cast<std::ptrdiff_t>(y) * width;
                for (int column = 0; column < columns; ++column) {
                    unsigned sum = 0;
                    for (int x = column * step; x < std::min(width, (column + 1) * step); ++x) sum += mask[x];
                    into[column] += static_cast<float>(sum);
                }
            }
            for (int column = 0; column < columns; ++column) into[column] /= full;
        }
    });

    // Smoothed three times over each way, which comes close to a Gaussian blur: along each row
    // into `smoothed`, then down the columns back and forth, ending in `grid`.
    InBands(rows, [&](int fromRow, int toRow) {
        std::vector<float> once(static_cast<std::size_t>(columns)), twice(static_cast<std::size_t>(columns));
        for (int row = fromRow; row < toRow; ++row) {
            const std::ptrdiff_t start = static_cast<std::ptrdiff_t>(row) * columns;
            BoxPass(scratch.grid.data() + start, once.data(), columns, 1, grid.smoothing);
            BoxPass(once.data(), twice.data(), columns, 1, grid.smoothing);
            BoxPass(twice.data(), scratch.smoothed.data() + start, columns, 1, grid.smoothing);
        }
    });
    InBands(columns, [&](int fromColumn, int toColumn) {
        BoxPassDown(scratch.smoothed.data(), scratch.grid.data(), columns, rows, fromColumn, toColumn, grid.smoothing);
        BoxPassDown(scratch.grid.data(), scratch.smoothed.data(), columns, rows, fromColumn, toColumn, grid.smoothing);
        BoxPassDown(scratch.smoothed.data(), scratch.grid.data(), columns, rows, fromColumn, toColumn, grid.smoothing);
    });
    const float* const smooth = scratch.grid.data();

    // Each pixel takes its darkness from the four squares round it. At a straight edge
    // half of what is round it is window, which is where the glow is darkest.
    scratch.cellOf.resize(static_cast<std::size_t>(width));
    scratch.intoCell.resize(static_cast<std::size_t>(width));
    for (int x = 0; x < width; ++x) {
        const GridPlace place = PlaceOnGrid(x, columns, step);
        scratch.cellOf[static_cast<std::size_t>(x)] = place.cell;
        scratch.intoCell[static_cast<std::size_t>(x)] = place.on;
    }
    MakeShades(look, scratch.shades);
    const std::vector<float>& shades = scratch.shades;
    // Premultiplied: each channel of the colour as far as the glow is solid.
    const float red = static_cast<float>((look.color >> 16) & 0xFF) / 255.0F;
    const float green = static_cast<float>((look.color >> 8) & 0xFF) / 255.0F;
    const float blue = static_cast<float>(look.color & 0xFF) / 255.0F;
    const bool black = (look.color & 0xFFFFFF) == 0;
    InBands(height, [&](int fromRow, int toRow) {
        std::vector<float> line(static_cast<std::size_t>(columns) + 1); // the grid, smoothed, at the row's height
        for (int y = fromRow; y < toRow; ++y) {
            std::uint32_t* const out = pixels.data() + static_cast<std::ptrdiff_t>(y) * width;
            const GridPlace place = PlaceOnGrid(y, rows, step);
            const float* const above = smooth + static_cast<std::ptrdiff_t>(place.cell) * columns;
            const float* const below = place.on > 0 ? above + columns : above;
            float most = 0;
            for (int column = 0; column < columns; ++column) {
                const float value = above[column] + (below[column] - above[column]) * place.on;
                line[static_cast<std::size_t>(column)] = value;
                most = std::max(most, value);
            }
            line[static_cast<std::size_t>(columns)] = line[static_cast<std::size_t>(columns) - 1];
            if (ShadeOf(shades, most) < 0.5F) {
                std::fill_n(out, width, 0u);
                continue;
            }
            const std::uint8_t* const clear = scratch.clear.data() + static_cast<std::ptrdiff_t>(y) * width;
            for (int x = 0; x < width; ++x) {
                const unsigned kept = clear[x];
                if (kept == 0xFF) {
                    out[x] = 0;
                    continue;
                }
                const auto cell = static_cast<std::size_t>(scratch.cellOf[static_cast<std::size_t>(x)]);
                const float cover =
                    line[cell] + (line[cell + 1] - line[cell]) * scratch.intoCell[static_cast<std::size_t>(x)];
                float dark = ShadeOf(shades, cover);
                if (kept != 0) dark *= static_cast<float>(0xFF - kept) * (1.0F / 255.0F);
                const auto alpha = static_cast<std::uint32_t>(dark + 0.5F);
                if (black) {
                    out[x] = alpha << 24;
                    continue;
                }
                const auto part = [alpha](float channel) {
                    return static_cast<std::uint32_t>(channel * static_cast<float>(alpha) + 0.5F);
                };
                out[x] = alpha << 24 | part(red) << 16 | part(green) << 8 | part(blue);
            }
        }
    });
}

} // namespace exodium
