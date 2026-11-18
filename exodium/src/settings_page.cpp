// Exodium's page in Winamp's Preferences, and what the browser does when a setting on it
// changes. What the settings are, and how winamp.ini keeps them, is settings.h.
//
// Winamp lists the page under General Preferences and makes it from the dialog template
// IDD_PREFS whenever it is picked in the tree, and destroys it when another page is
// picked or the Preferences close. A change takes effect as it is made: a checkbox or a
// choice from a list when it is clicked, typed text once the box is left (or the page
// closed), and it is written to winamp.ini then as well. The glow's sliders show what they
// do while they are being dragged, and are written once they are let go.
#include "browser.h"

#include "resource.h"

#include <commdlg.h>
#include <windowsx.h>

#include <charconv>
#include <iterator>
#include <string>
#include <system_error>

namespace exodium {

namespace {

// Where in the Preferences' tree the page goes: under General Preferences, as Winamp's
// own Global Hotkeys and Jump to File pages are.
constexpr intptr_t kUnderGeneralPreferences = 0;

// The page's checkboxes, and the settings they stand for.
struct Checkbox {
    int id;
    bool Settings::* member;
};
constexpr Checkbox kCheckboxes[] = {
    {IDC_PREFS_RESTORE_WINDOWS, &Settings::restoreWindows},
    {IDC_PREFS_TAB_CLOSE_BUTTONS, &Settings::tabCloseButtons},
    {IDC_PREFS_CTRL_N_IN_WINAMP, &Settings::ctrlNInWinamp},
    {IDC_PREFS_SOUND_IN_VISUALIZERS, &Settings::soundInVisualizers},
    {IDC_PREFS_GLOW_ALWAYS, &Settings::glowAlways},
    {IDC_PREFS_BITWARDEN, &Settings::bitwarden},
    {IDC_PREFS_SLEEP_TABS, &Settings::sleepTabs},
};

// The glow's sliders, and what they say they are at.
struct Slider {
    int id;
    int textId;
    int GlowLook::* member;
    const wchar_t* name;
    const wchar_t* unit;
};
constexpr Slider kSliders[] = {
    {IDC_PREFS_GLOW_SIZE, IDC_PREFS_GLOW_SIZE_TEXT, &GlowLook::size, L"Size", L" px"},
    {IDC_PREFS_GLOW_OPACITY, IDC_PREFS_GLOW_OPACITY_TEXT, &GlowLook::opacity, L"Opacity", L"%"},
    {IDC_PREFS_GLOW_SPREAD, IDC_PREFS_GLOW_SPREAD_TEXT, &GlowLook::spread, L"Spread", L"%"},
};
// The controls that are only of use while there is a glow.
constexpr int kGlowLookControls[] = {
    IDC_PREFS_GLOW_ALWAYS,       IDC_PREFS_GLOW_SWATCH,      IDC_PREFS_GLOW_COLOR,  IDC_PREFS_GLOW_DEFAULT,
    IDC_PREFS_GLOW_SIZE,         IDC_PREFS_GLOW_OPACITY,     IDC_PREFS_GLOW_SPREAD, IDC_PREFS_GLOW_SIZE_TEXT,
    IDC_PREFS_GLOW_OPACITY_TEXT, IDC_PREFS_GLOW_SPREAD_TEXT,
};

[[nodiscard]] const GlowNumber& NumberOf(const Slider& slider) noexcept {
    for (const GlowNumber& number : kGlowNumbers) {
        if (number.member == slider.member) return number;
    }
    return kGlowNumbers[0];
}

// The engine list has the engines on offer, in order, and then this.
constexpr wchar_t kCustomEngineName[] = L"Custom";
constexpr int kCustomEngineIndex = static_cast<int>(kSearchEngines.size());

// The longest value read from winamp.ini, in characters.
constexpr DWORD kMaxIniValue = 32768;

wchar_t g_pageName[] = L"Exodium";

INT_PTR CALLBACK PageProc(HWND page, UINT msg, WPARAM wParam, LPARAM lParam);

// Winamp keeps a pointer to this for as long as the page is listed, and writes to it.
prefsDlgRecW g_page{
    .dlgID = IDD_PREFS,
    .proc = reinterpret_cast<void*>(&PageProc),
    .name = g_pageName,
    .where = kUnderGeneralPreferences,
};

// The value of a key in Exodium's section of winamp.ini. Nothing for a key that isn't
// there, or is empty, which every setting reads the same way.
[[nodiscard]] std::optional<std::wstring> ReadIniValue(std::wstring_view key) {
    if (!g_app.winampIni) return std::nullopt;
    const std::wstring name(key);
    std::wstring value(256, L'\0');
    for (;;) {
        const DWORD length = ::GetPrivateProfileStringW(kSettingsSection, name.c_str(), L"", value.data(),
                                                        static_cast<DWORD>(value.size()), g_app.winampIni);
        // A value that fills the buffer may have been cut short.
        if (length + 1 < value.size() || value.size() >= kMaxIniValue) {
            value.resize(length);
            break;
        }
        value.resize(value.size() * 2);
    }
    if (value.empty()) return std::nullopt;
    return value;
}

// The settings as winamp.ini has them, which differ from App::settings while a slider of the
// glow's is being dragged.
Settings g_saved;

// Writes what of `settings` is different from what winamp.ini has.
void SaveSettings(const Settings& settings) {
    if (g_app.winampIni) {
        for (const SettingEntry& entry : ChangedEntries(g_saved, settings)) {
            ::WritePrivateProfileStringW(kSettingsSection, std::wstring(entry.key).c_str(), entry.value.c_str(),
                                         g_app.winampIni);
        }
    }
    g_saved = settings;
}

// Makes `changed` the settings: writes what is different about it to winamp.ini, and
// has the browser follow it.
void ChangeSettings(const Settings& changed) {
    SaveSettings(changed);
    if (changed == g_app.settings) return;
    g_app.settings = changed;
    ApplySettings();
}

[[nodiscard]] int EngineIndex(const Settings& settings) noexcept {
    if (settings.searchEngine == kCustomSearchId) return kCustomEngineIndex;
    const SearchEngine* const engine = FindSearchEngine(settings.searchEngine);
    return engine ? static_cast<int>(engine - kSearchEngines.data()) : 0;
}

// The skin list has the skins in corner_skins.h's order.
[[nodiscard]] int CornerSkinIndex(const Settings& settings) noexcept {
    const CornerSkin* const skin = FindCornerSkin(settings.cornerSkin);
    return skin ? static_cast<int>(skin - kCornerSkins.data()) : 0;
}

// The glow's list has the choices in kGlowChoices' order.
[[nodiscard]] int GlowIndex(const Settings& settings) noexcept {
    return static_cast<int>(&GlowChoiceFor(settings.glow) - kGlowChoices);
}

// The search address box shows the address searches go to. Only a custom engine's can
// be changed; an engine on offer has its own, shown to be read and copied.
void ShowSearchUrl(HWND page, const Settings& settings) {
    const HWND box = ::GetDlgItem(page, IDC_PREFS_SEARCH_URL);
    const bool custom = settings.searchEngine == kCustomSearchId;
    const SearchEngine* const engine = FindSearchEngine(settings.searchEngine);
    const std::wstring url(custom ? std::wstring_view(settings.customSearchUrl)
                                  : (engine ? engine->url : kSearchEngines.front().url));
    ::SetWindowTextW(box, url.c_str());
    ::SendMessageW(box, EM_SETREADONLY, !custom, 0);
}

// What Windows says a failure was, such as "Access is denied."
[[nodiscard]] std::wstring SystemMessage(HRESULT result) {
    wchar_t text[512];
    DWORD length = ::FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
                                    static_cast<DWORD>(result), 0, text, static_cast<DWORD>(std::size(text)), nullptr);
    while (length > 0 && (text[length - 1] == L'\r' || text[length - 1] == L'\n' || text[length - 1] == L' ')) --length;
    if (length > 0) return std::wstring(text, length);
    char code[2 * sizeof(result)];
    const char* const begin = code;
    const char* const end = std::to_chars(code, code + sizeof(code), static_cast<unsigned long>(result), 16).ptr;
    return std::wstring(L"Error 0x").append(begin, end).append(L".");
}

[[nodiscard]] bool OpenerIsThere() {
    std::error_code error;
    return std::filesystem::is_regular_file(OpenerProgram(), error);
}

// The Start menu's checkbox isn't a setting kept in winamp.ini: the shortcut is made or
// removed as it is clicked, and the checkbox shows whether the shortcut is there. Ticking
// it needs exodium_open.exe beside the plugin, which is what the shortcut runs; without
// it the note under the options says so, and the checkbox can only be unticked.
void ShowStartMenu(HWND page, std::wstring_view problem = {}) {
    const bool there = HasStartMenuShortcut();
    const bool openerThere = OpenerIsThere();
    const HWND box = ::GetDlgItem(page, IDC_PREFS_START_MENU);
    ::CheckDlgButton(page, IDC_PREFS_START_MENU, there ? BST_CHECKED : BST_UNCHECKED);
    ::EnableWindow(box, there || openerThere);
    std::wstring note(problem);
    if (note.empty() && !openerThere) {
        note = L"exodium_open.exe isn't in Winamp's Plugins folder. The Start menu's entry runs it, so "
               L"copy it there to add one.";
    }
    ::SetDlgItemTextW(page, IDC_PREFS_START_MENU_NOTE, note.c_str());
}

void TakeStartMenu(HWND page) {
    const bool wanted = ::IsDlgButtonChecked(page, IDC_PREFS_START_MENU) == BST_CHECKED;
    const HRESULT result = wanted ? AddStartMenuShortcut() : RemoveStartMenuShortcut();
    std::wstring problem;
    // Without exodium_open.exe there is a note about that already.
    if (FAILED(result) && OpenerIsThere()) {
        problem = wanted ? L"Exodium couldn't be added to the Start menu: "
                         : L"Exodium couldn't be taken out of the Start menu: ";
        problem += SystemMessage(result);
    }
    ShowStartMenu(page, problem);
}

[[nodiscard]] COLORREF ColorRefOf(std::uint32_t rgb) noexcept {
    return RGB((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF);
}

// Puts the glow's sliders where its look has them, says what they are at, and has the square
// of its colour painted again. Of no use without a glow, they can't be used then.
void ShowGlowLook(HWND page, const Settings& settings) {
    const GlowLook& look = settings.glowLook;
    for (const Slider& slider : kSliders) {
        const int value = look.*slider.member;
        ::SendDlgItemMessageW(page, slider.id, TBM_SETPOS, TRUE, value);
        const std::wstring text = std::wstring(slider.name) + L": " + std::to_wstring(value) + slider.unit;
        ::SetDlgItemTextW(page, slider.textId, text.c_str());
    }
    ::InvalidateRect(::GetDlgItem(page, IDC_PREFS_GLOW_SWATCH), nullptr, TRUE);
    const bool glowing = settings.glow != GlowAround::Nothing;
    for (const int id : kGlowLookControls) ::EnableWindow(::GetDlgItem(page, id), glowing);
}

void FillPage(HWND page) {
    const Settings& settings = g_app.settings;
    ::SetDlgItemTextW(page, IDC_PREFS_HOME_PAGE, settings.homePage.c_str());

    const HWND engines = ::GetDlgItem(page, IDC_PREFS_SEARCH_ENGINE);
    for (const SearchEngine& engine : kSearchEngines) ComboBox_AddString(engines, std::wstring(engine.name).c_str());
    ComboBox_AddString(engines, kCustomEngineName);
    ComboBox_SetCurSel(engines, EngineIndex(settings));
    ShowSearchUrl(page, settings);

    for (const Checkbox& box : kCheckboxes) {
        ::CheckDlgButton(page, box.id, settings.*box.member ? BST_CHECKED : BST_UNCHECKED);
    }
    ShowStartMenu(page);

    const HWND skins = ::GetDlgItem(page, IDC_PREFS_CORNER_SKIN);
    for (const CornerSkin& skin : kCornerSkins) ComboBox_AddString(skins, std::wstring(skin.name).c_str());
    ComboBox_SetCurSel(skins, CornerSkinIndex(settings));

    const HWND glow = ::GetDlgItem(page, IDC_PREFS_GLOW);
    for (const GlowChoice& choice : kGlowChoices) ComboBox_AddString(glow, std::wstring(choice.name).c_str());
    ComboBox_SetCurSel(glow, GlowIndex(settings));
    for (const Slider& slider : kSliders) {
        const GlowNumber& number = NumberOf(slider);
        const HWND bar = ::GetDlgItem(page, slider.id);
        ::SendMessageW(bar, TBM_SETRANGEMIN, FALSE, number.least);
        ::SendMessageW(bar, TBM_SETRANGEMAX, FALSE, number.most);
        ::SendMessageW(bar, TBM_SETLINESIZE, 0, 1);
        ::SendMessageW(bar, TBM_SETPAGESIZE, 0, 10);
    }
    ShowGlowLook(page, settings);
}

// Puts `text` in a box if it isn't there already. Typed text is shown as it was taken:
// "example.com" as https://example.com, and nothing as the default.
void ShowTaken(HWND box, const std::wstring& text) {
    if (WindowText(box) != text) ::SetWindowTextW(box, text.c_str());
}

void TakeHomePage(HWND page) {
    const HWND box = ::GetDlgItem(page, IDC_PREFS_HOME_PAGE);
    Settings changed = g_app.settings;
    changed.homePage = NormalizeHomePage(WindowText(box));
    ShowTaken(box, changed.homePage);
    ChangeSettings(changed);
}

void TakeSearchUrl(HWND page) {
    if (g_app.settings.searchEngine != kCustomSearchId) return; // the box shows an engine's own address
    const HWND box = ::GetDlgItem(page, IDC_PREFS_SEARCH_URL);
    Settings changed = g_app.settings;
    changed.customSearchUrl = NormalizeSearchUrl(WindowText(box));
    ShowTaken(box, changed.customSearchUrl);
    ChangeSettings(changed);
}

void TakeSearchEngine(HWND page) {
    const int index = ComboBox_GetCurSel(::GetDlgItem(page, IDC_PREFS_SEARCH_ENGINE));
    if (index < 0 || index > kCustomEngineIndex) return;
    Settings changed = g_app.settings;
    if (index < kCustomEngineIndex) {
        changed.searchEngine = kSearchEngines[static_cast<std::size_t>(index)].id;
    } else {
        changed.searchEngine = kCustomSearchId;
        // A custom engine starts out as the one it takes over from, to be changed from there.
        if (changed.customSearchUrl.empty()) changed.customSearchUrl = SearchUrlTemplate();
    }
    ChangeSettings(changed);
    ShowSearchUrl(page, g_app.settings);
}

void TakeCornerSkin(HWND page) {
    const int index = ComboBox_GetCurSel(::GetDlgItem(page, IDC_PREFS_CORNER_SKIN));
    if (index < 0 || index >= static_cast<int>(kCornerSkins.size())) return;
    Settings changed = g_app.settings;
    changed.cornerSkin = kCornerSkins[static_cast<std::size_t>(index)].id;
    ChangeSettings(changed);
}

void TakeGlow(HWND page) {
    const int index = ComboBox_GetCurSel(::GetDlgItem(page, IDC_PREFS_GLOW));
    if (index < 0 || index >= static_cast<int>(std::size(kGlowChoices))) return;
    Settings changed = g_app.settings;
    changed.glow = kGlowChoices[index].around;
    ChangeSettings(changed);
    ShowGlowLook(page, g_app.settings);
}

// A slider of the glow's moved. While it is dragged the glow follows it, and once it is let
// go that is written.
void TakeSlider(HWND page, HWND bar, int code) {
    const int id = ::GetDlgCtrlID(bar);
    const auto slider = std::ranges::find(kSliders, id, &Slider::id);
    if (slider == std::ranges::end(kSliders)) return;
    const GlowNumber& number = NumberOf(*slider);
    const int value = std::clamp(static_cast<int>(::SendMessageW(bar, TBM_GETPOS, 0, 0)), number.least, number.most);
    Settings changed = g_app.settings;
    changed.glowLook.*slider->member = value;
    if (code == TB_THUMBTRACK) {
        g_app.settings = changed;
        FollowGlow();
    } else {
        ChangeSettings(changed);
    }
    ShowGlowLook(page, g_app.settings);
}

// Windows' own colour picker, which keeps the colours it is given to keep for as long as
// Winamp runs.
void PickGlowColor(HWND page) {
    static COLORREF kept[16] = {};
    CHOOSECOLORW choose{
        .lStructSize = sizeof(choose),
        .hwndOwner = ::GetAncestor(page, GA_ROOT),
        .rgbResult = ColorRefOf(g_app.settings.glowLook.color),
        .lpCustColors = kept,
        .Flags = CC_RGBINIT | CC_FULLOPEN,
    };
    if (!::ChooseColorW(&choose)) return;
    Settings changed = g_app.settings;
    changed.glowLook.color = static_cast<std::uint32_t>(GetRValue(choose.rgbResult)) << 16 |
                             static_cast<std::uint32_t>(GetGValue(choose.rgbResult)) << 8 | GetBValue(choose.rgbResult);
    ChangeSettings(changed);
    ShowGlowLook(page, g_app.settings);
}

// The glow's look as it was to begin with, whatever it goes round.
void PutBackGlowLook(HWND page) {
    Settings changed = g_app.settings;
    changed.glowLook = GlowLook{};
    ChangeSettings(changed);
    ShowGlowLook(page, g_app.settings);
}

// Says whether the command was one of the page's.
bool OnPageCommand(HWND page, int id, int code) {
    switch (id) {
    case IDC_PREFS_HOME_PAGE:
        if (code == EN_KILLFOCUS) TakeHomePage(page);
        return true;
    case IDC_PREFS_SEARCH_URL:
        if (code == EN_KILLFOCUS) TakeSearchUrl(page);
        return true;
    case IDC_PREFS_SEARCH_ENGINE:
        if (code == CBN_SELCHANGE) TakeSearchEngine(page);
        return true;
    case IDC_PREFS_CORNER_SKIN:
        if (code == CBN_SELCHANGE) TakeCornerSkin(page);
        return true;
    case IDC_PREFS_GLOW:
        if (code == CBN_SELCHANGE) TakeGlow(page);
        return true;
    case IDC_PREFS_START_MENU:
        if (code == BN_CLICKED) TakeStartMenu(page);
        return true;
    case IDC_PREFS_GLOW_COLOR:
        if (code == BN_CLICKED) PickGlowColor(page);
        return true;
    case IDC_PREFS_GLOW_SWATCH:
        if (code == STN_CLICKED) PickGlowColor(page);
        return true;
    case IDC_PREFS_GLOW_DEFAULT:
        if (code == BN_CLICKED) PutBackGlowLook(page);
        return true;
    default:
        break;
    }
    for (const Checkbox& box : kCheckboxes) {
        if (box.id != id || code != BN_CLICKED) continue;
        Settings changed = g_app.settings;
        changed.*box.member = ::IsDlgButtonChecked(page, id) == BST_CHECKED;
        ChangeSettings(changed);
        return true;
    }
    return false;
}

INT_PTR CALLBACK PageProc(HWND page, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_INITDIALOG:
        FillPage(page);
        // The keyboard stays in the Preferences' tree, where the page was picked.
        return FALSE;
    case WM_COMMAND:
        return OnPageCommand(page, LOWORD(wParam), HIWORD(wParam)) ? TRUE : FALSE;
    case WM_HSCROLL:
        TakeSlider(page, reinterpret_cast<HWND>(lParam), LOWORD(wParam));
        return TRUE;
    case WM_CTLCOLORSTATIC:
        // The square of the glow's colour is painted that colour.
        if (reinterpret_cast<HWND>(lParam) == ::GetDlgItem(page, IDC_PREFS_GLOW_SWATCH)) {
            const auto dc = reinterpret_cast<HDC>(wParam);
            ::SetDCBrushColor(dc, ColorRefOf(g_app.settings.glowLook.color));
            return reinterpret_cast<INT_PTR>(::GetStockObject(DC_BRUSH));
        }
        return FALSE;
    case WM_DESTROY:
        // The Preferences closing, or another page picked, while text was being typed or a
        // slider dragged.
        TakeHomePage(page);
        TakeSearchUrl(page);
        SaveSettings(g_app.settings);
        return FALSE;
    default:
        return FALSE;
    }
}

} // namespace

const std::wstring& HomePage() noexcept {
    return g_app.settings.homePage;
}

void LoadSettings() {
    g_app.settings = g_saved = ReadSettings(ReadIniValue);
    ApplySettings();
}

void ApplySettings() {
    UseSearchEngine(g_app.settings.searchEngine, g_app.settings.customSearchUrl);
    // Another skin for the corners gives every window new ornaments. They lie over the tab
    // strip by as much as their skin's pictures do, so this goes before the strips.
    FollowCornerSkin();
    // Whether tabs have close buttons changes what the tab strip holds.
    RefreshTabStrips();
    // Ctrl+N in Winamp's windows is only watched for while it is wanted.
    FollowWinampNewWindowKey();
    // Turned off, Winamp stops playing the pages' sound; turned on, it may start.
    FollowPageSound();
    // The glow goes round other windows, or stops.
    FollowGlow();
    // Bitwarden is turned on or off in the profile, and its button comes or goes.
    FollowExtensionSettings();
}

void RefreshTabStrips() {
    for (const auto& win : g_app.windows) {
        if (!win->client) continue;
        LayoutToolbarAndTabs(*win);
        RECT strip = ClientRectOf(win->client);
        strip.bottom = kTabBarHeight;
        ::RedrawWindow(win->client, &strip, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
    }
}

void AddPreferencesPage() {
    g_page.hInst = g_app.instance;
    ::SendMessageW(g_app.winamp, WM_WA_IPC, reinterpret_cast<WPARAM>(&g_page), IPC_ADD_PREFS_DLGW);
}

void RemovePreferencesPage() {
    if (::IsWindow(g_app.winamp)) {
        ::SendMessageW(g_app.winamp, WM_WA_IPC, reinterpret_cast<WPARAM>(&g_page), IPC_REMOVE_PREFS_DLG);
    }
}

void OpenPreferencesPage() {
    // Posted, because Winamp's Configure button asks for it, and the page it is on goes
    // when this one is shown.
    ::PostMessageW(g_app.winamp, WM_WA_IPC, reinterpret_cast<WPARAM>(&g_page), IPC_OPENPREFSTOPAGE);
}

} // namespace exodium
