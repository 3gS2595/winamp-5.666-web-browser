#include "browser.h"

#include "gdiplus_headers.h"
#include "search_engines.h"
#include "tab_parts.h"

#include <algorithm>
#include <array>

namespace exodium {

namespace {

constexpr int kIconSize = kTabIconSize;
constexpr float kCloseCrossInset = 4.0f; // between the close button's edges and its cross
constexpr float kCloseCrossWidth = 1.5f;
constexpr float kSpinnerInset = 2.0f;
constexpr float kSpinnerSweep = 270.0f; // three quarters of a ring
constexpr int kSuggestionPadding = 6;

// A speaker giving out sound, in a square the size of a page's icon.
void DrawSpeaker(HDC dc, int x, int y) {
    Gdiplus::Graphics graphics(dc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::Color inkColor;
    inkColor.SetFromCOLORREF(color::kText);
    const Gdiplus::SolidBrush ink(inkColor);
    const Gdiplus::Pen wave(inkColor, 1.5f);
    const auto at = [x, y](float px, float py) { return Gdiplus::PointF(x + px, y + py); };

    const std::array body{at(1.5f, 6.0f),  at(4.5f, 6.0f),  at(8.5f, 2.5f),
                          at(8.5f, 13.5f), at(4.5f, 10.0f), at(1.5f, 10.0f)};
    graphics.FillPolygon(&ink, body.data(), static_cast<INT>(body.size()));
    // Two waves, a quarter of a circle each, spreading out from the cone's mouth.
    for (const float radius : {3.0f, 6.0f}) {
        graphics.DrawArc(&wave, x + 8.5f - radius, y + 8.0f - radius, 2 * radius, 2 * radius, -45.0f, 90.0f);
    }
}

// Three quarters of a ring, turned by the clock rather than by the frame.
void DrawSpinner(HDC dc, int x, int y) {
    Gdiplus::Graphics graphics(dc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::Color ringColor;
    ringColor.SetFromCOLORREF(color::kSpinner);
    const Gdiplus::Pen ring(ringColor, 2.0f);
    const float size = kIconSize - 2 * kSpinnerInset;
    const float turn = static_cast<float>(::GetTickCount64() % kSpinTurnMs) * 360.0f / kSpinTurnMs;
    graphics.DrawArc(&ring, x + kSpinnerInset, y + kSpinnerInset, size, size, turn, kSpinnerSweep);
}

// A tab's close button: a cross, on a disc while the mouse is over it, and on a lighter
// one while it is held down.
void DrawCloseButton(HDC dc, const TabBox& box, bool hot, bool pressed) {
    Gdiplus::Graphics graphics(dc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    // GDI+ has the middle of a pixel on whole coordinates, so the disc over the button's
    // pixels runs from half a pixel before its first to half a pixel after its last.
    if (hot || pressed) {
        Gdiplus::Color discColor;
        discColor.SetFromCOLORREF(pressed ? color::kClosePressed : color::kCloseHot);
        const Gdiplus::SolidBrush disc(discColor);
        graphics.FillEllipse(&disc, box.left - 0.5f, box.top - 0.5f, static_cast<float>(box.width()),
                             static_cast<float>(box.height()));
    }
    Gdiplus::Color crossColor;
    crossColor.SetFromCOLORREF(hot || pressed ? color::kText : color::kCloseCross);
    const Gdiplus::Pen cross(crossColor, kCloseCrossWidth);
    const float left = box.left + kCloseCrossInset, right = box.right - 1 - kCloseCrossInset;
    const float top = box.top + kCloseCrossInset, bottom = box.bottom - 1 - kCloseCrossInset;
    graphics.DrawLine(&cross, left, top, right, bottom);
    graphics.DrawLine(&cross, left, bottom, right, top);
}

} // namespace

// A tab shows its page's icon, which gives way to a spinner while the tab is busy
// and to a mark once it has crashed. A speaker towards its other end, as in Chrome, says
// that its page is making sound, and a close button is at the very end, lit while the
// mouse is over it. A narrow tab leaves out what it has no room for: see tab_parts.h.
// Each of several tabs selected together has a border of its own colour, and those not
// in view are a shade lighter than the rest.
void DrawOwnerButton(const DRAWITEMSTRUCT& item, const Tab* tab, const ButtonLook& look) {
    // Drawn off screen and copied over in one go, so that a turning spinner never
    // shows half painted.
    const RECT rect = item.rcItem;
    const UniqueDc buffer(::CreateCompatibleDC(item.hDC));
    const UniqueBitmap bitmap(::CreateCompatibleBitmap(item.hDC, rect.right, rect.bottom));
    const HDC dc = buffer.get();
    const SelectedObject selectedBitmap(dc, bitmap.get());
    const SelectedObject selectedFont(dc, g_app.theme.font);

    const Theme& theme = g_app.theme;
    const bool inSelection = tab && look.inSelection;
    const UniqueBrush& fill = tab && look.active ? theme.activeTab : inSelection ? theme.selectedTab : theme.panel;
    ::FillRect(dc, &rect, fill.get());
    ::FrameRect(dc, &rect, (inSelection ? theme.selectedBorder : theme.border).get());
    ::SetBkMode(dc, TRANSPARENT);

    RECT textRect = rect;
    if (tab) {
        const bool busy = tab->busy();
        const TabParts parts = PlaceTabParts({rect.left, rect.top, rect.right, rect.bottom},
                                             {.icon = busy || tab->crashed || tab->favicon,
                                              .speaker = tab->playingAudio,
                                              .closeButton = g_app.settings.tabCloseButtons,
                                              .active = look.closeAlways});
        if (const TabBox& icon = parts.icon; !icon.empty()) {
            if (busy) {
                DrawSpinner(dc, icon.left, icon.top);
            } else if (tab->crashed) {
                RECT mark{icon.left, icon.top, icon.right, icon.bottom};
                ::SetTextColor(dc, color::kCrashed);
                ::DrawTextW(dc, L"!", 1, &mark, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            } else {
                ::DrawIconEx(dc, icon.left, icon.top, tab->favicon.get(), kIconSize, kIconSize, 0, nullptr, DI_NORMAL);
            }
        }
        if (!parts.speaker.empty()) DrawSpeaker(dc, parts.speaker.left, parts.speaker.top);
        if (!parts.close.empty()) DrawCloseButton(dc, parts.close, look.closeHot, look.closePressed);
        textRect = {parts.title.left, parts.title.top, parts.title.right, parts.title.bottom};
    }

    // DT_NOPREFIX: an ampersand in a page's title is an ampersand, not a shortcut mark.
    const std::wstring text = look.text ? *look.text : WindowText(item.hwndItem);
    ::SetTextColor(dc, color::kText);
    ::DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &textRect,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);

    if (item.itemState & ODS_FOCUS) {
        RECT focus = textRect;
        ::InflateRect(&focus, -3, -3);
        // A tab too narrow to show its title has none to go round.
        if (!::IsRectEmpty(&focus)) ::DrawFocusRect(dc, &focus);
    }

    ::BitBlt(item.hDC, rect.left, rect.top, Width(rect), Height(rect), dc, rect.left, rect.top, SRCCOPY);
}

void DrawSuggestion(const BrowserWindow& win, const DRAWITEMSTRUCT& item) {
    if (item.itemID >= win.suggestions.size()) return; // an empty list asks for item -1
    const Suggestion& suggestion = win.suggestions[item.itemID];
    const HDC dc = item.hDC;
    RECT rect = item.rcItem;

    const Theme& theme = g_app.theme;
    ::FillRect(dc, &rect, ((item.itemState & ODS_SELECTED) ? theme.suggestSelected : theme.panel).get());

    // A page reads "Title - address" and a search "words - Google Search", or whichever
    // engine is in use. An address whose title isn't known stands alone.
    const bool titled = !suggestion.search && !suggestion.title.empty();
    const std::wstring& lead = titled ? suggestion.title : suggestion.text;
    std::wstring tail;
    if (suggestion.search) tail = L" - " + SearchLabel();
    else if (titled) tail = L" - " + suggestion.text;

    ::SetBkMode(dc, TRANSPARENT);
    const SelectedObject selectedFont(dc, theme.font);
    ::InflateRect(&rect, -kSuggestionPadding, 0);
    SIZE leadSize{}, tailSize{};
    ::GetTextExtentPoint32W(dc, lead.c_str(), static_cast<int>(lead.size()), &leadSize);
    ::GetTextExtentPoint32W(dc, tail.c_str(), static_cast<int>(tail.size()), &tailSize);

    // When the two don't fit, the first gives way, but not past half the row.
    const int width = Width(rect);
    RECT leadRect = rect;
    leadRect.right = leadRect.left + std::min<int>(leadSize.cx, std::max<int>(width - tailSize.cx, width / 2));
    RECT tailRect = rect;
    tailRect.left = leadRect.right;

    const UINT format = DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX;
    ::SetTextColor(dc, (suggestion.search || titled) ? color::kText : color::kSuggestUrl);
    ::DrawTextW(dc, lead.c_str(), static_cast<int>(lead.size()), &leadRect, format);
    ::SetTextColor(dc, suggestion.search ? color::kSuggestDim : color::kSuggestUrl);
    ::DrawTextW(dc, tail.c_str(), static_cast<int>(tail.size()), &tailRect, format);
}

} // namespace exodium
