// What the downloads panel says about a download, worked out with no window or webview in
// it: sizes, speeds and time left as text, the name the file is saved under, and the line
// under that name telling how the download is going.
#pragma once

#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <string_view>

namespace exodium {

// A size as the panel shows it: "512 B", "1.5 KB", "15.2 MB", "152 MB". Units go up by
// 1024, as Windows counts them, and keep one decimal below 100.
[[nodiscard]] std::wstring FormatBytes(std::int64_t bytes);
// How much of a download has come: "1.2/5.0 MB", "512 KB/5.0 MB" when the two are in
// different units, or "1.2 MB" alone when its size isn't known (a total of 0 or less).
[[nodiscard]] std::wstring FormatProgress(std::int64_t received, std::int64_t total);
// "2.3 MB/s".
[[nodiscard]] std::wstring FormatSpeed(double bytesPerSecond);
// As Chrome puts it: "1 sec left", "45 secs left", "1 min left", "3 hours left", "2 days left".
[[nodiscard]] std::wstring FormatTimeLeft(std::int64_t seconds);
// How long the rest of a download will take at `bytesPerSecond`, in whole seconds. Nothing
// when that can't be told: its size isn't known, it isn't moving, or it is all in.
[[nodiscard]] std::optional<std::int64_t> SecondsLeft(std::int64_t received, std::int64_t total, double bytesPerSecond);

// The name a download is saved under: the last part of its path.
[[nodiscard]] std::wstring FileNameOf(std::wstring_view path);

// Whether a file of this name is of a type that can harm the computer: a program, an
// installer, a script, a shortcut, a disk image, a document with macros. The browser
// holds such a download until the user keeps it. The list errs on the side of too many:
// one more type than the browser's own list is only asked about once more, where one it
// lacks would be blocked outright (see downloads.cpp).
[[nodiscard]] bool CanHarmComputer(std::wstring_view fileName);

// How fast a download is going, measured over the last few seconds, so that it follows the
// download without jumping about with every chunk, and falls away when the download stalls.
class DownloadSpeed {
public:
    static constexpr std::uint64_t kWindowMs = 3000; // what it is measured over
    static constexpr std::uint64_t kMinSpanMs = 250; // anything shorter is too short to tell

    // The download had `bytes` in all at `nowMs`, by any clock that only goes forward.
    void Add(std::uint64_t nowMs, std::int64_t bytes);
    // 0 until it can be told.
    [[nodiscard]] double BytesPerSecond(std::uint64_t nowMs) const;

private:
    struct Sample {
        std::uint64_t ms = 0;
        std::int64_t bytes = 0;
    };
    std::deque<Sample> samples_;
};

// Where a download has got to.
enum class DownloadPhase { InProgress, Paused, Completed, Canceled, Failed };

// Why a download failed, in the terms the user is told it in.
enum class DownloadFailure {
    Unknown,
    Network,
    ServerUnreachable,
    Server,
    NoFile,
    NeedsAuthorization,
    BadCertificate,
    Forbidden,
    AccessDenied,
    DiskFull,
    PathTooLong,
    FileTooLarge,
    Virus,
    Blocked,
    VirusScanFailed,
    Incomplete,
    SystemBusy,
    Shutdown,
    Crashed,
};
// "Network error", "Disk full", ...
[[nodiscard]] std::wstring_view FailureText(DownloadFailure failure) noexcept;

// What there is to tell about a download.
struct DownloadFacts {
    DownloadPhase phase = DownloadPhase::InProgress;
    DownloadFailure failure = DownloadFailure::Unknown; // for a failed one
    std::int64_t received = 0;
    std::int64_t total = 0; // 0 or less when not known
    double bytesPerSecond = 0;
    bool fileGone = false; // finished, and the file has since been deleted or moved
    bool held = false;     // in progress, but of a type that can harm the computer, and waiting for the user to keep it
};
// The line under the file's name: "1.2/5.0 MB • 2.3 MB/s • 3 secs left", "Paused • 1.2/5.0 MB",
// "5.0 MB • Done", "Canceled", "Failed • Network error", "This type of file can be harmful".
[[nodiscard]] std::wstring StatusText(const DownloadFacts& facts);
// The state in a word: "Downloading", "Held", "Paused", "Done", "Deleted", "Canceled", "Failed".
[[nodiscard]] std::wstring_view PhaseWord(const DownloadFacts& facts) noexcept;

} // namespace exodium
