#include "settings.h"

#include "layout.h"

#include <algorithm>

namespace covers {

namespace {

int Read(const std::wstring& ini, const wchar_t* key, int fallback) {
    return static_cast<int>(::GetPrivateProfileIntW(kSettingsSection, key, fallback, ini.c_str()));
}

void Write(const std::wstring& ini, const wchar_t* key, int value) {
    ::WritePrivateProfileStringW(kSettingsSection, key, std::to_wstring(value).c_str(), ini.c_str());
}

std::wstring ReadText(const std::wstring& ini, const wchar_t* section, const std::wstring& key) {
    wchar_t text[260] = {};
    ::GetPrivateProfileStringW(section, key.c_str(), L"", text, static_cast<DWORD>(std::size(text)), ini.c_str());
    return text;
}

} // namespace

Settings LoadSettings(const std::wstring& iniFile) {
    Settings settings;
    if (iniFile.empty()) return settings;
    // What the first versions kept, for a window of its own: no longer used.
    for (const wchar_t* gone : {L"left", L"top", L"width", L"height", L"visible", L"split", L"title_share",
                                L"artist_share", L"art_size"}) {
        ::WritePrivateProfileStringW(kSettingsSection, gone, nullptr, iniFile.c_str());
    }
    const int size = Read(iniFile, L"cover_size", settings.coverSize);
    settings.coverSize = size == 0 ? 0 : std::clamp(size, 32, 320);
    settings.coversShare = std::clamp(Read(iniFile, L"covers_share", settings.coversShare), 100, 900);
    settings.order = std::clamp(Read(iniFile, L"order", settings.order), 0, 2);
    return settings;
}

void SaveSettings(const std::wstring& iniFile, const Settings& settings) {
    if (iniFile.empty()) return;
    Write(iniFile, L"cover_size", settings.coverSize);
    Write(iniFile, L"covers_share", settings.coversShare);
    Write(iniFile, L"order", settings.order);
}

// gen_ml.ini names each of the library's views (queryN_name_utf8) and the file of its
// settings (queryN_meta); the Audio view's has the album art view's mode in it.
AudioViewLook ReadAudioViewLook(const std::wstring& libraryFolder) {
    AudioViewLook look;
    if (libraryFolder.empty()) return look;
    const std::wstring library = libraryFolder + L"\\gen_ml.ini";
    constexpr wchar_t kSection[] = L"gen_ml_config";
    std::wstring meta;
    for (int n = 1; n <= 64 && meta.empty(); ++n) {
        const std::wstring query = L"query" + std::to_wstring(n);
        if (ReadText(library, kSection, query + L"_name_utf8") == L"Audio") meta = ReadText(library, kSection, query + L"_meta");
    }
    // Renamed or in another language: the first view, which Audio is unless moved.
    if (meta.empty()) meta = ReadText(library, kSection, L"query1_meta");
    if (meta.empty()) return look;
    const std::wstring view = libraryFolder + L"\\ml\\views\\" + meta;
    look.coverSize = ArtSizeOfViewMode(static_cast<int>(::GetPrivateProfileIntW(kSection, L"albumartviewmode", 1, view.c_str())));
    look.names = ::GetPrivateProfileIntW(kSection, L"albumarticonmode", 0, view.c_str()) == 0;
    return look;
}

} // namespace covers
