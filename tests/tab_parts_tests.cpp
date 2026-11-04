#include "test_framework.h"

#include "tab_parts.h"

#include <array>
#include <string>

using namespace exodium;

namespace {

// A tab's inside as its button has it: 140 pixels wide, less the border, and 22 high.
constexpr TabBox kWideTab{0, 0, 138, 22};
// The narrowest a tab's inside gets: kTabMinWidth, less the border.
constexpr int kNarrowest = 22;
// Where a square part sits up and down in a tab that high.
constexpr int kTop = (22 - kTabIconSize) / 2;

[[nodiscard]] constexpr TabBox Tab(int width) {
    return {0, 0, width, 22};
}

// A part as the span it takes along the tab, so that a failure shows where it went.
[[nodiscard]] std::wstring Span(const TabBox& box) {
    if (box.empty()) return L"none";
    return std::to_wstring(box.left) + L"-" + std::to_wstring(box.right);
}

[[nodiscard]] bool Overlap(const TabBox& a, const TabBox& b) {
    return !a.empty() && !b.empty() && a.left < b.right && b.left < a.right && a.top < b.bottom && b.top < a.bottom;
}

[[nodiscard]] bool Inside(const TabBox& part, const TabBox& tab) {
    return part.empty() ||
           (part.left >= tab.left && part.right <= tab.right && part.top >= tab.top && part.bottom <= tab.bottom);
}

// Every combination of what a tab can have to show.
[[nodiscard]] std::array<TabFace, 16> EveryFace() {
    std::array<TabFace, 16> faces;
    for (int i = 0; i < 16; ++i) {
        faces[i] = {.icon = (i & 1) != 0, .speaker = (i & 2) != 0, .closeButton = (i & 4) != 0, .active = (i & 8) != 0};
    }
    return faces;
}

} // namespace

TEST_CASE(TheCloseButtonSitsAtTheRightEndOfAWideTab) {
    for (const bool active : {false, true}) {
        const TabParts parts = PlaceTabParts(kWideTab, {.icon = true, .closeButton = true, .active = active});
        EXPECT_EQ(Span(parts.close), L"118-134");
        EXPECT_EQ(parts.close.top, kTop);
        EXPECT_EQ(parts.close.height(), kTabIconSize);
        EXPECT_EQ(Span(parts.icon), L"4-20");
        EXPECT_EQ(Span(parts.speaker), L"none");
        EXPECT_EQ(Span(parts.title), L"24-114");
        EXPECT_EQ(parts.title.height(), 22);
    }
}

TEST_CASE(TheSpeakerSitsLeftOfTheCloseButton) {
    const TabParts parts = PlaceTabParts(kWideTab, {.icon = true, .speaker = true, .closeButton = true});
    EXPECT_EQ(Span(parts.close), L"118-134");
    EXPECT_EQ(Span(parts.speaker), L"94-110");
    EXPECT_EQ(Span(parts.icon), L"4-20");
    EXPECT_EQ(Span(parts.title), L"24-90");
}

TEST_CASE(WithoutCloseButtonsATabIsLaidOutAsBefore) {
    // The speaker at the right end, the icon at the left.
    TabParts parts = PlaceTabParts(kWideTab, {.icon = true, .speaker = true});
    EXPECT_EQ(Span(parts.close), L"none");
    EXPECT_EQ(Span(parts.speaker), L"118-134");
    EXPECT_EQ(Span(parts.icon), L"4-20");
    EXPECT_EQ(Span(parts.title), L"24-114");

    // Too narrow for both, the speaker takes the icon's place.
    parts = PlaceTabParts(Tab(47), {.icon = true, .speaker = true, .active = true});
    EXPECT_EQ(Span(parts.speaker), L"4-20");
    EXPECT_EQ(Span(parts.icon), L"none");
    EXPECT_EQ(Span(parts.title), L"24-47");
    parts = PlaceTabParts(Tab(48), {.icon = true, .speaker = true, .active = true});
    EXPECT_EQ(Span(parts.speaker), L"28-44");
    EXPECT_EQ(Span(parts.icon), L"4-20");

    // A tab with no icon has its title from the left end.
    parts = PlaceTabParts(kWideTab, {.speaker = true});
    EXPECT_EQ(Span(parts.speaker), L"118-134");
    EXPECT_EQ(Span(parts.title), L"0-114");
    parts = PlaceTabParts(kWideTab, {});
    EXPECT_EQ(Span(parts.title), L"0-138");
}

TEST_CASE(OnlyTheActiveTabKeepsItsCloseButtonWhenNarrow) {
    EXPECT_EQ(Span(PlaceTabParts(Tab(kTabCloseMinWidth), {.icon = true, .closeButton = true}).close), L"56-72");
    EXPECT_EQ(Span(PlaceTabParts(Tab(kTabCloseMinWidth - 1), {.icon = true, .closeButton = true}).close), L"none");
    EXPECT_EQ(Span(PlaceTabParts(Tab(kTabCloseMinWidth - 1), {.closeButton = true, .active = true}).close), L"55-71");
    // An inactive tab that has lost it has its icon and title as before.
    const TabParts parts = PlaceTabParts(Tab(60), {.icon = true, .closeButton = true});
    EXPECT_EQ(Span(parts.icon), L"4-20");
    EXPECT_EQ(Span(parts.title), L"24-60");
}

TEST_CASE(TheActiveTabGivesUpItsIconBeforeItsCloseButton) {
    // Room for two: the close button and the speaker, which takes the icon's place.
    TabParts parts = PlaceTabParts(Tab(60), {.icon = true, .speaker = true, .closeButton = true, .active = true});
    EXPECT_EQ(Span(parts.close), L"40-56");
    EXPECT_EQ(Span(parts.speaker), L"4-20");
    EXPECT_EQ(Span(parts.icon), L"none");
    EXPECT_EQ(Span(parts.title), L"none"); // 12 pixels are too few to show any of it
    // Or the close button and the icon.
    parts = PlaceTabParts(Tab(64), {.icon = true, .closeButton = true, .active = true});
    EXPECT_EQ(Span(parts.close), L"44-60");
    EXPECT_EQ(Span(parts.icon), L"4-20");
    EXPECT_EQ(Span(parts.title), L"24-40");
    // Room for three: all of them.
    parts = PlaceTabParts(Tab(72), {.icon = true, .speaker = true, .closeButton = true, .active = true});
    EXPECT_EQ(Span(parts.close), L"52-68");
    EXPECT_EQ(Span(parts.speaker), L"28-44");
    EXPECT_EQ(Span(parts.icon), L"4-20");
    EXPECT_EQ(Span(parts.title), L"none");
}

TEST_CASE(ANarrowActiveTabHasOnlyItsCloseButtonInTheMiddle) {
    for (const int width : {22, 30, 47}) {
        const TabParts parts =
            PlaceTabParts(Tab(width), {.icon = true, .speaker = true, .closeButton = true, .active = true});
        const int left = (width - kTabIconSize) / 2;
        EXPECT_EQ(Span(parts.close), std::to_wstring(left) + L"-" + std::to_wstring(left + kTabIconSize));
        EXPECT_EQ(Span(parts.icon), L"none");
        EXPECT_EQ(Span(parts.speaker), L"none");
        EXPECT_EQ(Span(parts.title), L"none");
    }
    // The tabs beside it have their icon, or their speaker in its place.
    EXPECT_EQ(Span(PlaceTabParts(Tab(22), {.icon = true, .closeButton = true}).icon), L"4-20");
    EXPECT_EQ(Span(PlaceTabParts(Tab(22), {.icon = true, .speaker = true, .closeButton = true}).speaker), L"4-20");
    // With close buttons off, the active tab keeps its icon.
    EXPECT_EQ(Span(PlaceTabParts(Tab(22), {.icon = true, .active = true}).icon), L"4-20");
}

TEST_CASE(ATitleWithNoRoomToShowIsLeftOut) {
    EXPECT_EQ(Span(PlaceTabParts(Tab(24 + kTabTitleMinWidth), {.icon = true}).title), L"24-40");
    EXPECT_EQ(Span(PlaceTabParts(Tab(24 + kTabTitleMinWidth - 1), {.icon = true}).title), L"none");
}

TEST_CASE(PartsStayInsideTheTabAndApart) {
    for (int width = kNarrowest; width <= 160; ++width) {
        const TabBox tab{100, 7, 100 + width, 29}; // anywhere, as in the ghost of several dragged tabs
        for (const TabFace& face : EveryFace()) {
            const TabParts parts = PlaceTabParts(tab, face);
            const std::array<TabBox, 4> all{parts.icon, parts.speaker, parts.close, parts.title};
            for (std::size_t i = 0; i < all.size(); ++i) {
                EXPECT(Inside(all[i], tab));
                for (std::size_t j = i + 1; j < all.size(); ++j) EXPECT(!Overlap(all[i], all[j]));
            }
            EXPECT(parts.title.empty() || parts.title.width() >= kTabTitleMinWidth);
            EXPECT_EQ(parts.title.height(), tab.height());
            const bool closes = face.closeButton && (face.active || width >= kTabCloseMinWidth);
            EXPECT_EQ(!parts.close.empty(), closes);
            if (!parts.close.empty()) EXPECT_EQ(parts.close.top, tab.top + kTop);
            // The icon is only left out for want of room, and never for the close button of
            // a tab that isn't active.
            if (face.icon && parts.icon.empty()) {
                EXPECT(width < 3 * kTabIconSlot && (face.speaker || (face.active && face.closeButton)));
            }
        }
    }
}

TEST_CASE(TheCloseBoxIsWhereTheCloseButtonIsDrawn) {
    for (int width = kNarrowest; width <= 160; ++width) {
        for (const TabBox tab : {Tab(width), TabBox{200, 5, 200 + width, 27}}) {
            for (const TabFace& face : EveryFace()) {
                EXPECT(TabCloseBox(tab, face.closeButton, face.active) == PlaceTabParts(tab, face).close);
            }
        }
    }
}

TEST_CASE(AClickHitsTheCloseButtonOnlyInsideIt) {
    const TabBox box = TabCloseBox(kWideTab, true, false);
    EXPECT(box.contains(126, 11)); // its middle
    EXPECT(box.contains(118, kTop));
    EXPECT(box.contains(133, kTop + kTabIconSize - 1));
    EXPECT(!box.contains(117, 11)); // just left of it
    EXPECT(!box.contains(134, 11)); // just right of it
    EXPECT(!box.contains(126, kTop - 1));
    EXPECT(!box.contains(126, kTop + kTabIconSize));
    EXPECT(!box.contains(10, 10)); // where a tab is clicked to bring it into view
    // With close buttons off there is nothing to hit.
    EXPECT(TabCloseBox(kWideTab, false, true).empty());
    EXPECT(!TabCloseBox(kWideTab, false, true).contains(126, 11));
}
