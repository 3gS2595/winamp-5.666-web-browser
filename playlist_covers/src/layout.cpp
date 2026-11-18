#include "layout.h"

#include <algorithm>

namespace covers {

int Grid::Rows(int tiles) const noexcept {
    return tiles <= 0 ? 0 : (tiles + columns - 1) / columns;
}

int Grid::ContentHeight(int tiles) const noexcept {
    return Rows(tiles) * cellHeight;
}

Box Grid::Cell(int index) const noexcept {
    const int left = (index % columns) * cellWidth, top = (index / columns) * cellHeight;
    return {left, top, left + cellWidth, top + cellHeight};
}

Box Grid::Art(int index) const noexcept {
    const Box cell = Cell(index);
    const int left = cell.left + kBorder, top = cell.top + kBorder;
    return {left, top, left + artSize, top + artSize};
}

// The library's rectangle for it (DrawItemIcon): from half a line above the line kept for
// the name to the cell's bottom, as wide as the cover.
Box Grid::Name(int index) const noexcept {
    const Box cell = Cell(index);
    const int line = cell.bottom - textHeight;
    return {cell.left + kBorder, line - textHeight / 2 - 1, cell.left + kBorder + artSize, line + textHeight};
}

Box Grid::Highlight(int index) const noexcept {
    const Box cell = Cell(index);
    return {cell.left, cell.top, cell.right, cell.bottom - (textHeight > 0 ? kFoot : 0)};
}

int Grid::TileAt(int tiles, int x, int y) const noexcept {
    if (x < 0 || y < 0 || cellWidth <= 0 || cellHeight <= 0) return -1;
    const int column = x / cellWidth, row = y / cellHeight;
    if (column >= columns) return -1;
    const int index = row * columns + column;
    return index < tiles ? index : -1;
}

void Grid::VisibleRange(int tiles, int top, int bottom, int& first, int& last) const noexcept {
    first = 0;
    last = -1;
    if (tiles <= 0 || cellHeight <= 0) return;
    const int firstRow = std::max(0, top / cellHeight);
    const int lastRow = std::max(0, (bottom - 1) / cellHeight);
    first = std::min(tiles - 1, firstRow * columns);
    last = std::min(tiles - 1, (lastRow + 1) * columns - 1);
}

Grid MakeGrid(int width, int artSize, int textHeight) noexcept {
    Grid grid;
    grid.artSize = artSize;
    grid.textHeight = std::max(0, textHeight);
    grid.cellWidth = artSize + 2 * Grid::kBorder;
    grid.cellHeight = artSize + 2 * Grid::kBorder + grid.textHeight;
    grid.columns = std::max(1, width / std::max(1, grid.cellWidth));
    return grid;
}

int ArtSizeOfViewMode(int mode) noexcept {
    switch (mode) {
    case 0:
    case 3: return 60;
    case 2:
    case 5: return 120;
    case 6: return 180;
    default: return 90; // 1 and 4, and the library's default
    }
}

int ClampOffset(int offset, int content, int visible) noexcept {
    return std::clamp(offset, 0, std::max(0, content - visible));
}

Split SplitArea(const Box& area, int share, int least) noexcept {
    const int height = area.Height();
    if (height < 2 * least) return {{area.left, area.top, area.right, area.top}, area};
    const int covers = std::clamp(static_cast<int>(static_cast<long long>(height) * share / 1000), least, height - least);
    return {{area.left, area.top, area.right, area.top + covers}, {area.left, area.top + covers, area.right, area.bottom}};
}

int ShareFor(const Box& area, int y) noexcept {
    if (area.Height() <= 0) return 500;
    return std::clamp(static_cast<int>(static_cast<long long>(y - area.top) * 1000 / area.Height()), 100, 900);
}

} // namespace covers
