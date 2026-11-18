// Ids of the resources built into the plugin (res\exodium.rc). The resource compiler
// reads this file too, so it holds nothing but definitions.
#ifndef EXODIUM_RESOURCE_H
#define EXODIUM_RESOURCE_H

// The pictures of the corner ornaments' skins (corner_skins.h).
#define IDR_CORNER_ORNAMENT 101
#define IDR_CORNER_ORNAMENT_PLAIN 102
#define IDR_CORNER_VENOM_TOP_RIGHT 103
#define IDR_CORNER_VENOM_TOP_LEFT 104
#define IDR_CORNER_VENOM_BOTTOM_RIGHT 105
#define IDR_CORNER_VENOM_BOTTOM_LEFT 106
#define IDR_CORNER_CHARMS_TOP_RIGHT 107
#define IDR_CORNER_CHARMS_TOP_LEFT 108
#define IDR_CORNER_CHARMS_BOTTOM_RIGHT 109
#define IDR_CORNER_CHARMS_BOTTOM_LEFT 110
#define IDR_CORNER_DARKCHARMS_TOP_RIGHT 111
#define IDR_CORNER_DARKCHARMS_TOP_LEFT 112
#define IDR_CORNER_DARKCHARMS_BOTTOM_RIGHT 113
#define IDR_CORNER_DARKCHARMS_BOTTOM_LEFT 114

// Exodium's page in Winamp's Preferences (settings_page.cpp). The smoke test finds its
// controls by these numbers.
#define IDD_PREFS 201
#define IDC_PREFS_HOME_PAGE 1301
#define IDC_PREFS_SEARCH_ENGINE 1302
#define IDC_PREFS_SEARCH_URL 1303
#define IDC_PREFS_CORNER_SKIN 1304
#define IDC_PREFS_GLOW 1305
// The glow's look: its sliders, its colour and the button that puts them back.
#define IDC_PREFS_GLOW_SIZE 1306
#define IDC_PREFS_GLOW_OPACITY 1307
#define IDC_PREFS_GLOW_SPREAD 1308
#define IDC_PREFS_GLOW_COLOR 1309
#define IDC_PREFS_GLOW_DEFAULT 1310
// The checkboxes, one after another.
#define IDC_PREFS_TAB_CLOSE_BUTTONS 1311
#define IDC_PREFS_CTRL_N_IN_WINAMP 1312
#define IDC_PREFS_START_MENU 1313
#define IDC_PREFS_SOUND_IN_VISUALIZERS 1314
#define IDC_PREFS_GLOW_ALWAYS 1315     // in the Looks box, under the glow's list
#define IDC_PREFS_BITWARDEN 1316       // beside the close buttons' box, the page having no room under the others
#define IDC_PREFS_RESTORE_WINDOWS 1317 // the first in the Options box
#define IDC_PREFS_SLEEP_TABS 1318      // beside the Start menu's box, as Bitwarden's is beside the close buttons'
// The note at the foot of the options, which says why the Start menu's entry can't be made.
#define IDC_PREFS_START_MENU_NOTE 1331
// What the glow's sliders are at, and a square of its colour.
#define IDC_PREFS_GLOW_SIZE_TEXT 1332
#define IDC_PREFS_GLOW_OPACITY_TEXT 1333
#define IDC_PREFS_GLOW_SPREAD_TEXT 1334
#define IDC_PREFS_GLOW_SWATCH 1335

#endif
