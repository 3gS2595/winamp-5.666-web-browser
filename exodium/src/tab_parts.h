// Where the things a tab shows go inside it: the page's icon on the left (or the spinner
// or crash mark in its stead), the title, and on the right the speaker of a page making
// sound and the button that closes the tab. Nothing here has a window in it; drawing.cpp
// draws a tab by it, and tab_drag.cpp tells a click on the close button by it.
//
// A narrow tab drops what it has no room for, as Chrome's do. The active tab keeps its
// close button however narrow it gets, and the others only have one while they are wide
// enough not to be closed by accident. On a tab with room for nothing else, the close
// button takes the icon's place, in the middle of the tab.
#pragma once

namespace exodium {

inline constexpr int kTabIconSize = 16;   // the icon, the speaker and the close button are squares this size
inline constexpr int kTabIconPadding = 4; // left on either side of each of them
inline constexpr int kTabIconSlot = kTabIconSize + 2 * kTabIconPadding;
// Chrome takes the close buttons off tabs other than the active one once they have less
// than 68 pixels inside their padding.
inline constexpr int kTabCloseMinWidth = 68 + 2 * kTabIconPadding;
// With less room than this a title would show no more than a sliver of a letter or of its
// ellipsis, and it isn't shown at all.
inline constexpr int kTabTitleMinWidth = 16;

// A rectangle inside a tab, in whatever coordinates the tab is given in.
struct TabBox {
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;

    [[nodiscard]] constexpr int width() const noexcept { return right - left; }
    [[nodiscard]] constexpr int height() const noexcept { return bottom - top; }
    [[nodiscard]] constexpr bool empty() const noexcept { return right <= left || bottom <= top; }
    [[nodiscard]] constexpr bool contains(int x, int y) const noexcept {
        return x >= left && x < right && y >= top && y < bottom;
    }
    friend bool operator==(const TabBox&, const TabBox&) = default;
};

// What a tab has to show besides its title.
struct TabFace {
    bool icon = false;        // the page's icon, or a spinner or crash mark
    bool speaker = false;     // its page is making sound
    bool closeButton = false; // tabs have close buttons; a setting
    bool active = false;      // it is the tab in view, whose close button stays however narrow it is
};

// Where each part goes. What isn't shown has an empty box.
struct TabParts {
    TabBox icon;
    TabBox speaker; // on the right, or in the icon's place when there is no room for both
    TabBox close;
    TabBox title; // what is left between them, if that is room enough
};

[[nodiscard]] TabParts PlaceTabParts(const TabBox& tab, const TabFace& face);

// Where a tab's close button is, empty if it has none: the place PlaceTabParts gives it,
// which only depends on these.
[[nodiscard]] TabBox TabCloseBox(const TabBox& tab, bool closeButtons, bool active);

} // namespace exodium
