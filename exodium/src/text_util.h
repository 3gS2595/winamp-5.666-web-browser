// Text helpers with no knowledge of the browser: case folding, trimming, encoding
// conversion, and the plain-text files the plugin keeps its data in.
#pragma once

#include <charconv>
#include <cstddef>
#include <filesystem>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace exodium {

// A character in lower case. Unlike towlower in the "C" locale, this folds letters
// outside ASCII as well.
[[nodiscard]] wchar_t FoldCase(wchar_t c) noexcept;
[[nodiscard]] std::wstring ToLower(std::wstring_view text);

[[nodiscard]] bool EqualsIgnoreCase(std::wstring_view a, std::wstring_view b) noexcept;
[[nodiscard]] bool StartsWithIgnoreCase(std::wstring_view text, std::wstring_view prefix) noexcept;
// Where `term` first turns up in `text` at or after `from`, or npos.
[[nodiscard]] std::size_t FindIgnoreCase(std::wstring_view text, std::wstring_view term, std::size_t from = 0) noexcept;

// `text` without the spaces, tabs and line ends around it.
[[nodiscard]] std::wstring_view Trim(std::wstring_view text) noexcept;
// The runs of `text` between any of `separators`, empty ones left out.
[[nodiscard]] std::vector<std::wstring_view> SplitWords(std::wstring_view text, std::wstring_view separators = L" \t");
// The columns of a line of one of the plugin's files, which have tabs between them. Empty
// ones are kept: a column is known by its place.
[[nodiscard]] std::vector<std::wstring_view> SplitColumns(std::wstring_view line);
// `text` made fit to be a column of such a line: cut to `maxLength`, with a space for
// each tab and line end in it.
[[nodiscard]] std::wstring SingleLine(std::wstring_view text, std::size_t maxLength);

// The value of a hexadecimal digit, or -1.
[[nodiscard]] constexpr int HexDigitValue(wchar_t c) noexcept {
    if (c >= L'0' && c <= L'9') return c - L'0';
    if (c >= L'a' && c <= L'f') return c - L'a' + 10;
    if (c >= L'A' && c <= L'F') return c - L'A' + 10;
    return -1;
}

[[nodiscard]] std::string ToUtf8(std::wstring_view text);
[[nodiscard]] std::wstring FromUtf8(std::string_view text);

// The number `text` begins with, as _wtoi and _wtof read one, but whatever the
// locale. Nothing if it doesn't begin with a number.
template <typename Number>
    requires std::is_arithmetic_v<Number>
[[nodiscard]] std::optional<Number> ParseNumber(std::wstring_view text) noexcept {
    text = Trim(text);
    char digits[64];
    std::size_t count = 0;
    for (const wchar_t c : text) {
        if (c > 0x7F || count == std::size(digits)) break;
        digits[count++] = static_cast<char>(c);
    }
    Number value{};
    if (std::from_chars(digits, digits + count, value).ec != std::errc{}) return std::nullopt;
    return value;
}

// `value` in hexadecimal, padded with zeros to at least `minDigits`.
[[nodiscard]] std::wstring ToHex(unsigned long value, std::size_t minDigits, bool upperCase = false);

// A string as script source. '<' is written out so that nothing in a title or an
// address can end the script it is put in.
[[nodiscard]] std::wstring JsStringLiteral(std::wstring_view text);

// Reads a whole text file, with every line ending in '\n'. The files the plugin
// writes are UTF-16; UTF-8, with a byte-order mark or without, is read as well.
[[nodiscard]] std::optional<std::wstring> ReadTextFile(const std::filesystem::path& path);
// Writes `text`, whose lines end in '\n', as the UTF-16 text file earlier versions
// wrote. The file is replaced in one step, so a failed write leaves the old one, and
// what is written is on the disk before it takes the old one's place: a power cut
// leaves one or the other whole.
bool WriteTextFile(const std::filesystem::path& path, std::wstring_view text);
// Adds `text`, whose lines end in '\n', to the end of such a file, which is made if it
// isn't there. ReadTextFile reads the whole of it. Nothing is flushed: what a crash of
// the program leaves is there, and a power cut may lose the last of it.
bool AppendTextFile(const std::filesystem::path& path, std::wstring_view text);

} // namespace exodium
