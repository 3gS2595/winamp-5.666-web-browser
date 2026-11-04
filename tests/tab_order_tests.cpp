#include "test_framework.h"

#include "tab_order.h"

#include <string>
#include <string_view>

using namespace exodium;

namespace {

// Selections are written as marks, "x" for a selected tab and "." for one that isn't,
// and orders as the places the tabs came from, so that a failure shows what differs.
[[nodiscard]] TabSelection Marks(std::wstring_view marks) {
    TabSelection selection;
    for (const wchar_t mark : marks) selection.push_back(mark == L'x');
    return selection;
}
[[nodiscard]] std::wstring Show(const TabSelection& selection) {
    std::wstring marks;
    for (const bool selected : selection) marks += selected ? L'x' : L'.';
    return marks;
}
[[nodiscard]] std::wstring Show(const std::vector<int>& order) {
    std::wstring text;
    for (const int place : order) text += std::to_wstring(place);
    return text;
}

} // namespace

TEST_CASE(ShiftClickSelectsFromTheAnchorEitherWay) {
    EXPECT_EQ(Show(SelectRange(Marks(L"..x..."), 2, 4, false)), L"..xxx.");
    EXPECT_EQ(Show(SelectRange(Marks(L"..x..."), 2, 0, false)), L"xxx...");
    EXPECT_EQ(Show(SelectRange(Marks(L"..x..."), 2, 2, false)), L"..x...");
}

TEST_CASE(ShiftClickReplacesTheSelectionUnlessCtrlIsHeld) {
    EXPECT_EQ(Show(SelectRange(Marks(L"x....x"), 2, 3, false)), L"..xx..");
    EXPECT_EQ(Show(SelectRange(Marks(L"x....x"), 2, 3, true)), L"x.xx.x");
}

TEST_CASE(ShiftClickRangeStaysInsideTheStrip) {
    EXPECT_EQ(Show(SelectRange(Marks(L"...."), 9, 1, false)), L".xxx");
    EXPECT_EQ(Show(SelectRange(Marks(L"...."), -3, 1, false)), L"xx..");
}

TEST_CASE(NearestWantedLooksLeftFirst) {
    EXPECT_EQ(NearestWanted(Marks(L"x.x.x"), 2), 2);
    EXPECT_EQ(NearestWanted(Marks(L"x...x"), 2), 0);
    EXPECT_EQ(NearestWanted(Marks(L"....x"), 2), 4);
    EXPECT_EQ(NearestWanted(Marks(L"....."), 2), -1);
    EXPECT_EQ(NearestWanted(Marks(L""), 0), -1);
    EXPECT_EQ(NearestWanted(Marks(L".x"), 7), 1);
}

TEST_CASE(GatheringPutsTheMovedTabsTogetherInTheirOrder) {
    // Tabs 1 and 3 of five, put at the end, then at the start, then after tab 2.
    EXPECT_EQ(Show(GatheredOrder(Marks(L".x.x."), 3)), L"02413");
    EXPECT_EQ(Show(GatheredOrder(Marks(L".x.x."), 0)), L"13024");
    EXPECT_EQ(Show(GatheredOrder(Marks(L".x.x."), 2)), L"02134");
}

TEST_CASE(GatheringOneTabIsAnOrdinaryMove) {
    EXPECT_EQ(Show(GatheredOrder(Marks(L"x..."), 3)), L"1230");
    EXPECT_EQ(Show(GatheredOrder(Marks(L"...x"), 0)), L"3012");
    EXPECT_EQ(Show(GatheredOrder(Marks(L".x.."), 1)), L"0123");
    EXPECT_EQ(Show(GatheredOrder(Marks(L".x.."), 99)), L"0231");
}

// Places of openers, -1 for none: tab 1 opened tab 2, which opened tab 3; tab 4 is unrelated.
TEST_CASE(ALinkInFrontGoesRightAfterTheActiveTab) {
    EXPECT_EQ(LinkTabPlace({-1, -1, 1, 2, -1}, 1, true), 2);
    EXPECT_EQ(LinkTabPlace({-1, -1, -1}, 2, true), 3);
}

TEST_CASE(ALinkBehindGoesAfterWhatTheActiveTabOpened) {
    // After tab 2, and tab 3 which tab 2 opened, but not past tab 4.
    EXPECT_EQ(LinkTabPlace({-1, -1, 1, 2, -1}, 1, false), 4);
    // Nothing opened from it yet: right after it.
    EXPECT_EQ(LinkTabPlace({-1, -1, 1, 2, -1}, 4, false), 5);
    EXPECT_EQ(LinkTabPlace({-1, -1, -1, -1}, 0, false), 1);
}

TEST_CASE(OpenedTabsCountOnlyWhileTheyRunOnUnbroken) {
    // Tab 3 was opened from tab 0, but tab 2 between them wasn't.
    EXPECT_EQ(LinkTabPlace({-1, 0, -1, 0}, 0, false), 2);
}

TEST_CASE(LinksOpenedBehindOneAfterAnotherStayInOrder) {
    // Each new tab goes where LinkTabPlace says, opened from tab 1 of three.
    std::vector<int> openers{-1, -1, -1};
    std::wstring order = L"012";
    for (const wchar_t name : std::wstring(L"abc")) {
        const int place = LinkTabPlace(openers, 1, false);
        openers.insert(openers.begin() + place, 1);
        order.insert(order.begin() + place, name);
    }
    EXPECT_EQ(order, L"01abc2");
}

TEST_CASE(WithNoActiveTabALinkGoesAtTheEnd) {
    EXPECT_EQ(LinkTabPlace({-1, -1}, -1, false), 2);
    EXPECT_EQ(LinkTabPlace({}, 0, true), 0);
}

TEST_CASE(GoingBetweenRelatedTabsKeepsTheirOpeners) {
    // From a tab to one it opened, and back.
    EXPECT(!SwitchForgetsOpeners(1, 0, 2, 1));
    EXPECT(!SwitchForgetsOpeners(2, 1, 1, 0));
    // Between two opened from the same tab.
    EXPECT(!SwitchForgetsOpeners(2, 1, 3, 1));
    // Between two opened from no tab.
    EXPECT(!SwitchForgetsOpeners(4, 0, 5, 0));
}

TEST_CASE(GoingToAnUnrelatedTabForgetsThem) {
    EXPECT(SwitchForgetsOpeners(2, 1, 4, 0));
    EXPECT(SwitchForgetsOpeners(4, 0, 2, 1));
    EXPECT(SwitchForgetsOpeners(2, 1, 5, 4));
}
