// Ownership and small conveniences for the Win32 and COM handles the plugin uses.
#pragma once

#include <windows.h>

#include <objbase.h>

#include <charconv>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace exodium {

// ---- handles that are released on scope exit ----

template <typename Handle, auto Close>
struct HandleCloser {
    using pointer = Handle;
    void operator()(Handle handle) const noexcept { Close(handle); }
};
template <typename Handle, auto Close>
using UniqueHandle = std::unique_ptr<std::remove_pointer_t<Handle>, HandleCloser<Handle, Close>>;

using UniqueWindow = UniqueHandle<HWND, &::DestroyWindow>;
using UniqueIcon = UniqueHandle<HICON, &::DestroyIcon>;
using UniqueDc = UniqueHandle<HDC, &::DeleteDC>; // a DC made with CreateCompatibleDC
using UniqueBrush = UniqueHandle<HBRUSH, &::DeleteObject>;
using UniquePen = UniqueHandle<HPEN, &::DeleteObject>;
using UniqueBitmap = UniqueHandle<HBITMAP, &::DeleteObject>;

// Selects a GDI object into a DC and puts the previous one back afterwards.
class SelectedObject {
public:
    SelectedObject(HDC dc, HGDIOBJ object) noexcept : dc_(dc), previous_(::SelectObject(dc, object)) {}
    ~SelectedObject() { ::SelectObject(dc_, previous_); }
    SelectedObject(const SelectedObject&) = delete;
    SelectedObject& operator=(const SelectedObject&) = delete;

private:
    HDC dc_;
    HGDIOBJ previous_;
};

// A window's DC, for its client area or for the whole of it, given back afterwards.
class WindowDc {
public:
    enum class Area { Client, Whole };

    explicit WindowDc(HWND window, Area area = Area::Client) noexcept
        : window_(window), dc_(area == Area::Client ? ::GetDC(window) : ::GetWindowDC(window)) {}
    ~WindowDc() {
        if (dc_) ::ReleaseDC(window_, dc_);
    }
    WindowDc(const WindowDc&) = delete;
    WindowDc& operator=(const WindowDc&) = delete;

    [[nodiscard]] HDC get() const noexcept { return dc_; }
    explicit operator bool() const noexcept { return dc_ != nullptr; }

private:
    HWND window_;
    HDC dc_;
};

// ---- COM ----

struct CoTaskMemFreer {
    void operator()(void* memory) const noexcept { ::CoTaskMemFree(memory); }
};
using CoTaskMemString = std::unique_ptr<wchar_t, CoTaskMemFreer>;

// Calls a COM getter that hands back a string for the caller to free, such as
// ComString(webview, &ICoreWebView2::get_Source). Nothing if the call fails.
template <typename Object, typename Interface>
[[nodiscard]] std::optional<std::wstring> ComString(Object* object,
                                                    HRESULT (STDMETHODCALLTYPE Interface::*getter)(LPWSTR*)) {
    CoTaskMemString text;
    if (!object || FAILED((object->*getter)(std::out_ptr(text)))) return std::nullopt;
    return std::wstring(text ? text.get() : L"");
}

// ---- windows ----

[[nodiscard]] constexpr int Width(const RECT& rect) noexcept {
    return rect.right - rect.left;
}
[[nodiscard]] constexpr int Height(const RECT& rect) noexcept {
    return rect.bottom - rect.top;
}

[[nodiscard]] inline RECT ClientRectOf(HWND window) noexcept {
    RECT rect{};
    ::GetClientRect(window, &rect);
    return rect;
}
// In screen coordinates.
[[nodiscard]] inline RECT WindowRectOf(HWND window) noexcept {
    RECT rect{};
    ::GetWindowRect(window, &rect);
    return rect;
}

[[nodiscard]] inline std::wstring WindowText(HWND window) {
    std::wstring text;
    const int length = ::GetWindowTextLengthW(window);
    if (length <= 0) return text;
    text.resize(static_cast<std::size_t>(length));
    text.resize(static_cast<std::size_t>(::GetWindowTextW(window, text.data(), length + 1)));
    return text;
}

[[nodiscard]] inline bool HasStyle(HWND window, LONG_PTR style) noexcept {
    return (::GetWindowLongPtrW(window, GWL_STYLE) & style) != 0;
}

struct Selection {
    DWORD from = 0;
    DWORD to = 0;
    [[nodiscard]] bool empty() const noexcept { return from == to; }
};
[[nodiscard]] inline Selection EditSelection(HWND edit) noexcept {
    Selection selection;
    ::SendMessageW(edit, EM_GETSEL, reinterpret_cast<WPARAM>(&selection.from), reinterpret_cast<LPARAM>(&selection.to));
    return selection;
}
inline void SelectAllText(HWND edit) noexcept {
    ::SendMessageW(edit, EM_SETSEL, 0, -1);
}

// ---- keyboard ----

[[nodiscard]] inline bool IsKeyDown(int virtualKey) noexcept {
    return (::GetKeyState(virtualKey) & 0x8000) != 0;
}

// The modifier keys held with a key press. Compared whole, so that Ctrl+T is not
// also taken for Ctrl+Shift+T.
struct Modifiers {
    bool ctrl = false;
    bool shift = false;
    bool alt = false;

    friend bool operator==(const Modifiers&, const Modifiers&) = default;
};
inline constexpr Modifiers kNoModifiers{};
inline constexpr Modifiers kCtrl{.ctrl = true};
inline constexpr Modifiers kCtrlShift{.ctrl = true, .shift = true};
inline constexpr Modifiers kAlt{.alt = true};

[[nodiscard]] inline Modifiers CurrentModifiers() noexcept {
    return {.ctrl = IsKeyDown(VK_CONTROL), .shift = IsKeyDown(VK_SHIFT), .alt = IsKeyDown(VK_MENU)};
}

// ---- diagnostics ----

// Reports a failed call in the debugger's output window; the plugin has no log of
// its own.
inline void TraceFailure(std::wstring_view what, HRESULT result) {
    char code[2 * sizeof(result)];
    const char* const begin = code;
    const char* const end = std::to_chars(code, code + sizeof(code), static_cast<unsigned long>(result), 16).ptr;
    std::wstring line = L"[exodium] ";
    line.append(what).append(L" failed: 0x").append(begin, end).append(1, L'\n');
    ::OutputDebugStringW(line.c_str());
}

} // namespace exodium
