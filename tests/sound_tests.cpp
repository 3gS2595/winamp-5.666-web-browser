#include "test_framework.h"

#include "sound_blocks.h"
#include "sound_rule.h"

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
[[nodiscard]] SoundSituation At(std::uint64_t ms, bool audible, bool ours,
                                WinampPlayback winamp = WinampPlayback::kStopped, bool enabled = true) {
    if (ours && winamp == WinampPlayback::kStopped) winamp = WinampPlayback::kPlaying;
    return {.enabled = enabled, .audible = audible, .ours = ours, .winamp = winamp, .nowMs = ms};
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

} // namespace

TEST_CASE(SoundStartsWhenAPageMakesSoundAndWinampIsStopped) {
    SoundRule rule;
    EXPECT_EQ(Name(rule.Look(At(0, false, false))), L"nothing");
    EXPECT_EQ(Name(rule.Look(At(0, true, false))), L"start");
}

TEST_CASE(SoundLeavesWinampAloneWhileItPlaysOrIsPaused) {
    SoundRule rule;
    EXPECT_EQ(Name(rule.Look(At(0, true, false, WinampPlayback::kPlaying))), L"nothing");
    EXPECT_EQ(Name(rule.Look(At(0, true, false, WinampPlayback::kPaused))), L"nothing");
    // Once Winamp has stopped, the pages it is still hearing are its to show.
    EXPECT_EQ(Name(rule.Look(At(0, true, false, WinampPlayback::kStopped))), L"start");
}

TEST_CASE(SoundNeverStartsWhenTurnedOff) {
    SoundRule rule;
    EXPECT_EQ(Name(rule.Look(At(0, true, false, WinampPlayback::kStopped, false))), L"nothing");
}

TEST_CASE(SoundTurnedOffStopsAtOnce) {
    SoundRule rule;
    EXPECT_EQ(Name(LookAndDo(rule, At(0, true, false))), L"start");
    EXPECT_EQ(Name(rule.Look(At(10, true, true, WinampPlayback::kPlaying, false))), L"stop");
}

TEST_CASE(SoundStopsOnlyOnceThePagesHaveStayedQuiet) {
    SoundRule rule;
    EXPECT_EQ(Name(LookAndDo(rule, At(0, true, false))), L"start");
    EXPECT_EQ(Name(LookAndDo(rule, At(1000, true, true))), L"nothing");
    EXPECT_EQ(Name(LookAndDo(rule, At(2000, false, true))), L"nothing");
    EXPECT_EQ(Name(LookAndDo(rule, At(2000 + SoundRule::kQuietMs - 1, false, true))), L"nothing");
    EXPECT_EQ(Name(LookAndDo(rule, At(2000 + SoundRule::kQuietMs, false, true))), L"stop");
}

TEST_CASE(SoundKeepsPlayingThroughAShortQuietSpell) {
    SoundRule rule;
    EXPECT_EQ(Name(LookAndDo(rule, At(0, true, false))), L"start");
    EXPECT_EQ(Name(LookAndDo(rule, At(1000, false, true))), L"nothing");
    EXPECT_EQ(Name(LookAndDo(rule, At(3000, true, true))), L"nothing"); // the next video
    EXPECT_EQ(Name(LookAndDo(rule, At(4500, false, true))), L"nothing");
    EXPECT_EQ(Name(LookAndDo(rule, At(4500 + SoundRule::kQuietMs - 1, false, true))), L"nothing");
}

TEST_CASE(SoundPausedInWinampStopsOnceThePagesGoQuiet) {
    SoundRule rule;
    EXPECT_EQ(Name(LookAndDo(rule, At(0, true, false))), L"start");
    EXPECT_EQ(Name(LookAndDo(rule, At(500, true, true, WinampPlayback::kPaused))), L"nothing");
    EXPECT_EQ(Name(LookAndDo(rule, At(1000, false, true, WinampPlayback::kPaused))), L"nothing");
    EXPECT_EQ(Name(LookAndDo(rule, At(1000 + SoundRule::kQuietMs, false, true, WinampPlayback::kPaused))),
              L"stop");
}

TEST_CASE(SoundStoppedByTheUserStaysStoppedUntilThePagesGoQuiet) {
    SoundRule rule;
    EXPECT_EQ(Name(LookAndDo(rule, At(0, true, false))), L"start");
    EXPECT_EQ(Name(LookAndDo(rule, At(500, true, true))), L"nothing");
    // Stopped from Winamp while the page still makes sound.
    EXPECT_EQ(Name(LookAndDo(rule, At(1000, true, false))), L"nothing");
    EXPECT(rule.declined());
    EXPECT_EQ(Name(LookAndDo(rule, At(5000, true, false))), L"nothing");
    // The page goes quiet, then makes sound again: that is new sound.
    EXPECT_EQ(Name(LookAndDo(rule, At(6000, false, false))), L"nothing");
    EXPECT(!rule.declined());
    EXPECT_EQ(Name(LookAndDo(rule, At(7000, true, false))), L"start");
}

TEST_CASE(SoundReplacedByTheUsersMusicIsntTakenBackWhenTheMusicEnds) {
    SoundRule rule;
    EXPECT_EQ(Name(LookAndDo(rule, At(0, true, false))), L"start");
    EXPECT_EQ(Name(LookAndDo(rule, At(500, true, false, WinampPlayback::kPlaying))), L"nothing");
    EXPECT_EQ(Name(LookAndDo(rule, At(90000, true, false, WinampPlayback::kStopped))), L"nothing");
}

TEST_CASE(SoundItStoppedItselfIsntTakenForTheUsersDoing) {
    SoundRule rule;
    EXPECT_EQ(Name(LookAndDo(rule, At(0, true, false))), L"start");
    EXPECT_EQ(Name(LookAndDo(rule, At(100, false, true))), L"nothing");
    EXPECT_EQ(Name(LookAndDo(rule, At(100 + SoundRule::kQuietMs, false, true))), L"stop");
    EXPECT_EQ(Name(LookAndDo(rule, At(5000, true, false))), L"start");
}

TEST_CASE(SoundBlocksAreWhole576Frames) {
    SoundBlocks blocks;
    std::vector<Block> got;
    EXPECT_EQ(Collect(blocks, Frames(441), 0, got).size(), 0u);
    EXPECT_EQ(Collect(blocks, Frames(441), FramesToTicks(441), got).size(), 1u);
    EXPECT_EQ(Collect(blocks, Frames(441), FramesToTicks(882), got).size(), 2u);
    EXPECT_EQ(got[0].samples.size(), static_cast<std::size_t>(kVisBlockFrames * kSoundChannels));
    EXPECT_EQ(got[0].samples.front(), 100);
}

TEST_CASE(SoundBlockTimesFollowEachPacketsOwnTime) {
    SoundBlocks blocks;
    std::vector<Block> got;
    (void)Collect(blocks, Frames(441), 0, got);
    // The second packet says it came 500 ticks later than a count of frames would put it.
    (void)Collect(blocks, Frames(441), FramesToTicks(441) + 500, got);
    EXPECT_EQ(got.size(), 1u);
    EXPECT_EQ(got[0].heardAt, std::int64_t{500});
    // The next block starts where that one ended, give or take the rounding of a tick.
    (void)Collect(blocks, Frames(1000), FramesToTicks(882) + 500, got);
    EXPECT_EQ(got.size(), 3u);
    const std::int64_t off = got[1].heardAt - (500 + FramesToTicks(kVisBlockFrames));
    EXPECT(off >= -1 && off <= 1);
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
    EXPECT_EQ(got[0].samples[0], 32767);
    EXPECT_EQ(got[0].samples[1], -32768);
    EXPECT_EQ(got[0].samples[2], 20000);
}
