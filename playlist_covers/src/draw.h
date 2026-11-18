// Drawing in the skin's colours, which Winamp gives its media library (wa_dlg.h).
#pragma once

#include "layout.h"

#include <windows.h>

#include <wa_dlg.h>

#include <string_view>

namespace covers {

[[nodiscard]] inline COLORREF SkinColor(int index) {
    return static_cast<COLORREF>(WADlg_getColor(index));
}
[[nodiscard]] COLORREF Mix(COLORREF a, COLORREF b, int perThousandOfB);
[[nodiscard]] int Luma(COLORREF color);

[[nodiscard]] inline RECT ToRect(const Box& box) {
    return {box.left, box.top, box.right, box.bottom};
}
[[nodiscard]] Box Moved(Box box, int dx, int dy);
[[nodiscard]] Box Inset(Box box, int by);

void Fill(HDC dc, const Box& box, COLORREF color);
void Outline(HDC dc, const Box& box, COLORREF color, int thickness = 1);
// One line, cut short with "..." if it doesn't fit.
void Text(HDC dc, std::wstring_view text, const Box& box, COLORREF color, UINT format = DT_LEFT);
// A play triangle, or a pause sign (Winamp's status 3), filling `box`.
void PlayMark(HDC dc, const Box& box, int status, COLORREF color);

} // namespace covers
