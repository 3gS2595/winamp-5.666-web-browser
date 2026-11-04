// When Winamp is to play the pages' sound for its visualizers, and when it is to stop
// (winamp_sound.cpp does it). The rule on its own, apart from Winamp and the pages, so
// the tests can run it.
//
// Winamp plays the pages' sound while a page makes sound and Winamp would otherwise be
// stopped. It stops once the pages have been quiet for a while, so a pause between two
// videos doesn't stop it. Whatever the user does in Winamp goes: if they stop it, or
// play something else, it isn't started again until the pages have gone quiet and then
// make sound again.
#pragma once

#include <cstdint>
#include <optional>

namespace exodium {

enum class WinampPlayback { kStopped, kPlaying, kPaused };

// What there is to go on at a look.
struct SoundSituation {
    bool enabled = false; // wanted (Settings::soundInVisualizers), and in_exodium.dll is there to play it
    bool audible = false; // a page in an open window makes sound
    bool ours = false;    // what Winamp plays, or is paused on, is the pages' sound
    WinampPlayback winamp = WinampPlayback::kStopped;
    std::uint64_t nowMs = 0; // a clock that only goes forward, such as GetTickCount64
};

enum class SoundStep { kNothing, kStart, kStop };

class SoundRule {
public:
    // How long the pages must stay quiet before Winamp stops.
    static constexpr std::uint64_t kQuietMs = 3000;

    // What to do now. Winamp's playing having been the pages' sound at the last look and
    // not being now is taken for the user's doing, since the rule only ever starts or stops
    // it by the steps it gives, and is told of them by Did.
    [[nodiscard]] SoundStep Look(const SoundSituation& now);
    // The step the last look gave has been taken.
    void Did(SoundStep step) noexcept;

    // The user turned Winamp away from the pages' sound, and the pages have yet to go quiet.
    [[nodiscard]] bool declined() const noexcept { return declined_; }

private:
    bool wasOurs_ = false;
    bool declined_ = false;
    std::optional<std::uint64_t> quietSince_; // while Winamp plays the pages' sound and they are quiet
};

} // namespace exodium
