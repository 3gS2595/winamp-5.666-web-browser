// How a link gets from exodium_open.exe, the program Windows runs for it, to the plugin
// in the running Winamp: as a WM_COPYDATA to Winamp's main window.
#pragma once

#include <windows.h>

namespace exodium {

// The window class of Winamp's main window, unless it was started with /CLASS=.
inline constexpr wchar_t kWinampClass[] = L"Winamp v1.x";

// In dwData. Winamp's own commands are small numbers, so this is none of them. The data
// is the link in UTF-16, without a terminator; no data asks for a window on the default page.
inline constexpr ULONG_PTR kOpenLinkCommand = 0x45584F44; // "EXOD"

// The plugin's answer when it has the link. Anything else means to ask again: Winamp
// answers something else until the plugin is loaded, and the plugin does until Winamp
// is ready for a window to open.
inline constexpr LRESULT kLinkTaken = 0x4C4E4B; // "LNK"

} // namespace exodium
