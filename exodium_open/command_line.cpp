#include "command_line.h"

#include <algorithm>
#include <cstddef>

namespace exodium {

namespace {

constexpr std::wstring_view kClassOption = L"--class=";
constexpr std::wstring_view kSpace = L" \t";

[[nodiscard]] std::wstring_view TrimSpace(std::wstring_view text) noexcept {
    const std::size_t first = text.find_first_not_of(kSpace);
    if (first == std::wstring_view::npos) return {};
    return text.substr(first, text.find_last_not_of(kSpace) - first + 1);
}

// The program's name, quoted or not, comes first.
[[nodiscard]] std::wstring_view AfterProgramName(std::wstring_view commandLine) noexcept {
    const bool quoted = commandLine.starts_with(L'"');
    const std::size_t end = quoted ? commandLine.find(L'"', 1) : commandLine.find_first_of(kSpace);
    if (end == std::wstring_view::npos) return {};
    return commandLine.substr(end + (quoted ? 1 : 0));
}

} // namespace

OpenCommand ParseOpenCommand(std::wstring_view commandLine) noexcept {
    OpenCommand command;
    std::wstring_view rest = TrimSpace(AfterProgramName(commandLine));
    if (rest.starts_with(kClassOption)) {
        rest.remove_prefix(kClassOption.size());
        const std::size_t end = std::min(rest.find_first_of(kSpace), rest.size());
        command.winampClass = rest.substr(0, end);
        rest = TrimSpace(rest.substr(end));
    }
    if (rest.size() >= 2 && rest.front() == L'"' && rest.back() == L'"') rest = rest.substr(1, rest.size() - 2);
    command.link = rest;
    return command;
}

} // namespace exodium
