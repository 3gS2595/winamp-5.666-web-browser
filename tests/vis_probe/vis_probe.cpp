// vis_probe.dll: a visualization plug-in for the smoke test, never for real use. It draws
// nothing; it writes down what Winamp gives the visualizers, so the test can see whether
// Exodium's sound reaches them.
//
// Every 100 ms it appends a line to the file EXODIUM_VIS_PROBE_LOG names:
//   <ms since start> <wave> <spectrum> <analyzer>
// wave and spectrum are the loudest values in the data visualization plug-ins get
// (MilkDrop, AVS and the like); analyzer the loudest in what the main window's analyzer
// draws, from IPC_GETSADATAFUNC, or -1 when Winamp won't say.
#include <windows.h>

#include <vis.h>
#include <wa_ipc.h>

#include <share.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace {

constexpr ULONGLONG kLogEveryMs = 100;

FILE* g_log = nullptr;
ULONGLONG g_startedAt = 0;
ULONGLONG g_loggedAt = 0;
char* (*g_saGet)(char data[75 * 2 + 8]) = nullptr;

void Config(winampVisModule*) {}

// Never fails, which Winamp would report in a message box: without a log it just writes nothing.
int Init(winampVisModule* module) {
    wchar_t path[MAX_PATH]{};
    if (!::GetEnvironmentVariableW(L"EXODIUM_VIS_PROBE_LOG", path, MAX_PATH)) return 0;
    // Shared, so the test can read it while it grows.
    g_log = _wfsopen(path, L"a", _SH_DENYNO);
    if (!g_log) return 0;
    g_startedAt = ::GetTickCount64();
    // The main window's analyzer data, worked out whatever the analyzer is set to show.
    void (*setReq)(int) = nullptr;
    *reinterpret_cast<void**>(&setReq) =
        reinterpret_cast<void*>(::SendMessageW(module->hwndParent, WM_WA_IPC, 1, IPC_GETSADATAFUNC));
    if (setReq) setReq(1);
    *reinterpret_cast<void**>(&g_saGet) =
        reinterpret_cast<void*>(::SendMessageW(module->hwndParent, WM_WA_IPC, 2, IPC_GETSADATAFUNC));
    std::fprintf(g_log, "started\n");
    std::fflush(g_log);
    return 0;
}

int Render(winampVisModule* module) {
    const ULONGLONG now = ::GetTickCount64();
    if (!g_log || now - g_loggedAt < kLogEveryMs) return 0;
    g_loggedAt = now;
    int wave = 0, spectrum = 0, analyzer = -1;
    for (int channel = 0; channel < std::min(module->waveformNch, 2); ++channel) {
        for (const unsigned char value : module->waveformData[channel]) {
            wave = std::max(wave, std::abs(static_cast<int>(static_cast<signed char>(value))));
        }
    }
    for (int channel = 0; channel < std::min(module->spectrumNch, 2); ++channel) {
        for (const unsigned char value : module->spectrumData[channel]) spectrum = std::max(spectrum, int{value});
    }
    if (g_saGet) {
        char data[75 * 2 + 8]{};
        if (const char* values = g_saGet(data)) {
            analyzer = 0;
            for (int i = 0; i < 75 * 2; ++i) {
                analyzer = std::max(analyzer, std::abs(static_cast<int>(static_cast<signed char>(values[i]))));
            }
        }
    }
    std::fprintf(g_log, "%llu %d %d %d\n", now - g_startedAt, wave, spectrum, analyzer);
    std::fflush(g_log);
    return 0;
}

void Quit(winampVisModule*) {
    if (g_log) {
        std::fprintf(g_log, "stopped\n");
        std::fclose(g_log);
        g_log = nullptr;
    }
}

char g_moduleName[] = "Exodium smoke test probe";
winampVisModule g_module{
    .description = g_moduleName,
    .latencyMs = 0,
    .delayMs = 25,
    .spectrumNch = 2,
    .waveformNch = 2,
    .Config = Config,
    .Init = Init,
    .Render = Render,
    .Quit = Quit,
};

winampVisModule* GetModule(int which) {
    return which == 0 ? &g_module : nullptr;
}

char g_headerName[] = "Exodium smoke test probe";
winampVisHeader g_header{VIS_HDRVER, g_headerName, GetModule};

} // namespace

extern "C" __declspec(dllexport) winampVisHeader* winampVisGetHeader(HWND) {
    return &g_header;
}
