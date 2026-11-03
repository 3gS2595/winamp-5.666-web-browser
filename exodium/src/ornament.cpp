// The ornaments on a browser window's corners: ironwork that bulges out past the
// frame. The one on the top right carries the buttons that minimize, maximize and
// close the window. The other three are the same piece turned to their corners,
// with a plain plate where the buttons would be.
//
// Winamp's frame is a rectangle that Winamp paints, so each ornament is a window of
// its own, see-through around its picture. The frame owns them, which keeps them
// right above the frame, and they are moved to the frame's corners whenever the
// frame moves.
#include "browser.h"

#include "gdiplus_headers.h"
#include "resource.h"

#include <shlwapi.h>
#include <windowsx.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace exodium {

namespace {

// Which corner an ornament is on, and its place in Ornaments::windows. The picture
// is drawn for the top right corner. It is mirrored for the corners on the left and
// stood on its head for the ones on the bottom.
enum Corner : int { kTopRight, kTopLeft, kBottomRight, kBottomLeft };
constexpr int kOnLeft = 1, kOnBottom = 2; // what a Corner is made of
constexpr std::array kPlainCorners{kTopLeft, kBottomRight, kBottomLeft};

// The picture's measurements, in its own pixels.
constexpr SIZE kArtSize{748, 496};
// The point of it that lies on the frame's corner: the underside of the bar along
// the top, and a little way into the upright that hangs down the side.
constexpr POINT kArtCorner{674, 86};
constexpr int kArtPodLeft = 430;   // where the pod that holds the buttons begins
constexpr int kArtPodBottom = 140; // and where it ends, with the upright below it
constexpr int kArtTipRight = 150;  // the bar's far tip, which hangs lower than the bar, ends here
constexpr int kArtShoulder = 617;  // how far left the upright reaches where it comes out of the pod
constexpr int kArtButtonRadius = 29;
enum Button : int { kNoButton = -1, kMinimize, kMaximize, kClose };
constexpr std::array<POINT, 3> kArtButtons{{{552, 79}, {618, 78}, {685, 79}}};

// The width it is shown at. Everything else follows from this.
constexpr int kWidth = 340;
[[nodiscard]] constexpr int Scaled(int art) noexcept {
    return (art * kWidth + kArtSize.cx / 2) / kArtSize.cx;
}
constexpr int kHeight = Scaled(kArtSize.cy);
// How far an ornament out on its corner runs along the frame's edges from there.
constexpr int kBarReach = Scaled(kArtCorner.x);
constexpr int kUprightReach = kHeight - Scaled(kArtCorner.y);
// Tucked into the frame's corner only the bar and the pod show. The pod ends where
// the tab strip does, as if the upright went on behind the toolbar.
constexpr int kTuckedHeight = Scaled(kArtPodBottom);
constexpr int kTuckedTop = kFrameInsetTop + kTabBarHeight - kTuckedHeight; // from the frame's top

// How far the button under the cursor is lightened, and a held one darkened.
constexpr std::uint32_t kHotLighten = 3;                     // by a third of the way to white
constexpr std::uint32_t kPressedKeep = 3, kPressedOutOf = 5; // to three fifths

// Posted to an ornament that was maximized or minimized from outside (see OrnamentProc).
constexpr UINT kMsgPassOnState = WM_APP + 1;

// One of the pictures at the size it is shown: premultiplied BGRA, top row first.
// Empty if it can't be had, and a window then goes without those ornaments.
std::vector<std::uint32_t> LoadArt(int resourceId) {
    const HRSRC found = ::FindResourceW(g_app.instance, MAKEINTRESOURCEW(resourceId), RT_RCDATA);
    const HGLOBAL loaded = found ? ::LoadResource(g_app.instance, found) : nullptr;
    const void* const bytes = loaded ? ::LockResource(loaded) : nullptr;
    if (!bytes) return {};
    ComPtr<IStream> stream;
    stream.Attach(::SHCreateMemStream(static_cast<const BYTE*>(bytes), ::SizeofResource(g_app.instance, found)));
    if (!stream) return {};

    Gdiplus::Bitmap picture(stream.Get());
    Gdiplus::Bitmap scaled(kWidth, kHeight, PixelFormat32bppPARGB);
    if (picture.GetLastStatus() != Gdiplus::Ok || scaled.GetLastStatus() != Gdiplus::Ok) return {};
    {
        Gdiplus::Graphics graphics(&scaled);
        graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
        // Otherwise the outermost pixels are blended with nothing, and fade.
        Gdiplus::ImageAttributes edges;
        edges.SetWrapMode(Gdiplus::WrapModeTileFlipXY);
        const Gdiplus::Status drawn = graphics.DrawImage(
            &picture, Gdiplus::Rect(0, 0, kWidth, kHeight), 0, 0, static_cast<INT>(picture.GetWidth()),
            static_cast<INT>(picture.GetHeight()), Gdiplus::UnitPixel, &edges);
        if (drawn != Gdiplus::Ok) return {};
    }

    const Gdiplus::Rect all(0, 0, kWidth, kHeight);
    Gdiplus::BitmapData data{};
    if (scaled.LockBits(&all, Gdiplus::ImageLockModeRead, PixelFormat32bppPARGB, &data) != Gdiplus::Ok) return {};
    std::vector<std::uint32_t> art(static_cast<std::size_t>(kWidth) * kHeight);
    for (int y = 0; y < kHeight; ++y) {
        const auto* const row =
            reinterpret_cast<const std::uint32_t*>(static_cast<const BYTE*>(data.Scan0) + y * data.Stride);
        std::copy_n(row, kWidth, art.begin() + static_cast<std::ptrdiff_t>(y) * kWidth);
    }
    scaled.UnlockBits(&data);
    return art;
}

[[nodiscard]] POINT ButtonCentre(int button) noexcept {
    return {Scaled(kArtButtons[button].x), Scaled(kArtButtons[button].y)};
}

// The button at a point of the top right ornament's window.
[[nodiscard]] int ButtonAt(POINT point) noexcept {
    const int radius = Scaled(kArtButtonRadius);
    for (int button = 0; button < static_cast<int>(kArtButtons.size()); ++button) {
        const POINT centre = ButtonCentre(button);
        const int dx = point.x - centre.x, dy = point.y - centre.y;
        if (dx * dx + dy * dy <= radius * radius) return button;
    }
    return kNoButton;
}

// Lightens a button in a copy of the picture, or darkens it while it is held down.
void ShadeButton(std::uint32_t* pixels, int rows, int button, bool pressed) noexcept {
    const int centreX = ButtonCentre(button).x, centreY = ButtonCentre(button).y;
    const int radius = Scaled(kArtButtonRadius);
    for (int y = std::max(0, centreY - radius); y <= std::min(rows - 1, centreY + radius); ++y) {
        for (int x = std::max(0, centreX - radius); x <= std::min(kWidth - 1, centreX + radius); ++x) {
            const int dx = x - centreX, dy = y - centreY;
            if (dx * dx + dy * dy > radius * radius) continue;
            std::uint32_t& pixel = pixels[y * kWidth + x];
            // Premultiplied: a channel at full strength is as large as its pixel's alpha.
            const std::uint32_t alpha = pixel >> 24;
            std::uint32_t shaded = alpha << 24;
            for (int shift = 0; shift < 24; shift += 8) {
                const std::uint32_t channel = std::min((pixel >> shift) & 0xFF, alpha);
                shaded |= (pressed ? channel * kPressedKeep / kPressedOutOf : channel + (alpha - channel) / kHotLighten)
                          << shift;
            }
            pixel = shaded;
        }
    }
}

// Gives an ornament's window its picture, kWidth wide, which also sets the window's
// size. The pixels are premultiplied BGRA, top row first.
void SetPicture(HWND window, std::span<const std::uint32_t> picture) {
    SIZE size{kWidth, static_cast<LONG>(picture.size() / kWidth)};
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = size.cx;
    info.bmiHeader.biHeight = -size.cy; // top row first
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    const WindowDc screen(nullptr);
    const UniqueBitmap bitmap(::CreateDIBSection(screen.get(), &info, DIB_RGB_COLORS, &bits, nullptr, 0));
    if (!bitmap || !bits) return;
    std::ranges::copy(picture, static_cast<std::uint32_t*>(bits));

    const UniqueDc dc(::CreateCompatibleDC(screen.get()));
    const SelectedObject selected(dc.get(), bitmap.get());
    POINT origin{};
    BLENDFUNCTION blend{.BlendOp = AC_SRC_OVER, .SourceConstantAlpha = 255, .AlphaFormat = AC_SRC_ALPHA};
    ::UpdateLayeredWindow(window, screen.get(), nullptr, &size, dc.get(), &origin, 0, &blend, ULW_ALPHA);
}

// Says for as long as it lives that what is being done to a window's ornaments is
// this file's own doing, and no move or resize from outside (see OrnamentProc).
class OwnDoing {
public:
    explicit OwnDoing(Ornaments& ornaments) noexcept
        : ornaments_(ornaments), before_(std::exchange(ornaments.ownDoing, true)) {}
    ~OwnDoing() { ornaments_.ownDoing = before_; }
    OwnDoing(const OwnDoing&) = delete;
    OwnDoing& operator=(const OwnDoing&) = delete;

private:
    Ornaments& ornaments_;
    bool before_;
};

// Paints the ornament with the buttons, as they now are.
void Paint(BrowserWindow& win) {
    Ornaments& ornaments = win.ornaments;
    const std::vector<std::uint32_t>& art = g_app.ornamentArt;
    if (!ornaments.windows[kTopRight] || art.empty()) return;

    const int rows = ornaments.tucked ? kTuckedHeight : kHeight;
    std::vector<std::uint32_t> picture(art.begin(), art.begin() + static_cast<std::ptrdiff_t>(rows) * kWidth);
    if (ornaments.tucked) {
        // The bar's tip would hang down over the tabs. It is cut off level with the bar.
        for (int y = Scaled(kArtCorner.y); y < rows; ++y) {
            std::fill_n(picture.begin() + static_cast<std::ptrdiff_t>(y) * kWidth, Scaled(kArtTipRight), 0u);
        }
    }
    if (ornaments.hot != kNoButton && (ornaments.pressed == kNoButton || ornaments.pressed == ornaments.hot)) {
        ShadeButton(picture.data(), rows, ornaments.hot, ornaments.pressed == ornaments.hot);
    }
    const OwnDoing ours(ornaments);
    SetPicture(ornaments.windows[kTopRight].get(), picture);
}

// Paints a plain ornament, turned to its corner. It never changes after that.
void PaintPlain(HWND window, int corner) {
    const std::vector<std::uint32_t>& art = g_app.ornamentPlainArt;
    std::vector<std::uint32_t> picture(art.size());
    for (int y = 0; y < kHeight; ++y) {
        const auto from =
            art.begin() + static_cast<std::ptrdiff_t>((corner & kOnBottom) ? kHeight - 1 - y : y) * kWidth;
        const auto to = picture.begin() + static_cast<std::ptrdiff_t>(y) * kWidth;
        if (corner & kOnLeft) std::reverse_copy(from, from + kWidth, to);
        else std::copy_n(from, kWidth, to);
    }
    SetPicture(window, picture);
}

void SetHot(BrowserWindow& win, int button) {
    if (std::exchange(win.ornaments.hot, button) != button) Paint(win);
}

void SetPressed(BrowserWindow& win, int button) {
    if (std::exchange(win.ornaments.pressed, button) != button) Paint(win);
}

void Press(BrowserWindow& win, int button) {
    switch (button) {
    case kMinimize:
        ::ShowWindow(win.frame, SW_MINIMIZE);
        break;
    case kMaximize:
        ::ShowWindow(win.frame, ::IsZoomed(win.frame) ? SW_RESTORE : SW_MAXIMIZE);
        break;
    case kClose:
        // Posted, and to the frame: closed the way the taskbar closes it, and not from
        // inside a click on a window that may go with it.
        ::PostMessageW(win.frame, WM_CLOSE, 0, 0);
        break;
    default:
        break;
    }
}

// Where an ornament's window goes for it to sit out on its corner of the frame.
[[nodiscard]] POINT PlaceOnCorner(const RECT& frame, int corner) noexcept {
    return {(corner & kOnLeft) ? frame.left - (kWidth - kBarReach) : frame.right - kBarReach,
            (corner & kOnBottom) ? frame.bottom - kUprightReach : frame.top - (kHeight - kUprightReach)};
}

// Whether the whole pod is on a screen when an ornament's window is at `place`.
[[nodiscard]] bool PodIsOnScreen(POINT place, int corner) noexcept {
    // Its outermost corner and the two next to that, turned as the picture is.
    const auto onScreen = [place, corner](int x, int y) {
        const POINT point{place.x + ((corner & kOnLeft) ? kWidth - 1 - x : x),
                          place.y + ((corner & kOnBottom) ? kHeight - 1 - y : y)};
        return ::MonitorFromPoint(point, MONITOR_DEFAULTTONULL) != nullptr;
    };
    return onScreen(Scaled(kArtPodLeft), 0) && onScreen(kWidth - 1, 0) && onScreen(kWidth - 1, Scaled(kArtPodBottom));
}

// Keeps a stretch of the frame at least `least` long, by holding back whichever end
// of it is being dragged.
void HoldToLeast(LONG& low, LONG& high, bool lowMoves, LONG least) noexcept {
    if (high - low >= least) return;
    if (lowMoves) low = high - least;
    else high = low + least;
}

// An ornament has no place of its own. Moved or sized from outside, it stays where it
// is and the frame goes in its stead; the ornament then follows the frame as it always
// does. Each request is taken from where the one before it asked the ornament to be,
// which is where the window manager goes on from, so a drag that grows the ornament
// grows the frame by as much and no more.
void FollowOutsideDrag(BrowserWindow& win, HWND ornament, const WINDOWPOS& request) {
    std::optional<OutsideDrag>& drag = win.ornaments.outside;
    const RECT frame = WindowRectOf(win.frame);
    // Anything else that moved the frame meanwhile starts things afresh.
    if (!drag || drag->ornament != ornament || !::EqualRect(&drag->frame, &frame)) {
        drag = OutsideDrag{.ornament = ornament, .asked = WindowRectOf(ornament), .wanted = frame, .frame = frame};
    }
    const RECT from = drag->asked;
    RECT to = from;
    if (!(request.flags & SWP_NOMOVE)) ::OffsetRect(&to, request.x - from.left, request.y - from.top);
    if (!(request.flags & SWP_NOSIZE)) {
        to.right = to.left + request.cx;
        to.bottom = to.top + request.cy;
    }
    drag->asked = to;

    // A move or a drag of the ornament's edges moves the frame's edges as far. A request
    // that changes every edge by a different amount gives the ornament another size in
    // another place altogether, as a window manager snapping it to half the screen does.
    // What that would make of the frame isn't worth having, so the frame stays put.
    const LONG left = to.left - from.left, top = to.top - from.top;
    const LONG right = to.right - from.right, bottom = to.bottom - from.bottom;
    const auto edgewise = [](LONG low, LONG high) { return low == 0 || high == 0 || low == high || low == -high; };
    if (!edgewise(left, right) || !edgewise(top, bottom)) return;
    RECT& wanted = drag->wanted;
    wanted = {wanted.left + left, wanted.top + top, wanted.right + right, wanted.bottom + bottom};

    RECT place = wanted;
    HoldToLeast(place.left, place.right, left != 0, kMinFrameSize.x);
    HoldToLeast(place.top, place.bottom, top != 0, kMinFrameSize.y);
    if (!::EqualRect(&place, &frame)) {
        ::SetWindowPos(win.frame, nullptr, place.left, place.top, Width(place), Height(place),
                       SWP_NOZORDER | SWP_NOACTIVATE);
    }
    if (drag) drag->frame = WindowRectOf(win.frame);
}

} // namespace

void CreateOrnaments(BrowserWindow& win) {
    if (g_app.ornamentArt.empty()) g_app.ornamentArt = LoadArt(IDR_CORNER_ORNAMENT);
    if (g_app.ornamentArt.empty()) return;
    if (g_app.ornamentPlainArt.empty()) g_app.ornamentPlainArt = LoadArt(IDR_CORNER_ORNAMENT_PLAIN);

    // A click must not take the frame's place as the active window: Winamp would
    // paint the frame as inactive under the very button being pressed.
    const OwnDoing ours(win.ornaments);
    const auto create = [&win] {
        return ::CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kOrnamentClass, L"", WS_POPUP, 0,
                                 0, kWidth, kHeight, win.frame, nullptr, g_app.instance, &win);
    };
    win.ornaments.windows[kTopRight].reset(create());
    if (!g_app.ornamentPlainArt.empty()) {
        for (const int corner : kPlainCorners) {
            win.ornaments.windows[corner].reset(create());
            if (const HWND plain = win.ornaments.windows[corner].get()) PaintPlain(plain, corner);
        }
    }
    UpdateOrnaments(win);
}

void UpdateOrnaments(BrowserWindow& win) {
    Ornaments& ornaments = win.ornaments;
    const HWND buttons = ornaments.windows[kTopRight].get();
    if (!buttons) return;

    // A frame that a skin has docked into its own window has no corners of its own.
    if (!::IsWindowVisible(win.frame) || win.minimized || ::IsIconic(win.frame) || HasStyle(win.frame, WS_CHILD)) {
        for (const UniqueWindow& window : ornaments.windows) {
            if (window) ::ShowWindow(window.get(), SW_HIDE);
        }
        ornaments.hot = ornaments.pressed = kNoButton;
        return;
    }

    const OwnDoing ours(ornaments);
    // The one with the buttons sticks out above and beside the frame where there is
    // screen for it to be seen on, and otherwise sits inside the corner. Maximized
    // there never is.
    const RECT frame = WindowRectOf(win.frame);
    const bool zoomed = ::IsZoomed(win.frame) != FALSE;
    const POINT out = PlaceOnCorner(frame, kTopRight);
    const bool tucked = zoomed || !PodIsOnScreen(out, kTopRight);
    const POINT at = tucked ? POINT{frame.right - kWidth, frame.top + kTuckedTop} : out;

    const bool retucked = std::exchange(ornaments.tucked, tucked) != tucked;
    if (retucked || !::IsWindowVisible(buttons)) Paint(win);
    ::SetWindowPos(buttons, nullptr, at.x, at.y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    bool relayout = retucked; // the tab strip makes room for it, or takes the room back

    // The plain ones are only for show, and only shown whole: where the pod has
    // screen to be seen on, and on a frame long enough for the ironwork along an
    // edge not to run into what comes the other way.
    for (const int corner : kPlainCorners) {
        const HWND plain = ornaments.windows[corner].get();
        if (!plain) continue;
        const POINT place = PlaceOnCorner(frame, corner);
        const int comingTheOtherWay = (corner & kOnBottom) || !tucked ? kBarReach : kWidth;
        const bool room = (!(corner & kOnLeft) || Width(frame) >= kBarReach + comingTheOtherWay) &&
                          (!(corner & kOnBottom) || Height(frame) >= 2 * kUprightReach);
        const bool shown = !zoomed && room && PodIsOnScreen(place, corner);
        // The tab strip keeps clear of the one on the top left as well.
        if (corner == kTopLeft && shown != (::IsWindowVisible(plain) != FALSE)) relayout = true;
        ::SetWindowPos(plain, nullptr, place.x, place.y, 0, 0,
                       SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | (shown ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
    }
    if (relayout) LayoutToolbarAndTabs(win);
}

StripCover OrnamentStripCover(const BrowserWindow& win) noexcept {
    const Ornaments& ornaments = win.ornaments;
    if (!ornaments.windows[kTopRight] || HasStyle(win.frame, WS_CHILD)) return {};
    // Out on a corner an ornament lies over the strip with the shoulder of its
    // upright. Tucked in, the one with the buttons does with its whole pod. Both are
    // measured from the frame's edge, which Winamp at times keeps the client a
    // little short of.
    const int shoulder = Scaled(kArtCorner.x - kArtShoulder);
    const HWND topLeft = ornaments.windows[kTopLeft].get();
    const int left = topLeft && ::IsWindowVisible(topLeft) ? shoulder : 0;
    const int right = ornaments.tucked ? kWidth - Scaled(kArtPodLeft) : shoulder;
    const RECT frame = WindowRectOf(win.frame), client = WindowRectOf(win.client);
    return {.left = std::max(0, left - static_cast<int>(client.left - frame.left)),
            .right = std::max(0, right - static_cast<int>(frame.right - client.right))};
}

LRESULT CALLBACK OrnamentProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        return ::DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    auto* const self = reinterpret_cast<BrowserWindow*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!self) return ::DefWindowProcW(hwnd, msg, wParam, lParam);
    BrowserWindow& win = *self;
    const auto& windows = win.ornaments.windows;
    const auto found = std::ranges::find(windows, hwnd, [](const UniqueWindow& window) { return window.get(); });
    if (found == windows.end()) return ::DefWindowProcW(hwnd, msg, wParam, lParam); // still being made
    const bool plain = found != windows.begin();
    // Only the picture itself gets the mouse; the see-through part lets it through.
    const POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
    const auto buttonAt = [plain](POINT at) { return plain ? kNoButton : ButtonAt(at); };

    switch (msg) {
    case WM_NCHITTEST:
        // Where a plain one lies over the frame it lets the mouse through as well, to
        // the title bar and the resize grip that are under it.
        if (const RECT frame = WindowRectOf(win.frame); plain && ::PtInRect(&frame, point)) return HTTRANSPARENT;
        break;

    case WM_WINDOWPOSCHANGING: {
        // Moved or sized from outside, as a window manager such as AltSnap does to
        // whichever window is under the cursor: see FollowOutsideDrag.
        auto& asked = *reinterpret_cast<WINDOWPOS*>(lParam);
        constexpr UINT kStay = SWP_NOMOVE | SWP_NOSIZE;
        if (win.ornaments.ownDoing || (asked.flags & kStay) == kStay) break;
        const WINDOWPOS request = asked;
        asked.flags |= kStay;
        if (::IsZoomed(hwnd) || ::IsIconic(hwnd)) ::PostMessageW(hwnd, kMsgPassOnState, 0, 0);
        else if (!::IsZoomed(win.frame)) FollowOutsideDrag(win, hwnd, request);
        return 0;
    }
    case WM_EXITSIZEMOVE:
        // The window manager is done with it: the next drag starts from where things are.
        win.ornaments.outside.reset();
        return 0;
    case kMsgPassOnState: {
        // Maximized or minimized from outside: the ornament goes back to how it was, and
        // the frame is maximized or minimized instead.
        const bool minimize = ::IsIconic(hwnd) != FALSE;
        if (!minimize && !::IsZoomed(hwnd)) return 0;
        {
            const OwnDoing ours(win.ornaments);
            ::ShowWindow(hwnd, SW_SHOWNOACTIVATE);
        }
        ::ShowWindow(win.frame, minimize ? SW_MINIMIZE : SW_MAXIMIZE);
        UpdateOrnaments(win);
        return 0;
    }
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;

    case WM_MOUSEMOVE: {
        TRACKMOUSEEVENT track{.cbSize = sizeof(track), .dwFlags = TME_LEAVE, .hwndTrack = hwnd};
        ::TrackMouseEvent(&track);
        SetHot(win, buttonAt(point));
        return 0;
    }
    case WM_MOUSELEAVE:
        SetHot(win, kNoButton);
        return 0;

    case WM_LBUTTONDOWN:
        ::SetForegroundWindow(win.frame);
        if (const int button = buttonAt(point); button != kNoButton) {
            win.ornaments.hot = button;
            SetPressed(win, button);
            ::SetCapture(hwnd);
        } else {
            // Anywhere else the ironwork is something to move the window by, as the
            // title bar under it is. Windows does the moving, and so the snapping.
            ::DefWindowProcW(win.frame, WM_SYSCOMMAND, SC_MOVE | HTCAPTION, static_cast<LPARAM>(::GetMessagePos()));
        }
        return 0;

    case WM_LBUTTONUP: {
        // A button is pressed by letting go of the mouse over the one it went down on.
        const int pressed = std::exchange(win.ornaments.pressed, kNoButton);
        if (::GetCapture() == hwnd) ::ReleaseCapture();
        if (pressed == kNoButton) return 0;
        Paint(win);
        if (pressed == buttonAt(point)) Press(win, pressed);
        return 0;
    }
    case WM_CAPTURECHANGED:
        SetPressed(win, kNoButton);
        return 0;

    default:
        break;
    }
    return ::DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace exodium
