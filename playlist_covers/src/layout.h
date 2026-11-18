// Where things go: the grid of covers, its scrolling, the split with the playlist's list.
// Plain arithmetic, so the tests can check it.
#pragma once

namespace covers {

struct Box {
    int left = 0, top = 0, right = 0, bottom = 0;
    [[nodiscard]] constexpr int Width() const noexcept { return right - left; }
    [[nodiscard]] constexpr int Height() const noexcept { return bottom - top; }
    [[nodiscard]] constexpr bool Contains(int x, int y) const noexcept {
        return x >= left && x < right && y >= top && y < bottom;
    }
    bool operator==(const Box&) const = default;
};

// The grid of covers, laid out as the media library's album art icon view lays out its
// own (ml_local's AlbumArtFilter): cells of the cover and two pixels round it, and a line
// for the name below, packed from the top left with no space between them. In the grid's
// own coordinates (0, 0 is the top left of what scrolls).
struct Grid {
    static constexpr int kBorder = 2; // round a cover, inside its cell
    static constexpr int kFoot = 4;   // at a cell's bottom, left out of its picked colour

    int artSize = 0;
    int textHeight = 0; // 0: covers only, no names
    int columns = 1;
    int cellWidth = 0, cellHeight = 0;

    [[nodiscard]] int Rows(int tiles) const noexcept;
    [[nodiscard]] int ContentHeight(int tiles) const noexcept;
    [[nodiscard]] Box Cell(int index) const noexcept;
    [[nodiscard]] Box Art(int index) const noexcept;
    // Where the name is drawn (centred in it), as the library draws its own.
    [[nodiscard]] Box Name(int index) const noexcept;
    // What a picked cell's colour fills.
    [[nodiscard]] Box Highlight(int index) const noexcept;
    // The tile at a point, or -1 past the last one or right of the last column.
    [[nodiscard]] int TileAt(int tiles, int x, int y) const noexcept;
    // The tiles any part of which is in rows [top, bottom).
    void VisibleRange(int tiles, int top, int bottom, int& first, int& last) const noexcept;
};

// As many columns as fit in `width`; what is left over stays at the right.
[[nodiscard]] Grid MakeGrid(int width, int artSize, int textHeight) noexcept;

// The cover size of the library's album art view modes (its "albumartviewmode": small,
// medium and large details, then small, medium, large and extra large icons).
[[nodiscard]] int ArtSizeOfViewMode(int mode) noexcept;

// How far down `content` pixels can scroll in a view `visible` high.
[[nodiscard]] int ClampOffset(int offset, int content, int visible) noexcept;

// The place the playlist view gives its list, shared: the covers on top, `share` per
// thousand of it (with the bar that sizes them at their bottom), the list below. Each
// keeps at least `least` pixels; with no room for both, the list has it all.
struct Split {
    Box covers; // empty when there is no room
    Box list;
};
[[nodiscard]] Split SplitArea(const Box& area, int share, int least) noexcept;
// The share that puts the covers' bottom at `y`.
[[nodiscard]] int ShareFor(const Box& area, int y) noexcept;

} // namespace covers
