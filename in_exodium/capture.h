// Records the sound Exodium's pages make. They play in WebView2's processes: a browser
// process that Winamp starts, and the processes that one starts in turn, the audio
// service among them. This records that browser process and everything it started,
// which is the pages' sound and nothing else: not Winamp's own playing, not other
// programs, not the rest of the computer.
//
// It is Windows' process loopback recording. Microsoft documents it for Windows 10 build
// 20348 and later; it was found to work on 19045 as well (2026-11-03). Aimed at Winamp's
// own process it hears nothing of the pages: Windows doesn't count the WebView2 processes
// as part of Winamp's tree for this, though Winamp is their parent.
#pragma once

#include "sound_blocks.h"

#include <windows.h>

#include <functional>

namespace exodium {

// Now, in the ticks a captured packet's time is given in.
[[nodiscard]] std::int64_t TicksNow() noexcept;

class SoundCapture {
public:
    // Called on the capture's own thread for each block recorded.
    using Give = std::function<void(const SoundBlock&)>;

    SoundCapture() = default;
    SoundCapture(const SoundCapture&) = delete;
    SoundCapture& operator=(const SoundCapture&) = delete;
    ~SoundCapture() { Stop(); }

    // Starts recording, on a thread of its own. It keeps at it for as long as it runs:
    // until there is a WebView2 browser process, and again after one goes and another
    // comes, as when the browser's process dies and Exodium starts it again. Nothing comes
    // of it while Windows can't record, which shows as silence, never as a failure: the
    // plug-in plays on regardless.
    void Start(Give give, int gain);
    // Stops, and returns once the thread is done: `give` is no longer called.
    void Stop() noexcept;

private:
    static DWORD WINAPI ThreadMain(void* self);
    void Run();

    HANDLE thread_ = nullptr;
    HANDLE stop_ = nullptr;
    Give give_;
    int gain_ = 1;
};

} // namespace exodium
