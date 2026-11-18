// vis_probe.dll: a visualization plug-in for the smoke test, never for real use. It draws
// nothing; it writes down what Winamp gives the visualizers, so the test can see whether
// Exodium's sound reaches them, and how old it is when it does.
//
// Every 100 ms it appends a line to the file EXODIUM_VIS_PROBE_LOG names:
//   <ms since start> <wave> <spectrum> <analyzer>
// wave and spectrum are the loudest values in the data visualization plug-ins get
// (MilkDrop, AVS and the like); analyzer the loudest in what the main window's analyzer
// draws, from IPC_GETSADATAFUNC, or -1 when Winamp won't say.
//
// With EXODIUM_VIS_PROBE_PICTURES naming a file, it also appends a line there for every
// picture Winamp has it draw, when in_exodium.dll marks its blocks (sound_mark.h):
//   <drawn> <heard>
// both in units of 100 µs, the low 32 bits of SoundMarkTime: when the picture was drawn,
// and when the sound it was given was heard, or "-" when that carries no mark (silence
// Winamp made up, or anything not from in_exodium.dll). Each time it starts, it writes
// "start <delayMs> <latencyMs>" there first.
//
// EXODIUM_VIS_PROBE_TIMING can name a file holding "<delayMs> <latencyMs>", read whenever
// Winamp loads the probe, to have it drawn at another pace, or have it ask for its data
// further ahead, as other visualization plug-ins do. Otherwise it is drawn every 25 ms and
// asks for none ahead.
#include <windows.h>

#include <vis.h>
#include <wa_ipc.h>

#include <share.h>

#include "sound_mark.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace {

constexpr ULONGLONG kLogEveryMs = 100;

FILE* g_log = nullptr;
FILE* g_pictures = nullptr;
ULONGLONG g_startedAt = 0;
ULONGLONG g_loggedAt = 0;
char* (*g_saGet)(char data[75 * 2 + 8]) = nullptr;

// Now, in the units in_exodium.dll marks its blocks in (its TicksNow, from the same clock).
[[nodiscard]] std::uint32_t MarkTimeNow() noexcept {
    LARGE_INTEGER frequency, now;
    ::QueryPerformanceFrequency(&frequency);
    ::QueryPerformanceCounter(&now);
    const std::int64_t ticks = now.QuadPart / frequency.QuadPart * 10'000'000 +
                               now.QuadPart % frequency.QuadPart * 10'000'000 / frequency.QuadPart;
    return exodium::SoundMarkTime(ticks);
}

[[nodiscard]] FILE* OpenFromEnvironment(const wchar_t* name) {
    wchar_t path[MAX_PATH]{};
    if (!::GetEnvironmentVariableW(name, path, MAX_PATH)) return nullptr;
    // Shared, so the test can read it while it grows.
    return _wfsopen(path, L"a", _SH_DENYNO);
}

void Config(winampVisModule*) {}

// Never fails, which Winamp would report in a message box: without a log it just writes nothing.
int Init(winampVisModule* module) {
    g_pictures = OpenFromEnvironment(L"EXODIUM_VIS_PROBE_PICTURES");
    if (g_pictures) {
        std::fprintf(g_pictures, "start %d %d\n", module->delayMs, module->latencyMs);
        std::fflush(g_pictures);
    }
    g_log = OpenFromEnvironment(L"EXODIUM_VIS_PROBE_LOG");
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
    const auto mark = module->waveformNch > 0 ? exodium::ReadSoundMark(module->waveformData[0]) : std::nullopt;
    if (g_pictures) {
        const std::uint32_t now = MarkTimeNow();
        if (mark) std::fprintf(g_pictures, "%lu %lu\n", static_cast<unsigned long>(now), static_cast<unsigned long>(*mark));
        else std::fprintf(g_pictures, "%lu -\n", static_cast<unsigned long>(now));
        std::fflush(g_pictures); // the test reads it as it grows
    }

    const ULONGLONG now = ::GetTickCount64();
    if (!g_log || now - g_loggedAt < kLogEveryMs) return 0;
    g_loggedAt = now;
    int wave = 0, spectrum = 0, analyzer = -1;
    for (int channel = 0; channel < std::min(module->waveformNch, 2); ++channel) {
        // A mark is no part of the sound.
        const int from = (mark && channel == 0) ? exodium::kSoundMarkFrames : 0;
        for (int i = from; i < 576; ++i) {
            wave = std::max(wave, std::abs(static_cast<int>(static_cast<signed char>(module->waveformData[channel][i]))));
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
    if (g_pictures) {
        std::fclose(g_pictures);
        g_pictures = nullptr;
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
    if (which != 0) return nullptr;
    wchar_t path[MAX_PATH]{};
    if (::GetEnvironmentVariableW(L"EXODIUM_VIS_PROBE_TIMING", path, MAX_PATH)) {
        if (FILE* timing = _wfsopen(path, L"r", _SH_DENYNO)) {
            int delayMs = 0, latencyMs = 0;
            if (::fscanf_s(timing, "%d %d", &delayMs, &latencyMs) == 2) {
                g_module.delayMs = std::clamp(delayMs, 0, 1000);
                g_module.latencyMs = std::clamp(latencyMs, 0, 1000);
            }
            std::fclose(timing);
        }
    }
    return &g_module;
}

char g_headerName[] = "Exodium smoke test probe";
winampVisHeader g_header{VIS_HDRVER, g_headerName, GetModule};

} // namespace

extern "C" __declspec(dllexport) winampVisHeader* winampVisGetHeader(HWND) {
    return &g_header;
}
