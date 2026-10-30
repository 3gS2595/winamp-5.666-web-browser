// gen_llama: plays demo.mp3 ("...it really whips the llama's ass!") on Winamp
// startup whenever the splash screen is enabled (winamp.ini, [Winamp] splash=1).

#include <windows.h>

#include <mmsystem.h>

#include <gen.h>
#include <wa_ipc.h>

#include <filesystem>
#include <string>
#include <system_error>

namespace {

constexpr wchar_t kMciAlias[] = L"genllama";

int __cdecl Init();
void __cdecl Config();
void __cdecl Quit();

char g_description[] = "Llama Whip Splash Sound v1.0";
winampGeneralPurposePlugin g_plugin{
    .version = GPPHDR_VER,
    .description = g_description,
    .init = Init,
    .config = Config,
    .quit = Quit,
};

bool g_soundOpen = false;

// Says whether MCI took the command.
bool SendMci(const std::wstring& command) {
    return ::mciSendStringW(command.c_str(), nullptr, 0, nullptr) == 0;
}

// winamp.ini, as Winamp names it. Empty if it won't say.
std::filesystem::path WinampIniFile() {
    const auto* path =
        reinterpret_cast<const wchar_t*>(::SendMessageW(g_plugin.hwndParent, WM_WA_IPC, 0, IPC_GETINIFILEW));
    return path ? std::filesystem::path(path) : std::filesystem::path();
}

int __cdecl Init() {
    const std::filesystem::path ini = WinampIniFile();
    if (ini.empty()) return GEN_INIT_SUCCESS;
    if (!::GetPrivateProfileIntW(L"Winamp", L"splash", 1, ini.c_str())) return GEN_INIT_SUCCESS;

    // demo.mp3 ships alongside winamp.ini.
    const std::filesystem::path sound = ini.parent_path() / L"demo.mp3";
    std::error_code error;
    if (!std::filesystem::exists(sound, error)) return GEN_INIT_SUCCESS;

    g_soundOpen = SendMci(L"open \"" + sound.wstring() + L"\" type mpegvideo alias " + kMciAlias);
    if (g_soundOpen) SendMci(std::wstring(L"play ") + kMciAlias);
    return GEN_INIT_SUCCESS;
}

void __cdecl Config() {
    // No settings to configure - behavior is entirely driven by Winamp's
    // own splash screen preference.
}

void __cdecl Quit() {
    if (g_soundOpen) SendMci(std::wstring(L"close ") + kMciAlias);
    g_soundOpen = false;
}

} // namespace

extern "C" __declspec(dllexport) winampGeneralPurposePlugin* __cdecl winampGetGeneralPurposePlugin() {
    return &g_plugin;
}
