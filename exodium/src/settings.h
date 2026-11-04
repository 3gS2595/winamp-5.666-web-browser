// Exodium's preferences, as its page in Winamp's Preferences shows them and winamp.ini
// keeps them. Nothing here has a window in it; the page is settings_page.cpp.
//
// A setting that is on or off is a member of Settings, with its default, and a line in
// kFlagSettings with its key in winamp.ini; reading and writing it come with that. Its
// checkbox on the page is a line in settings_page.cpp's kCheckboxes.
#pragma once

#include "search_engines.h"
#include "url_util.h"

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace exodium {

struct Settings {
    std::wstring homePage{kDefaultUrl};                   // where new windows and tabs open
    std::wstring searchEngine{kSearchEngines.front().id}; // the id of one on offer, or kCustomSearchId
    std::wstring customSearchUrl;                         // the custom engine's results page, %s for the words
    bool tabCloseButtons = true;                          // each tab has a button that closes it
    bool ctrlNInWinamp = true; // Ctrl+N in Winamp's own windows opens a browser window; see winamp_keys.h
    bool soundInVisualizers = true; // Winamp's visualizers show the pages' sound while it is stopped; see sound_rule.h

    friend bool operator==(const Settings&, const Settings&) = default;
};

// The section of winamp.ini they are kept in. remote_debugging_port is kept there too,
// but it is read once at start-up and isn't on the page.
inline constexpr wchar_t kSettingsSection[] = L"Exodium";

// The settings that are on or off, each with its key in the section.
struct FlagSetting {
    std::wstring_view key;
    bool Settings::* member;
};
inline constexpr FlagSetting kFlagSettings[] = {
    {L"tab_close_buttons", &Settings::tabCloseButtons},
    {L"ctrl_n_in_winamp", &Settings::ctrlNInWinamp},
    {L"sound_in_visualizers", &Settings::soundInVisualizers},
};

// A line of the section.
struct SettingEntry {
    std::wstring_view key;
    std::wstring value;

    friend bool operator==(const SettingEntry&, const SettingEntry&) = default;
};

// The settings a section holds. `read` gives the value of a key, or nothing for a key
// that isn't there. A setting whose key isn't there, or holds nothing it can be set to,
// keeps its default.
using SettingReader = std::function<std::optional<std::wstring>(std::wstring_view key)>;
[[nodiscard]] Settings ReadSettings(const SettingReader& read);
// Every setting as a line of the section.
[[nodiscard]] std::vector<SettingEntry> SettingEntries(const Settings& settings);
// The lines that differ from `before` in `after`: what a change writes. A setting left
// as it was isn't written, so the section only holds what was set at some time.
[[nodiscard]] std::vector<SettingEntry> ChangedEntries(const Settings& before, const Settings& after);

// On or off as the section has it. It is written 1 or 0, the way Winamp writes its own;
// true and false, yes and no, on and off are read as well.
[[nodiscard]] std::optional<bool> ParseFlag(std::wstring_view text) noexcept;

// A home page as typed, taken the way the address bar takes it: "example.com" is
// https://example.com. Nothing typed is the default.
[[nodiscard]] std::wstring NormalizeHomePage(std::wstring_view typed);

} // namespace exodium
