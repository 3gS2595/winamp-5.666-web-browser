// For tests only: a block can carry when it was heard, written over its first few samples,
// so that tests\vis_probe can read it back out of what Winamp gives the visualizers and so
// tell how old the sound in each of their pictures is. in_exodium.dll marks the blocks it
// hands on only when the smoke test asks it to (EXODIUM_SOUND_TEST_MARK); the sound the
// visualizers are given is otherwise never changed.
//
// Winamp gives a visualization plug-in each 16-bit sample's high byte, so the mark is in
// those: on the first channel, a byte to know it by, the time in 4 bytes, and a check byte.
#pragma once

#include <cstdint>
#include <optional>

namespace exodium {

inline constexpr int kSoundMarkFrames = 6;

// When the block was heard, as marked: the ticks of TicksNow over 1000 (units of 100 µs),
// kept to 32 bits. Two such times are compared by their difference as a signed 32-bit number.
[[nodiscard]] constexpr std::uint32_t SoundMarkTime(std::int64_t ticks) noexcept {
    return static_cast<std::uint32_t>(ticks / 1000);
}

namespace sound_mark_detail {
inline constexpr std::uint8_t kFirst = 0x5A;
[[nodiscard]] constexpr std::uint8_t Check(std::uint32_t time) noexcept {
    return static_cast<std::uint8_t>(0xA5 ^ time ^ (time >> 8) ^ (time >> 16) ^ (time >> 24));
}
} // namespace sound_mark_detail

// Marks `samples`, `channels` channels interleaved, with `time` (SoundMarkTime).
inline void MarkSound(std::int16_t* samples, int channels, std::uint32_t time) noexcept {
    const std::uint8_t bytes[kSoundMarkFrames] = {
        sound_mark_detail::kFirst,
        static_cast<std::uint8_t>(time),
        static_cast<std::uint8_t>(time >> 8),
        static_cast<std::uint8_t>(time >> 16),
        static_cast<std::uint8_t>(time >> 24),
        sound_mark_detail::Check(time),
    };
    for (int i = 0; i < kSoundMarkFrames; ++i) {
        samples[i * channels] = static_cast<std::int16_t>(static_cast<std::uint16_t>(bytes[i]) << 8);
    }
}

// The time a channel of 8-bit samples, as Winamp gives a visualization plug-in, was marked
// with, or none if it carries no mark.
[[nodiscard]] inline std::optional<std::uint32_t> ReadSoundMark(const unsigned char* high) noexcept {
    if (high[0] != sound_mark_detail::kFirst) return std::nullopt;
    const std::uint32_t time = std::uint32_t{high[1]} | std::uint32_t{high[2]} << 8 | std::uint32_t{high[3]} << 16 |
                               std::uint32_t{high[4]} << 24;
    if (high[5] != sound_mark_detail::Check(time)) return std::nullopt;
    return time;
}

} // namespace exodium
