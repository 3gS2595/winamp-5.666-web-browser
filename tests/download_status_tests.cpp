#include "test_framework.h"

#include "download_status.h"

#include <cstdint>
#include <string>

using namespace exodium;

namespace {

constexpr std::int64_t kKB = 1024;
constexpr std::int64_t kMB = 1024 * kKB;
constexpr std::int64_t kGB = 1024 * kMB;

[[nodiscard]] std::wstring Status(DownloadPhase phase, std::int64_t received, std::int64_t total, double speed = 0) {
    return StatusText({.phase = phase, .received = received, .total = total, .bytesPerSecond = speed});
}

} // namespace

TEST_CASE(BytesBelowAKilobyteAreCountedOut) {
    EXPECT_EQ(FormatBytes(0), L"0 B");
    EXPECT_EQ(FormatBytes(1), L"1 B");
    EXPECT_EQ(FormatBytes(1023), L"1023 B");
    EXPECT_EQ(FormatBytes(-5), L"0 B");
}

TEST_CASE(SizesKeepADecimalBelowAHundred) {
    EXPECT_EQ(FormatBytes(kKB), L"1.0 KB");
    EXPECT_EQ(FormatBytes(1536), L"1.5 KB");
    EXPECT_EQ(FormatBytes(15 * kMB + kMB / 5), L"15.2 MB");
    EXPECT_EQ(FormatBytes(99 * kMB), L"99.0 MB");
    EXPECT_EQ(FormatBytes(152 * kMB), L"152 MB");
    EXPECT_EQ(FormatBytes(3 * kGB / 2), L"1.5 GB");
}

TEST_CASE(ASizeThatRoundsUpToTheNextUnitIsShownInIt) {
    EXPECT_EQ(FormatBytes(kMB - 1), L"1.0 MB");
    EXPECT_EQ(FormatBytes(1023 * kKB + 600), L"1.0 MB");
    EXPECT_EQ(FormatBytes(1023 * kKB), L"1023 KB");
    // 99.95 KB would read "100.0" with its decimal, so it loses it.
    EXPECT_EQ(FormatBytes(102348), L"99.9 KB");
    EXPECT_EQ(FormatBytes(102349), L"100 KB");
}

TEST_CASE(TheLargestUnitIsTerabytes) {
    EXPECT_EQ(FormatBytes(2048 * kGB), L"2.0 TB");
    EXPECT_EQ(FormatBytes(5000 * 1024 * kGB), L"5000 TB");
}

TEST_CASE(ProgressSharesTheUnitWhenItCan) {
    EXPECT_EQ(FormatProgress(kMB + kMB / 5, 5 * kMB), L"1.2/5.0 MB");
    EXPECT_EQ(FormatProgress(512 * kKB, 5 * kMB), L"512 KB/5.0 MB");
    EXPECT_EQ(FormatProgress(0, 5 * kMB), L"0 B/5.0 MB");
    EXPECT_EQ(FormatProgress(5 * kMB, 5 * kMB), L"5.0/5.0 MB");
}

TEST_CASE(ProgressOfADownloadOfUnknownSizeIsWhatHasCome) {
    EXPECT_EQ(FormatProgress(3 * kMB, 0), L"3.0 MB");
    EXPECT_EQ(FormatProgress(3 * kMB, -1), L"3.0 MB");
}

TEST_CASE(SpeedIsASizeASecond) {
    EXPECT_EQ(FormatSpeed(2.3 * kMB), L"2.3 MB/s");
    EXPECT_EQ(FormatSpeed(800), L"800 B/s");
    EXPECT_EQ(FormatSpeed(-3), L"0 B/s");
}

TEST_CASE(TimeLeftIsInTheLargestUnitThatFits) {
    EXPECT_EQ(FormatTimeLeft(1), L"1 sec left");
    EXPECT_EQ(FormatTimeLeft(45), L"45 secs left");
    EXPECT_EQ(FormatTimeLeft(59), L"59 secs left");
    EXPECT_EQ(FormatTimeLeft(60), L"1 min left");
    EXPECT_EQ(FormatTimeLeft(89), L"1 min left");
    EXPECT_EQ(FormatTimeLeft(90), L"2 mins left");
    EXPECT_EQ(FormatTimeLeft(3 * 3600), L"3 hours left");
    EXPECT_EQ(FormatTimeLeft(2 * 86400), L"2 days left");
}

TEST_CASE(TimeLeftRoundsUpIntoTheNextUnit) {
    EXPECT_EQ(FormatTimeLeft(3569), L"59 mins left");
    EXPECT_EQ(FormatTimeLeft(3570), L"1 hour left");
    EXPECT_EQ(FormatTimeLeft(86399), L"1 day left");
}

TEST_CASE(TimeLeftIsNeverLessThanASecond) {
    EXPECT_EQ(FormatTimeLeft(0), L"1 sec left");
    EXPECT_EQ(FormatTimeLeft(-10), L"1 sec left");
}

TEST_CASE(SecondsLeftNeedsASizeAndASpeed) {
    EXPECT_EQ(SecondsLeft(0, 1000, 100).value_or(-1), 10);
    EXPECT_EQ(SecondsLeft(950, 1000, 100).value_or(-1), 1); // half a second is a second
    EXPECT(!SecondsLeft(0, 0, 100));                        // size unknown
    EXPECT(!SecondsLeft(0, 1000, 0));                       // not moving
    EXPECT(!SecondsLeft(1000, 1000, 100));                  // all in
}

TEST_CASE(FileNameIsThePathsLastPart) {
    EXPECT_EQ(FileNameOf(L"C:\\Users\\me\\Downloads\\a report (1).pdf"), L"a report (1).pdf");
    EXPECT_EQ(FileNameOf(L"C:/temp/x.zip"), L"x.zip");
    EXPECT_EQ(FileNameOf(L"plain.txt"), L"plain.txt");
    EXPECT_EQ(FileNameOf(L""), L"");
}

TEST_CASE(ProgramsInstallersAndScriptsCanHarmTheComputer) {
    EXPECT(CanHarmComputer(L"setup.exe"));
    EXPECT(CanHarmComputer(L"Installer.MSI"));
    EXPECT(CanHarmComputer(L"run.bat"));
    EXPECT(CanHarmComputer(L"script.ps1"));
    EXPECT(CanHarmComputer(L"site.url"));
    EXPECT(CanHarmComputer(L"disk.iso"));
    EXPECT(CanHarmComputer(L"report.docm"));
    EXPECT(CanHarmComputer(L"app.appref-ms"));
}

TEST_CASE(DocumentsMediaAndArchivesCantHarmTheComputer) {
    EXPECT(!CanHarmComputer(L"report.pdf"));
    EXPECT(!CanHarmComputer(L"photo.jpeg"));
    EXPECT(!CanHarmComputer(L"song.mp3"));
    EXPECT(!CanHarmComputer(L"archive.zip"));
    EXPECT(!CanHarmComputer(L"letter.docx"));
    EXPECT(!CanHarmComputer(L"notes.txt"));
}

TEST_CASE(OnlyTheLastExtensionCounts) {
    EXPECT(CanHarmComputer(L"invoice.pdf.exe"));
    EXPECT(!CanHarmComputer(L"setup.exe.txt"));
    EXPECT(!CanHarmComputer(L"exe"));  // no extension at all
    EXPECT(!CanHarmComputer(L"x.ex")); // a part of one isn't one
    EXPECT(!CanHarmComputer(L""));
}

TEST_CASE(DotsAndSpacesWindowsDropsDontHideAnExtension) {
    EXPECT(CanHarmComputer(L"setup.exe."));
    EXPECT(CanHarmComputer(L"setup.exe. . "));
    EXPECT(!CanHarmComputer(L"..."));
}

TEST_CASE(SpeedNeedsTwoSamplesAMomentApart) {
    DownloadSpeed speed;
    EXPECT_EQ(speed.BytesPerSecond(0), 0.0);
    speed.Add(1000, 0);
    EXPECT_EQ(speed.BytesPerSecond(1000), 0.0);
    speed.Add(1100, 500);
    EXPECT_EQ(speed.BytesPerSecond(1100), 0.0); // 100 ms is too short to tell
    speed.Add(2000, 1000);
    EXPECT_EQ(speed.BytesPerSecond(2000), 1000.0);
}

TEST_CASE(SpeedFollowsTheLastFewSeconds) {
    DownloadSpeed speed;
    std::int64_t bytes = 0;
    for (std::uint64_t ms = 0; ms <= 10'000; ms += 500, bytes += 500) speed.Add(ms, bytes); // 1000 B/s
    EXPECT_EQ(speed.BytesPerSecond(10'000), 1000.0);
    for (std::uint64_t ms = 10'500; ms <= 20'000; ms += 500, bytes += 2000) speed.Add(ms, bytes); // 4000 B/s
    EXPECT_EQ(speed.BytesPerSecond(20'000), 4000.0);
}

TEST_CASE(SpeedFallsAwayWhenADownloadStalls) {
    DownloadSpeed speed;
    speed.Add(0, 0);
    speed.Add(1000, 1000);
    speed.Add(2000, 2000);
    EXPECT_EQ(speed.BytesPerSecond(2000), 1000.0);
    EXPECT_EQ(speed.BytesPerSecond(4000), 500.0); // nothing more for two seconds
    EXPECT_EQ(speed.BytesPerSecond(5001), 0.0);   // nor for a whole window
}

TEST_CASE(SpeedStartsOverWhenTheDownloadDoes) {
    DownloadSpeed speed;
    speed.Add(0, 0);
    speed.Add(1000, 5000);
    speed.Add(2000, 0); // resumed from nothing
    EXPECT_EQ(speed.BytesPerSecond(2000), 0.0);
    speed.Add(3000, 300);
    EXPECT_EQ(speed.BytesPerSecond(3000), 300.0);
}

TEST_CASE(StatusOfADownloadInProgressSaysAllItCan) {
    EXPECT_EQ(Status(DownloadPhase::InProgress, kMB + kMB / 5, 5 * kMB, 2.3 * kMB),
              L"1.2/5.0 MB \u2022 2.3 MB/s \u2022 2 secs left");
    EXPECT_EQ(Status(DownloadPhase::InProgress, kMB + kMB / 5, 5 * kMB), L"1.2/5.0 MB");
    EXPECT_EQ(Status(DownloadPhase::InProgress, 3 * kMB, 0, kMB), L"3.0 MB \u2022 1.0 MB/s");
    EXPECT_EQ(Status(DownloadPhase::InProgress, 0, 0), L"Starting\u2026");
    EXPECT_EQ(Status(DownloadPhase::InProgress, 0, 5 * kMB), L"0 B/5.0 MB");
}

TEST_CASE(StatusOfAFinishedDownload) {
    EXPECT_EQ(Status(DownloadPhase::Completed, 5 * kMB, 5 * kMB), L"5.0 MB \u2022 Done");
    EXPECT_EQ(Status(DownloadPhase::Completed, 18, 0), L"18 B \u2022 Done"); // its size wasn't told
    EXPECT_EQ(StatusText({.phase = DownloadPhase::Completed, .received = 18, .fileGone = true}), L"Deleted");
    EXPECT_EQ(Status(DownloadPhase::Paused, kMB, 4 * kMB), L"Paused \u2022 1.0/4.0 MB");
    EXPECT_EQ(Status(DownloadPhase::Canceled, kMB, 4 * kMB), L"Canceled");
}

TEST_CASE(StatusOfAFailedDownloadSaysWhy) {
    EXPECT_EQ(StatusText({.phase = DownloadPhase::Failed, .failure = DownloadFailure::Network}),
              L"Failed \u2022 Network error");
    EXPECT_EQ(StatusText({.phase = DownloadPhase::Failed, .failure = DownloadFailure::DiskFull}),
              L"Failed \u2022 Disk full");
    EXPECT_EQ(StatusText({.phase = DownloadPhase::Failed}), L"Failed \u2022 Download error");
}

TEST_CASE(AHeldDownloadSaysWhyItIsWaiting) {
    const DownloadFacts held{.phase = DownloadPhase::InProgress, .received = 9, .bytesPerSecond = 100, .held = true};
    EXPECT_EQ(StatusText(held), L"This type of file can be harmful");
    EXPECT_EQ(PhaseWord(held), L"Held");
}

TEST_CASE(EveryPhaseHasAWord) {
    EXPECT_EQ(PhaseWord({.phase = DownloadPhase::InProgress}), L"Downloading");
    EXPECT_EQ(PhaseWord({.phase = DownloadPhase::Paused}), L"Paused");
    EXPECT_EQ(PhaseWord({.phase = DownloadPhase::Completed}), L"Done");
    EXPECT_EQ(PhaseWord({.phase = DownloadPhase::Completed, .fileGone = true}), L"Deleted");
    EXPECT_EQ(PhaseWord({.phase = DownloadPhase::Canceled}), L"Canceled");
    EXPECT_EQ(PhaseWord({.phase = DownloadPhase::Failed}), L"Failed");
}
