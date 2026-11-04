// Where tabs go and which are chosen, worked out as Chrome does it. This is the part that
// knows tabs only by their places in the strip, or by their ids; tabs.cpp applies it to a
// window.
//
// Choosing several tabs: Ctrl+click adds a tab or takes it out, Shift+click takes in the
// tabs from the last one clicked, and what is chosen is dragged and closed together.
//
// A tab opened from a page goes next to the tab it was opened from, not at the end of the
// strip, and tabs opened one after another from a page line up in that order. A tab
// remembers which tab it was opened from for that, until the user moves on to something
// else: see SwitchForgetsOpeners.
#pragma once

#include <vector>

namespace exodium {

// Whether each tab of a window is selected, in strip order.
using TabSelection = std::vector<bool>;

// Shift+click on the tab at `index`: the tabs from `anchor` to it, both included. With
// Ctrl as well (`add`), the tabs selected already stay so.
[[nodiscard]] TabSelection SelectRange(const TabSelection& current, int anchor, int index, bool add);

// The place nearest `from` that `wanted` says yes to: `from` itself, or else the nearest
// one left of it, or else the nearest right of it, as the next tab is chosen when the
// active one closes. -1 if there is none.
[[nodiscard]] int NearestWanted(const std::vector<bool>& wanted, int from);

// The order a strip's tabs take when those marked in `moving` are taken out and put back
// side by side, in the order they had, at `insertAt` among the rest. Each entry is where
// that tab was before.
[[nodiscard]] std::vector<int> GatheredOrder(const std::vector<bool>& moving, int insertAt);

// Where a tab opened from the page in the active tab goes. Brought into view, it goes
// right after the active tab. Left behind it, it goes after the tabs the active tab has
// opened already, and the ones those opened, so that tabs opened one after another stay
// in that order. `openers` holds, for each tab in strip order, the place of the tab it was
// opened from, or -1. With no active tab it goes at the end.
[[nodiscard]] int LinkTabPlace(const std::vector<int>& openers, int active, bool foreground);

// Whether the user going from one tab to another means they have moved on, and which tab
// was opened from which no longer counts. Not if one was opened from the other, or both
// from the same tab. Tabs are given by id; an opener of 0 is none.
[[nodiscard]] bool SwitchForgetsOpeners(int fromId, int fromOpener, int toId, int toOpener) noexcept;

} // namespace exodium
