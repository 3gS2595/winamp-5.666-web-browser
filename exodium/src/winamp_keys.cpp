#include "winamp_keys.h"

#include <algorithm>

namespace exodium {

namespace {

constexpr unsigned kNewWindowKey = 'N';
// The character Ctrl+N types, which a table can have in place of the key.
constexpr std::uint16_t kNewWindowChar = 'N' - '@';

constexpr WinampKeyTable kMainTables[] = {WinampKeyTable::kMain, WinampKeyTable::kGlobal};
constexpr WinampKeyTable kPlaylistEditorTables[] = {WinampKeyTable::kPlaylistEditor, WinampKeyTable::kGlobal};
constexpr WinampKeyTable kEqualizerTables[] = {WinampKeyTable::kEqualizer, WinampKeyTable::kGlobal};
constexpr WinampKeyTable kFramedTables[] = {WinampKeyTable::kGlobal};

} // namespace

WinampWindow WinampWindowOfClass(std::wstring_view className) noexcept {
    if (className == L"#32770") return WinampWindow::kDialog;
    if (className == L"Winamp Gen") return WinampWindow::kFramed;
    return WinampWindow::kOther;
}

std::span<const WinampKeyTable> KeyTablesOf(WinampWindow window) noexcept {
    switch (window) {
    case WinampWindow::kMain:
    case WinampWindow::kVideo:
        return kMainTables;
    case WinampWindow::kPlaylistEditor:
        return kPlaylistEditorTables;
    case WinampWindow::kEqualizer:
        return kEqualizerTables;
    case WinampWindow::kFramed:
        return kFramedTables;
    default:
        return {};
    }
}

bool TableHasNewWindowKey(std::span<const TableKey> table) noexcept {
    constexpr std::uint8_t modifiers = kTableShift | kTableControl | kTableAlt;
    return std::ranges::any_of(table, [](const TableKey& entry) {
        if (entry.flags & kTableVirtualKey) {
            return entry.key == kNewWindowKey && (entry.flags & modifiers) == kTableControl;
        }
        // A character is matched whatever the keys that typed it, Alt apart.
        return entry.key == kNewWindowChar && !(entry.flags & kTableAlt);
    });
}

bool OpensBrowserWindow(const WinampKeyPress& press) noexcept {
    if (!press.enabled || press.busy) return false;
    if (press.key != kNewWindowKey || !press.ctrl || press.shift || press.alt) return false;
    // In a dialog the key belongs to the dialog, in a browser window to the browser, and
    // what other windows do with it isn't known.
    const std::span<const WinampKeyTable> tables = KeyTablesOf(press.window);
    if (tables.empty()) return false;
    return std::ranges::none_of(
        tables, [&](WinampKeyTable table) { return std::ranges::contains(press.tablesWithKey, table); });
}

} // namespace exodium
