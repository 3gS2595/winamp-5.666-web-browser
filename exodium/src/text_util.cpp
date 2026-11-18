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

// The same answer as CompareStringOrdinal gives without case, which compares a character
// at a time, so texts of different lengths are never equal. Addresses are compared by the
// thousand for every key typed in the address bar, and nearly all of what is in them is
// ASCII, which is told apart here; the rest is left to Windows' own tables.
bool EqualsIgnoreCase(std::wstring_view a, std::wstring_view b) noexcept {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const wchar_t x = a[i], y = b[i];
        if (x == y) continue;
        if ((x | y) < 0x80) {
            if (FoldCase(x) != FoldCase(y)) return false;
            continue;
        }
        const int rest = static_cast<int>(a.size() - i);
        return ::CompareStringOrdinal(a.data() + i, rest, b.data() + i, rest, TRUE) == CSTR_EQUAL;
    }
    return true;
}

bool StartsWithIgnoreCase(std::wstring_view text, std::wstring_view prefix) noexcept {
    return text.size() >= prefix.size() && EqualsIgnoreCase(text.substr(0, prefix.size()), prefix);
}

std::size_t FindIgnoreCase(std::wstring_view text, std::wstring_view term, std::size_t from) noexcept {
    if (from > text.size()) return std::wstring_view::npos;
    if (term.empty()) return from;
    if (term.size() > text.size() - from) return std::wstring_view::npos;
    const wchar_t first = FoldCase(term.front());
    const std::size_t last = text.size() - term.size();
    const auto matchesAt = [&](std::size_t at) noexcept {
        std::size_t matched = 1;
        while (matched < term.size() && FoldCase(text[at + matched]) == FoldCase(term[matched])) ++matched;
        return matched == term.size();
    };
    if (first < 0x80) {
        // Nearly every term begins with an ASCII character, which is looked for as it
        // stands, in either case: the library does that many characters at a time, and the
        // address bar looks through every address and title there is for each key typed.
        // (So a character from outside ASCII that folds to an ASCII letter, as the Kelvin
        // sign does to k, isn't taken for the first letter of a term. It is for the rest.)
        const bool letter = first >= L'a' && first <= L'z';
        const wchar_t either[2] = {first, letter ? static_cast<wchar_t>(first - (L'a' - L'A')) : first};
        const std::wstring_view wanted(either, letter ? 2 : 1);
        for (std::size_t at = text.find_first_of(wanted, from); at <= last; at = text.find_first_of(wanted, at + 1)) {
            if (matchesAt(at)) return at;
        }
        return std::wstring_view::npos; // npos is past `last`, which ends the loop
    }
    for (std::size_t at = from; at <= last; ++at) {
        if (FoldCase(text[at]) == first && matchesAt(at)) return at;
    }
    return std::wstring_view::npos;
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

std::vector<std::wstring_view> SplitColumns(std::wstring_view line) {
    std::vector<std::wstring_view> columns;
    for (std::size_t begin = 0;;) {
        const std::size_t end = line.find(L'\t', begin);
        columns.push_back(line.substr(begin, end == std::wstring_view::npos ? end : end - begin));
        if (end == std::wstring_view::npos) return columns;
        begin = end + 1;
    }
}

std::wstring SingleLine(std::wstring_view text, std::size_t maxLength) {
    std::wstring line(text.substr(0, maxLength));
    std::ranges::replace_if(line, [](wchar_t c) { return c == L'\t' || c == L'\r' || c == L'\n'; }, L' ');
    return line;
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

namespace {

// `text` as the files hold it: every line end a CR LF, after `lead`.
[[nodiscard]] std::wstring WithFileLineEnds(std::wstring_view text, std::wstring_view lead) {
    std::wstring out;
    out.reserve(lead.size() + text.size() + text.size() / 16 + 1);
    out += lead;
    for (const wchar_t c : text) {
        if (c == L'\n') out += L'\r';
        out += c;
    }
    return out;
}

} // namespace

bool WriteTextFile(const std::filesystem::path& path, std::wstring_view text) {
    const std::wstring out = WithFileLineEnds(text, std::wstring_view(&kByteOrderMark, 1));
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
        // On the disk before it is given the file's name. A rename is kept by the file
        // system's journal and what a file holds is not, so without this a power cut just
        // after could leave the name on a file whose contents never got there.
        ::FlushFileBuffers(file.get());
    }
    if (!::MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        ::DeleteFileW(temp.c_str());
        return false;
    }
    return true;
}

bool AppendTextFile(const std::filesystem::path& path, std::wstring_view text) {
    const FileHandle file(::CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
                                        FILE_ATTRIBUTE_NORMAL, nullptr));
    LARGE_INTEGER size{};
    if (!file || !::GetFileSizeEx(file.get(), &size)) return false;
    // A new file starts with the mark that says what it is written in.
    const std::wstring out =
        WithFileLineEnds(text, size.QuadPart == 0 ? std::wstring_view(&kByteOrderMark, 1) : std::wstring_view());
    const std::size_t byteCount = out.size() * sizeof(wchar_t);
    if (static_cast<unsigned long long>(size.QuadPart) + byteCount > kMaxTextFileBytes) return false;
    DWORD written = 0;
    return ::WriteFile(file.get(), out.data(), static_cast<DWORD>(byteCount), &written, nullptr) &&
           written == byteCount;
}

} // namespace exodium
