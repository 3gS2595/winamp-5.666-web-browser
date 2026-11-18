// What is remembered between sessions, kept in winamp.ini under [PlaylistCovers].
#pragma once

#include <windows.h>

#include <string>

namespace covers {

struct Settings {
    int coverSize = 0;     // 0: the size the library's Audio view shows its albums at
    int coversShare = 450; // per thousand of the playlist view's list area the covers take
    int order = 0;         // GroupOrder
};

inline constexpr wchar_t kSettingsSection[] = L"PlaylistCovers";

[[nodiscard]] Settings LoadSettings(const std::wstring& iniFile);
void SaveSettings(const std::wstring& iniFile, const Settings& settings);

// How the library's Audio view shows its albums, read from its own settings (gen_ml.ini
// and the view's file in ml\views, below `libraryFolder`, Winamp's Plugins settings).
struct AudioViewLook {
    int coverSize = 90;
    bool names = true; // the album's name under its cover ("icons only" leaves it out)
};
[[nodiscard]] AudioViewLook ReadAudioViewLook(const std::wstring& libraryFolder);

} // namespace covers
