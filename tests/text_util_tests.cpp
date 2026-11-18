#include "test_framework.h"

#include "text_util.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

using namespace exodium;

namespace {

constexpr auto kNotFound = std::wstring_view::npos;

std::filesystem::path TempFile(std::wstring_view name) {
    return std::filesystem::temp_directory_path() / (L"exodium_tests_" + std::wstring(name));
}

std::string ReadBytes(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

} // namespace

TEST_CASE(FoldCaseFoldsBeyondAscii) {
    EXPECT_EQ(FoldCase(L'A'), L'a');
    EXPECT_EQ(FoldCase(L'z'), L'z');
    EXPECT_EQ(FoldCase(L'7'), L'7');
    EXPECT_EQ(FoldCase(L'\u00C4'), L'\u00E4'); // Ä -> ä
    EXPECT_EQ(ToLower(L"MiXeD \u00C4"), L"mixed \u00E4");
}

TEST_CASE(ComparesWithoutCase) {
    EXPECT(EqualsIgnoreCase(L"HTTPS://Example.com", L"https://example.COM"));
    EXPECT(!EqualsIgnoreCase(L"example.com", L"example.co"));
    EXPECT(EqualsIgnoreCase(L"", L""));
    EXPECT(StartsWithIgnoreCase(L"Hello", L"HE"));
    EXPECT(StartsWithIgnoreCase(L"Hello", L""));
    EXPECT(!StartsWithIgnoreCase(L"He", L"hello"));
}

TEST_CASE(ComparesWithoutCaseBeyondAscii) {
    EXPECT(EqualsIgnoreCase(L"Ärger", L"äRGER")); // Ä and ä
    EXPECT(EqualsIgnoreCase(L"cafÉ", L"CAFé"));   // É and é, after ASCII that differs in case
    EXPECT(!EqualsIgnoreCase(L"café", L"cafe"));
    EXPECT(!EqualsIgnoreCase(L"é", L"è"));
    EXPECT(!EqualsIgnoreCase(L"abc", L"abcd"));
    EXPECT(!EqualsIgnoreCase(L"[", L"{")); // 0x5B and 0x7B: a bit apart as letters' cases are, and no letters
    EXPECT(!EqualsIgnoreCase(L"@", L"`"));
    EXPECT(StartsWithIgnoreCase(L"Ärger", L"äR"));
}

TEST_CASE(FindsWithoutCase) {
    EXPECT_EQ(FindIgnoreCase(L"Hello World", L"WORLD"), std::size_t{6});
    EXPECT_EQ(FindIgnoreCase(L"abcabc", L"BC", 2), std::size_t{4});
    EXPECT_EQ(FindIgnoreCase(L"abc", L"x"), kNotFound);
    EXPECT_EQ(FindIgnoreCase(L"abc", L"abcd"), kNotFound);
    EXPECT_EQ(FindIgnoreCase(L"abc", L"c", 9), kNotFound);
    // The start of a match that doesn't go through, before one that does.
    EXPECT_EQ(FindIgnoreCase(L"aab aaB aAc", L"AAC"), std::size_t{8});
    EXPECT_EQ(FindIgnoreCase(L"abc", L"C"), std::size_t{2});
    EXPECT_EQ(FindIgnoreCase(L"Ärger", L"äRG"), std::size_t{0});
    // Nothing is found wherever the search starts, as far as the text's end.
    EXPECT_EQ(FindIgnoreCase(L"abc", L""), std::size_t{0});
    EXPECT_EQ(FindIgnoreCase(L"abc", L"", 3), std::size_t{3});
    EXPECT_EQ(FindIgnoreCase(L"abc", L"", 4), kNotFound);
}

TEST_CASE(TrimsAndSplits) {
    EXPECT_EQ(Trim(L"  a b \r\n"), std::wstring_view(L"a b"));
    EXPECT(Trim(L" \t\r\n").empty());
    EXPECT_EQ(Trim(L"x"), std::wstring_view(L"x"));

    const auto words = SplitWords(L"one  two\tthree ");
    EXPECT_EQ(words.size(), std::size_t{3});
    EXPECT_EQ(words[0], std::wstring_view(L"one"));
    EXPECT_EQ(words[1], std::wstring_view(L"two"));
    EXPECT_EQ(words[2], std::wstring_view(L"three"));
    EXPECT(SplitWords(L"   ").empty());
}

TEST_CASE(SplitsALineIntoItsColumns) {
    const auto columns = SplitColumns(L"a\t\tc d\t");
    EXPECT_EQ(columns.size(), std::size_t{4});
    EXPECT_EQ(columns[0], std::wstring_view(L"a"));
    EXPECT(columns[1].empty());
    EXPECT_EQ(columns[2], std::wstring_view(L"c d"));
    EXPECT(columns[3].empty());
    // A line with nothing on it has one column, with nothing in it.
    EXPECT_EQ(SplitColumns(L"").size(), std::size_t{1});
    EXPECT_EQ(SplitColumns(L"alone").size(), std::size_t{1});

    EXPECT_EQ(SingleLine(L"one\ttwo\r\nthree", 100), L"one two  three");
    EXPECT_EQ(SingleLine(L"abcdef", 3), L"abc");
    EXPECT_EQ(SingleLine(L"", 3), L"");
}

TEST_CASE(ReadsHexadecimalDigits) {
    EXPECT_EQ(HexDigitValue(L'0'), 0);
    EXPECT_EQ(HexDigitValue(L'9'), 9);
    EXPECT_EQ(HexDigitValue(L'a'), 10);
    EXPECT_EQ(HexDigitValue(L'F'), 15);
    EXPECT_EQ(HexDigitValue(L'g'), -1);
    EXPECT_EQ(HexDigitValue(L' '), -1);
}

TEST_CASE(ConvertsUtf8BothWays) {
    EXPECT(ToUtf8(L"\u00E9") == "\xC3\xA9");
    EXPECT(ToUtf8(L"").empty());
    const std::wstring text = L"\u65E5\u672C\u8A9E \u2715 plain";
    EXPECT_EQ(FromUtf8(ToUtf8(text)), text);
}

TEST_CASE(ParsesLeadingNumbers) {
    EXPECT_EQ(ParseNumber<int>(L"42").value_or(-1), 42);
    EXPECT_EQ(ParseNumber<int>(L"  12abc").value_or(-1), 12);
    EXPECT_EQ(ParseNumber<int>(L"-7").value_or(0), -7);
    EXPECT(!ParseNumber<int>(L"abc"));
    EXPECT(!ParseNumber<int>(L""));
    EXPECT_EQ(ParseNumber<std::int64_t>(L"1759968000123").value_or(0), std::int64_t{1759968000123});
    EXPECT_EQ(ParseNumber<double>(L"1.2500").value_or(0), 1.25);
}

TEST_CASE(EscapesScriptStrings) {
    EXPECT_EQ(JsStringLiteral(L"plain"), L"\"plain\"");
    EXPECT_EQ(JsStringLiteral(L"a\"b\\c"), L"\"a\\\"b\\\\c\"");
    EXPECT_EQ(JsStringLiteral(L"</script>\n"), L"\"\\u003c/script>\\u000a\"");
    EXPECT_EQ(JsStringLiteral(L"\u2028"), L"\"\\u2028\"");
}

TEST_CASE(WritesTheFileFormatEarlierVersionsWrote) {
    const auto path = TempFile(L"utf16.txt");
    const std::wstring text = L"line one\tx\nline two \u2715\n";
    EXPECT(WriteTextFile(path, text));

    // UTF-16 with a byte-order mark, and lines that end in CR LF.
    const std::string bytes = ReadBytes(path);
    EXPECT(bytes.starts_with("\xFF\xFE"));
    EXPECT(bytes.contains(std::string_view("\r\0\n\0", 4)));
    EXPECT(!std::filesystem::exists(path.wstring() + L".tmp"));

    const auto read = ReadTextFile(path);
    EXPECT(read.has_value());
    EXPECT_EQ(read.value_or(L""), text);
    std::filesystem::remove(path);
}

TEST_CASE(AddsToTheEndOfAFile) {
    const auto path = TempFile(L"appended.txt");
    std::filesystem::remove(path);
    // It is made if it isn't there, as the same kind of file WriteTextFile makes.
    EXPECT(AppendTextFile(path, L"one\n"));
    EXPECT(AppendTextFile(path, L"two ✕\nthree\n"));
    EXPECT(AppendTextFile(path, L""));
    const std::string bytes = ReadBytes(path);
    EXPECT(bytes.starts_with("\xFF\xFE"));
    EXPECT_EQ(bytes.size(), std::size_t{2 + 2 * (5 + 7 + 7)}); // one mark, and CR LF for each line end
    EXPECT_EQ(ReadTextFile(path).value_or(L""), L"one\ntwo ✕\nthree\n");

    // And to one WriteTextFile made.
    EXPECT(WriteTextFile(path, L"first\n"));
    EXPECT(AppendTextFile(path, L"second\n"));
    EXPECT_EQ(ReadTextFile(path).value_or(L""), L"first\nsecond\n");
    std::filesystem::remove(path);
}

TEST_CASE(ReadsUtf8Files) {
    const auto path = TempFile(L"utf8.txt");
    {
        std::ofstream file(path, std::ios::binary);
        file << "\xEF\xBB\xBF"
                "a\tb\r\n\xC3\xA9\n";
    }
    EXPECT_EQ(ReadTextFile(path).value_or(L""), L"a\tb\n\u00E9\n");
    std::filesystem::remove(path);
}

TEST_CASE(MissingFileReadsAsNothing) {
    EXPECT(!ReadTextFile(TempFile(L"does_not_exist.txt")));
}
