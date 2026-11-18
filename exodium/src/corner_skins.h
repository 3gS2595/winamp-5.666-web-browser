// The skins a browser window's corner ornaments can have, one of which Exodium's page in
// Winamp's Preferences picks; ornament.cpp puts them on the corners. Nothing here has a
// window in it.
//
// A skin is four pictures, one for each corner, built into the plugin (res\exodium.rc).
// Each is drawn for the top right corner and turned to its own: mirrored for the corners
// on the left, stood on its head for the ones on the bottom. The four are the same size,
// with the frame's corner on the same point of them. The one for the top right has the
// buttons that minimize, maximize and close the window.
//
// tools\cut-corner-skin.py cuts a new skin's pictures out of a picture of its four
// corners, and measures them for its line below.
#pragma once

#include "resource.h"
#include "text_util.h"

#include <algorithm>
#include <array>
#include <string_view>

namespace exodium {

// A point of a skin's pictures, in their own pixels.
struct ArtPoint {
    int x = 0;
    int y = 0;
};

struct CornerSkin {
    std::wstring_view id;   // as winamp.ini keeps it
    std::wstring_view name; // as the Preferences list it
    // The pictures' resources, by the corner they go on: top right, top left, bottom
    // right and bottom left.
    std::array<int, 4> pictures{};
    ArtPoint size;
    // The point of them that lies on the frame's corner: the underside of the bar along
    // the top, and a little way into the upright that hangs down the side.
    ArtPoint corner;
    // The top right one's pod, which holds the buttons: where it begins, and where it
    // ends, with the upright below it.
    int podLeft = 0;
    int podBottom = 0;
    int tipRight = 0; // the bar's far tip, which hangs lower than the bar, ends here
    // How far left the ironwork coming down the side reaches over the tab strip, on the
    // top right one and on the top left one.
    int shoulderTopRight = 0;
    int shoulderTopLeft = 0;
    std::array<ArtPoint, 3> buttons{}; // the middles of minimize, maximize and close
    int buttonRadius = 0;
};

// The first is the one windows have unless another is picked.
inline constexpr std::array kCornerSkins{
    // Wrought iron and chains, drawn by hand: one piece, with a plain plate where the
    // buttons would be on the other three corners.
    CornerSkin{
        .id = L"iron",
        .name = L"Iron",
        .pictures = {IDR_CORNER_ORNAMENT, IDR_CORNER_ORNAMENT_PLAIN, IDR_CORNER_ORNAMENT_PLAIN,
                     IDR_CORNER_ORNAMENT_PLAIN},
        .size = {748, 496},
        .corner = {674, 86},
        .podLeft = 430,
        .podBottom = 140,
        .tipRight = 150,
        .shoulderTopRight = 617,
        .shoulderTopLeft = 617,
        .buttons = {{{552, 79}, {618, 78}, {685, 79}}},
        .buttonRadius = 29,
    },
    // Purple and green chrome, a different piece on each corner. Cut from
    // res\sources\venom.png with
    //   python tools\cut-corner-skin.py exodium\res\sources\venom.png venom
    //       --buttons 1340,81 1405,81 1471,81 --pod-bottom 153
    CornerSkin{
        .id = L"venom",
        .name = L"Venom",
        .pictures = {IDR_CORNER_VENOM_TOP_RIGHT, IDR_CORNER_VENOM_TOP_LEFT, IDR_CORNER_VENOM_BOTTOM_RIGHT,
                     IDR_CORNER_VENOM_BOTTOM_LEFT},
        .size = {731, 496},
        .corner = {668, 86},
        .podLeft = 397,
        .podBottom = 156,
        .tipRight = 145,
        .shoulderTopRight = 456,
        .shoulderTopLeft = 637,
        .buttons = {{{542, 84}, {607, 84}, {673, 84}}},
        .buttonRadius = 29,
    },
    // Silver filigree crusted with charms, a different piece on each corner: a star, a
    // square and a heart with a cross for buttons, in a bar thick enough that, tucked into
    // the corner, all of it lies over the tab strip. Cut from res\sources\charms.png with
    //   python tools\cut-corner-skin.py exodium\res\sources\charms.png charms
    //       --buttons 1217,85 1301,90 1383,87 --button-radius 34 --pod-bottom 128 --split 724,521
    CornerSkin{
        .id = L"charms",
        .name = L"Charms",
        .pictures = {IDR_CORNER_CHARMS_TOP_RIGHT, IDR_CORNER_CHARMS_TOP_LEFT, IDR_CORNER_CHARMS_BOTTOM_RIGHT,
                     IDR_CORNER_CHARMS_BOTTOM_LEFT},
        .size = {719, 552},
        .corner = {640, 142},
        .podLeft = 17,
        .podBottom = 148,
        .tipRight = 50,
        .shoulderTopRight = 475,
        .shoulderTopLeft = 498,
        .buttons = {{{488, 105}, {572, 110}, {654, 107}}},
        .buttonRadius = 34,
    },
    // The same pieces in dark bronze and plum, from a picture with a see-through
    // background. Cut from res\sources\darkcharms.png with
    //   python tools\cut-corner-skin.py exodium\res\sources\darkcharms.png darkcharms
    //       --buttons 1217,84 1297,84 1382,82 --button-radius 34 --pod-bottom 128 --split 724,525
    CornerSkin{
        .id = L"darkcharms",
        .name = L"Dark Charms",
        .pictures = {IDR_CORNER_DARKCHARMS_TOP_RIGHT, IDR_CORNER_DARKCHARMS_TOP_LEFT, IDR_CORNER_DARKCHARMS_BOTTOM_RIGHT,
                     IDR_CORNER_DARKCHARMS_BOTTOM_LEFT},
        .size = {717, 555},
        .corner = {640, 146},
        .podLeft = 13,
        .podBottom = 150,
        .tipRight = 84,
        .shoulderTopRight = 474,
        .shoulderTopLeft = 499,
        .buttons = {{{487, 106}, {567, 106}, {652, 104}}},
        .buttonRadius = 34,
    },
};

// The skin with this id, in any case, or nothing.
[[nodiscard]] inline const CornerSkin* FindCornerSkin(std::wstring_view id) noexcept {
    const auto found =
        std::ranges::find_if(kCornerSkins, [id](const CornerSkin& skin) { return EqualsIgnoreCase(skin.id, id); });
    return found == kCornerSkins.end() ? nullptr : &*found;
}

} // namespace exodium
