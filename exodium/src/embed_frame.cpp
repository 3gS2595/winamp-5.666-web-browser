#include "embed_frame.h"

#include "browser.h"

#include <string>

namespace exodium::embed {

namespace {

// The section of winamp.ini the frame's place is kept in. The name comes from the
// Winamp SDK example this started from; changing it would lose the saved place.
constexpr wchar_t kIniSection[] = L"EmbedWndExample";

// The default size is one a web page is usable at. It sits on the classic skin's
// resize grid (275 + 25n wide, 116 + 29n high).
constexpr int kDefaultLeft = 80, kDefaultTop = 60, kDefaultWidth = 900, kDefaultHeight = 638;

// Which of Winamp's menus IPC_GET_HMENU hands out.
constexpr WPARAM kMainMenu = 0, kWindowsMenu = 4;

LRESULT SendIpc(WPARAM wParam, LPARAM command) noexcept {
    return ::SendMessageW(g_app.winamp, WM_WA_IPC, wParam, command);
}

HMENU WinampMenu(WPARAM which) noexcept {
    return reinterpret_cast<HMENU>(SendIpc(which, IPC_GET_HMENU));
}

int ReadSetting(const wchar_t* key, int fallback) noexcept {
    return static_cast<int>(::GetPrivateProfileIntW(kIniSection, key, fallback, g_app.winampIni));
}

void WriteSetting(const wchar_t* key, int value) {
    ::WritePrivateProfileStringW(kIniSection, key, std::to_wstring(value).c_str(), g_app.winampIni);
}

// Puts the entry in front of the item at `position`, or the one before it if that
// position holds no command.
void InsertMenuEntry(HMENU menu, UINT menuId, const wchar_t* title, int position, bool checked) {
    if (!menu) return;
    int before = static_cast<int>(::GetMenuItemID(menu, position));
    if (before <= 0) before = static_cast<int>(::GetMenuItemID(menu, position - 1));

    std::wstring text(title);
    MENUITEMINFOW item{.cbSize = sizeof(item)};
    item.fMask = MIIM_ID | MIIM_TYPE | MIIM_DATA;
    item.fType = MFT_STRING;
    item.wID = menuId;
    item.dwTypeData = text.data();
    item.cch = static_cast<UINT>(text.size());
    ::InsertMenuItemW(menu, static_cast<UINT>(before), FALSE, &item);
    ::CheckMenuItem(menu, menuId, MF_BYCOMMAND | (checked ? MF_CHECKED : MF_UNCHECKED));
}

} // namespace

HWND CreateFrame(embedWindowState& state, const GUID& guid) {
    // The GUID lets a modern skin, and the rest of Winamp, tell this frame from others.
    embedWindowState* const statePtr = &state;
    SET_EMBED_GUID(statePtr, guid);

    // Winamp places the frame itself when it is given the rectangle up front.
    state.r.left = ReadSetting(L"xpos", kDefaultLeft);
    state.r.top = ReadSetting(L"ypos", kDefaultTop);
    state.r.right = state.r.left + ReadSetting(L"width", kDefaultWidth);
    state.r.bottom = state.r.top + ReadSetting(L"height", kDefaultHeight);

    // The modern skin engine (gen_ff) would otherwise add a menu entry of its own.
    state.flags |= EMBED_FLAGS_NOWINDOWMENU;

    return reinterpret_cast<HWND>(SendIpc(reinterpret_cast<WPARAM>(&state), IPC_GET_EMBEDIF));
}

void SavePlacement(const embedWindowState& state) {
    WriteSetting(L"xpos", state.r.left);
    WriteSetting(L"ypos", state.r.top);
    WriteSetting(L"width", Width(state.r));
    WriteSetting(L"height", Height(state.r));
}

UINT RegisterMenuCommand() {
    return static_cast<UINT>(SendIpc(0, IPC_REGISTER_LOWORD_COMMAND));
}

void AddMenuItems(UINT menuId, const wchar_t* title, bool checked) {
    InsertMenuEntry(WinampMenu(kMainMenu), menuId, title, 9, checked);
    SendIpc(1, IPC_ADJUST_OPTIONSMENUPOS);
    InsertMenuEntry(WinampMenu(kWindowsMenu), menuId, title, 3, checked);
    SendIpc(1, IPC_ADJUST_FFWINDOWSMENUPOS);
}

void RemoveMenuItems(UINT menuId) {
    if (const HMENU menu = WinampMenu(kMainMenu)) ::DeleteMenu(menu, menuId, MF_BYCOMMAND);
    SendIpc(static_cast<WPARAM>(-1), IPC_ADJUST_OPTIONSMENUPOS);
    if (const HMENU menu = WinampMenu(kWindowsMenu)) ::DeleteMenu(menu, menuId, MF_BYCOMMAND);
    SendIpc(static_cast<WPARAM>(-1), IPC_ADJUST_FFWINDOWSMENUPOS);
}

void CheckMenuItems(UINT menuId, bool checked) {
    const UINT flags = MF_BYCOMMAND | (checked ? MF_CHECKED : MF_UNCHECKED);
    if (const HMENU menu = WinampMenu(kMainMenu)) ::CheckMenuItem(menu, menuId, flags);
    if (const HMENU menu = WinampMenu(kWindowsMenu)) ::CheckMenuItem(menu, menuId, flags);
}

void OnWinampMessage(HWND frame, UINT menuId, UINT msg, WPARAM wParam) {
    // Both messages, so that the entry works from the keyboard and from the menu
    // Winamp puts on its taskbar button as well.
    if ((msg != WM_SYSCOMMAND && msg != WM_COMMAND) || LOWORD(wParam) != menuId) return;

    // Picking a minimized window from the menu brings it back rather than hiding it.
    if (::IsWindowVisible(frame) && ::IsIconic(frame)) ::ShowWindow(frame, SW_RESTORE);
    else ::ShowWindow(frame, ::IsWindowVisible(frame) ? SW_HIDE : SW_SHOW);
    CheckMenuItems(menuId, ::IsWindowVisible(frame) != FALSE);
}

} // namespace exodium::embed
