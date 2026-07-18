// gen_llama: plays demo.mp3 ("...it really whips the llama's ass!") on Winamp
// startup whenever the splash screen is enabled (winamp.ini, [Winamp] splash=1).

#include <windows.h>
#include <mmsystem.h>
#include <shlwapi.h>
#include <string>

#include "gen.h"
#include "wa_ipc.h"

#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "shlwapi.lib")

static const wchar_t* kMciAlias = L"genllama";

static int __cdecl Init();
static void __cdecl Config();
static void __cdecl Quit();

static winampGeneralPurposePlugin plugin = {
    GPPHDR_VER,
    (char*)"Llama Whip Splash Sound v1.0",
    Init,
    Config,
    Quit,
    nullptr,
    nullptr,
};

// Looks up winamp.ini's full path via IPC and returns the directory it lives
// in (demo.mp3 ships alongside it), or an empty string if unavailable.
static std::wstring GetWinampDir()
{
    wchar_t iniPath[MAX_PATH] = {0};
    LRESULT p = SendMessageW(plugin.hwndParent, WM_WA_IPC, 0, IPC_GETINIFILEW);
    if (!p) return L"";
    lstrcpynW(iniPath, (wchar_t*)p, MAX_PATH);
    PathRemoveFileSpecW(iniPath);
    return iniPath;
}

static int __cdecl Init()
{
    std::wstring dir = GetWinampDir();
    if (dir.empty()) return GEN_INIT_SUCCESS;

    int splashEnabled = GetPrivateProfileIntW(L"Winamp", L"splash", 1, (dir + L"\\winamp.ini").c_str());
    if (!splashEnabled) return GEN_INIT_SUCCESS;

    std::wstring mp3Path = dir + L"\\demo.mp3";
    if (GetFileAttributesW(mp3Path.c_str()) == INVALID_FILE_ATTRIBUTES)
        return GEN_INIT_SUCCESS;

    wchar_t cmd[MAX_PATH + 64];
    wsprintfW(cmd, L"open \"%s\" type mpegvideo alias %s", mp3Path.c_str(), kMciAlias);
    if (mciSendStringW(cmd, nullptr, 0, nullptr) == 0)
    {
        wchar_t playCmd[64];
        wsprintfW(playCmd, L"play %s", kMciAlias);
        mciSendStringW(playCmd, nullptr, 0, nullptr);
    }

    return GEN_INIT_SUCCESS;
}

static void __cdecl Config()
{
    // No settings to configure - behavior is entirely driven by Winamp's
    // own splash screen preference.
}

static void __cdecl Quit()
{
    wchar_t closeCmd[64];
    wsprintfW(closeCmd, L"close %s", kMciAlias);
    mciSendStringW(closeCmd, nullptr, 0, nullptr);
}

extern "C" __declspec(dllexport) winampGeneralPurposePlugin* __cdecl winampGetGeneralPurposePlugin()
{
    return &plugin;
}
