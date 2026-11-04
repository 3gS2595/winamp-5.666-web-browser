// Reading exodium_open.exe's command line. This is apart from the program so that the
// unit tests can get at it.
#pragma once

#include <string_view>

namespace exodium {

struct OpenCommand {
    std::wstring_view winampClass; // the window class Winamp runs under, if it was given
    std::wstring_view link;        // may be empty
};

// `commandLine` is the whole of it, as GetCommandLineW has it, and the result points
// into it. Windows pastes a link into the command it was registered with just as the
// link stands, so the usual splitting into arguments won't do: a quote in the link would
// end the argument early and make options of what follows. Here everything after the
// one option there is, which goes first, is the link.
//
//     exodium_open.exe [--class=NAME] [link]
//
// The registered command quotes its link, which is what keeps a link from posing as
// the option.
[[nodiscard]] OpenCommand ParseOpenCommand(std::wstring_view commandLine) noexcept;

} // namespace exodium
