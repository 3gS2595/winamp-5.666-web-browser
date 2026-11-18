#include "test_framework.h"

#include "sound_blocks.h"
#include "sound_mark.h"
#include "sound_rule.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

using namespace exodium;

namespace {

// Steps are compared by name, so that a failure says which.
[[nodiscard]] std::wstring Name(SoundStep step) {
    switch (step) {
    case SoundStep::kNothing: return L"nothing";
    case SoundStep::kStart: return L"start";
    case SoundStep::kStop: return L"stop";
    }
    return L"?";
}

// A situation, the pages making sound or not, Winamp's playing the pages' or not.
[[nodiscard]] SoundSituation At(bool audible, bool ours, WinampPlayback winamp = WinampPlayback::kStopped,
                                bool enabled = true) {
    if (ours && winamp == WinampPlayback::kStopped) winamp = WinampPlayback::kPlaying;
    return {.enabled = enabled, .audible = audible, .ours = ours, .winamp = winamp};
}

// Looks, and takes whatever step comes of it.
SoundStep LookAndDo(SoundRule& rule, const SoundSituation& now) {
    const SoundStep step = rule.Look(now);
    rule.Did(step);
    return step;
}

struct Block {
    std::vector<std::int16_t> samples;
    std::int64_t heardAt;
};
[[nodiscard]] std::vector<Block> Collect(SoundBlocks& blocks, const std::vector<std::int16_t>& samples,
                                         std::int64_t at, std::vector<Block>& into) {
    const auto frames = static_cast<std::uint32_t>(samples.size() / kSoundChannels);
    blocks.Add(samples.data(), frames, at, [&](const SoundBlock& block) {
        into.push_back({{block.samples, block.samples + kVisBlockFrames * kSoundChannels}, block.heardAt});
    });
    return into;
}
[[nodiscard]] std::vector<std::int16_t> Frames(std::size_t count, std::int16_t value = 100) {
    return std::vector<std::int16_t>(count * kSoundChannels, value);
}
// Frames `from` to `from + count`, each holding its own number plus one: 0 is the silence
// before the first.
[[nodiscard]] std::vector<std::int16_t> Numbered(std::size_t from, std::size_t count) {
    std::vector<std::int16_t> samples;
    for (std::size_t frame = from; frame < from + count; ++frame) {
        samples.insert(samples.end(), kSoundChannels, static_cast<std::int16_t>(frame + 1));
    }
    return samples;
}
[[nodiscard]] bool Near(std::int64_t ticks, std::int64_t want) {
    return ticks >= want - 1 && ticks <= want + 1; // the rounding of a tick
}

} // namespace

TEST_CASE(SoundStartsWhenAPageMakesSoundAndWinampIsStopped) {
    SoundRule rule;
    EXPECT_EQ(Name(rule.Look(At(false, false))), L"nothing");
    EXPECT_EQ(Name(rule.Look(At(true, false))), L"start");
}

TEST_CASE(SoundLeavesWinampAloneWhileItPlaysOrIsPaused) {
    SoundRule rule;
    EXPECT_EQ(Name(rule.Look(At(true, false, WinampPlayback::kPlaying))), L"nothing");
    EXPECT_EQ(Name(rule.Look(At(true, false, WinampPlayback::kPaused))), L"nothing");
    // Once Winamp has stopped, the pages it is still hearing are its to show.
    EXPECT_EQ(Name(rule.Look(At(true, false, WinampPlayback::kStopped))), L"start");
}

TEST_CASE(SoundNeverStartsWhenTurnedOff) {
    SoundRule rule;
    EXPECT_EQ(Name(rule.Look(At(true, false, WinampPlayback::kStopped, false))), L"nothing");
}

TEST_CASE(SoundTurnedOffStopsAtOnce) {
    SoundRule rule;
    EXPECT_EQ(Name(LookAndDo(rule, At(true, false))), L"start");
    EXPECT_EQ(Name(rule.Look(At(true, true, WinampPlayback::kPlaying, false))), L"stop");
}

TEST_CASE(SoundStopsAsSoonAsThePagesGoQuiet) {
    SoundRule rule;
    EXPECT_EQ(Name(LookAndDo(rule, At(true, false))), L"start");
    EXPECT_EQ(Name(LookAndDo(rule, At(true, true))), L"nothing");
    EXPECT_EQ(Name(LookAndDo(rule, At(false, true))), L"stop");
}

// A gap between two videos stops it, and the next video starts it again.
TEST_CASE(SoundStartsAgainAfterAGap) {
    SoundRule rule;
    EXPECT_EQ(Name(LookAndDo(rule, At(true, false))), L"start");
    EXPECT_EQ(Name(LookAndDo(rule, At(false, true))), L"stop");
    EXPECT_EQ(Name(LookAndDo(rule, At(false, false))), L"nothing");
    EXPECT_EQ(Name(LookAndDo(rule, At(true, false))), L"start");
}

TEST_CASE(SoundPausedInWinampStopsOnceThePagesGoQuiet) {
    SoundRule rule;
    EXPECT_EQ(Name(LookAndDo(rule, At(true, false))), L"start");
    EXPECT_EQ(Name(LookAndDo(rule, At(true, true, WinampPlayback::kPaused))), L"nothing");
    EXPECT_EQ(Name(LookAndDo(rule, At(false, true, WinampPlayback::kPaused))), L"stop");
}

TEST_CASE(SoundStoppedByTheUserStaysStoppedUntilThePagesGoQuiet) {
    SoundRule rule;
    EXPECT_EQ(Name(LookAndDo(rule, At(true, false))), L"start");
    EXPECT_EQ(Name(LookAndDo(rule, At(true, true))), L"nothing");
    // Stopped from Winamp while the page still makes sound.
    EXPECT_EQ(Name(LookAndDo(rule, At(true, false))), L"nothing");
    EXPECT(rule.declined());
    EXPECT_EQ(Name(LookAndDo(rule, At(true, false))), L"nothing");
    // The page goes quiet, then makes sound again: that is new sound.
    EXPECT_EQ(Name(LookAndDo(rule, At(false, false))), L"nothing");
    EXPECT(!rule.declined());
    EXPECT_EQ(Name(LookAndDo(rule, At(true, false))), L"start");
}

TEST_CASE(SoundReplacedByTheUsersMusicIsntTakenBackWhenTheMusicEnds) {
    SoundRule rule;
    EXPECT_EQ(Name(LookAndDo(rule, At(true, false))), L"start");
    EXPECT_EQ(Name(LookAndDo(rule, At(true, false, WinampPlayback::kPlaying))), L"nothing");
    EXPECT_EQ(Name(LookAndDo(rule, At(true, false, WinampPlayback::kStopped))), L"nothing");
}

TEST_CASE(SoundItStoppedItselfIsntTakenForTheUsersDoing) {
    SoundRule rule;
    EXPECT_EQ(Name(LookAndDo(rule, At(true, false))), L"start");
    EXPECT_EQ(Name(LookAndDo(rule, At(false, true))), L"stop");
    // Even when the next sound comes before a look sees the pages quiet.
    EXPECT_EQ(Name(LookAndDo(rule, At(true, false))), L"start");
}

// Each packet is handed on as the newest block there is: the 576 frames ending with its
// last.
TEST_CASE(SoundBlocksAreTheNewestFrames) {
    static_assert(kVisBlockCopies >= 1 && kVisBlockSpacingFrames >= kVisBlockCopies * kSoundRate / 1000);
    SoundBlocks blocks;
    std::vector<Block> got;
    constexpr std::size_t kPacket = 480; // 10 ms, as Windows gives it
    for (std::size_t from = 0; from < 5 * kPacket; from += kPacket) {
        const std::size_t before = got.size();
        (void)Collect(blocks, Numbered(from, kPacket), FramesToTicks(static_cast<std::int64_t>(from)), got);
        EXPECT_EQ(got.size() - before, 1u);
        const auto end = static_cast<std::int64_t>(from + kPacket);
        EXPECT_EQ(got.back().samples.size(), static_cast<std::size_t>(kVisBlockFrames * kSoundChannels));
        EXPECT_EQ(std::int64_t{got.back().samples.back()}, end);
        EXPECT_EQ(std::int64_t{got.back().samples.front()}, std::max<std::int64_t>(0, end - kVisBlockFrames + 1));
        EXPECT(Near(got.back().heardAt, FramesToTicks(end - kVisBlockFrames)));
    }
}

// A visualizer is shown the pages' sound from the first packet on: what came before it is
// silence, and fills out its block.
TEST_CASE(SoundFirstPacketMakesABlockAfterSilence) {
    SoundBlocks blocks;
    std::vector<Block> got;
    (void)Collect(blocks, Numbered(0, 480), FramesToTicks(1000), got);
    EXPECT_EQ(got.size(), 1u);
    const std::size_t silent = kVisBlockFrames - 480;
    EXPECT_EQ(got[0].samples[(silent - 1) * kSoundChannels], 0);
    EXPECT_EQ(got[0].samples[silent * kSoundChannels], 1);
    EXPECT(Near(got[0].heardAt, FramesToTicks(1000 - static_cast<std::int64_t>(silent))));
    // After a gap, the same again.
    blocks.Clear();
    got.clear();
    (void)Collect(blocks, Numbered(0, 480), FramesToTicks(100'000), got);
    EXPECT_EQ(got.size(), 1u);
    EXPECT_EQ(got[0].samples[(silent - 1) * kSoundChannels], 0);
}

// Winamp is given each block several times, 1 ms apart, and takes the times in whole ms, in
// order; so each block starts at least that many ms after the last, and a short packet's
// that wouldn't is left out.
TEST_CASE(SoundBlockTimesGoForwardByTheirCopies) {
    SoundBlocks blocks;
    std::vector<Block> got;
    (void)Collect(blocks, Numbered(0, 480), 0, got);
    const std::int64_t last = got.back().heardAt;
    constexpr std::size_t kShort = kVisBlockSpacingFrames - 1;
    (void)Collect(blocks, Numbered(480, kShort), FramesToTicks(480), got);
    EXPECT_EQ(got.size(), 1u); // none: too soon after the last
    // Two frames on, past the spacing however a tick rounds.
    (void)Collect(blocks, Numbered(480 + kShort, 2), FramesToTicks(480 + kShort), got);
    EXPECT_EQ(got.size(), 2u);
    EXPECT_EQ(std::int64_t{got.back().samples.back()}, 480 + std::int64_t{kVisBlockSpacingFrames} + 1);
    EXPECT(got.back().heardAt - last >= FramesToTicks(kVisBlockSpacingFrames) - 1);
}

TEST_CASE(SoundBlockTimesFollowEachPacketsOwnTime) {
    SoundBlocks blocks;
    std::vector<Block> got;
    (void)Collect(blocks, Frames(480), 0, got);
    // The second packet says it came 500 ticks later than a count of frames would put it.
    (void)Collect(blocks, Frames(480), FramesToTicks(480) + 500, got);
    EXPECT_EQ(got.size(), 2u);
    EXPECT(Near(got.back().heardAt, FramesToTicks(960 - kVisBlockFrames) + 500));
}

TEST_CASE(SoundSilentPacketsAreZeros) {
    SoundBlocks blocks;
    std::vector<Block> got;
    blocks.Add(nullptr, kVisBlockFrames, 0, [&](const SoundBlock& block) {
        got.push_back({{block.samples, block.samples + kVisBlockFrames * kSoundChannels}, block.heardAt});
    });
    EXPECT_EQ(got.size(), 1u);
    bool allZero = true;
    for (const std::int16_t sample : got[0].samples) allZero = allZero && sample == 0;
    EXPECT(allZero);
}

TEST_CASE(SoundGainIsHeldToSixteenBits) {
    SoundBlocks blocks(1000);
    std::vector<Block> got;
    std::vector<std::int16_t> samples(kVisBlockFrames * kSoundChannels, 20);
    samples[0] = 20000;
    samples[1] = -20000;
    (void)Collect(blocks, samples, 0, got);
    // The block is the packet's 576 frames exactly.
    EXPECT_EQ(got[0].samples[0], 32767);
    EXPECT_EQ(got[0].samples[1], -32768);
    EXPECT_EQ(got[0].samples[2], 20000);
}

// The mark the smoke test has blocks carry is read back out of the high bytes Winamp gives
// a visualization plug-in.
TEST_CASE(SoundMarkReadsBackFromHighBytes) {
    std::vector<std::int16_t> samples(kVisBlockFrames * kSoundChannels, 1234);
    const std::uint32_t time = SoundMarkTime(std::int64_t{123'456'789'012});
    MarkSound(samples.data(), kSoundChannels, time);
    unsigned char high[kVisBlockFrames];
    for (int i = 0; i < kVisBlockFrames; ++i) high[i] = static_cast<unsigned char>(samples[i * kSoundChannels] >> 8);
    EXPECT(ReadSoundMark(high) == std::optional<std::uint32_t>{time});
    EXPECT_EQ(samples[1], 1234); // the other channel is left alone
    high[3] ^= 1;
    EXPECT(!ReadSoundMark(high));
    unsigned char unmarked[kVisBlockFrames]{};
    EXPECT(!ReadSoundMark(unmarked));
}
