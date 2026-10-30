// Winamp's side of a browser window: the skinned frame Winamp makes for it, where
// that frame's place is saved, and its entries in Winamp's menus.
#pragma once

#include <windows.h>

#include <wa_ipc.h>

namespace exodium::embed {

// Has Winamp make a frame at the place saved in winamp.ini. Winamp keeps a pointer
// to `state` for as long as the frame lives.
[[nodiscard]] HWND CreateFrame(embedWindowState& state, const GUID& guid);
// Saves the frame's place for the next session.
void SavePlacement(const embedWindowState& state);

// A command id of our own for a window's menu entries.
[[nodiscard]] UINT RegisterMenuCommand();
// Adds the window to Winamp's main menu and its Windows menu.
void AddMenuItems(UINT menuId, const wchar_t* title, bool checked);
void RemoveMenuItems(UINT menuId);
void CheckMenuItems(UINT menuId, bool checked);

// Called for each message on its way to Winamp's main window: picking the window's
// menu entry shows or hides its frame.
void OnWinampMessage(HWND frame, UINT menuId, UINT msg, WPARAM wParam);

} // namespace exodium::embed
