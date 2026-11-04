// Ids of the resources built into the plugin (res\exodium.rc). The resource compiler
// reads this file too, so it holds nothing but definitions.
#ifndef EXODIUM_RESOURCE_H
#define EXODIUM_RESOURCE_H

#define IDR_CORNER_ORNAMENT 101
#define IDR_CORNER_ORNAMENT_PLAIN 102

// Exodium's page in Winamp's Preferences (settings_page.cpp). The smoke test finds its
// controls by these numbers.
#define IDD_PREFS 201
#define IDC_PREFS_HOME_PAGE 1301
#define IDC_PREFS_SEARCH_ENGINE 1302
#define IDC_PREFS_SEARCH_URL 1303
// The checkboxes, one after another.
#define IDC_PREFS_TAB_CLOSE_BUTTONS 1311
#define IDC_PREFS_CTRL_N_IN_WINAMP 1312
#define IDC_PREFS_START_MENU 1313
#define IDC_PREFS_SOUND_IN_VISUALIZERS 1314
// The note at the foot of the options, which says why the Start menu's entry can't be made.
#define IDC_PREFS_START_MENU_NOTE 1331

#endif
