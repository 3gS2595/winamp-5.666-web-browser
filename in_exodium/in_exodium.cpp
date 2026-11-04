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

#include <windows.h>

#include <in2.h>

#include <algorithm>
#include <atomic>
#include <cwchar>
#include <mutex>
#include <string>

namespace exodium {

namespace {

// How far behind the moment the time Winamp is told is, and so the sound the visualizers
// are shown. Winamp only finds the visualization plug-ins' data reliably when it has been
// given some ahead of that time, as an input plug-in that plays ahead into an output
// plug-in's buffer always has; the newest sound here is 25 to 40 ms old when it arrives
// in blocks. At 30 MilkDrop and the like got nothing for a few moments at a time in the
// first seconds; at 60 they got every block from the start (measured 2026-11-03). The
// speakers play sound some 10 to 30 ms after it is recorded, so the pictures trail what
// is heard by less than that.
constexpr int kVisLagMs = 60;
// The most sound Winamp keeps for the visualizers, ahead of what they show.
constexpr int kVisBufferMs = 500;
// What the main window shows for it: kbps and kHz.
constexpr int kShownBitrate = kSoundRate * kSoundChannels * 16 / 1000;
constexpr int kShownKhz = kSoundRate / 1000;

In_Module g_module;
SoundCapture g_capture;
std::atomic<bool> g_playing{false}; // between Play and Stop; GetOutputTime is asked from Winamp's other threads too

// The time Winamp shows, which the visualizers' data is timed by: since Play, less pauses.
std::atomic<std::int64_t> g_startedAt{0}; // ticks
std::atomic<std::int64_t> g_pausedFor{0}; // ticks
std::atomic<std::int64_t> g_pausedAt{0};  // ticks; 0 when not paused

[[nodiscard]] int StreamMs(std::int64_t at) noexcept {
    return static_cast<int>((at - g_startedAt.load() - g_pausedFor.load()) / (kTicksPerSecond / 1000));
}

// The page's title, from gen_browser.dll.
std::mutex g_titleLock;
std::wstring g_title;

// Multiplies the sound by this much before the visualizers see it. Only ever set by the
// smoke test, whose tone is too quiet to hear and so too quiet for them to show.
int g_gain = 1;

[[nodiscard]] bool IsOurs(const wchar_t* file) noexcept {
    return file && ::_wcsnicmp(file, kSoundScheme, std::wcslen(kSoundScheme)) == 0;
}

void GiveToVisualizers(const SoundBlock& block) {
    if (g_pausedAt.load()) return;
    const int at = StreamMs(block.heardAt);
    if (at < 0) return; // from before Play
    auto* samples = const_cast<std::int16_t*>(block.samples); // Winamp's functions don't write to it
    g_module.SAAddPCMData(samples, kSoundChannels, 16, at);
    g_module.VSAAddPCMData(samples, kSoundChannels, 16, at);
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
    g_startedAt = TicksNow();
    g_pausedFor = 0;
    g_pausedAt = 0;
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

int GetOutputTime() {
    if (!g_playing) return 0;
    const std::int64_t paused = g_pausedAt.load();
    return std::max(0, StreamMs(paused ? paused : TicksNow()) - kVisLagMs);
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
