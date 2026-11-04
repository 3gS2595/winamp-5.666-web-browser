// Exodium's page in Winamp's Preferences, and what the browser does when a setting on it
// changes. What the settings are, and how winamp.ini keeps them, is settings.h.
//
// Winamp lists the page under General Preferences and makes it from the dialog template
// IDD_PREFS whenever it is picked in the tree, and destroys it when another page is
// picked or the Preferences close. A change takes effect as it is made: a checkbox or a
// choice of engine when it is clicked, typed text once the box is left (or the page
// closed), and it is written to winamp.ini then as well.
#include "browser.h"

#include "resource.h"

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
    {IDC_PREFS_TAB_CLOSE_BUTTONS, &Settings::tabCloseButtons},
    {IDC_PREFS_CTRL_N_IN_WINAMP, &Settings::ctrlNInWinamp},
    {IDC_PREFS_SOUND_IN_VISUALIZERS, &Settings::soundInVisualizers},
};

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

// Makes `changed` the settings: writes what is different about it to winamp.ini, and
// has the browser follow it.
void ChangeSettings(const Settings& changed) {
    if (changed == g_app.settings) return;
    if (g_app.winampIni) {
        for (const SettingEntry& entry : ChangedEntries(g_app.settings, changed)) {
            ::WritePrivateProfileStringW(kSettingsSection, std::wstring(entry.key).c_str(), entry.value.c_str(),
                                         g_app.winampIni);
        }
    }
    g_app.settings = changed;
    ApplySettings();
}

[[nodiscard]] int EngineIndex(const Settings& settings) noexcept {
    if (settings.searchEngine == kCustomSearchId) return kCustomEngineIndex;
    const SearchEngine* const engine = FindSearchEngine(settings.searchEngine);
    return engine ? static_cast<int>(engine - kSearchEngines.data()) : 0;
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
    case IDC_PREFS_START_MENU:
        if (code == BN_CLICKED) TakeStartMenu(page);
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

INT_PTR CALLBACK PageProc(HWND page, UINT msg, WPARAM wParam, LPARAM) {
    switch (msg) {
    case WM_INITDIALOG:
        FillPage(page);
        // The keyboard stays in the Preferences' tree, where the page was picked.
        return FALSE;
    case WM_COMMAND:
        return OnPageCommand(page, LOWORD(wParam), HIWORD(wParam)) ? TRUE : FALSE;
    case WM_DESTROY:
        // The Preferences closing, or another page picked, while text was being typed.
        TakeHomePage(page);
        TakeSearchUrl(page);
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
    g_app.settings = ReadSettings(ReadIniValue);
    ApplySettings();
}

void ApplySettings() {
    UseSearchEngine(g_app.settings.searchEngine, g_app.settings.customSearchUrl);
    // Whether tabs have close buttons changes what the tab strip holds.
    RefreshTabStrips();
    // Ctrl+N in Winamp's windows is only watched for while it is wanted.
    FollowWinampNewWindowKey();
    // Turned off, Winamp stops playing the pages' sound; turned on, it may start.
    FollowPageSound();
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
