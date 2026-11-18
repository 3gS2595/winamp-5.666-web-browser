// The picture the media library shows for an album without a cover: the llama in its
// own ml_local.dll (a PNG, resource 169), tinted with the skin's text colour as the
// library tints it.
#pragma once

#include <windows.h>

namespace covers {

// Premultiplied, top-down, `size` square; null if ml_local.dll or its picture isn't
// there. Kept until FreeNoArtPictures.
[[nodiscard]] HBITMAP NoArtPicture(COLORREF tint, int& size);
void FreeNoArtPictures();

} // namespace covers
