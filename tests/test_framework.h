// A test runner small enough to need no dependency: cases register themselves, and
// a failed expectation is reported with its place in the source.
#pragma once

#include <concepts>
#include <format>
#include <source_location>
#include <string>
#include <string_view>
#include <vector>

namespace test {

struct Case {
    const char* name;
    void (*run)();
};

[[nodiscard]] std::vector<Case>& Registry();

struct Registrar {
    Registrar(const char* name, void (*run)()) { Registry().push_back({name, run}); }
};

void ReportFailure(std::string_view message, const std::source_location& where);

[[nodiscard]] std::string Describe(std::wstring_view value);
[[nodiscard]] inline std::string Describe(const std::wstring& value) {
    return Describe(std::wstring_view(value));
}
[[nodiscard]] inline std::string Describe(const wchar_t* value) {
    return Describe(std::wstring_view(value));
}
[[nodiscard]] inline std::string Describe(wchar_t value) {
    return Describe(std::wstring_view(&value, 1));
}
[[nodiscard]] inline std::string Describe(bool value) {
    return value ? "true" : "false";
}
template <typename Number>
    requires std::integral<Number> || std::floating_point<Number>
[[nodiscard]] std::string Describe(Number value) {
    return std::format("{}", value);
}

inline void Expect(bool condition, std::string_view expression,
                   const std::source_location& where = std::source_location::current()) {
    if (!condition) ReportFailure(std::format("expected {}", expression), where);
}

template <typename Actual, typename Expected>
void ExpectEqual(const Actual& actual, const Expected& expected, std::string_view expression,
                 const std::source_location& where = std::source_location::current()) {
    if (actual == expected) return;
    ReportFailure(
        std::format("{}\n      actual:   {}\n      expected: {}", expression, Describe(actual), Describe(expected)),
        where);
}

} // namespace test

#define TEST_CASE(name)                                                                                                \
    static void name();                                                                                                \
    static const ::test::Registrar name##Registrar{#name, &name};                                                      \
    static void name()

#define EXPECT(condition) ::test::Expect(static_cast<bool>(condition), #condition)
#define EXPECT_EQ(actual, expected) ::test::ExpectEqual((actual), (expected), #actual " == " #expected)
