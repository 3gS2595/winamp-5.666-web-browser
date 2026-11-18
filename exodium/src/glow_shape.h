// The black glow round Winamp's windows, worked out from where the windows are: which of
// them glow together, how far the glow goes, and its picture. Nothing here has a window in
// it; glow.cpp finds the windows and shows the glow.
//
// The glow is a shadow under the windows. Windows docked together, edge to edge as Winamp
// docks them, are one: the glow goes round their outline as one, ornaments and window regions
// included, and never lies over any of them. A window that isn't docked to the others, such
// as a browser window put down apart from them, or over them, is one of its own: its glow
// lies over the windows under it, Winamp's too, as theirs does over it when it is under them.
// A picture of the glow goes right under the lowest of the windows it goes round, so the
// windows hide its middle and whatever is under them has it lying over it.
//
// Windows of other programs, or of Winamp's not docked with them, can come between docked
// windows in the Z order, and then one picture can't be under all of them and over those as
// well. The windows are then taken in runs: as many docked ones as lie together in the Z
// order, from the top, with no other window between them where their glow would lie over it.
// Each run has a picture of its own.
#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace exodium {

// How the glow looks, as Exodium's page in Winamp's Preferences has it (settings.h keeps it).
// As it is to begin with, it matches the mock-up the glow was asked for with: black at the
// edge, fading to nothing over some 54 pixels.
struct GlowLook {
    int size = 60;           // how far past a window's edge it goes, in Winamp's pixels
    int opacity = 100;       // how solid it is at the edge, in percent
    int spread = 0;          // how far out it keeps that before it fades, in percent
    std::uint32_t color = 0; // 0xRRGGBB

    friend bool operator==(const GlowLook&, const GlowLook&) = default;
};
inline constexpr int kGlowSizeLeast = 10, kGlowSizeMost = 200;
inline constexpr int kGlowSpreadMost = 90;

// The glow is worked out on a grid of squares `step` pixels a side, smoothed over `smoothing`
// squares each way three times over, which takes it 3 * smoothing * step pixels out and no
// further: as near to the size asked for as that comes.
struct GlowGrid {
    int step = 4;
    int smoothing = 5;
    [[nodiscard]] constexpr int reach() const noexcept { return 3 * smoothing * step; }
};
[[nodiscard]] GlowGrid GlowGridFor(int size) noexcept;

// How it fades out from the edge: the cover the smoothing leaves round a pixel, from half at
// the edge, raised to kGlowCurve, less kGlowFloor of it. That was matched to the mock-up.
inline constexpr double kGlowCurve = 0.85;
inline constexpr double kGlowFloor = 0.03;

// A window as the glow sees it, among the others, which are listed from the top of the Z
// order down.
enum class GlowRole {
    Casts,  // the glow goes round it
    Winamp, // another of Winamp's windows: no glow lies over it, and it parts no run
    Other,  // another program's window
};
struct StackedWindow {
    RECT rect{}; // on the screen
    GlowRole role = GlowRole::Other;
    int group = -1; // for Winamp's: which windows it is docked with (DockedGroups)
};

// Windows that glow together, by their places in the list: those the glow goes round, and
// the lowest of them, which the run's picture goes right under. They are all of one group.
struct GlowRun {
    std::vector<std::size_t> casting;
    std::size_t bottom = 0;
    int group = -1;
};

// The runs, from the top. A window of another group, or of another program, parts a run where
// it lies within `reach` of a window above it in the run: under the run's picture, it would
// have the glow of the windows above it lie over it, and over the picture, it would hide
// that glow. A window of another group the glow goes round starts a run of its own.
[[nodiscard]] std::vector<GlowRun> FindGlowRuns(std::span<const StackedWindow> stack, int reach);

// How far apart two windows' edges can be, in pixels either way, for them to count as docked.
inline constexpr int kDockSlack = 2;

// Which windows are docked together, by their places in the list: a group number for each,
// from 0. Two windows are docked where an edge of one lies along an edge of the other, as
// Winamp docks its windows, within `slack`; windows docked to one docked to another are in
// its group too. Windows that only overlap, or meet at a corner, aren't docked.
[[nodiscard]] std::vector<int> DockedGroups(std::span<const RECT> windows, int slack = kDockSlack);

// What a window covers: its rectangle, or the parts of it that a window region keeps, or
// that other windows leave in sight; and how solid each pixel is, for a see-through window
// such as a corner ornament.
struct GlowShape {
    RECT rect{};                         // on the screen
    std::vector<RECT> parts;             // on the screen, within rect; none for all of rect
    std::span<const std::uint8_t> alpha; // for each of rect's pixels, top row first; none for solid
};

// Where a picture of the glow round `casting` goes: round them as far as the glow reaches,
// as far as `screen` goes. Empty when nothing casts it.
[[nodiscard]] RECT GlowArea(std::span<const GlowShape> casting, int reach, const RECT& screen);

// Buffers PaintGlow works in, kept from one picture to the next.
struct GlowScratch {
    std::vector<std::uint8_t> casting;
    std::vector<std::uint8_t> clear;
    std::vector<float> grid;
    std::vector<float> smoothed;
    std::vector<int> cellOf;
    std::vector<float> intoCell;
    std::vector<float> shades;
};

// Paints the glow round `casting` over `area`, kept off `keptClear`, into `pixels`: area's
// pixels, premultiplied BGRA, top row first.
void PaintGlow(const RECT& area, std::span<const GlowShape> casting, std::span<const GlowShape> keptClear,
               const GlowLook& look, std::span<std::uint32_t> pixels, GlowScratch& scratch);

} // namespace exodium
