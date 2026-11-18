#include "test_framework.h"

#include "corner_skins.h"

#include <set>
#include <string>

using namespace exodium;

// Windows have the ironwork they always had unless another skin is picked.
TEST_CASE(CornerSkinsStartWithIron) {
    EXPECT_EQ(std::wstring(kCornerSkins.front().id), L"iron");
    EXPECT(kCornerSkins.front().pictures[0] == IDR_CORNER_ORNAMENT);
}

TEST_CASE(CornerSkinsAreFoundByTheirIdsInAnyCase) {
    std::set<std::wstring> ids;
    for (const CornerSkin& skin : kCornerSkins) {
        EXPECT(ids.insert(ToLower(skin.id)).second);
        EXPECT(FindCornerSkin(skin.id) == &skin);
        EXPECT(!skin.name.empty());
    }
    EXPECT(FindCornerSkin(L"VeNoM") == FindCornerSkin(L"venom"));
    EXPECT(FindCornerSkin(L"venom") != nullptr);
    EXPECT(FindCornerSkin(L"charms") != nullptr);
    EXPECT(FindCornerSkin(L"DarkCharms") == FindCornerSkin(L"darkcharms"));
    EXPECT(FindCornerSkin(L"darkcharms") != nullptr);
    EXPECT(!FindCornerSkin(L"chrome"));
    EXPECT(!FindCornerSkin(L""));
}

// What ornament.cpp takes from a skin's measurements holds together: the frame's corner
// on the picture, with the bar's tip and the pod along the bar to its left, and the
// buttons in the pod, side by side. Where the buttons sit in the bar itself, as on Charms,
// the pod is all of the bar and begins before its tip ends.
TEST_CASE(CornerSkinsMeasureUp) {
    for (const CornerSkin& skin : kCornerSkins) {
        for (const int picture : skin.pictures) EXPECT(picture != 0);
        EXPECT(0 < skin.corner.x && skin.corner.x < skin.size.x);
        EXPECT(0 < skin.corner.y && skin.corner.y < skin.podBottom && skin.podBottom < skin.size.y);
        EXPECT(0 < skin.tipRight && skin.tipRight < skin.corner.x);
        EXPECT(0 < skin.podLeft && skin.podLeft < skin.corner.x);
        EXPECT(0 < skin.shoulderTopRight && skin.shoulderTopRight < skin.corner.x);
        EXPECT(0 < skin.shoulderTopLeft && skin.shoulderTopLeft < skin.corner.x);
        const int r = skin.buttonRadius;
        EXPECT(r > 0);
        for (std::size_t i = 0; i < skin.buttons.size(); ++i) {
            const ArtPoint b = skin.buttons[i];
            EXPECT(b.x - r >= skin.podLeft && b.x + r <= skin.size.x && b.y - r >= 0 && b.y + r <= skin.podBottom);
            if (i > 0) EXPECT(b.x - skin.buttons[i - 1].x >= 2 * r);
        }
    }
}
