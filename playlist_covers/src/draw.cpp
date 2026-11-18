#include "draw.h"

#define WA_DLG_IMPLEMENT
#include <wa_dlg.h>

#include <algorithm>

namespace covers {

COLORREF Mix(COLORREF a, COLORREF b, int perThousandOfB) {
    const auto mix = [perThousandOfB](int ca, int cb) { return (ca * (1000 - perThousandOfB) + cb * perThousandOfB) / 1000; };
    return RGB(mix(GetRValue(a), GetRValue(b)), mix(GetGValue(a), GetGValue(b)), mix(GetBValue(a), GetBValue(b)));
}

int Luma(COLORREF color) {
    return (GetRValue(color) * 299 + GetGValue(color) * 587 + GetBValue(color) * 114) / 1000;
}

Box Moved(Box box, int dx, int dy) {
    box.left += dx;
    box.right += dx;
    box.top += dy;
    box.bottom += dy;
    return box;
}

Box Inset(Box box, int by) {
    return {box.left + by, box.top + by, box.right - by, box.bottom - by};
}

void Fill(HDC dc, const Box& box, COLORREF color) {
    if (box.Width() <= 0 || box.Height() <= 0) return;
    ::SetDCBrushColor(dc, color);
    const RECT rect = ToRect(box);
    ::FillRect(dc, &rect, static_cast<HBRUSH>(::GetStockObject(DC_BRUSH)));
}

void Outline(HDC dc, const Box& box, COLORREF color, int thickness) {
    Fill(dc, {box.left, box.top, box.right, box.top + thickness}, color);
    Fill(dc, {box.left, box.bottom - thickness, box.right, box.bottom}, color);
    Fill(dc, {box.left, box.top + thickness, box.left + thickness, box.bottom - thickness}, color);
    Fill(dc, {box.right - thickness, box.top + thickness, box.right, box.bottom - thickness}, color);
}

void Text(HDC dc, std::wstring_view text, const Box& box, COLORREF color, UINT format) {
    if (text.empty() || box.Width() <= 0) return;
    ::SetTextColor(dc, color);
    RECT rect = ToRect(box);
    ::DrawTextW(dc, text.data(), static_cast<int>(text.size()), &rect,
                format | DT_SINGLELINE | DT_NOPREFIX | DT_VCENTER | DT_END_ELLIPSIS);
}

void PlayMark(HDC dc, const Box& box, int status, COLORREF color) {
    if (status == 3) {
        const int bar = std::max(2, box.Width() / 3);
        Fill(dc, {box.left, box.top, box.left + bar, box.bottom}, color);
        Fill(dc, {box.right - bar, box.top, box.right, box.bottom}, color);
        return;
    }
    const POINT points[] = {{box.left, box.top}, {box.right, (box.top + box.bottom) / 2}, {box.left, box.bottom}};
    ::SetDCBrushColor(dc, color);
    ::SetDCPenColor(dc, color);
    const HGDIOBJ oldBrush = ::SelectObject(dc, ::GetStockObject(DC_BRUSH));
    const HGDIOBJ oldPen = ::SelectObject(dc, ::GetStockObject(DC_PEN));
    ::Polygon(dc, points, 3);
    ::SelectObject(dc, oldPen);
    ::SelectObject(dc, oldBrush);
}

} // namespace covers
