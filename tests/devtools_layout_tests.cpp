#include "test_framework.h"

#include "devtools_layout.h"

#include <map>
#include <string>

using namespace exodium;

namespace {

// A rectangle as "left,top,right,bottom", so that a failure shows what differs.
[[nodiscard]] std::wstring Show(const RECT& rect) {
    return std::to_wstring(rect.left) + L"," + std::to_wstring(rect.top) + L"," + std::to_wstring(rect.right) + L"," +
           std::to_wstring(rect.bottom);
}
[[nodiscard]] std::wstring Show(const std::optional<POINT>& point) {
    return point ? std::to_wstring(point->x) + L"," + std::to_wstring(point->y) : L"none";
}

using Section = std::map<std::wstring, std::wstring, std::less<>>;

[[nodiscard]] DevToolsPrefs Read(const Section& section) {
    return ReadDevToolsPrefs([&section](std::wstring_view key) -> std::optional<std::wstring> {
        const auto found = section.find(key);
        if (found == section.end()) return std::nullopt;
        return found->second;
    });
}

// The page's place in a window 1000 wide and 600 high, a margin of 6 in from its edges.
constexpr RECT kArea{6, 58, 1006, 658};

} // namespace

TEST_CASE(DevToolsDockOnTheRightTakeTheirWidthOffThePage) {
    const DevToolsSplit split = SplitForDevTools(kArea, {.side = DockSide::Right, .width = 400});
    EXPECT_EQ(Show(split.page), L"6,58,601,658");
    EXPECT_EQ(Show(split.splitter), L"601,58,606,658");
    EXPECT_EQ(Show(split.bar), L"606,58,1006,82");
    EXPECT_EQ(Show(split.panel), L"606,82,1006,658");
}

TEST_CASE(DevToolsDockOnTheLeftAreTheMirrorOfThat) {
    const DevToolsSplit split = SplitForDevTools(kArea, {.side = DockSide::Left, .width = 400});
    EXPECT_EQ(Show(split.bar), L"6,58,406,82");
    EXPECT_EQ(Show(split.panel), L"6,82,406,658");
    EXPECT_EQ(Show(split.splitter), L"406,58,411,658");
    EXPECT_EQ(Show(split.page), L"411,58,1006,658");
}

TEST_CASE(DevToolsDockAtTheBottomTakeTheirHeightOffThePage) {
    // The width kept for the sides is nothing to the bottom.
    const DevToolsSplit split = SplitForDevTools(kArea, {.side = DockSide::Bottom, .width = 400, .height = 250});
    EXPECT_EQ(Show(split.page), L"6,58,1006,403");
    EXPECT_EQ(Show(split.splitter), L"6,403,1006,408");
    EXPECT_EQ(Show(split.bar), L"6,408,1006,432");
    EXPECT_EQ(Show(split.panel), L"6,432,1006,658");
}

TEST_CASE(DevToolsNeverDraggedTakeTwoFifthsOfTheRoom) {
    EXPECT_EQ(FitDevToolsSize(1000, 0), 400);
    EXPECT_EQ(Show(SplitForDevTools(kArea, {}).panel), L"606,82,1006,658");
    EXPECT_EQ(Show(SplitForDevTools(kArea, {.side = DockSide::Bottom}).panel), L"6,442,1006,658");
}

TEST_CASE(DevToolsAndThePageBothKeepSomeRoom) {
    EXPECT_EQ(FitDevToolsSize(1000, 20), kDevToolsMinSize);
    EXPECT_EQ(FitDevToolsSize(1000, 5000), 1000 - kDevToolsSplitterSize - kDevToolsMinPage);
    // A window made narrower than the width they were dragged to takes it off them.
    EXPECT_EQ(FitDevToolsSize(500, 800), 500 - kDevToolsSplitterSize - kDevToolsMinPage);
}

TEST_CASE(DevToolsShareAWindowTooSmallForBoth) {
    EXPECT_EQ(FitDevToolsSize(205, 800), 100);
    EXPECT_EQ(FitDevToolsSize(205, 0), 100);
    EXPECT_EQ(FitDevToolsSize(3, 400), 0);
    EXPECT_EQ(FitDevToolsSize(0, 400), 0);
    // Nothing of the split lies outside the area, or is turned inside out.
    const RECT tiny{6, 58, 30, 70};
    for (const DockSide side : {DockSide::Right, DockSide::Bottom, DockSide::Left}) {
        const DevToolsSplit split = SplitForDevTools(tiny, {.side = side, .width = 400, .height = 400});
        for (const RECT& part : {split.page, split.splitter, split.bar, split.panel}) {
            EXPECT(part.left >= tiny.left && part.right <= tiny.right && part.top >= tiny.top &&
                   part.bottom <= tiny.bottom);
            EXPECT(part.left <= part.right && part.top <= part.bottom);
        }
    }
}

TEST_CASE(DraggingTheSplitterSizesTheDevToolsFromTheirEdge) {
    // Docked right, their left edge follows the mouse.
    EXPECT_EQ(DevToolsSizeForEdgeAt(kArea, DockSide::Right, 706), 300);
    EXPECT_EQ(DevToolsSizeForEdgeAt(kArea, DockSide::Left, 306), 300);
    EXPECT_EQ(DevToolsSizeForEdgeAt(kArea, DockSide::Bottom, 458), 200);
    // Dragged past either end, they stop at their least and at the page's.
    EXPECT_EQ(DevToolsSizeForEdgeAt(kArea, DockSide::Right, 5000), kDevToolsMinSize);
    EXPECT_EQ(DevToolsSizeForEdgeAt(kArea, DockSide::Right, -5000), 1000 - kDevToolsSplitterSize - kDevToolsMinPage);
    EXPECT_EQ(DevToolsSizeForEdgeAt(kArea, DockSide::Bottom, 5000), kDevToolsMinSize);
}

TEST_CASE(TheEdgeDraggedIsWhereTheSplitPutsIt) {
    for (const DockSide side : {DockSide::Right, DockSide::Bottom, DockSide::Left}) {
        const DevToolsPrefs prefs{.side = side, .width = 333, .height = 222};
        const int edge = DevToolsEdge(SplitForDevTools(kArea, prefs), side);
        EXPECT_EQ(DevToolsSizeForEdgeAt(kArea, side, edge), IsBesidePage(side) ? 333 : 222);
    }
}

TEST_CASE(TheStripsButtonsRunFromItsRightEnd) {
    EXPECT_EQ(Show(DevToolsButtonRect(400, DevToolsButton::Close)), L"378,2,398,22");
    EXPECT_EQ(Show(DevToolsButtonRect(400, DevToolsButton::OwnWindow)), L"356,2,376,22");
    EXPECT_EQ(Show(DevToolsButtonRect(400, DevToolsButton::DockLeft)), L"290,2,310,22");
    EXPECT(DevToolsButtonAt(400, {388, 12}) == DevToolsButton::Close);
    EXPECT(DevToolsButtonAt(400, {300, 12}) == DevToolsButton::DockLeft);
    EXPECT(DevToolsButtonAt(400, {377, 12}) == DevToolsButton::None); // between two
    EXPECT(DevToolsButtonAt(400, {100, 12}) == DevToolsButton::None); // on the title
    EXPECT(DevToolsButtonAt(400, {388, 1}) == DevToolsButton::None);
}

TEST_CASE(AStripTooNarrowLeavesOutTheButtonsFurthestFromItsEnd) {
    // Room for two: closing them is the last to go.
    EXPECT_EQ(Show(DevToolsButtonRect(50, DevToolsButton::Close)), L"28,2,48,22");
    EXPECT_EQ(Show(DevToolsButtonRect(50, DevToolsButton::OwnWindow)), L"6,2,26,22");
    EXPECT_EQ(Show(DevToolsButtonRect(50, DevToolsButton::DockRight)), L"0,0,0,0");
    EXPECT(DevToolsButtonAt(50, {0, 0}) == DevToolsButton::None);
    EXPECT_EQ(Show(DevToolsButtonRect(10, DevToolsButton::Close)), L"0,0,0,0");
    EXPECT_EQ(Show(DevToolsButtonRect(400, DevToolsButton::None)), L"0,0,0,0");
}

TEST_CASE(EveryButtonSaysWhatItDoes) {
    for (const DevToolsButton button : kDevToolsButtons) EXPECT(!DevToolsButtonTip(button).empty());
    EXPECT(DevToolsButtonTip(DevToolsButton::None).empty());
}

TEST_CASE(DevToolsPrefsStartOnTheRightAtTheNormalSize) {
    const DevToolsPrefs prefs;
    EXPECT(prefs.side == DockSide::Right);
    EXPECT_EQ(prefs.width, 0);
    EXPECT_EQ(prefs.height, 0);
    EXPECT_EQ(prefs.zoomPercent, 100);
    EXPECT(Read({}) == prefs);
}

TEST_CASE(DevToolsPrefsComeBackAsTheyWereWritten) {
    const DevToolsPrefs prefs{.side = DockSide::Bottom, .width = 512, .height = 300, .zoomPercent = 125};
    Section section;
    for (const DevToolsPrefEntry& entry : DevToolsPrefEntries(prefs)) section.emplace(entry.key, entry.value);
    EXPECT_EQ(section[L"devtools_dock"], L"bottom");
    EXPECT_EQ(section[L"devtools_width"], L"512");
    EXPECT_EQ(section[L"devtools_height"], L"300");
    EXPECT_EQ(section[L"devtools_zoom"], L"125");
    EXPECT(Read(section) == prefs);
    // Every side has a name that reads back as it.
    for (const DockSide side : {DockSide::Right, DockSide::Bottom, DockSide::Left}) {
        EXPECT(ParseDockSide(DockSideName(side)) == side);
    }
}

TEST_CASE(DevToolsPrefsThatAreNothingTheyCanBeAreLeftAsAtFirst) {
    const DevToolsPrefs read = Read({{L"devtools_dock", L"top"},
                                     {L"devtools_width", L"-40"},
                                     {L"devtools_height", L"wide"},
                                     {L"devtools_zoom", L"5000"}});
    EXPECT(read == DevToolsPrefs{});
    EXPECT(Read({{L"devtools_dock", L" LEFT "}}).side == DockSide::Left);
    EXPECT_EQ(Read({{L"devtools_width", L"99999999"}}).width, 0);
    EXPECT_EQ(Read({{L"devtools_zoom", L"49"}}).zoomPercent, 100);
    EXPECT_EQ(Read({{L"devtools_zoom", L"50"}}).zoomPercent, 50);
}

TEST_CASE(ZoomStepsThroughTheSizesChromeHas) {
    EXPECT_EQ(NextDevToolsZoom(100, true), 110);
    EXPECT_EQ(NextDevToolsZoom(100, false), 90);
    EXPECT_EQ(NextDevToolsZoom(110, false), 100);
    // From a size between two, as Ctrl and the wheel leave, to the nearest on that side.
    EXPECT_EQ(NextDevToolsZoom(104, true), 110);
    EXPECT_EQ(NextDevToolsZoom(104, false), 100);
    EXPECT_EQ(NextDevToolsZoom(300, true), 300);
    EXPECT_EQ(NextDevToolsZoom(50, false), 50);
}

TEST_CASE(TheDevToolsAddressIsThePortsOwnForThePage) {
    EXPECT_EQ(DevToolsUrl(9333, L"E96F849C8DE90B0BA9B75CF87149F278").value_or(L"none"),
              L"http://127.0.0.1:9333/devtools/devtools_app.html?ws=127.0.0.1:9333/devtools/page/"
              L"E96F849C8DE90B0BA9B75CF87149F278");
    EXPECT_EQ(DevToolsUrl(9334, L"ab-12", L"console").value_or(L"none"),
              L"http://127.0.0.1:9334/devtools/devtools_app.html?ws=127.0.0.1:9334/devtools/page/ab-12&panel=console");
}

TEST_CASE(OnlyWhatAnIdIsMadeOfGoesIntoTheAddress) {
    EXPECT(!DevToolsUrl(9333, L""));
    EXPECT(!DevToolsUrl(9333, L"ABC&ws=evil.example/x"));
    EXPECT(!DevToolsUrl(9333, L"ABC/../x"));
    EXPECT(!DevToolsUrl(9333, L"ABC DEF"));
    EXPECT(!DevToolsUrl(9333, std::wstring(65, L'A')));
    EXPECT(!DevToolsUrl(9333, L"ABC", L"console&x=1"));
    EXPECT(!DevToolsUrl(0, L"ABC"));
    EXPECT(!DevToolsUrl(70000, L"ABC"));
}

TEST_CASE(TheDevToolsWebviewShowsNothingButThePortsDevTools) {
    EXPECT(IsDevToolsUrl(9333, L"http://127.0.0.1:9333/devtools/devtools_app.html?ws=127.0.0.1:9333/devtools/page/AB"));
    EXPECT(IsDevToolsUrl(9333, L"http://127.0.0.1:9333/devtools/inspector.html"));
    EXPECT(!IsDevToolsUrl(9333, L"http://127.0.0.1:9333/json/list"));
    EXPECT(!IsDevToolsUrl(9333, L"http://127.0.0.1:93330/devtools/x"));
    EXPECT(!IsDevToolsUrl(9333, L"http://127.0.0.1:9334/devtools/devtools_app.html"));
    EXPECT(!IsDevToolsUrl(9333, L"http://localhost:9333/devtools/devtools_app.html"));
    EXPECT(!IsDevToolsUrl(9333, L"https://example.com/http://127.0.0.1:9333/devtools/"));
    EXPECT(!IsDevToolsUrl(0, L"http://127.0.0.1:0/devtools/x"));
}

TEST_CASE(APointOfTheViewIsAPlaceInThePagesDocument) {
    // A view 1000 pixels wide showing a page 1000 of its own pixels wide: one to one.
    EXPECT_EQ(Show(DocumentPointOf({600, 400}, 1000, 800, {.innerWidth = 1000, .innerHeight = 800})), L"600,400");
    // Scrolled, the place is that much further down the document.
    EXPECT_EQ(Show(DocumentPointOf({600, 400}, 1000, 800, {.innerWidth = 1000, .innerHeight = 800, .scrollY = 1500})),
              L"600,1900");
    // On a screen scaled to 125%, or zoomed, the view has more pixels than the page counts.
    EXPECT_EQ(Show(DocumentPointOf({750, 500}, 2640, 1534, {.innerWidth = 2112, .innerHeight = 1227.2})), L"600,400");
    EXPECT_EQ(Show(DocumentPointOf({300, 300}, 1000, 800, {.innerWidth = 500, .innerHeight = 400, .scrollX = 10.5})),
              L"160,150");
}

TEST_CASE(APageThatSaysNothingOfItsSizeHasNoSuchPlace) {
    EXPECT_EQ(Show(DocumentPointOf({10, 10}, 0, 800, {.innerWidth = 1000, .innerHeight = 800})), L"none");
    EXPECT_EQ(Show(DocumentPointOf({10, 10}, 1000, 800, {.innerWidth = 0, .innerHeight = 800})), L"none");
    EXPECT_EQ(Show(DocumentPointOf({10, 10}, 1000, 800, {})), L"none");
}

TEST_CASE(ThePagesAccountOfItselfIsFourNumbers) {
    const auto page = ParsePageMetrics(L"[2112,1227,0,1500.5]");
    EXPECT(page.has_value());
    if (page) {
        EXPECT_EQ(page->innerWidth, 2112.0);
        EXPECT_EQ(page->innerHeight, 1227.0);
        EXPECT_EQ(page->scrollX, 0.0);
        EXPECT_EQ(page->scrollY, 1500.5);
    }
    EXPECT(ParsePageMetrics(L" [ 1 , 2 , 3 , 4 ] ").has_value());
    EXPECT(ParsePageMetrics(L"[1e3,2,3,-4]").has_value());
    EXPECT(!ParsePageMetrics(L"[1,2,3]"));
    EXPECT(!ParsePageMetrics(L"[1,2,3,4,5]"));
    EXPECT(!ParsePageMetrics(L"[1,2,,4]"));
    EXPECT(!ParsePageMetrics(L"[1,2,\"3\",4]"));
    EXPECT(!ParsePageMetrics(L"[1,2,null,4]"));
    EXPECT(!ParsePageMetrics(L"null"));
    EXPECT(!ParsePageMetrics(L""));
    EXPECT(!ParsePageMetrics(L"{}"));
}
