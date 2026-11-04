// The downloads button on a window's toolbar, and the panel it opens: the latest downloads,
// newest first, each with its file's name, how it is going, and buttons to pause, resume or
// cancel it, or to find the file in its folder. A click on a finished one opens its file.
// What is in the list, and what the buttons do to a download, is downloads.cpp's.
//
// Neither ever takes the keyboard or makes a window active: the page keeps the keyboard
// while the panel is open, and with it the browser's shortcuts. Escape closes the panel
// wherever the keyboard is in the window (HandleGlobalShortcut). So do a click anywhere
// outside it, its own close button and the toolbar button. One that came up by itself, for
// a download that started, also goes again after a few seconds unless the mouse is on it.
//
// Every window has a button and a panel of its own, and each lists every download: they
// are the browser's, not a tab's. The panel comes up by itself only in the window the
// download was started from. It is a child of the window's client, in front of the page's
// top right corner, as the address bar's suggestions are in front of the page under it,
// so it moves with the window and is put back in its corner whenever the window is laid
// out.
#include "browser.h"

#include "gdiplus_headers.h"

#include <windowsx.h>

#include <algorithm>
#include <array>
#include <optional>
#include <utility>
#include <vector>

namespace exodium {

namespace {

constexpr int kDownloadsButtonWidth = 28; // its height is the toolbar's
constexpr int kPanelWidth = 360;
constexpr int kPanelTopGap = 2; // between the toolbar and the panel
constexpr int kHeaderHeight = 34;
constexpr int kRowHeight = 46;
constexpr int kPadding = 10; // inside the panel's edges
constexpr int kBottomPadding = 6;
constexpr int kMaxRows = 8;
constexpr int kIconSize = 16;
constexpr int kIconGap = 10;         // between a row's icon and its text
constexpr int kActionSize = 24;      // the buttons in the header and on each row
constexpr int kTextActionWidth = 50; // a held download's Keep and Delete, which say so in words
constexpr int kActionGap = 2;
constexpr int kClearWidth = 44;
constexpr int kTextTop = 7; // a row's first line, from the row's top
constexpr int kLineGap = 2; // between a row's two lines
constexpr int kBarGap = 3;  // between the second line and the progress bar
constexpr int kBarHeight = 3;
constexpr COLORREF kHotAction = RGB(72, 72, 72);
constexpr float kRingRadius = 8.0f; // round the toolbar button's arrow while downloads are going

constexpr UINT_PTR kPanelTimer = 1;
constexpr UINT kPanelTickMs = 40;        // how often the open panel looks at the mouse
constexpr ULONGLONG kAutoCloseMs = 5000; // how long one that came up by itself stays
constexpr ULONGLONG kRefreshMs = 1000;   // how often speeds and times left are worked out again

// The parts of the panel that do something when clicked.
enum Part : int { kNone, kClear, kFolder, kClose, kRow, kPause, kResume, kCancel, kShowInFolder, kKeep, kDelete };

struct Hit {
    int part = kNone;
    int downloadId = 0; // the download a row's part belongs to

    friend bool operator==(const Hit&, const Hit&) = default;
};

[[nodiscard]] bool IsOpen(const DownloadsUi& ui) noexcept {
    return ui.panel && HasStyle(ui.panel, WS_VISIBLE);
}

[[nodiscard]] bool IsFinished(const Download& download) noexcept {
    return download.phase == DownloadPhase::Completed || download.phase == DownloadPhase::Canceled ||
           download.phase == DownloadPhase::Failed;
}

[[nodiscard]] bool CanOpen(const Download& download) noexcept {
    return download.phase == DownloadPhase::Completed && !download.fileGone;
}

[[nodiscard]] bool IsHeld(const Download& download) noexcept {
    return download.keepDeferral != nullptr;
}

// ---- where things are in the panel ----

// How many downloads a panel of this size shows, newest first.
[[nodiscard]] int RowsShown(const RECT& panel) noexcept {
    const int fit = (Height(panel) - kHeaderHeight - kBottomPadding) / kRowHeight;
    return std::clamp(fit, 0, static_cast<int>(g_app.downloads.size()));
}

[[nodiscard]] RECT RowRect(const RECT& panel, int row) noexcept {
    const int top = kHeaderHeight + row * kRowHeight;
    return {1, top, panel.right - 1, top + kRowHeight};
}

// The buttons along the right of the header, counted from its right end.
[[nodiscard]] RECT ActionRect(const RECT& strip, int fromRight) noexcept {
    const int right = strip.right - kPadding / 2 - fromRight * (kActionSize + kActionGap);
    const int top = strip.top + (Height(strip) - kActionSize) / 2;
    return {right - kActionSize, top, right, top + kActionSize};
}

[[nodiscard]] RECT HeaderRect(const RECT& panel) noexcept {
    return {0, 0, panel.right, kHeaderHeight};
}

// The header's buttons, from its right end: close, the downloads folder, and Clear while
// there is anything finished to clear.
[[nodiscard]] RECT CloseRect(const RECT& panel) noexcept {
    return ActionRect(HeaderRect(panel), 0);
}
[[nodiscard]] RECT FolderRect(const RECT& panel) noexcept {
    return ActionRect(HeaderRect(panel), 1);
}
[[nodiscard]] std::optional<RECT> ClearRect(const RECT& panel) {
    if (std::ranges::none_of(g_app.downloads, IsFinished)) return std::nullopt;
    const RECT folder = FolderRect(panel);
    return RECT{folder.left - kActionGap - kClearWidth, folder.top, folder.left - kActionGap, folder.bottom};
}

// A row's buttons, from its right end.
[[nodiscard]] std::vector<int> RowActions(const Download& download) {
    if (IsHeld(download)) return {kDelete, kKeep};
    const bool followed = download.operation != nullptr;
    switch (download.phase) {
    case DownloadPhase::InProgress:
        return followed ? std::vector{int{kCancel}, int{kPause}} : std::vector<int>{};
    case DownloadPhase::Paused:
        return followed ? std::vector{int{kCancel}, int{kResume}} : std::vector<int>{};
    case DownloadPhase::Failed:
        return followed && download.canResume ? std::vector{int{kResume}} : std::vector<int>{};
    case DownloadPhase::Completed:
        return download.fileGone ? std::vector<int>{} : std::vector{int{kShowInFolder}};
    case DownloadPhase::Canceled:
        break;
    }
    return {};
}

[[nodiscard]] int ActionWidth(int part) noexcept {
    return part == kKeep || part == kDelete ? kTextActionWidth : kActionSize;
}

// The button at `index` of a row's buttons, as RowActions gives them.
[[nodiscard]] RECT RowActionRect(const RECT& row, const std::vector<int>& actions, std::size_t index) {
    int right = row.right - kPadding / 2;
    for (std::size_t i = 0; i < index; ++i) right -= ActionWidth(actions[i]) + kActionGap;
    const int top = row.top + (kRowHeight - kActionSize) / 2;
    return {right - ActionWidth(actions[index]), top, right, top + kActionSize};
}

[[nodiscard]] Hit HitTest(HWND panel, POINT point) {
    const RECT rect = ClientRectOf(panel);
    if (!::PtInRect(&rect, point)) return {};
    if (point.y < kHeaderHeight) {
        if (const RECT close = CloseRect(rect); ::PtInRect(&close, point)) return {.part = kClose};
        if (const RECT folder = FolderRect(rect); ::PtInRect(&folder, point)) return {.part = kFolder};
        if (const auto clear = ClearRect(rect); clear && ::PtInRect(&*clear, point)) return {.part = kClear};
        return {};
    }
    for (int row = 0; row < RowsShown(rect); ++row) {
        const RECT box = RowRect(rect, row);
        if (!::PtInRect(&box, point)) continue;
        const Download& download = g_app.downloads[static_cast<std::size_t>(row)];
        const std::vector<int> actions = RowActions(download);
        for (std::size_t i = 0; i < actions.size(); ++i) {
            if (const RECT action = RowActionRect(box, actions, i); ::PtInRect(&action, point)) {
                return {.part = actions[i], .downloadId = download.id};
            }
        }
        return {.part = kRow, .downloadId = download.id};
    }
    return {};
}

// Whether a click on this part does anything, which the pointer shows.
[[nodiscard]] bool IsClickable(const Hit& hit) {
    if (hit.part != kRow) return hit.part != kNone;
    const Download* const download = FindDownload(hit.downloadId);
    return download && CanOpen(*download);
}

// ---- drawing ----

// What the whole of the downloads still to finish add up to, for the toolbar button's ring.
struct Overall {
    bool going = false;         // there are downloads in progress or paused
    bool moving = false;        // and they aren't all paused
    std::optional<double> done; // how much of them is in, unless one of them is of a size not told
};

[[nodiscard]] Overall OverallProgress() noexcept {
    Overall overall;
    std::int64_t received = 0, total = 0;
    bool sizeUnknown = false;
    for (const Download& download : g_app.downloads) {
        if (download.phase != DownloadPhase::InProgress && download.phase != DownloadPhase::Paused) continue;
        overall.going = true;
        overall.moving = overall.moving || download.phase == DownloadPhase::InProgress;
        if (download.total <= 0) {
            sizeUnknown = true;
        } else {
            received += std::min(download.received, download.total);
            total += download.total;
        }
    }
    if (overall.going && !sizeUnknown && total > 0) {
        overall.done = static_cast<double>(received) / static_cast<double>(total);
    }
    return overall;
}

[[nodiscard]] Gdiplus::Color GdiplusColor(COLORREF color) {
    Gdiplus::Color result;
    result.SetFromCOLORREF(color);
    return result;
}

// A button's picture, in the middle of `box`.
void DrawGlyph(Gdiplus::Graphics& graphics, int part, const RECT& box, COLORREF ink) {
    const Gdiplus::Color color = GdiplusColor(ink);
    const Gdiplus::Pen pen(color, 1.5f);
    const Gdiplus::SolidBrush brush(color);
    const float x = (box.left + box.right) / 2.0f, y = (box.top + box.bottom) / 2.0f;
    switch (part) {
    case kClose:
    case kCancel:
        graphics.DrawLine(&pen, x - 4.0f, y - 4.0f, x + 4.0f, y + 4.0f);
        graphics.DrawLine(&pen, x - 4.0f, y + 4.0f, x + 4.0f, y - 4.0f);
        break;
    case kPause:
        graphics.FillRectangle(&brush, x - 4.0f, y - 5.0f, 2.5f, 10.0f);
        graphics.FillRectangle(&brush, x + 1.5f, y - 5.0f, 2.5f, 10.0f);
        break;
    case kResume: {
        const std::array points{Gdiplus::PointF(x - 3.5f, y - 5.5f), Gdiplus::PointF(x + 5.0f, y),
                                Gdiplus::PointF(x - 3.5f, y + 5.5f)};
        graphics.FillPolygon(&brush, points.data(), static_cast<INT>(points.size()));
        break;
    }
    case kFolder:
    case kShowInFolder: {
        // A folder from the front, its tab on the top left.
        const float left = x - 6.5f, right = x + 6.5f, top = y - 5.0f, bottom = y + 5.0f;
        const std::array points{Gdiplus::PointF(left, top),
                                Gdiplus::PointF(left + 4.5f, top),
                                Gdiplus::PointF(left + 6.0f, top + 2.0f),
                                Gdiplus::PointF(right, top + 2.0f),
                                Gdiplus::PointF(right, bottom),
                                Gdiplus::PointF(left, bottom)};
        graphics.DrawPolygon(&pen, points.data(), static_cast<INT>(points.size()));
        break;
    }
    default:
        break;
    }
}

// An arrow down onto a line, in a ring that fills as the downloads still to finish come in,
// grey while they are all paused. While one of them is of a size not told, a piece of the
// ring goes round instead, a step with each piece that comes in.
void DrawButtonPicture(HDC dc, const RECT& rect) {
    Gdiplus::Graphics graphics(dc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    const float x = rect.right / 2.0f, y = rect.bottom / 2.0f;

    if (const Overall overall = OverallProgress(); overall.going) {
        const Gdiplus::Pen track(GdiplusColor(color::kBorder), 2.0f);
        const Gdiplus::Pen done(GdiplusColor(overall.moving ? color::kSpinner : color::kSuggestDim), 2.0f);
        const Gdiplus::RectF ring(x - kRingRadius, y - kRingRadius, 2 * kRingRadius, 2 * kRingRadius);
        graphics.DrawEllipse(&track, ring);
        if (overall.done) {
            graphics.DrawArc(&done, ring, -90.0f, static_cast<float>(*overall.done * 360.0));
        } else {
            const float turn =
                overall.moving ? static_cast<float>(::GetTickCount64() % kSpinTurnMs) * 360.0f / kSpinTurnMs : -90.0f;
            graphics.DrawArc(&done, ring, turn, 90.0f);
        }
    }

    // In the colour of a warning while a download waits to be kept.
    const bool held = std::ranges::any_of(g_app.downloads, IsHeld);
    const Gdiplus::Pen ink(GdiplusColor(held ? color::kCrashed : color::kText), 1.5f);
    graphics.DrawLine(&ink, x, y - 4.5f, x, y + 2.0f);
    graphics.DrawLine(&ink, x - 3.0f, y - 1.0f, x, y + 2.0f);
    graphics.DrawLine(&ink, x + 3.0f, y - 1.0f, x, y + 2.0f);
    graphics.DrawLine(&ink, x - 4.0f, y + 4.5f, x + 4.0f, y + 4.5f);
}

void PaintButton(HWND button, const BrowserWindow* win) {
    PAINTSTRUCT paint{};
    const HDC target = ::BeginPaint(button, &paint);
    if (!target) return;
    const RECT rect = ClientRectOf(button);
    // Drawn off screen and copied over in one go, so that the ring never shows half drawn.
    const UniqueDc buffer(::CreateCompatibleDC(target));
    const UniqueBitmap bitmap(::CreateCompatibleBitmap(target, rect.right, rect.bottom));
    const HDC dc = buffer.get();
    {
        const SelectedObject selectedBitmap(dc, bitmap.get());
        const Theme& theme = g_app.theme;
        // Pressed in while the mouse is down on it, and while its panel is open.
        const bool down = win && (win->downloads.buttonPressed || IsOpen(win->downloads));
        ::FillRect(dc, &rect, (down ? theme.activeTab : theme.panel).get());
        ::FrameRect(dc, &rect, theme.border.get());
        DrawButtonPicture(dc, rect);
        ::BitBlt(target, 0, 0, rect.right, rect.bottom, dc, 0, 0, SRCCOPY);
    }
    ::EndPaint(button, &paint);
}

void DrawLine(HDC dc, const std::wstring& text, RECT box, COLORREF ink, UINT align = DT_LEFT) {
    ::SetTextColor(dc, ink);
    ::DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &box,
                align | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
}

void PaintRow(HDC dc, Gdiplus::Graphics& graphics, const DownloadsUi& ui, const Download& download, const RECT& row,
              int lineHeight) {
    const Theme& theme = g_app.theme;
    if (ui.hotId == download.id) {
        RECT hot = row;
        ::InflateRect(&hot, 0, -1);
        ::FillRect(dc, &hot, theme.suggestSelected.get());
    }

    const int iconTop = row.top + (kRowHeight - kIconSize) / 2;
    if (download.icon) {
        ::DrawIconEx(dc, kPadding, iconTop, download.icon.get(), kIconSize, kIconSize, 0, nullptr, DI_NORMAL);
    }

    const std::vector<int> actions = RowActions(download);
    const int textLeft = kPadding + kIconSize + kIconGap;
    const int textRight =
        actions.empty() ? row.right - kPadding : RowActionRect(row, actions, actions.size() - 1).left - 2 * kActionGap;
    const DownloadFacts facts = FactsOf(download);
    const int nameTop = row.top + kTextTop;
    const int statusTop = nameTop + lineHeight + kLineGap;
    DrawLine(dc, download.name, {textLeft, nameTop, textRight, nameTop + lineHeight}, color::kText);
    DrawLine(dc, StatusText(facts), {textLeft, statusTop, textRight, statusTop + lineHeight},
             facts.phase == DownloadPhase::Failed || facts.held ? color::kCrashed : color::kSuggestDim);

    // A bar under the two lines while it is still to finish.
    if ((facts.phase == DownloadPhase::InProgress || facts.phase == DownloadPhase::Paused) && !facts.held) {
        const int barTop = statusTop + lineHeight + kBarGap;
        const RECT track{textLeft, barTop, textRight, barTop + kBarHeight};
        ::FillRect(dc, &track, theme.border.get());
        if (facts.total > 0 && facts.received > 0) {
            const double done = std::min(1.0, static_cast<double>(facts.received) / static_cast<double>(facts.total));
            RECT filled = track;
            filled.right = track.left + static_cast<LONG>(done * Width(track));
            const UniqueBrush fill(
                ::CreateSolidBrush(facts.phase == DownloadPhase::Paused ? color::kSuggestDim : color::kSpinner));
            ::FillRect(dc, &filled, fill.get());
        }
    }

    for (std::size_t i = 0; i < actions.size(); ++i) {
        const int part = actions[i];
        const RECT box = RowActionRect(row, actions, i);
        const bool hot = ui.hotId == download.id && ui.hotPart == part;
        if (hot) {
            const UniqueBrush fill(::CreateSolidBrush(kHotAction));
            ::FillRect(dc, &box, fill.get());
        }
        if (part == kKeep || part == kDelete) {
            // Buttons that say what they do: this is a choice to be made with care.
            ::FrameRect(dc, &box, theme.border.get());
            DrawLine(dc, part == kKeep ? L"Keep" : L"Delete", box, part == kKeep ? color::kSuggestUrl : color::kText,
                     DT_CENTER);
        } else {
            DrawGlyph(graphics, part, box, hot ? color::kText : color::kSuggestDim);
        }
    }
}

void PaintPanel(HWND panel, const BrowserWindow* win) {
    PAINTSTRUCT paint{};
    const HDC target = ::BeginPaint(panel, &paint);
    if (!target) return;
    const RECT rect = ClientRectOf(panel);
    const UniqueDc buffer(::CreateCompatibleDC(target));
    const UniqueBitmap bitmap(::CreateCompatibleBitmap(target, rect.right, rect.bottom));
    const HDC dc = buffer.get();
    {
        const SelectedObject selectedBitmap(dc, bitmap.get());
        const SelectedObject selectedFont(dc, g_app.theme.font);
        const Theme& theme = g_app.theme;
        ::FillRect(dc, &rect, theme.panel.get());
        ::SetBkMode(dc, TRANSPARENT);
        TEXTMETRICW metrics{};
        ::GetTextMetricsW(dc, &metrics);
        const int lineHeight = metrics.tmHeight;

        Gdiplus::Graphics graphics(dc);
        graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        const DownloadsUi empty;
        const DownloadsUi& ui = win ? win->downloads : empty;
        const auto headerButton = [&](int part, const RECT& box) {
            const bool hot = ui.hotPart == part;
            if (hot) {
                const UniqueBrush fill(::CreateSolidBrush(kHotAction));
                ::FillRect(dc, &box, fill.get());
            }
            return hot;
        };

        // The header: the title, and its buttons on the right.
        const RECT folder = FolderRect(rect);
        const std::optional<RECT> clear = ClearRect(rect);
        DrawLine(dc, L"Downloads", {kPadding, 0, (clear ? clear->left : folder.left) - kActionGap, kHeaderHeight},
                 color::kText);
        if (clear) {
            const bool hot = headerButton(kClear, *clear);
            DrawLine(dc, L"Clear", *clear, hot ? color::kText : color::kSuggestUrl, DT_CENTER);
        }
        const RECT close = CloseRect(rect);
        DrawGlyph(graphics, kFolder, folder, headerButton(kFolder, folder) ? color::kText : color::kSuggestDim);
        DrawGlyph(graphics, kClose, close, headerButton(kClose, close) ? color::kText : color::kSuggestDim);
        const RECT rule{kPadding / 2, kHeaderHeight - 1, rect.right - kPadding / 2, kHeaderHeight};
        ::FillRect(dc, &rule, theme.border.get());

        for (int row = 0; row < RowsShown(rect); ++row) {
            PaintRow(dc, graphics, ui, g_app.downloads[static_cast<std::size_t>(row)], RowRect(rect, row), lineHeight);
        }
        ::FrameRect(dc, &rect, theme.border.get());
        ::BitBlt(target, 0, 0, rect.right, rect.bottom, dc, 0, 0, SRCCOPY);
    }
    ::EndPaint(panel, &paint);
}

// What the panel lists, as its window's text: what a screen reader names it by.
[[nodiscard]] std::wstring PanelText() {
    std::wstring text = L"Downloads";
    const wchar_t* separator = L": ";
    for (const Download& download : g_app.downloads) {
        text.append(separator).append(download.name).append(L" (").append(PhaseWord(FactsOf(download))).append(L")");
        separator = L"; ";
    }
    return text;
}

// ---- the panel's place, and the mouse ----

// In front of everything else in the window, in the top right corner of the page, as
// many rows high as the window has room for.
void PlacePanel(BrowserWindow& win, bool show) {
    const RECT client = ClientRectOf(win.client);
    const int top = WebViewBounds(win).top + kPanelTopGap;
    const int width = std::max(0, std::min(kPanelWidth, static_cast<int>(client.right) - 2 * kMargin));
    const int fit = (static_cast<int>(client.bottom) - kMargin - top - kHeaderHeight - kBottomPadding) / kRowHeight;
    const int rows = std::min({std::max(fit, 1), kMaxRows, static_cast<int>(g_app.downloads.size())});
    ::SetWindowPos(win.downloads.panel, HWND_TOP, client.right - kMargin - width, top, width,
                   kHeaderHeight + rows * kRowHeight + kBottomPadding, SWP_NOACTIVATE | (show ? SWP_SHOWWINDOW : 0));
}

// The address bar makes room for the button, or takes back the room it gave, without
// the rest of the window being laid out again: it may be being typed in.
void FitAddressBar(BrowserWindow& win) {
    RECT edit = WindowRectOf(win.urlEdit);
    ::MapWindowPoints(nullptr, win.client, reinterpret_cast<POINT*>(&edit), 2);
    // Where LayoutToolbarAndTabs ends the toolbar.
    const int right = LayoutDownloads(win, ClientRectOf(win.client).right - kMargin, edit.top, Height(edit));
    ::MoveWindow(win.urlEdit, edit.left, edit.top, std::max(kUrlMinWidth, right - static_cast<int>(edit.left)),
                 Height(edit), TRUE);
}

// One bit for each of the mouse's buttons that is down.
[[nodiscard]] BYTE MouseButtonsDown() noexcept {
    unsigned down = 0, bit = 1;
    for (const int button : {VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_XBUTTON1, VK_XBUTTON2}) {
        if (::GetAsyncKeyState(button) < 0) down |= bit;
        bit <<= 1;
    }
    return static_cast<BYTE>(down);
}

void SetHot(BrowserWindow& win, const Hit& hit) {
    DownloadsUi& ui = win.downloads;
    if (ui.hotPart == hit.part && ui.hotId == hit.downloadId) return;
    ui.hotPart = hit.part;
    ui.hotId = hit.downloadId;
    ::InvalidateRect(ui.panel, nullptr, FALSE);
}

// While the panel is open. A click on the page goes to the browser process, which this
// thread hears nothing of, so the mouse's buttons are looked at instead: one that goes
// down anywhere but on the panel or its button closes it, as it would a menu.
void OnPanelTick(BrowserWindow& win) {
    DownloadsUi& ui = win.downloads;
    const ULONGLONG now = ::GetTickCount64();
    POINT cursor{};
    const HWND under = ::GetCursorPos(&cursor) ? ::WindowFromPoint(cursor) : nullptr;
    const bool onIt = under && (under == ui.panel || under == ui.button);

    const BYTE down = MouseButtonsDown();
    const auto pressed = static_cast<BYTE>(down & ~ui.mouseButtons);
    ui.mouseButtons = down;
    if (pressed && !onIt) {
        CloseDownloadsPanel(win);
        return;
    }
    // The mouse left it: nothing is under the mouse any more.
    if (under != ui.panel) SetHot(win, {});

    // Not while a download waits in it for the user to say whether to keep it.
    if (ui.autoOpened) {
        if (onIt || std::ranges::any_of(g_app.downloads, IsHeld)) ui.closeAt = now + kAutoCloseMs;
        else if (now >= ui.closeAt) {
            CloseDownloadsPanel(win);
            return;
        }
    }
    // Speeds and times left change when nothing comes in as well.
    if (now - ui.refreshedAt >= kRefreshMs) {
        ui.refreshedAt = now;
        CheckDownloadedFiles();
        ::InvalidateRect(ui.panel, nullptr, FALSE);
    }
}

void Act(BrowserWindow& win, const Hit& hit) {
    switch (hit.part) {
    case kClose:
        CloseDownloadsPanel(win);
        break;
    case kFolder:
        CloseDownloadsPanel(win);
        OpenDownloadsFolder();
        break;
    case kClear:
        ClearFinishedDownloads();
        break;
    case kRow:
        if (const Download* download = FindDownload(hit.downloadId); download && CanOpen(*download)) {
            CloseDownloadsPanel(win);
            OpenDownload(hit.downloadId);
        }
        break;
    case kShowInFolder:
        CloseDownloadsPanel(win);
        ShowDownloadInFolder(hit.downloadId);
        break;
    case kPause:
        PauseDownload(hit.downloadId);
        break;
    case kResume:
        ResumeDownload(hit.downloadId);
        break;
    case kCancel:
        CancelDownload(hit.downloadId);
        break;
    case kKeep:
        KeepDownload(hit.downloadId);
        break;
    case kDelete:
        DeleteDownload(hit.downloadId);
        break;
    default:
        break;
    }
}

[[nodiscard]] POINT PointOf(LPARAM lParam) noexcept {
    return {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
}

} // namespace

void CreateDownloadsControls(BrowserWindow& win) {
    const auto create = [&win](const wchar_t* className, int id) {
        return ::CreateWindowExW(0, className, L"Downloads", WS_CHILD | WS_CLIPSIBLINGS, 0, 0, 0, 0, win.client,
                                 reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), g_app.instance, nullptr);
    };
    win.downloads.button = create(kDownloadsButtonClass, kIdDownloads);
    win.downloads.panel = create(kDownloadsPanelClass, kIdDownloadsPanel);
    if (win.downloads.panel) ::SetWindowTextW(win.downloads.panel, PanelText().c_str());
}

int LayoutDownloads(BrowserWindow& win, int right, int top, int height) {
    DownloadsUi& ui = win.downloads;
    if (!ui.button) return right;
    const bool shown = !g_app.downloads.empty() && !win.isFullscreen;
    if (shown) ::MoveWindow(ui.button, right - kDownloadsButtonWidth, top, kDownloadsButtonWidth, height, TRUE);
    ::ShowWindow(ui.button, shown ? SW_SHOWNA : SW_HIDE);
    if (IsOpen(ui)) PlacePanel(win, false);
    return shown ? right - kDownloadsButtonWidth - kButtonGap : right;
}

void ShowDownloadsPanel(BrowserWindow& win, bool byItself) {
    DownloadsUi& ui = win.downloads;
    if (!ui.panel || g_app.downloads.empty() || win.isFullscreen || win.closing) return;
    const bool open = IsOpen(ui);
    if (byItself) {
        // Only where the user can see it, and never in place of one they opened themselves.
        if (!::IsWindowVisible(win.frame) || win.minimized) return;
        if (open && !ui.autoOpened) return;
        ui.autoOpened = true;
        ui.closeAt = ::GetTickCount64() + kAutoCloseMs;
    } else {
        ui.autoOpened = false;
    }
    if (!open) {
        // A button already down when it opens isn't a click outside it.
        ui.mouseButtons = MouseButtonsDown();
        ui.hotPart = ui.hotId = ui.pressedPart = ui.pressedId = 0;
    }
    CheckDownloadedFiles();
    ui.refreshedAt = ::GetTickCount64();
    PlacePanel(win, true);
    ::SetTimer(ui.panel, kPanelTimer, kPanelTickMs, nullptr);
    ::InvalidateRect(ui.panel, nullptr, FALSE);
    if (ui.button) ::InvalidateRect(ui.button, nullptr, FALSE);
}

bool CloseDownloadsPanel(BrowserWindow& win) {
    DownloadsUi& ui = win.downloads;
    if (!IsOpen(ui)) return false;
    ::KillTimer(ui.panel, kPanelTimer);
    if (::GetCapture() == ui.panel) ::ReleaseCapture();
    ::ShowWindow(ui.panel, SW_HIDE);
    ui.autoOpened = false;
    ui.hotPart = ui.hotId = ui.pressedPart = ui.pressedId = 0;
    if (ui.button) ::InvalidateRect(ui.button, nullptr, FALSE);
    return true;
}

void RaiseDownloadsPanel(BrowserWindow& win) {
    if (IsOpen(win.downloads)) {
        ::SetWindowPos(win.downloads.panel, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
}

void ShowDownloadsButton(BrowserWindow& win, bool visible) {
    if (!visible) CloseDownloadsPanel(win);
    if (win.downloads.button) {
        ::ShowWindow(win.downloads.button, visible && !g_app.downloads.empty() ? SW_SHOWNA : SW_HIDE);
    }
}

void RefreshDownloadsUi(DownloadsChange change) {
    const std::wstring text = change == DownloadsChange::Progress ? std::wstring() : PanelText();
    for (const auto& owned : g_app.windows) {
        BrowserWindow& win = *owned;
        DownloadsUi& ui = win.downloads;
        if (!ui.button || !ui.panel) continue;
        if (change == DownloadsChange::List) {
            if (g_app.downloads.empty()) CloseDownloadsPanel(win);
            // The button comes with the first download and goes with the last.
            const bool wanted = !g_app.downloads.empty() && !win.isFullscreen;
            if (wanted != HasStyle(ui.button, WS_VISIBLE)) FitAddressBar(win);
            else if (IsOpen(ui)) PlacePanel(win, false);
        }
        if (change != DownloadsChange::Progress) ::SetWindowTextW(ui.panel, text.c_str());
        ::InvalidateRect(ui.button, nullptr, FALSE);
        if (IsOpen(ui)) ::InvalidateRect(ui.panel, nullptr, FALSE);
    }
}

LRESULT CALLBACK DownloadsButtonProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    BrowserWindow* const win = WindowFromChild(hwnd);
    switch (msg) {
    case WM_MOUSEACTIVATE:
        // A click on it leaves the keyboard where it was and makes no window active.
        return MA_NOACTIVATE;
    case WM_SETCURSOR:
        if (LOWORD(lParam) != HTCLIENT) break;
        ::SetCursor(::LoadCursorW(nullptr, IDC_ARROW));
        return TRUE;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        PaintButton(hwnd, win);
        return 0;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
        if (win) {
            win->downloads.buttonPressed = true;
            ::SetCapture(hwnd);
            ::InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_LBUTTONUP: {
        const bool pressed = win && std::exchange(win->downloads.buttonPressed, false);
        if (::GetCapture() == hwnd) ::ReleaseCapture();
        const RECT rect = ClientRectOf(hwnd);
        const POINT point = PointOf(lParam);
        if (pressed && ::PtInRect(&rect, point) && !CloseDownloadsPanel(*win)) ShowDownloadsPanel(*win, false);
        ::InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_CAPTURECHANGED:
        if (win && std::exchange(win->downloads.buttonPressed, false)) ::InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    default:
        break;
    }
    return ::DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT CALLBACK DownloadsPanelProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    BrowserWindow* const win = WindowFromChild(hwnd);
    switch (msg) {
    case WM_MOUSEACTIVATE:
        // A click on it leaves the keyboard where it was and makes no window active.
        return MA_NOACTIVATE;
    case WM_SETCURSOR: {
        if (LOWORD(lParam) != HTCLIENT || !win) break;
        POINT cursor{};
        ::GetCursorPos(&cursor);
        ::ScreenToClient(hwnd, &cursor);
        ::SetCursor(::LoadCursorW(nullptr, IsClickable(HitTest(hwnd, cursor)) ? IDC_HAND : IDC_ARROW));
        return TRUE;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        PaintPanel(hwnd, win);
        return 0;
    case WM_MOUSEMOVE:
        if (win) SetHot(*win, HitTest(hwnd, PointOf(lParam)));
        return 0;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
        if (win) {
            // Used, it stays until it is closed.
            win->downloads.autoOpened = false;
            const Hit hit = HitTest(hwnd, PointOf(lParam));
            win->downloads.pressedPart = hit.part;
            win->downloads.pressedId = hit.downloadId;
            ::SetCapture(hwnd);
        }
        return 0;
    case WM_LBUTTONUP:
        if (win) {
            const Hit pressed{.part = std::exchange(win->downloads.pressedPart, kNone),
                              .downloadId = std::exchange(win->downloads.pressedId, 0)};
            if (::GetCapture() == hwnd) ::ReleaseCapture();
            if (const Hit hit = HitTest(hwnd, PointOf(lParam)); hit == pressed) Act(*win, hit);
        }
        return 0;
    case WM_CAPTURECHANGED:
        if (win) win->downloads.pressedPart = win->downloads.pressedId = 0;
        return 0;
    case WM_TIMER:
        if (win && wParam == kPanelTimer) {
            OnPanelTick(*win);
            return 0;
        }
        break;
    default:
        break;
    }
    return ::DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace exodium
