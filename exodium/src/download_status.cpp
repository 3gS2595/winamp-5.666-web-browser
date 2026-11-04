#include "download_status.h"

#include "text_util.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace exodium {

namespace {

constexpr std::wstring_view kSeparator = L" • "; // a bullet with a space either side
constexpr double kUnitStep = 1024.0;
constexpr std::array<std::wstring_view, 5> kUnits{L"B", L"KB", L"MB", L"GB", L"TB"};
// Below this a size keeps a decimal: "15.2 MB", but "152 MB".
constexpr double kDecimalsBelow = 99.95;

// After the types the browser engine warns about on Windows, and then some: programs and
// installers, scripts of every kind Windows has a host for, shortcuts, help and control
// panel files, certificates, Office's databases and documents with macros, and disk images,
// which open as a drive and can carry any of the others.
constexpr std::wstring_view kHarmfulExtensions[] = {
    L"ad",
    L"ade",
    L"adp",
    L"app",
    L"application",
    L"appref-ms",
    L"appx",
    L"appxbundle",
    L"asp",
    L"asx",
    L"bas",
    L"bat",
    L"cer",
    L"chi",
    L"chm",
    L"cmd",
    L"com",
    L"cpl",
    L"crt",
    L"crx",
    L"csh",
    L"der",
    L"diagcab",
    L"dll",
    L"docm",
    L"dotm",
    L"drv",
    L"exe",
    L"fxp",
    L"gadget",
    L"grp",
    L"hlp",
    L"hpj",
    L"hta",
    L"htt",
    L"img",
    L"inf",
    L"ins",
    L"iso",
    L"isp",
    L"jar",
    L"jnlp",
    L"js",
    L"jse",
    L"ksh",
    L"library-ms",
    L"lnk",
    L"local",
    L"mad",
    L"maf",
    L"mag",
    L"mam",
    L"manifest",
    L"maq",
    L"mar",
    L"mas",
    L"mat",
    L"mau",
    L"mav",
    L"maw",
    L"mcf",
    L"mda",
    L"mdb",
    L"mde",
    L"mdt",
    L"mdw",
    L"mdz",
    L"mht",
    L"mhtml",
    L"mmc",
    L"mof",
    L"msc",
    L"msh",
    L"msh1",
    L"msh1xml",
    L"msh2",
    L"msh2xml",
    L"mshxml",
    L"msi",
    L"msix",
    L"msixbundle",
    L"msp",
    L"mst",
    L"ocx",
    L"ops",
    L"pcd",
    L"pif",
    L"pl",
    L"plg",
    L"potm",
    L"ppam",
    L"ppsm",
    L"pptm",
    L"prf",
    L"prg",
    L"ps1",
    L"ps1xml",
    L"ps2",
    L"ps2xml",
    L"psc1",
    L"psc2",
    L"psd1",
    L"psm1",
    L"pst",
    L"py",
    L"pyc",
    L"pyw",
    L"reg",
    L"scf",
    L"scr",
    L"sct",
    L"search-ms",
    L"settingcontent-ms",
    L"shb",
    L"shs",
    L"sys",
    L"url",
    L"vb",
    L"vbe",
    L"vbp",
    L"vbs",
    L"vhd",
    L"vhdx",
    L"vsmacros",
    L"vsw",
    L"ws",
    L"wsc",
    L"wsf",
    L"wsh",
    L"xbap",
    L"xlam",
    L"xlsm",
    L"xltm",
    L"xnk",
};

// A size as its number and its unit, apart, so that two sizes in the same unit can share it.
struct SizeParts {
    std::wstring number;
    std::wstring_view unit;
};

[[nodiscard]] SizeParts SplitBytes(std::int64_t bytes) {
    bytes = std::max<std::int64_t>(bytes, 0);
    if (bytes < static_cast<std::int64_t>(kUnitStep)) return {std::to_wstring(bytes), kUnits[0]};
    double value = static_cast<double>(bytes);
    std::size_t unit = 0;
    while (value >= kUnitStep && unit + 1 < kUnits.size()) {
        value /= kUnitStep;
        ++unit;
    }
    // A number that rounds up to a whole step is shown in the next unit instead: not
    // "1024 KB" but "1.0 MB".
    for (;;) {
        if (value < kDecimalsBelow) {
            const std::int64_t tenths = std::llround(value * 10);
            return {std::to_wstring(tenths / 10) + L'.' + std::to_wstring(tenths % 10), kUnits[unit]};
        }
        const std::int64_t whole = std::llround(value);
        if (whole < static_cast<std::int64_t>(kUnitStep) || unit + 1 == kUnits.size()) {
            return {std::to_wstring(whole), kUnits[unit]};
        }
        value /= kUnitStep;
        ++unit;
    }
}

[[nodiscard]] std::wstring Joined(const SizeParts& size) {
    return size.number + L' ' + std::wstring(size.unit);
}

[[nodiscard]] std::wstring Counted(std::int64_t count, std::wstring_view one, std::wstring_view many) {
    return std::to_wstring(count) + L' ' + std::wstring(count == 1 ? one : many) + L" left";
}

} // namespace

std::wstring FormatBytes(std::int64_t bytes) {
    return Joined(SplitBytes(bytes));
}

std::wstring FormatProgress(std::int64_t received, std::int64_t total) {
    if (total <= 0) return FormatBytes(received);
    const SizeParts got = SplitBytes(received), all = SplitBytes(total);
    if (got.unit == all.unit) return got.number + L'/' + Joined(all);
    return Joined(got) + L'/' + Joined(all);
}

std::wstring FormatSpeed(double bytesPerSecond) {
    return FormatBytes(std::llround(std::max(bytesPerSecond, 0.0))) + L"/s";
}

std::wstring FormatTimeLeft(std::int64_t seconds) {
    // Rounded to the nearest of whatever it is counted in.
    const std::int64_t s = std::max<std::int64_t>(seconds, 1);
    if (s < 60) return Counted(s, L"sec", L"secs");
    if (const std::int64_t minutes = (s + 30) / 60; minutes < 60) return Counted(minutes, L"min", L"mins");
    if (const std::int64_t hours = (s + 1800) / 3600; hours < 24) return Counted(hours, L"hour", L"hours");
    return Counted((s + 43200) / 86400, L"day", L"days");
}

std::optional<std::int64_t> SecondsLeft(std::int64_t received, std::int64_t total, double bytesPerSecond) {
    if (total <= 0 || received >= total || bytesPerSecond <= 0) return std::nullopt;
    return static_cast<std::int64_t>(std::ceil(static_cast<double>(total - received) / bytesPerSecond));
}

std::wstring FileNameOf(std::wstring_view path) {
    const std::size_t slash = path.find_last_of(L"\\/");
    return std::wstring(slash == std::wstring_view::npos ? path : path.substr(slash + 1));
}

bool CanHarmComputer(std::wstring_view fileName) {
    // Windows drops dots and spaces from the end of a file's name: "setup.exe." is setup.exe.
    const std::size_t end = fileName.find_last_not_of(L". ");
    if (end == std::wstring_view::npos) return false;
    fileName = fileName.substr(0, end + 1);
    const std::size_t dot = fileName.rfind(L'.');
    if (dot == std::wstring_view::npos) return false;
    const std::wstring extension = ToLower(fileName.substr(dot + 1));
    return std::ranges::find(kHarmfulExtensions, extension) != std::ranges::end(kHarmfulExtensions);
}

void DownloadSpeed::Add(std::uint64_t nowMs, std::int64_t bytes) {
    // Fewer bytes than before: it started over, and is measured afresh.
    if (!samples_.empty() && bytes < samples_.back().bytes) samples_.clear();
    samples_.push_back({nowMs, bytes});
    // One sample from before the window is kept, to measure the whole window from.
    while (samples_.size() > 2 && samples_[1].ms + kWindowMs <= nowMs) samples_.pop_front();
}

double DownloadSpeed::BytesPerSecond(std::uint64_t nowMs) const {
    if (samples_.size() < 2) return 0;
    const Sample& first = samples_.front();
    const Sample& last = samples_.back();
    // Nothing for a whole window is a stall, not a slow download.
    if (nowMs > last.ms + kWindowMs) return 0;
    // Measured up to now rather than to the last sample, so that a download that has
    // stopped coming in is seen to slow down.
    const std::uint64_t span = std::max(nowMs, last.ms) - first.ms;
    if (span < kMinSpanMs) return 0;
    return static_cast<double>(last.bytes - first.bytes) * 1000.0 / static_cast<double>(span);
}

std::wstring_view FailureText(DownloadFailure failure) noexcept {
    switch (failure) {
    case DownloadFailure::Network:
        return L"Network error";
    case DownloadFailure::ServerUnreachable:
        return L"Server unreachable";
    case DownloadFailure::Server:
        return L"Server problem";
    case DownloadFailure::NoFile:
        return L"No file";
    case DownloadFailure::NeedsAuthorization:
        return L"Needs authorization";
    case DownloadFailure::BadCertificate:
        return L"Bad certificate";
    case DownloadFailure::Forbidden:
        return L"Forbidden";
    case DownloadFailure::AccessDenied:
        return L"Insufficient permissions";
    case DownloadFailure::DiskFull:
        return L"Disk full";
    case DownloadFailure::PathTooLong:
        return L"Path too long";
    case DownloadFailure::FileTooLarge:
        return L"File too large";
    case DownloadFailure::Virus:
        return L"Virus detected";
    case DownloadFailure::Blocked:
        return L"Blocked";
    case DownloadFailure::VirusScanFailed:
        return L"Virus scan failed";
    case DownloadFailure::Incomplete:
        return L"File incomplete";
    case DownloadFailure::SystemBusy:
        return L"System busy";
    case DownloadFailure::Shutdown:
        return L"Browser closed";
    case DownloadFailure::Crashed:
        return L"Crashed";
    case DownloadFailure::Unknown:
        break;
    }
    return L"Download error";
}

std::wstring StatusText(const DownloadFacts& facts) {
    switch (facts.phase) {
    case DownloadPhase::InProgress: {
        if (facts.held) return L"This type of file can be harmful";
        if (facts.received <= 0 && facts.total <= 0 && facts.bytesPerSecond <= 0) return L"Starting…";
        std::wstring text = FormatProgress(facts.received, facts.total);
        if (facts.bytesPerSecond > 0) text.append(kSeparator).append(FormatSpeed(facts.bytesPerSecond));
        if (const auto left = SecondsLeft(facts.received, facts.total, facts.bytesPerSecond)) {
            text.append(kSeparator).append(FormatTimeLeft(*left));
        }
        return text;
    }
    case DownloadPhase::Paused:
        return std::wstring(L"Paused").append(kSeparator).append(FormatProgress(facts.received, facts.total));
    case DownloadPhase::Completed:
        if (facts.fileGone) return L"Deleted";
        return FormatBytes(std::max(facts.received, facts.total)).append(kSeparator).append(L"Done");
    case DownloadPhase::Canceled:
        return L"Canceled";
    case DownloadPhase::Failed:
        return std::wstring(L"Failed").append(kSeparator).append(FailureText(facts.failure));
    }
    return {};
}

std::wstring_view PhaseWord(const DownloadFacts& facts) noexcept {
    switch (facts.phase) {
    case DownloadPhase::InProgress:
        return facts.held ? L"Held" : L"Downloading";
    case DownloadPhase::Paused:
        return L"Paused";
    case DownloadPhase::Completed:
        return facts.fileGone ? L"Deleted" : L"Done";
    case DownloadPhase::Canceled:
        return L"Canceled";
    case DownloadPhase::Failed:
        return L"Failed";
    }
    return {};
}

} // namespace exodium
