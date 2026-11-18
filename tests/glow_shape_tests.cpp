#include "test_framework.h"

#include "glow_shape.h"

#include <cmath>
#include <cstdint>
#include <vector>

using namespace exodium;

namespace {

constexpr RECT kScreen{0, 0, 1000, 800};
// As far as the glow reaches as it is to begin with, and how solid it is at its most.
constexpr int kGlowReach = GlowGrid{}.reach();
constexpr int kGlowDarkest = 255;

StackedWindow Casting(RECT rect) {
    return {rect, GlowRole::Casts};
}
StackedWindow Winamps(RECT rect) {
    return {rect, GlowRole::Winamp};
}
StackedWindow Others(RECT rect) {
    return {rect, GlowRole::Other};
}

// A picture of the glow, and the alpha of its pixel at a point on the screen.
struct Picture {
    RECT area;
    std::vector<std::uint32_t> pixels;

    // Nothing outside the picture.
    [[nodiscard]] std::uint32_t PixelAt(int x, int y) const {
        if (x < area.left || x >= area.right || y < area.top || y >= area.bottom) return 0;
        return pixels[static_cast<std::size_t>(y - area.top) * (area.right - area.left) + static_cast<std::size_t>(x - area.left)];
    }
    [[nodiscard]] int AlphaAt(int x, int y) const { return static_cast<int>(PixelAt(x, y) >> 24); }
};

Picture Paint(const std::vector<GlowShape>& casting, const std::vector<GlowShape>& keptClear,
              const GlowLook& look = {}) {
    Picture picture{GlowArea(casting, GlowGridFor(look.size).reach(), kScreen), {}};
    picture.pixels.assign(static_cast<std::size_t>(picture.area.right - picture.area.left) *
                              static_cast<std::size_t>(picture.area.bottom - picture.area.top),
                          0xDEADBEEF);
    GlowScratch scratch;
    PaintGlow(picture.area, casting, keptClear, look, picture.pixels, scratch);
    return picture;
}

} // namespace

// Winamp's windows all together at the top glow as one, right under the lowest of them, and
// windows of other programs above them or below them change nothing.
TEST_CASE(WindowsTogetherGlowAsOne) {
    const std::vector<StackedWindow> stack{
        Others({0, 0, 1000, 800}), Casting({100, 100, 300, 300}), Casting({300, 100, 500, 200}),
        Winamps({120, 120, 200, 200}), Casting({100, 300, 300, 400}), Others({0, 0, 1000, 800}),
    };
    const std::vector<GlowRun> runs = FindGlowRuns(stack, kGlowReach);
    EXPECT_EQ(runs.size(), std::size_t{1});
    if (runs.size() != 1) return;
    EXPECT(runs[0].casting == (std::vector<std::size_t>{1, 2, 4}));
    EXPECT_EQ(runs[0].bottom, std::size_t{4});
}

// Another program's window between them, where the glow of those above would lie over it,
// parts them: each run then has its own picture.
TEST_CASE(AnotherProgramsWindowBetweenPartsThem) {
    const std::vector<StackedWindow> stack{
        Casting({100, 100, 300, 300}),
        Others({250, 150, 600, 500}),
        Casting({400, 100, 600, 300}),
        Casting({600, 100, 700, 300}),
    };
    const std::vector<GlowRun> runs = FindGlowRuns(stack, kGlowReach);
    EXPECT_EQ(runs.size(), std::size_t{2});
    if (runs.size() != 2) return;
    EXPECT(runs[0].casting == std::vector<std::size_t>{0});
    EXPECT_EQ(runs[0].bottom, std::size_t{0});
    EXPECT(runs[1].casting == (std::vector<std::size_t>{2, 3}));
    EXPECT_EQ(runs[1].bottom, std::size_t{3});
}

// One that is out of reach of their glow doesn't, nor does one of Winamp's.
TEST_CASE(AWindowOutOfReachPartsNothing) {
    const int justOut = 300 + kGlowReach;
    const std::vector<StackedWindow> stack{
        Casting({100, 100, 300, 300}),
        Others({justOut, 100, 900, 200}),
        Winamps({250, 150, 600, 500}),
        Casting({100, 300, 300, 400}),
    };
    const std::vector<GlowRun> runs = FindGlowRuns(stack, kGlowReach);
    EXPECT_EQ(runs.size(), std::size_t{1});
    // In reach of the window below it, but not of the one above: under the picture it is
    // under that window too, which is what lies over it.
    const std::vector<StackedWindow> under{
        Casting({100, 100, 300, 300}),
        Others({100, 500, 300, 600}),
        Casting({100, 450, 300, 550}),
    };
    EXPECT_EQ(FindGlowRuns(under, kGlowReach).size(), std::size_t{1});
    EXPECT(FindGlowRuns(std::vector{Others({0, 0, 10, 10}), Winamps({0, 0, 10, 10})}, kGlowReach).empty());
}

TEST_CASE(TheGlowGoesAsFarAsItReachesOnTheScreen) {
    const std::vector<GlowShape> casting{{.rect = {100, 100, 200, 200}}, {.rect = {300, 80, 400, 150}}};
    const RECT area = GlowArea(casting, kGlowReach, kScreen);
    EXPECT_EQ(area.left, 100 - kGlowReach);
    EXPECT_EQ(area.top, 80 - kGlowReach);
    EXPECT_EQ(area.right, 400 + kGlowReach);
    EXPECT_EQ(area.bottom, 200 + kGlowReach);
    const RECT clipped = GlowArea(std::vector<GlowShape>{{.rect = {0, 0, 100, 100}}}, kGlowReach, kScreen);
    EXPECT(clipped.left == 0 && clipped.top == 0 && clipped.right == 100 + kGlowReach);
    const RECT none = GlowArea({}, kGlowReach, kScreen);
    EXPECT(none.right <= none.left);
}

// Darkest at a window's edge, fading to nothing as far out as it reaches, and nothing over the
// window itself.
TEST_CASE(TheGlowFadesOutFromTheEdge) {
    const RECT window{300, 300, 600, 500};
    const Picture picture = Paint({{.rect = window}}, {{.rect = window}});
    EXPECT(picture.AlphaAt(450, 400) == 0);
    EXPECT(picture.AlphaAt(300, 400) == 0);
    const int atEdge = picture.AlphaAt(299, 400);
    EXPECT(atEdge >= kGlowDarkest * 8 / 10);
    EXPECT(atEdge <= kGlowDarkest);
    int before = atEdge;
    for (int out = 2; out <= kGlowReach; ++out) {
        const int alpha = picture.AlphaAt(300 - out, 400);
        EXPECT(alpha <= before);
        before = alpha;
    }
    EXPECT(picture.AlphaAt(300 - kGlowReach / 2, 400) > 0);
    EXPECT(picture.AlphaAt(300 - kGlowReach, 400) == 0);
    EXPECT(picture.AlphaAt(450, 500 + kGlowReach - 1) <= 1);
    // Black: only the alpha is ever more than nothing.
    for (const std::uint32_t pixel : picture.pixels) EXPECT((pixel & 0xFFFFFF) == 0);
}

// It lies over no window of Winamp's, glowing or not.
TEST_CASE(TheGlowIsKeptOffWinampsWindows) {
    const RECT window{300, 300, 600, 500}, beside{600, 300, 700, 400};
    const Picture picture = Paint({{.rect = window}}, {{.rect = window}, {.rect = beside}});
    EXPECT(picture.AlphaAt(600, 350) == 0);
    EXPECT(picture.AlphaAt(620, 399) == 0);
    EXPECT(picture.AlphaAt(620, 400) > 0); // below it
    // Only where the parts in sight are.
    const Picture partly = Paint({{.rect = window}}, {{.rect = window}, {.rect = beside, .parts = {{600, 300, 620, 400}}}});
    EXPECT(partly.AlphaAt(610, 350) == 0);
    EXPECT(partly.AlphaAt(630, 350) > 0);
}

// It goes round what is solid of a see-through window, and a window region's parts.
TEST_CASE(TheGlowFollowsTheOutline) {
    const RECT rect{300, 300, 500, 500};
    std::vector<std::uint8_t> alpha(200 * 200, 0);
    for (int y = 0; y < 200; ++y) {
        for (int x = 0; x < 50; ++x) alpha[static_cast<std::size_t>(y) * 200 + x] = 0xFF;
    }
    const GlowShape solidLeft{.rect = rect, .alpha = alpha};
    const Picture picture = Paint({solidLeft}, {solidLeft});
    EXPECT(picture.AlphaAt(299, 400) >= kGlowDarkest * 8 / 10);
    EXPECT(picture.AlphaAt(350, 400) >= kGlowDarkest * 8 / 10); // just past the solid part, inside the window
    EXPECT(picture.AlphaAt(300, 400) == 0);
    EXPECT(picture.AlphaAt(450, 400) == 0); // far from it
    EXPECT(picture.AlphaAt(501, 400) == 0);

    const GlowShape leftOnly{.rect = rect, .parts = {{300, 300, 350, 500}}};
    const Picture parts = Paint({leftOnly}, {leftOnly});
    EXPECT(parts.AlphaAt(352, 400) >= kGlowDarkest * 8 / 10);
    EXPECT(parts.AlphaAt(501, 400) == 0);
}

// As it is to begin with it reaches 60 pixels out, as the mock-up's did, and any size the
// slider can be set to comes out near what was asked for.
TEST_CASE(TheGlowReachesAboutAsFarAsItsSize) {
    EXPECT_EQ(GlowGrid{}.reach(), 60);
    EXPECT_EQ(GlowGridFor(GlowLook{}.size).reach(), 60);
    for (int size = kGlowSizeLeast; size <= kGlowSizeMost; ++size) {
        const GlowGrid grid = GlowGridFor(size);
        EXPECT(grid.step >= 2 && grid.smoothing >= 1);
        EXPECT(std::abs(grid.reach() - size) <= 3 * grid.step / 2 + 2);
    }
    EXPECT(GlowGridFor(5).reach() == GlowGridFor(kGlowSizeLeast).reach());
}

// A larger size goes further out; less opacity is lighter at the edge; spread keeps it solid
// further out before it fades; and it comes in any colour, premultiplied.
TEST_CASE(TheGlowLooksAsTheSlidersSay) {
    const RECT window{400, 300, 600, 500};
    const std::vector<GlowShape> shapes{{.rect = window}};
    const Picture plain = Paint(shapes, shapes);
    const Picture large = Paint(shapes, shapes, {.size = 150});
    EXPECT(plain.AlphaAt(400 - 80, 400) == 0);
    EXPECT(large.AlphaAt(400 - 80, 400) > 0);
    EXPECT(large.AlphaAt(400 - 30, 400) > plain.AlphaAt(400 - 30, 400));

    const Picture half = Paint(shapes, shapes, {.opacity = 50});
    EXPECT(std::abs(half.AlphaAt(399, 400) - plain.AlphaAt(399, 400) / 2) <= 2);
    EXPECT(Paint(shapes, shapes, {.opacity = 0}).AlphaAt(399, 400) == 0);

    const Picture spread = Paint(shapes, shapes, {.spread = 60});
    EXPECT(spread.AlphaAt(400 - 15, 400) > plain.AlphaAt(400 - 15, 400));
    EXPECT(spread.AlphaAt(399, 400) >= plain.AlphaAt(399, 400));
    EXPECT(spread.AlphaAt(400 - kGlowReach, 400) == 0);

    const Picture red = Paint(shapes, shapes, {.color = 0xFF0000});
    const std::uint32_t pixel = red.PixelAt(400 - 20, 400);
    EXPECT((pixel >> 24) > 0);
    EXPECT(((pixel >> 16) & 0xFF) == (pixel >> 24));
    EXPECT((pixel & 0xFFFF) == 0);
    EXPECT((pixel >> 24) == static_cast<std::uint32_t>(plain.AlphaAt(400 - 20, 400)));
}

// Winamp's windows as the user had them on 2026-11-07, edge to edge, a pixel over here and
// there: all docked, one group. A browser window put down apart from them, or over them
// without an edge along one of theirs, is a group of its own, as are windows meeting only
// at a corner.
TEST_CASE(WindowsEdgeToEdgeAreDockedTogether) {
    const std::vector<RECT> layout{
        {633, 301, 2272, 1415},   // the browser
        {2272, 301, 2932, 1415},  // the library, beside it
        {2132, 115, 2572, 301},   // the main window, on top of both
        {2572, 208, 2792, 301},   // album art, beside the main window
        {1770, 1414, 2210, 1600}, // the equalizer, a pixel up into the browser
        {2128, 1414, 2496, 1516}, // a visualizer
        {2496, 1414, 2716, 1553}, // the playlist editor
    };
    for (const int group : DockedGroups(layout)) EXPECT_EQ(group, 0);

    std::vector<RECT> apart = layout;
    apart.push_back({100, 1700, 600, 2000});  // nowhere near
    apart.push_back({900, 500, 1500, 900});   // over the browser
    apart.push_back({2932 + 3, 301, 3200, 900}); // three pixels off the library's edge
    apart.push_back({2932, 1415, 3100, 1600}); // only the library's corner
    const std::vector<int> groups = DockedGroups(apart);
    EXPECT_EQ(groups.size(), apart.size());
    for (std::size_t i = layout.size(); i < apart.size(); ++i) {
        EXPECT(groups[i] != groups[0]);
        for (std::size_t j = i + 1; j < apart.size(); ++j) EXPECT(groups[i] != groups[j]);
    }
    EXPECT(groups[0] == 0 && groups[layout.size()] == 1); // numbered as they come

    // Two browser windows side by side, apart from the rest, are one group of their own.
    const std::vector<int> pair = DockedGroups(std::vector<RECT>{{0, 0, 100, 100}, {100, 10, 200, 90}, {500, 500, 600, 600}});
    EXPECT(pair[0] == pair[1] && pair[2] != pair[0]);
}

// A window that isn't docked with the others glows on its own, over them where it lies above
// them: its run ends where theirs begins, and theirs where a window of another group within
// reach comes between.
TEST_CASE(EachGroupGlowsOnItsOwn) {
    const auto inGroup = [](RECT rect, GlowRole role, int group) { return StackedWindow{rect, role, group}; };
    const std::vector<StackedWindow> floating{
        inGroup({300, 300, 500, 500}, GlowRole::Casts, 1), // a browser window over the others
        inGroup({100, 100, 400, 400}, GlowRole::Casts, 0),
        inGroup({400, 100, 700, 400}, GlowRole::Casts, 0),
    };
    const std::vector<GlowRun> runs = FindGlowRuns(floating, kGlowReach);
    EXPECT_EQ(runs.size(), std::size_t{2});
    if (runs.size() == 2) {
        EXPECT(runs[0].group == 1 && runs[0].casting == std::vector<std::size_t>{0} && runs[0].bottom == 0);
        EXPECT(runs[1].group == 0 && runs[1].casting == (std::vector<std::size_t>{1, 2}) && runs[1].bottom == 2);
    }
    // A dialog of another group under the first of a group, within reach, parts it; one of
    // the group's own doesn't.
    const std::vector<StackedWindow> dialog{
        inGroup({100, 100, 400, 400}, GlowRole::Casts, 0),
        inGroup({350, 350, 600, 600}, GlowRole::Winamp, 3),
        inGroup({400, 100, 700, 400}, GlowRole::Casts, 0),
    };
    EXPECT_EQ(FindGlowRuns(dialog, kGlowReach).size(), std::size_t{2});
    std::vector<StackedWindow> own = dialog;
    own[1].group = 0;
    EXPECT_EQ(FindGlowRuns(own, kGlowReach).size(), std::size_t{1});
}
