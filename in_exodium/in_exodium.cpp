// in_exodium.dll: the input plug-in that gives Winamp's visualizers the sound Exodium's
// pages make.
//
// Winamp's visualizers (the analyzer in its main window, and visualization plug-ins such
// as MilkDrop and AVS) only ever see what the input plug-in playing hands them. So while
// a page makes sound and Winamp has nothing else to play, gen_browser.dll has Winamp play
// kSoundUrl, which is this plug-in's: it records the pages (see capture.h) and passes on
// what it hears. It never plays anything itself, since the pages are heard already, and
// so it never touches Winamp's output plug-in.
#define UNICODE_INPUT_PLUGIN
#include "capture.h"
#include "sound_blocks.h"
#include "sound_ipc.h"
#include "sound_mark.h"
#include "vis_latency.h"

#include <windows.h>

#include <in2.h>
#include <wa_ipc.h>

#include <algorithm>
#include <atomic>
#include <cwchar>
#include <iterator>
#include <mutex>
#include <string>

namespace exodium {

namespace {

// The most sound Winamp keeps for the visualizers, ahead of what they show.
constexpr int kVisBufferMs = 500;
// What the main window shows for it: kbps and kHz.
constexpr int kShownBitrate = kSoundRate * kSoundChannels * 16 / 1000;
constexpr int kShownKhz = kSoundRate / 1000;
// How often to look again at which visualization plug-in runs (vis_latency.h): one started
// after the pages' sound is given its sound from that far ahead within this long.
constexpr std::int64_t kVisLookEvery = kTicksPerSecond / 4;

In_Module g_module;
SoundCapture g_capture;
std::atomic<bool> g_playing{false}; // between Play and Stop; GetOutputTime is asked from Winamp's other threads too

// The blocks' times, which the visualizers' data is timed by: since Play, less pauses.
std::atomic<std::int64_t> g_startedAt{0}; // ticks
std::atomic<std::int64_t> g_pausedFor{0}; // ticks
std::atomic<std::int64_t> g_pausedAt{0};  // ticks; 0 when not paused

[[nodiscard]] int StreamMs(std::int64_t at) noexcept {
    return static_cast<int>((at - g_startedAt.load() - g_pausedFor.load()) / (kTicksPerSecond / 1000));
}

// The time Winamp is told it is playing, less the running visualization plug-in's latencyMs:
// just before the first copy of the last block handed on (sound_blocks.h). At each picture
// Winamp hands a visualization plug-in the first block it hasn't yet that starts after the
// time it asks for, so this has it hand on the newest sound there is, the end of a packet
// that came at most 10 ms before, and the same again to a plug-in that draws again before
// the next packet. A time that went on with the clock instead, as a player's does, has to
// stay far enough behind it for there to be a block after it even just before a packet
// comes: so it was until 2026-11-07, 35 ms behind, and the visualizers were shown sound 30
// to 34 ms old, and AVS, which asks for 14 ms ahead, nothing or sound from seconds before in
// half its pictures.
std::atomic<int> g_shownFromMs{0};
std::atomic<int> g_visLatencyMs{0}; // the running visualization plug-in's (vis_latency.h)
VisPlugins g_visPlugins;
// The capture thread's own: when it last looked for the visualization plug-in.
std::int64_t g_visLookedAt = 0;

// The page's title, from gen_browser.dll.
std::mutex g_titleLock;
std::wstring g_title;

// Multiplies the sound by this much before the visualizers see it. Only ever set by the
// smoke test, whose tone is too quiet to hear and so too quiet for them to show.
int g_gain = 1;
// Has each block carry when it was heard (sound_mark.h). Only ever set by the smoke test.
bool g_mark = false;

[[nodiscard]] bool IsOurs(const wchar_t* file) noexcept {
    return file && ::_wcsnicmp(file, kSoundScheme, std::wcslen(kSoundScheme)) == 0;
}

void GiveToVisualizers(const SoundBlock& block) {
    if (g_pausedAt.load()) return;
    const int at = StreamMs(block.heardAt);
    if (at < 0) return; // from before Play
    auto* samples = const_cast<std::int16_t*>(block.samples); // Winamp's functions don't write to it
    std::int16_t marked[kVisBlockFrames * kSoundChannels];
    if (g_mark) {
        std::copy_n(block.samples, std::size(marked), marked);
        MarkSound(marked, kSoundChannels, SoundMarkTime(block.heardAt));
        samples = marked;
    }
    for (int copy = 0; copy < kVisBlockCopies; ++copy) {
        g_module.SAAddPCMData(samples, kSoundChannels, 16, at + copy);
        g_module.VSAAddPCMData(samples, kSoundChannels, 16, at + copy);
    }
    // Only once Winamp has the block is it told to show it.
    g_shownFromMs = at - 1;

    if (block.heardAt - g_visLookedAt >= kVisLookEvery) {
        g_visLookedAt = block.heardAt;
        g_visLatencyMs = g_visPlugins.RunningLatencyMs(g_module.hMainWindow);
    }
}

// Where Winamp loads visualization plug-ins from; asked on Winamp's own thread. A Winamp
// that doesn't answer is taken to load them from where it loads this one.
std::wstring VisFolder() {
    const LRESULT answer = ::SendMessageW(g_module.hMainWindow, WM_WA_IPC, 0, IPC_GETVISDIRECTORYW);
    if (answer > 0xFFFF) {
        if (const auto* folder = reinterpret_cast<const wchar_t*>(answer); *folder) return folder;
    }
    std::wstring self(MAX_PATH, L'\0');
    self.resize(::GetModuleFileNameW(g_module.hDllInstance, self.data(), MAX_PATH));
    return self.substr(0, self.find_last_of(L'\\'));
}

void ShowAbout(HWND parent) {
    ::MessageBoxW(parent,
                  L"Gives Winamp's visualizers the sound Exodium's pages make, while Winamp has nothing else "
                  L"to play.\n\nIt is part of Exodium and has nothing to set; Exodium's page in Preferences "
                  L"turns it on and off.",
                  L"Exodium sound", MB_OK | MB_ICONINFORMATION);
}

void Init() {
    wchar_t gain[16]{};
    if (::GetEnvironmentVariableW(L"EXODIUM_SOUND_TEST_GAIN", gain, 16)) g_gain = std::clamp(_wtoi(gain), 1, 10000);
    g_mark = ::GetEnvironmentVariableW(L"EXODIUM_SOUND_TEST_MARK", nullptr, 0) != 0;
}

void Stop();

void Quit() {
    Stop();
}

void GetFileInfo(const wchar_t* file, wchar_t* title, int* length) {
    // A stream's length, which Winamp shows as none. With -1 its ticker showed "(0:01)".
    if (length) *length = -1000;
    if (!title) return;
    std::wstring text = L"Exodium";
    if (!file || !*file || IsOurs(file)) {
        const std::lock_guard lock(g_titleLock);
        if (!g_title.empty()) text.append(L": ").append(g_title);
    }
    // Well inside the room Winamp gives.
    ::wcsncpy_s(title, 1024, text.c_str(), _TRUNCATE);
}

int InfoBox(const wchar_t*, HWND parent) {
    ShowAbout(parent);
    return 1; // nothing changed
}

int IsOurFile(const wchar_t* file) {
    return IsOurs(file) ? 1 : 0;
}

int Play(const wchar_t*) {
    Stop();
    // A block's worth before now, so that the silence the first packet's blocks begin with
    // (sound_blocks.h) is after it too.
    g_startedAt = TicksNow() - FramesToTicks(kVisBlockFrames);
    g_pausedFor = 0;
    g_pausedAt = 0;
    g_shownFromMs = 0;
    g_visLookedAt = g_startedAt;
    g_visPlugins.Find(VisFolder().c_str());
    g_visLatencyMs = g_visPlugins.RunningLatencyMs(g_module.hMainWindow);
    g_module.SAVSAInit(kVisBufferMs, kSoundRate);
    g_module.VSASetInfo(kSoundRate, kSoundChannels);
    g_module.SetInfo(kShownBitrate, kShownKhz, 1, 1);
    g_capture.Start(GiveToVisualizers, g_gain);
    g_playing = true;
    return 0; // always: a failure would have Winamp go on to the next track in the playlist
}

void Pause() {
    std::int64_t none = 0;
    g_pausedAt.compare_exchange_strong(none, TicksNow());
}

void UnPause() {
    if (const std::int64_t at = g_pausedAt.exchange(0)) g_pausedFor += TicksNow() - at;
}

int IsPaused() {
    return g_pausedAt.load() ? 1 : 0;
}

void Stop() {
    g_capture.Stop();
    if (g_playing.exchange(false)) g_module.SAVSADeInit();
}

int GetLength() {
    return -1000; // a stream's: it goes on as long as the pages make sound
}

// Paused, no blocks are handed on, so it stays where it was.
int GetOutputTime() {
    if (!g_playing) return 0;
    return std::max(0, g_shownFromMs.load() - g_visLatencyMs.load());
}

void SetOutputTime(int) {}
void SetVolume(int) {}
void SetPan(int) {}
void EqSet(int, char[10], int) {}

char g_description[] = "Exodium sound for visualizers";
char g_extensions[] = "\0"; // no file types; IsOurFile takes kSoundUrl

} // namespace

} // namespace exodium

extern "C" __declspec(dllexport) In_Module* winampGetInModule2() {
    using namespace exodium;
    // Winamp fills in its half once it has this; asked again, it must find that still there.
    if (g_module.version) return &g_module;
    g_module.version = IN_VER;
    g_module.description = g_description;
    g_module.FileExtensions = g_extensions;
    g_module.is_seekable = 0;
    g_module.UsesOutputPlug = 0;
    g_module.Config = ShowAbout;
    g_module.About = ShowAbout;
    g_module.Init = Init;
    g_module.Quit = Quit;
    g_module.GetFileInfo = GetFileInfo;
    g_module.InfoBox = InfoBox;
    g_module.IsOurFile = IsOurFile;
    g_module.Play = Play;
    g_module.Pause = Pause;
    g_module.UnPause = UnPause;
    g_module.IsPaused = IsPaused;
    g_module.Stop = Stop;
    g_module.GetLength = GetLength;
    g_module.GetOutputTime = GetOutputTime;
    g_module.SetOutputTime = SetOutputTime;
    g_module.SetVolume = SetVolume;
    g_module.SetPan = SetPan;
    g_module.EQSet = EqSet;
    return &g_module;
}

extern "C" __declspec(dllexport) void __cdecl ExodiumSetSoundTitle(const wchar_t* title) {
    const std::lock_guard lock(exodium::g_titleLock);
    exodium::g_title = title ? title : L"";
}

extern "C" __declspec(dllexport) int __cdecl ExodiumSoundPlaying() {
    return exodium::g_playing ? 1 : 0;
}
