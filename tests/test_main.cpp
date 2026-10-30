#include "test_framework.h"

#include "text_util.h"

#include <cstdio>
#include <exception>

namespace test {

namespace {

int g_failures = 0;

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
    return std::format("\"{}\"", exodium::ToUtf8(value));
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
        const bool passed = test::g_failures == before;
        if (!passed) ++failedCases;
        std::puts(std::format("[{}] {}", passed ? " ok " : "FAIL", testCase.name).c_str());
    }
    std::puts(std::format("{} of {} test cases passed", test::Registry().size() - failedCases, test::Registry().size())
                  .c_str());
    return failedCases == 0 ? 0 : 1;
}
