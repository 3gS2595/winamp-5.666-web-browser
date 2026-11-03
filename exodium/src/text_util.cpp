#include "text_util.h"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <functional>
#include <memory>

namespace exodium {

namespace {

constexpr std::wstring_view kWhitespace = L" \t\r\n";
constexpr std::string_view kUtf16Bom = "\xFF\xFE";
constexpr std::string_view kUtf8Bom = "\xEF\xBB\xBF";

// More than any file the plugin keeps should ever hold; a file past it is left alone.
constexpr std::size_t kMaxTextFileBytes = 64 * 1024 * 1024;
constexpr wchar_t kByteOrderMark = 0xFEFF;

// A file handle that is closed on scope exit. CreateFile's failure value is not null.
struct FileCloser {
    void operator()(HANDLE handle) const noexcept { ::CloseHandle(handle); }
};
class FileHandle {
public:
    explicit FileHandle(HANDLE handle) noexcept : handle_(handle == INVALID_HANDLE_VALUE ? nullptr : handle) {}
    [[nodiscard]] HANDLE get() const noexcept { return handle_.get(); }
    explicit operator bool() const noexcept { return handle_ != nullptr; }

private:
    std::unique_ptr<void, FileCloser> handle_;
};

} // namespace

wchar_t FoldCase(wchar_t c) noexcept {
    if (c < 0x80) return c >= L'A' && c <= L'Z' ? static_cast<wchar_t>(c + (L'a' - L'A')) : c;
    // CharLowerW takes a single character in place of a string when the rest of its
    // argument is zero, and hands it back the same way.
    const auto folded = ::CharLowerW(reinterpret_cast<LPWSTR>(static_cast<ULONG_PTR>(c)));
    return static_cast<wchar_t>(reinterpret_cast<ULONG_PTR>(folded));
}

std::wstring ToLower(std::wstring_view text) {
    std::wstring lower(text);
    std::ranges::transform(lower, lower.begin(), FoldCase);
    return lower;
}

bool EqualsIgnoreCase(std::wstring_view a, std::wstring_view b) noexcept {
    return ::CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(), static_cast<int>(b.size()), TRUE) ==
           CSTR_EQUAL;
}

bool StartsWithIgnoreCase(std::wstring_view text, std::wstring_view prefix) noexcept {
    return text.size() >= prefix.size() && EqualsIgnoreCase(text.substr(0, prefix.size()), prefix);
}

std::size_t FindIgnoreCase(std::wstring_view text, std::wstring_view term, std::size_t from) noexcept {
    if (from > text.size()) return std::wstring_view::npos;
    const std::wstring_view tail = text.substr(from);
    const auto found = std::ranges::search(tail, term, std::ranges::equal_to{}, FoldCase, FoldCase);
    if (found.begin() == tail.end() && !term.empty()) return std::wstring_view::npos;
    return from + static_cast<std::size_t>(found.begin() - tail.begin());
}

std::wstring_view Trim(std::wstring_view text) noexcept {
    const auto first = text.find_first_not_of(kWhitespace);
    if (first == std::wstring_view::npos) return {};
    return text.substr(first, text.find_last_not_of(kWhitespace) - first + 1);
}

std::vector<std::wstring_view> SplitWords(std::wstring_view text, std::wstring_view separators) {
    std::vector<std::wstring_view> words;
    for (std::size_t begin = 0; begin < text.size();) {
        const std::size_t end = std::min(text.find_first_of(separators, begin), text.size());
        if (end > begin) words.push_back(text.substr(begin, end - begin));
        begin = end + 1;
    }
    return words;
}

std::string ToUtf8(std::wstring_view text) {
    std::string utf8;
    if (text.empty()) return utf8;
    const int length = static_cast<int>(text.size());
    const int size = ::WideCharToMultiByte(CP_UTF8, 0, text.data(), length, nullptr, 0, nullptr, nullptr);
    if (size <= 0) return utf8;
    utf8.resize_and_overwrite(static_cast<std::size_t>(size), [&](char* buffer, std::size_t capacity) {
        const int written = ::WideCharToMultiByte(CP_UTF8, 0, text.data(), length, buffer, static_cast<int>(capacity),
                                                  nullptr, nullptr);
        return static_cast<std::size_t>(std::max(written, 0));
    });
    return utf8;
}

std::wstring FromUtf8(std::string_view text) {
    std::wstring wide;
    if (text.empty()) return wide;
    const int length = static_cast<int>(text.size());
    const int size = ::MultiByteToWideChar(CP_UTF8, 0, text.data(), length, nullptr, 0);
    if (size <= 0) return wide;
    wide.resize_and_overwrite(static_cast<std::size_t>(size), [&](wchar_t* buffer, std::size_t capacity) {
        const int written = ::MultiByteToWideChar(CP_UTF8, 0, text.data(), length, buffer, static_cast<int>(capacity));
        return static_cast<std::size_t>(std::max(written, 0));
    });
    return wide;
}

std::wstring ToHex(unsigned long value, std::size_t minDigits, bool upperCase) {
    char digits[2 * sizeof(value)];
    const char* const end = std::to_chars(digits, digits + std::size(digits), value, 16).ptr;
    const auto count = static_cast<std::size_t>(end - digits);

    std::wstring hex(minDigits > count ? minDigits - count : 0, L'0');
    for (const char* digit = digits; digit != end; ++digit) {
        const bool letter = *digit >= 'a';
        hex += static_cast<wchar_t>(letter && upperCase ? *digit - 'a' + 'A' : *digit);
    }
    return hex;
}

std::wstring JsStringLiteral(std::wstring_view text) {
    std::wstring out;
    out.reserve(text.size() + 2);
    out += L'"';
    for (const wchar_t c : text) {
        if (c == L'"' || c == L'\\') {
            out += L'\\';
            out += c;
        } else if (c < 0x20 || c == L'<' || c == 0x2028 || c == 0x2029) {
            out += L"\\u";
            out += ToHex(c, 4);
        } else {
            out += c;
        }
    }
    out += L'"';
    return out;
}

std::optional<std::wstring> ReadTextFile(const std::filesystem::path& path) {
    const FileHandle file(::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                        FILE_ATTRIBUTE_NORMAL, nullptr));
    LARGE_INTEGER size{};
    if (!file || !::GetFileSizeEx(file.get(), &size)) return std::nullopt;
    const auto byteCount = static_cast<unsigned long long>(size.QuadPart);
    if (byteCount > kMaxTextFileBytes) return std::nullopt;

    std::string bytes(static_cast<std::size_t>(byteCount), '\0');
    DWORD read = 0;
    if (!::ReadFile(file.get(), bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr)) return std::nullopt;
    bytes.resize(read);

    std::string_view data = bytes;
    std::wstring text;
    if (data.starts_with(kUtf16Bom)) {
        data.remove_prefix(kUtf16Bom.size());
        text.resize(data.size() / sizeof(wchar_t));
        std::memcpy(text.data(), data.data(), text.size() * sizeof(wchar_t));
    } else {
        if (data.starts_with(kUtf8Bom)) data.remove_prefix(kUtf8Bom.size());
        text = FromUtf8(data);
    }
    std::erase(text, L'\r');
    return text;
}

bool WriteTextFile(const std::filesystem::path& path, std::wstring_view text) {
    std::wstring out;
    out.reserve(text.size() + text.size() / 16 + 1);
    out += kByteOrderMark;
    for (const wchar_t c : text) {
        if (c == L'\n') out += L'\r';
        out += c;
    }
    const std::size_t byteCount = out.size() * sizeof(wchar_t);
    if (byteCount > kMaxTextFileBytes) return false;

    // Written beside the file and moved over it, so the file is always whole.
    std::filesystem::path temp = path;
    temp += L".tmp";
    {
        const FileHandle file(
            ::CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
        DWORD written = 0;
        if (!file || !::WriteFile(file.get(), out.data(), static_cast<DWORD>(byteCount), &written, nullptr) ||
            written != byteCount) {
            return false;
        }
    }
    if (!::MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        ::DeleteFileW(temp.c_str());
        return false;
    }
    return true;
}

} // namespace exodium
