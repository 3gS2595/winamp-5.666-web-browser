#include "tab_parts.h"

namespace exodium {

TabParts PlaceTabParts(const TabBox& tab, const TabFace& face) {
    const int width = tab.width();
    // How many of the icon, the speaker and the close button there is room for. There is
    // always room for one: the narrowest tab is wider than an icon.
    const int room = width >= 3 * kTabIconSlot ? 3 : width >= 2 * kTabIconSlot ? 2 : 1;
    const bool hasClose = face.closeButton && (face.active || width >= kTabCloseMinWidth);

    // They are given room in this order: the active tab's close button first, as in
    // Chrome; then the speaker, which says what the icon doesn't; then the icon. Another
    // tab's close button comes last, and it only has one with room for all three.
    int left = room;
    const auto take = [&left](bool wanted) {
        if (!wanted || left == 0) return false;
        --left;
        return true;
    };
    bool close = hasClose && face.active && take(true);
    const bool speaker = take(face.speaker);
    const bool icon = take(face.icon);
    close = close || (hasClose && !face.active && take(true));

    TabParts parts;
    const int top = tab.top + (tab.height() - kTabIconSize) / 2;
    const auto square = [top](int x) { return TabBox{x, top, x + kTabIconSize, top + kTabIconSize}; };

    // With room for nothing else, the close button goes in the middle and the title gives way.
    if (close && room == 1) {
        parts.close = square(tab.left + (width - kTabIconSize) / 2);
        parts.title = {tab.left, tab.top, tab.left, tab.bottom};
        return parts;
    }

    int right = tab.right;
    if (close) {
        parts.close = square(right - kTabIconPadding - kTabIconSize);
        right -= kTabIconSlot;
    }
    // The speaker goes at the right end, left of any close button, if that leaves room for
    // the icon's place at the left end as well. If not, it takes the icon's place, which
    // it then has to itself.
    const bool speakerRight = speaker && room >= (close ? 3 : 2);
    if (speakerRight) {
        parts.speaker = square(right - kTabIconPadding - kTabIconSize);
        right -= kTabIconSlot;
    }
    int x = tab.left;
    if (icon || (speaker && !speakerRight)) {
        (icon ? parts.icon : parts.speaker) = square(x + kTabIconPadding);
        x += kTabIconSlot;
    }
    parts.title = {x, tab.top, right - x >= kTabTitleMinWidth ? right : x, tab.bottom};
    return parts;
}

TabBox TabCloseBox(const TabBox& tab, bool closeButtons, bool active) {
    return PlaceTabParts(tab, {.closeButton = closeButtons, .active = active}).close;
}

} // namespace exodium
