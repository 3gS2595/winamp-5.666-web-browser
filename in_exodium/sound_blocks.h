// Cuts captured sound into the blocks Winamp's visualizers are given, and says when each
// was heard. Nothing here touches Windows' audio or Winamp, so the tests can run it.
#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>
#include <vector>

namespace exodium {

// The sound in_exodium.dll asks Windows for and hands to Winamp. 48 kHz is what Windows
// mixes the speakers' sound at (on the computer this was written for, and most others), so
// the recording comes as it is, without a resampler's delay; and 576 frames of it are 12 ms,
// against 13 at 44.1 kHz.
inline constexpr int kSoundRate = 48000;
inline constexpr int kSoundChannels = 2;
// Winamp's visualizers take 576 frames at a time.
inline constexpr int kVisBlockFrames = 576;
// How many times in_exodium.dll hands Winamp each block, under times 1 ms apart (Winamp is
// given them in whole ms). Winamp hands a visualization plug-in each block only once: at
// each picture, the first block it hasn't handed on yet that starts after the time it asks
// for. Asked for the time just before the first copy (in_exodium.cpp), it hands the next
// picture the newest sound there is, and a plug-in that draws again before the next packet
// comes, some 10 ms later, the same again, which is all there is; so three pictures in 10 ms
// find sound to show. A single copy would leave a second picture with none, which Winamp
// fills with silence or, once its store of blocks has gone round, sound from long before.
// Until 2026-11-07 the copies were blocks from 1 and 2 ms before the newest, and the first
// picture after a packet was shown sound 2 ms older than it could have been.
inline constexpr int kVisBlockCopies = 3;
// The least a block starts after the one before it, so that its copies' times all come
// after the last copy's of that one.
inline constexpr int kVisBlockSpacingFrames = kVisBlockCopies * kSoundRate / 1000;

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
    // a packet of silence. Calls give(const SoundBlock&) for the newest block there is, the
    // 576 frames ending with the packet's last, unless it would start less than
    // kVisBlockSpacingFrames after the last one handed on, as after a very short packet, so
    // the times only ever go forward by at least that much. Each packet's own time is
    // believed over a count of the frames before it, so a sound card's clock that runs a
    // little fast or slow never pulls the times off.
    // What came before the first packet, or the first after a gap, counts as silence, so
    // that one packet, of 480 frames when Windows mixes every 10 ms, is enough for blocks:
    // the visualizers show the pages' sound a packet sooner after it starts.
    template <typename Give>
    void Add(const std::int16_t* samples, std::uint32_t frames, std::int64_t at, Give&& give) {
        if (pending_.empty()) pending_.assign(static_cast<std::size_t>(kVisBlockFrames) * kSoundChannels, 0);
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

        const std::int64_t have = static_cast<std::int64_t>(pending_.size() / kSoundChannels);
        const std::int64_t first = have - kVisBlockFrames;
        const std::int64_t heardAt = start_ + FramesToTicks(first);
        if (!given_ || heardAt >= lastGiven_ + FramesToTicks(kVisBlockSpacingFrames)) {
            give(SoundBlock{pending_.data() + first * kSoundChannels, heardAt});
            lastGiven_ = heardAt;
            given_ = true;
        }

        // Only the frames a later packet's block can start in are kept.
        const std::int64_t drop = have - kVisBlockFrames;
        if (drop <= 0) return;
        pending_.erase(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(drop * kSoundChannels));
        start_ += FramesToTicks(drop);
    }

    // Forgets the frames kept for blocks to come, as after a gap in the sound. The blocks
    // after it still start after the last one before it.
    void Clear() noexcept { pending_.clear(); }

private:
    [[nodiscard]] std::int16_t Amplified(std::int16_t sample) const noexcept {
        constexpr int kLow = std::numeric_limits<std::int16_t>::min();
        constexpr int kHigh = std::numeric_limits<std::int16_t>::max();
        return static_cast<std::int16_t>(std::clamp(sample * gain_, kLow, kHigh));
    }

    int gain_;
    std::vector<std::int16_t> pending_; // the newest frames, from where a later block may start
    std::int64_t start_ = 0;            // when the first of them was heard
    std::int64_t lastGiven_ = 0;        // when the last block handed on starts
    bool given_ = false;
};

} // namespace exodium
