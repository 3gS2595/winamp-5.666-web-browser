// Ctrl+N pressed in one of Winamp's own windows opens a new browser window, as it does in
// the browser's, unless Winamp uses the key there itself. What Winamp uses a key for is
// in its shortcut tables. Winamp 5.666 has Ctrl+N in the playlist editor's, where it
// starts a new playlist, and in no other, and none of the plugins it comes with has it.
//
// This is the part that decides, and it has no window in it; winamp_key_hook.cpp watches
// the keys, finds out where they were pressed and reads Winamp's tables.
#pragma once

#include <cstdint>
#include <span>
#include <string_view>

namespace exodium {

// The kinds of window on Winamp's thread a key can be pressed in.
enum class WinampWindow {
    kMain,
    kPlaylistEditor,
    kEqualizer,
    kVideo,
    kFramed,  // in one of Winamp's frames: the Media Library, and the windows plugins put in them
    kDialog,  // a dialog, Winamp's or a plugin's: the Preferences, a file dialog, a message box
    kBrowser, // a browser window, which has a Ctrl+N of its own
    kOther,   // anything else, such as a modern skin's windows, whose keys aren't known here
};

// What a top-level window is by its class, for the kinds that aren't known by their
// handles: a dialog, or one of Winamp's frames. Anything else is kOther.
[[nodiscard]] WinampWindow WinampWindowOfClass(std::wstring_view className) noexcept;

// Winamp's shortcut tables, by their ids among winamp.exe's resources. A language pack
// can have tables of its own under the same ids, which Winamp takes instead.
enum class WinampKeyTable : std::uint16_t {
    kEqualizer = 1371,
    kPlaylistEditor = 1372,
    kMain = 1373,
    kGlobal = 1374, // for every one of Winamp's windows
};
inline constexpr WinampKeyTable kWinampKeyTables[] = {WinampKeyTable::kEqualizer, WinampKeyTable::kPlaylistEditor,
                                                      WinampKeyTable::kMain, WinampKeyTable::kGlobal};
// The tables that have Ctrl+N in Winamp 5.666, in English and in the language packs it
// comes with. They are what is gone by when Winamp's own can't be read.
inline constexpr WinampKeyTable kTablesWithNewWindowKeyIn5666[] = {WinampKeyTable::kPlaylistEditor};

// The tables Winamp looks up a key pressed in a window in. The video window passes its
// keys on to the main window's. A window in a frame has the table every window has,
// which is all that is known of it; a plugin's own tables aren't among these.
[[nodiscard]] std::span<const WinampKeyTable> KeyTablesOf(WinampWindow window) noexcept;

// An entry of a shortcut table as Windows keeps it (ACCEL), without its command.
struct TableKey {
    std::uint8_t flags = 0; // kTableVirtualKey, kTableShift, kTableControl, kTableAlt
    std::uint16_t key = 0;  // a virtual key with kTableVirtualKey, or else a character
};
// The flags, which are ACCEL's: FVIRTKEY, FSHIFT, FCONTROL and FALT.
inline constexpr std::uint8_t kTableVirtualKey = 0x01;
inline constexpr std::uint8_t kTableShift = 0x04;
inline constexpr std::uint8_t kTableControl = 0x08;
inline constexpr std::uint8_t kTableAlt = 0x10;

// Whether a table has Ctrl+N: the key with Ctrl alone held, or the character it types.
[[nodiscard]] bool TableHasNewWindowKey(std::span<const TableKey> table) noexcept;

// A key going down in one of Winamp's windows, and what is known around it.
struct WinampKeyPress {
    unsigned key = 0; // the virtual key
    bool ctrl = false;
    bool shift = false;
    bool alt = false;
    WinampWindow window = WinampWindow::kOther;
    // Winamp is in the middle of something the key may be meant for: a modal dialog or a
    // message box is up, or the Preferences are open, or a menu, or the mouse is held down.
    bool busy = false;
    bool enabled = true; // the setting, Settings::ctrlNInWinamp
    std::span<const WinampKeyTable> tablesWithKey = kTablesWithNewWindowKeyIn5666; // Winamp's tables that have Ctrl+N
};

// Whether the key opens a browser window: it is Ctrl+N, in a window of Winamp's whose
// tables don't have it, while nothing else is going on.
[[nodiscard]] bool OpensBrowserWindow(const WinampKeyPress& press) noexcept;

} // namespace exodium
