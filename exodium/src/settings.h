// Exodium's preferences, as its page in Winamp's Preferences shows them and winamp.ini
// keeps them. Nothing here has a window in it; the page is settings_page.cpp.
//
// A setting that is on or off is a member of Settings, with its default, and a line in
// kFlagSettings with its key in winamp.ini; reading and writing it come with that. Its
// checkbox on the page is a line in settings_page.cpp's kCheckboxes.
#pragma once

#include "corner_skins.h"
#include "glow_shape.h"
#include "search_engines.h"
#include "url_util.h"

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace exodium {

// What the black glow goes round while Winamp is in front (glow_shape.h, glow.cpp).
enum class GlowAround {
    Nothing,
    Exodium, // the browser's windows and their ornaments
    Winamp,  // all of Winamp's windows, the browser's included
};
// As the page's list offers them, in this order, and winamp.ini keeps them.
struct GlowChoice {
    GlowAround around;
    std::wstring_view id;
    std::wstring_view name;
};
inline constexpr GlowChoice kGlowChoices[] = {
    {GlowAround::Winamp, L"winamp", L"All of Winamp"},
    {GlowAround::Exodium, L"exodium", L"Exodium only"},
    {GlowAround::Nothing, L"off", L"Off"},
};

struct Settings {
    std::wstring homePage{kDefaultUrl};                   // where new windows and tabs open
    std::wstring searchEngine{kSearchEngines.front().id}; // the id of one on offer, or kCustomSearchId
    std::wstring customSearchUrl;                         // the custom engine's results page, %s for the words
    std::wstring cornerSkin{kCornerSkins.front().id};     // the id of the corner ornaments' skin; see corner_skins.h
    bool restoreWindows = true;     // Winamp starting opens the windows and tabs open when it closed; see session.cpp
    bool tabCloseButtons = true;    // each tab has a button that closes it
    bool ctrlNInWinamp = true;      // Ctrl+N in Winamp's own windows opens a browser window; see winamp_keys.h
    bool soundInVisualizers = true; // Winamp's visualizers show the pages' sound while it is stopped; see sound_rule.h
    GlowAround glow = GlowAround::Winamp; // what the black glow goes round while Winamp is in front
    GlowLook glowLook;                    // and how it looks
    bool glowAlways = false;              // shown whether Winamp is in front or not
    bool bitwarden = true;                // Bitwarden is in the user's profile, kept up to date; see extensions.cpp
    bool sleepTabs = true; // tabs left out of view for a couple of hours give up their page; see sleeping_tabs.cpp

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
    {L"restore_windows", &Settings::restoreWindows}, {L"tab_close_buttons", &Settings::tabCloseButtons},
    {L"ctrl_n_in_winamp", &Settings::ctrlNInWinamp}, {L"sound_in_visualizers", &Settings::soundInVisualizers},
    {L"glow_always", &Settings::glowAlways},         {L"bitwarden", &Settings::bitwarden},
    {L"sleep_tabs", &Settings::sleepTabs},
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

// The glow's look, as numbers on the page's sliders, each kept under a key of its own. One
// out of their range is kept to the nearest end of it.
struct GlowNumber {
    std::wstring_view key;
    int GlowLook::* member;
    int least;
    int most;
};
inline constexpr GlowNumber kGlowNumbers[] = {
    {L"glow_size", &GlowLook::size, kGlowSizeLeast, kGlowSizeMost},
    {L"glow_opacity", &GlowLook::opacity, 0, 100},
    {L"glow_spread", &GlowLook::spread, 0, kGlowSpreadMost},
};
// Its colour is kept as RRGGBB in hexadecimal, as on the web, and read with a # before it too.
[[nodiscard]] std::optional<std::uint32_t> ParseColor(std::wstring_view text) noexcept;
[[nodiscard]] std::wstring ColorText(std::uint32_t color);
// A whole number, such as a slider's.
[[nodiscard]] std::optional<int> ParseNumber(std::wstring_view text) noexcept;

// The choice for the glow kept under an id, in any case; nothing for an id that isn't one.
[[nodiscard]] const GlowChoice* FindGlowChoice(std::wstring_view id) noexcept;
[[nodiscard]] const GlowChoice& GlowChoiceFor(GlowAround around) noexcept;

// On or off as the section has it. It is written 1 or 0, the way Winamp writes its own;
// true and false, yes and no, on and off are read as well.
[[nodiscard]] std::optional<bool> ParseFlag(std::wstring_view text) noexcept;

// A home page as typed, taken the way the address bar takes it: "example.com" is
// https://example.com. Nothing typed is the default.
[[nodiscard]] std::wstring NormalizeHomePage(std::wstring_view typed);

} // namespace exodium
