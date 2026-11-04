// Cuts captured sound into the blocks Winamp's visualizers are given, and says when each
// was heard. Nothing here touches Windows' audio or Winamp, so the tests can run it.
#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>
#include <vector>

namespace exodium {

// The sound in_exodium.dll asks Windows for and hands to Winamp.
inline constexpr int kSoundRate = 44100;
inline constexpr int kSoundChannels = 2;
// Winamp's visualizers take 576 frames at a time.
inline constexpr int kVisBlockFrames = 576;

// Times are in the units Windows gives a captured packet's time in: 100 ns.
inline constexpr std::int64_t kTicksPerSecond = 10'000'000;
[[nodiscard]] constexpr std::int64_t FramesToTicks(std::int64_t frames) noexcept {
    return frames * kTicksPerSecond / kSoundRate;
}

// A block, ready for SAAddPCMData and VSAAddPCMData.
struct SoundBlock {
    const std::int16_t* samples; // kVisBlockFrames frames, the channels interleaved
    std::int64_t heardAt;        // when its first frame was, in ticks
};

class SoundBlocks {
public:
    // `gain` multiplies every sample, held to what 16 bits can hold. It is 1 but for tests.
    explicit SoundBlocks(int gain = 1) noexcept : gain_(gain) {}

    // Takes a packet of `frames` frames, the first of them at `at`; `samples` is null for
    // a packet of silence. Calls give(const SoundBlock&) for each block it completes, in
    // order. Each packet's own time is believed over a count of the frames before it, so
    // a sound card's clock that runs a little fast or slow never pulls the times off.
    template <typename Give>
    void Add(const std::int16_t* samples, std::uint32_t frames, std::int64_t at, Give&& give) {
        const std::int64_t waiting = static_cast<std::int64_t>(pending_.size() / kSoundChannels);
        start_ = at - FramesToTicks(waiting);
        const std::size_t count = static_cast<std::size_t>(frames) * kSoundChannels;
        if (!samples) {
            pending_.insert(pending_.end(), count, 0);
        } else if (gain_ == 1) {
            pending_.insert(pending_.end(), samples, samples + count);
        } else {
            for (std::size_t i = 0; i < count; ++i) pending_.push_back(Amplified(samples[i]));
        }

        std::size_t done = 0; // frames handed on
        while (pending_.size() / kSoundChannels - done >= kVisBlockFrames) {
            give(SoundBlock{pending_.data() + done * kSoundChannels,
                            start_ + FramesToTicks(static_cast<std::int64_t>(done))});
            done += kVisBlockFrames;
        }
        if (done == 0) return;
        pending_.erase(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(done * kSoundChannels));
        start_ += FramesToTicks(static_cast<std::int64_t>(done));
    }

    // Forgets the frames that have yet to make a block, as after a gap in the sound.
    void Clear() noexcept { pending_.clear(); }

private:
    [[nodiscard]] std::int16_t Amplified(std::int16_t sample) const noexcept {
        constexpr int kLow = std::numeric_limits<std::int16_t>::min();
        constexpr int kHigh = std::numeric_limits<std::int16_t>::max();
        return static_cast<std::int16_t>(std::clamp(sample * gain_, kLow, kHigh));
    }

    int gain_;
    std::vector<std::int16_t> pending_; // frames not yet making up a block
    std::int64_t start_ = 0;            // when the first of them was heard
};

} // namespace exodium
