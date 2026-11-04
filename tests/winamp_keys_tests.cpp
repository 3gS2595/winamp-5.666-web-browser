#include "test_framework.h"

#include "settings.h"
#include "winamp_keys.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace exodium;

namespace {

constexpr std::uint8_t kNoInvert = 0x02; // FNOINVERT, which every entry of Winamp's has
constexpr std::uint16_t kDelete = 0x2E;  // VK_DELETE

// Entries of Winamp 5.666's own tables, as winamp.exe has them.
const std::vector<TableKey> kPlaylistEditorEntries{
    {kTableVirtualKey | kNoInvert, 'L'},
    {kTableVirtualKey | kTableControl | kNoInvert, 'N'}, // New playlist (clear)
    {kTableVirtualKey | kTableControl | kTableShift | kNoInvert, kDelete},
    {kTableVirtualKey | kTableAlt | kNoInvert, 'S'},
};
const std::vector<TableKey> kEqualizerEntries{
    {kTableVirtualKey | kNoInvert, 'N'}, // the equalizer on or off
    {kTableVirtualKey | kNoInvert, 'A'},
};
const std::vector<TableKey> kGlobalEntries{
    {kTableVirtualKey | kTableControl | kNoInvert, 'P'},             // Preferences
    {kTableVirtualKey | kTableControl | kTableAlt | kNoInvert, 'N'}, // a new Winamp
    {kTableVirtualKey | kTableControl | kTableShift | kNoInvert, 'K'},
};

// Ctrl+N in a window of this kind, with nothing going on and the setting on.
WinampKeyPress CtrlNIn(WinampWindow window) {
    return {.key = 'N', .ctrl = true, .window = window};
}

} // namespace

TEST_CASE(CtrlNInWinampsMainWindowOpensABrowserWindow) {
    EXPECT(OpensBrowserWindow(CtrlNIn(WinampWindow::kMain)));
}

TEST_CASE(AsItDoesInTheEqualizerTheVideoWindowAndWinampsFrames) {
    EXPECT(OpensBrowserWindow(CtrlNIn(WinampWindow::kEqualizer)));
    EXPECT(OpensBrowserWindow(CtrlNIn(WinampWindow::kVideo)));
    EXPECT(OpensBrowserWindow(CtrlNIn(WinampWindow::kFramed)));
}

// There Ctrl+N is Winamp's New playlist.
TEST_CASE(CtrlNInThePlaylistEditorIsLeftToWinamp) {
    EXPECT(!OpensBrowserWindow(CtrlNIn(WinampWindow::kPlaylistEditor)));
}

TEST_CASE(CtrlNInADialogABrowserWindowOrAnUnknownWindowIsLeftAlone) {
    EXPECT(!OpensBrowserWindow(CtrlNIn(WinampWindow::kDialog)));
    // A browser window opens one of its own, and only the one.
    EXPECT(!OpensBrowserWindow(CtrlNIn(WinampWindow::kBrowser)));
    EXPECT(!OpensBrowserWindow(CtrlNIn(WinampWindow::kOther)));
}

TEST_CASE(NothingOpensWhileWinampIsBusy) {
    for (const WinampWindow window : {WinampWindow::kMain, WinampWindow::kEqualizer, WinampWindow::kFramed}) {
        WinampKeyPress press = CtrlNIn(window);
        press.busy = true;
        EXPECT(!OpensBrowserWindow(press));
    }
}

TEST_CASE(NothingOpensWithTheSettingOff) {
    WinampKeyPress press = CtrlNIn(WinampWindow::kMain);
    press.enabled = false;
    EXPECT(!OpensBrowserWindow(press));
}

TEST_CASE(OnlyCtrlNItselfOpensOne) {
    const auto with = [](unsigned key, bool ctrl, bool shift, bool alt) {
        return OpensBrowserWindow(
            {.key = key, .ctrl = ctrl, .shift = shift, .alt = alt, .window = WinampWindow::kMain});
    };
    EXPECT(with('N', true, false, false));
    EXPECT(!with('N', false, false, false));
    EXPECT(!with('N', true, true, false));
    EXPECT(!with('N', true, false, true)); // Winamp's own: a new Winamp
    EXPECT(!with('N', false, false, true));
    EXPECT(!with('M', true, false, false));
    EXPECT(!with('T', true, false, false));
}

// A language pack can bring tables of its own, and Ctrl+N may be somewhere else in them.
TEST_CASE(WhereWinampsTablesHaveCtrlNItIsWinamps) {
    const WinampKeyTable inMain[] = {WinampKeyTable::kMain};
    WinampKeyPress press = CtrlNIn(WinampWindow::kMain);
    press.tablesWithKey = inMain;
    EXPECT(!OpensBrowserWindow(press));
    press.window = WinampWindow::kVideo; // which passes its keys on to the main window's table
    EXPECT(!OpensBrowserWindow(press));
    press.window = WinampWindow::kEqualizer;
    EXPECT(OpensBrowserWindow(press));
    press.window = WinampWindow::kPlaylistEditor; // no longer has it
    EXPECT(OpensBrowserWindow(press));

    const WinampKeyTable inGlobal[] = {WinampKeyTable::kGlobal};
    press.tablesWithKey = inGlobal;
    for (const WinampWindow window : {WinampWindow::kMain, WinampWindow::kPlaylistEditor, WinampWindow::kEqualizer,
                                      WinampWindow::kVideo, WinampWindow::kFramed}) {
        press.window = window;
        EXPECT(!OpensBrowserWindow(press));
    }
}

TEST_CASE(TablesHaveCtrlNOnlyWithCtrlAloneHeld) {
    EXPECT(TableHasNewWindowKey(kPlaylistEditorEntries));
    EXPECT(!TableHasNewWindowKey(kEqualizerEntries));
    EXPECT(!TableHasNewWindowKey(kGlobalEntries));
    EXPECT(!TableHasNewWindowKey({}));

    // Or as the character it types, which isn't a key and is matched however it is typed.
    const std::vector<TableKey> character{{0, 'N' - '@'}};
    EXPECT(TableHasNewWindowKey(character));
    const std::vector<TableKey> withAlt{{kTableAlt, 'N' - '@'}};
    EXPECT(!TableHasNewWindowKey(withAlt));
    const std::vector<TableKey> letter{{0, 'N'}};
    EXPECT(!TableHasNewWindowKey(letter));
}

TEST_CASE(WindowsAreKnownByTheirClasses) {
    EXPECT(WinampWindowOfClass(L"#32770") == WinampWindow::kDialog);
    EXPECT(WinampWindowOfClass(L"Winamp Gen") == WinampWindow::kFramed);
    // A modern skin's.
    EXPECT(WinampWindowOfClass(L"BaseWindow_RootWnd") == WinampWindow::kOther);
    EXPECT(WinampWindowOfClass(L"") == WinampWindow::kOther);
}

TEST_CASE(EachWindowHasTheTableEveryWindowHas) {
    for (const WinampWindow window : {WinampWindow::kMain, WinampWindow::kPlaylistEditor, WinampWindow::kEqualizer,
                                      WinampWindow::kVideo, WinampWindow::kFramed}) {
        EXPECT(std::ranges::contains(KeyTablesOf(window), WinampKeyTable::kGlobal));
    }
    EXPECT(std::ranges::contains(KeyTablesOf(WinampWindow::kVideo), WinampKeyTable::kMain));
    EXPECT(KeyTablesOf(WinampWindow::kDialog).empty());
    EXPECT(KeyTablesOf(WinampWindow::kBrowser).empty());
}

TEST_CASE(CtrlNInWinampIsOnUnlessTurnedOff) {
    EXPECT(Settings{}.ctrlNInWinamp);
    const Settings off = ReadSettings([](std::wstring_view key) -> std::optional<std::wstring> {
        if (key == L"ctrl_n_in_winamp") return L"0";
        return std::nullopt;
    });
    EXPECT(!off.ctrlNInWinamp);
}
