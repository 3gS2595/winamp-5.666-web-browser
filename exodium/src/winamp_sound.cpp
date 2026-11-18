// The pages' sound in Winamp's visualizers. While a page makes sound and Winamp would
// otherwise be stopped, Winamp plays kSoundUrl, which in_exodium.dll plays by recording
// the pages and handing what it hears to the visualizers. sound_rule.h says when.
//
// Winamp only plays what is in its playlist, so kSoundUrl goes in at the end just long
// enough to be started, and comes out again at once, with the playlist's position put
// back. Winamp plays on regardless, and the playlist is left as it was: Play in Winamp
// still plays the track the user had, which is also how they take Winamp back.
#include "browser.h"
#include "sound_ipc.h"
#include "sound_rule.h"

namespace exodium {

namespace {

// The playlist editor's own WM_WA_IPC message (the SDK's ipc_pe.h) that deletes the
// entry lParam says. Unlike the main window's, its wParam says which message it is.
constexpr WPARAM kIpcPeDeleteIndex = 104;

// On the first window's client, which lasts until Winamp exits: every so often for as long
// as Winamp plays the pages' sound, to see them go quiet whichever way they do, closed tabs
// and windows too. Anything that may change what is to be done has a look taken at once
// instead (FollowPageSound).
constexpr UINT_PTR kSoundTimer = 2;
constexpr UINT kSoundLookMs = 1000;

SoundRule g_rule;
std::wstring g_givenTitle;                                   // the title in_exodium.dll has from us
const std::wstring kPrivateSoundTitle = L"A private window"; // in place of a private page's own

LRESULT Ipc(WPARAM param, LPARAM id) {
    return ::SendMessageW(g_app.winamp, WM_WA_IPC, param, id);
}

void Command(WORD id) {
    ::SendMessageW(g_app.winamp, WM_COMMAND, MAKEWPARAM(id, 0), 0);
}

// One of in_exodium.dll's exports, or null when it isn't loaded.
template <typename Function>
[[nodiscard]] Function SoundExport(const char* name) noexcept {
    const HMODULE plugin = ::GetModuleHandleW(kSoundPluginFile);
    return plugin ? reinterpret_cast<Function>(::GetProcAddress(plugin, name)) : nullptr;
}

[[nodiscard]] bool OursPlaying() noexcept {
    const auto playing = SoundExport<SoundPlayingFunction>(kSoundPlayingExport);
    return playing && playing() != 0;
}

[[nodiscard]] WinampPlayback Playback() {
    switch (Ipc(0, IPC_ISPLAYING)) {
    case 1:
        return WinampPlayback::kPlaying;
    case 3:
        return WinampPlayback::kPaused;
    default:
        return WinampPlayback::kStopped;
    }
}

// The tab whose title Winamp shows: one making sound, a window's tab in view if any is.
[[nodiscard]] const Tab* SoundingTab() noexcept {
    const Tab* found = nullptr;
    for (const auto& win : g_app.windows) {
        if (win->closing) continue;
        for (int i = 0; i < static_cast<int>(win->tabs.size()); ++i) {
            const Tab& tab = win->tabs[i];
            // A muted page is still said to be making sound, of which there is none to show.
            if (!tab.playingAudio || tab.muted) continue;
            if (i == win->active) return &tab;
            if (!found) found = &tab;
        }
    }
    return found;
}

// Has in_exodium.dll show the tab's title, and Winamp ask again for it if it is playing.
// A private window's page isn't named to Winamp, or to whatever follows what Winamp plays.
void ShowTitle(const Tab& tab, bool playing) {
    const std::wstring& title = tab.isPrivate ? kPrivateSoundTitle : tab.title;
    if (title == g_givenTitle) return;
    const auto set = SoundExport<SetSoundTitleFunction>(kSetSoundTitleExport);
    if (!set) return;
    g_givenTitle = title;
    set(g_givenTitle.c_str());
    if (playing) Ipc(0, IPC_UPDTITLE);
}

void Start(const Tab& tab) {
    ShowTitle(tab, false);
    const LRESULT length = Ipc(0, IPC_GETLISTLENGTH);
    const LRESULT position = Ipc(0, IPC_GETLISTPOS);
    COPYDATASTRUCT copied{
        .dwData = IPC_PLAYFILEW,
        .cbData = sizeof(kSoundUrl),
        .lpData = const_cast<wchar_t*>(kSoundUrl),
    };
    ::SendMessageW(g_app.winamp, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&copied));
    if (Ipc(0, IPC_GETLISTLENGTH) != length + 1) return; // it didn't go in
    Ipc(static_cast<WPARAM>(length), IPC_SETPLAYLISTPOS);
    Command(WINAMP_BUTTON2); // play the entry at the position
    if (const auto playlist = reinterpret_cast<HWND>(Ipc(IPC_GETWND_PE, IPC_GETWND))) {
        ::SendMessageW(playlist, WM_WA_IPC, kIpcPeDeleteIndex, length);
    }
    Ipc(static_cast<WPARAM>(position), IPC_SETPLAYLISTPOS);
}

void Stop() {
    Command(WINAMP_BUTTON4);
    // Winamp goes on showing the pages' title until it is asked again, which it then
    // asks of the playlist's own entry.
    Ipc(0, IPC_UPDTITLE);
    g_givenTitle.clear();
}

void CALLBACK OnSoundTimer(HWND, UINT, UINT_PTR, DWORD);

void KeepLooking(UINT everyMs) {
    const HWND host = g_app.windows.empty() ? nullptr : g_app.windows.front()->client;
    if (!host) return;
    if (everyMs) ::SetTimer(host, kSoundTimer, everyMs, OnSoundTimer);
    else ::KillTimer(host, kSoundTimer);
}

void Look() {
    if (g_app.shutDown || !g_app.winamp) return;
    const Tab* sounding = SoundingTab();
    const bool ours = OursPlaying();
    const SoundStep step = g_rule.Look({
        .enabled = g_app.settings.soundInVisualizers && ::GetModuleHandleW(kSoundPluginFile),
        .audible = sounding != nullptr,
        .ours = ours,
        .winamp = Playback(),
    });
    switch (step) {
    case SoundStep::kStart:
        Start(*sounding);
        break;
    case SoundStep::kStop:
        Stop();
        break;
    case SoundStep::kNothing:
        if (ours && sounding) ShowTitle(*sounding, true);
        break;
    }
    g_rule.Did(step);
    KeepLooking(OursPlaying() ? kSoundLookMs : 0);
}

void CALLBACK OnSoundTimer(HWND, UINT, UINT_PTR, DWORD) {
    Look();
}

} // namespace

void FollowPageSound() {
    // Not from in here: this is called from inside webview events and Winamp's own messages.
    // A message posted comes as soon as they are done with. Until 2026-11-07 a timer did it,
    // which took 10 ms at the least and often a tick of Windows' clock (15.6 ms) more, every
    // time a page started making sound.
    if (g_app.windows.empty() || !g_app.windows.front()->client) return;
    ::PostMessageW(g_app.windows.front()->client, kMsgFollowSound, 0, 0);
}

void LookAtPageSound() {
    Look();
}

} // namespace exodium
