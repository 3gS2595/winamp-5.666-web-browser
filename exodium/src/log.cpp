// The plugin's log: exodium.log in its data folder. What goes wrong out of sight is
// written there with the time it happened: a webview that couldn't be made, a page's
// process that died and why, a file that couldn't be written. Until 2026-11-09 these only
// went to a debugger's output window, which nobody has open when they happen.
//
// It is a text file like the plugin's others, added to a line at a time. Past half a
// megabyte it is put aside as exodium.log.old, in the place of the one before it, and
// begun again, so the two never come to more than a megabyte. Any thread may write to it.
#include "browser.h"

#include "text_util.h"

#include <mutex>

namespace exodium {

namespace {

constexpr std::uintmax_t kMaxLogBytes = 512 * 1024;

std::mutex g_logLock;

void AppendPadded(std::wstring& text, unsigned value, std::size_t digits) {
    const std::wstring number = std::to_wstring(value);
    if (number.size() < digits) text.append(digits - number.size(), L'0');
    text += number;
}

// The time now, as in "2026-11-09 14:03:22".
[[nodiscard]] std::wstring TimeStamp() {
    SYSTEMTIME now{};
    ::GetLocalTime(&now);
    std::wstring text;
    AppendPadded(text, now.wYear, 4);
    text += L'-';
    AppendPadded(text, now.wMonth, 2);
    text += L'-';
    AppendPadded(text, now.wDay, 2);
    text += L' ';
    AppendPadded(text, now.wHour, 2);
    text += L':';
    AppendPadded(text, now.wMinute, 2);
    text += L':';
    AppendPadded(text, now.wSecond, 2);
    return text;
}

} // namespace

void LogLine(std::wstring_view text) {
    std::wstring line = TimeStamp();
    line.append(L"  ").append(SingleLine(text, 2000)).append(1, L'\n');
    ::OutputDebugStringW((L"[exodium] " + line).c_str());

    const std::lock_guard lock(g_logLock);
    const std::filesystem::path file = LogFile();
    std::error_code error;
    if (std::filesystem::file_size(file, error) > kMaxLogBytes && !error) {
        std::filesystem::path old = file;
        old += L".old";
        ::MoveFileExW(file.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING);
    }
    AppendTextFile(file, line);
}

void LogFailure(std::wstring_view what, HRESULT result) {
    LogLine(std::wstring(what).append(L" failed: 0x").append(ToHex(static_cast<unsigned long>(result), 8)));
}

} // namespace exodium
