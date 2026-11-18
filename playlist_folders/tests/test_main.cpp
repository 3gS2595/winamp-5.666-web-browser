#include "../../tests/test_framework.h"

#include <windows.h>

#include <cstdio>
#include <exception>

namespace test {

namespace {

int g_failures = 0;

std::string Utf8(std::wstring_view text) {
    if (text.empty()) return {};
    const int length =
        ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string utf8(static_cast<size_t>(length), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), utf8.data(), length, nullptr, nullptr);
    return utf8;
}

} // namespace

std::vector<Case>& Registry() {
    static std::vector<Case> cases;
    return cases;
}

void ReportFailure(std::string_view message, const std::source_location& where) {
    ++g_failures;
    std::fputs(std::format("    {}({}): {}\n", where.file_name(), where.line(), message).c_str(), stderr);
}

std::string Describe(std::wstring_view value) {
    return std::format("\"{}\"", Utf8(value));
}

} // namespace test

int main() {
    int failedCases = 0;
    for (const test::Case& testCase : test::Registry()) {
        const int before = test::g_failures;
        try {
            testCase.run();
        } catch (const std::exception& error) {
            test::ReportFailure(std::format("threw {}", error.what()), std::source_location::current());
        }
        if (test::g_failures != before) {
            ++failedCases;
            std::fprintf(stderr, "FAIL %s\n", testCase.name);
        }
    }
    std::printf("%zu tests, %d failed\n", test::Registry().size(), failedCases);
    return failedCases == 0 ? 0 : 1;
}
