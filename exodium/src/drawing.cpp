#include "browser.h"

#include "gdiplus_headers.h"

#include <algorithm>
#include <array>

namespace exodium {

namespace {

constexpr int kIconSize = 16;
constexpr int kIconPadding = 4;
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

} // namespace

// A tab shows its page's icon, which gives way to a spinner while the tab is busy
// and to a mark once it has crashed. A speaker at its other end, as in Chrome, says
// that its page is making sound. In a tab too narrow for both, the speaker is shown
// in place of the icon.
void DrawOwnerButton(const DRAWITEMSTRUCT& item, const Tab* tab, bool isActive) {
    // Drawn off screen and copied over in one go, so that a turning spinner never
    // shows half painted.
    const RECT rect = item.rcItem;
    const UniqueDc buffer(::CreateCompatibleDC(item.hDC));
    const UniqueBitmap bitmap(::CreateCompatibleBitmap(item.hDC, rect.right, rect.bottom));
    const HDC dc = buffer.get();
    const SelectedObject selectedBitmap(dc, bitmap.get());
    const SelectedObject selectedFont(dc, g_app.theme.font);

    const Theme& theme = g_app.theme;
    ::FillRect(dc, &rect, (tab && isActive ? theme.activeTab : theme.panel).get());
    ::FrameRect(dc, &rect, theme.border.get());
    ::SetBkMode(dc, TRANSPARENT);

    RECT textRect = rect;
    const int iconY = rect.top + (Height(rect) - kIconSize) / 2;
    const bool busy = tab && tab->busy();
    const bool sounding = tab && tab->playingAudio;
    const bool speakerOnly = sounding && Width(rect) < 2 * (kIconSize + 2 * kIconPadding);
    if (sounding && !speakerOnly) {
        textRect.right -= kIconSize + kIconPadding * 2;
        DrawSpeaker(dc, textRect.right + kIconPadding, iconY);
    }
    if (tab && (busy || tab->crashed || tab->favicon || speakerOnly)) {
        const int iconX = rect.left + kIconPadding;
        if (speakerOnly) {
            DrawSpeaker(dc, iconX, iconY);
        } else if (busy) {
            DrawSpinner(dc, iconX, iconY);
        } else if (tab->crashed) {
            RECT mark{iconX, iconY, iconX + kIconSize, iconY + kIconSize};
            ::SetTextColor(dc, color::kCrashed);
            ::DrawTextW(dc, L"!", 1, &mark, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        } else {
            ::DrawIconEx(dc, iconX, iconY, tab->favicon.get(), kIconSize, kIconSize, 0, nullptr, DI_NORMAL);
        }
        textRect.left += kIconSize + kIconPadding * 2;
    }

    // DT_NOPREFIX: an ampersand in a page's title is an ampersand, not a shortcut mark.
    const std::wstring text = WindowText(item.hwndItem);
    ::SetTextColor(dc, color::kText);
    ::DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &textRect,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);

    if (item.itemState & ODS_FOCUS) {
        RECT focus = textRect;
        ::InflateRect(&focus, -3, -3);
        ::DrawFocusRect(dc, &focus);
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

    // A page reads "Title - address" and a search "words - Google Search". An address
    // whose title isn't known stands alone.
    const bool titled = !suggestion.search && !suggestion.title.empty();
    const std::wstring& lead = titled ? suggestion.title : suggestion.text;
    std::wstring tail;
    if (suggestion.search) tail = L" - Google Search";
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
