// exodium_open.exe: what Windows runs for a link when Exodium is the default browser, and
// what Exodium's entry in the Start menu runs, with no link, for a window on the home page.
// The browser is a plugin inside Winamp, so all this does is see that Winamp is running,
// starting it if need be, and hand the link to the plugin. It sits in Winamp's Plugins
// folder, which is how it finds Winamp.
#include "command_line.h"
#include "link_ipc.h"

#include <windows.h>

#include <filesystem>
#include <string>
#include <string_view>

namespace exodium {

namespace {

// Long enough for Winamp to start and load its plugins from a cold disk.
constexpr ULONGLONG kPatienceMs = 30000;
constexpr DWORD kRetryMs = 100;
constexpr UINT kAnswerMs = 5000; // for Winamp to take the message, each time it is tried

// The folder above the one this program is in.
[[nodiscard]] std::filesystem::path WinampDir() {
    std::wstring path(32768, L'\0'); // as long as a path gets
    path.resize(::GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size())));
    return std::filesystem::path(std::move(path)).parent_path().parent_path();
}

[[nodiscard]] bool StartWinamp(std::wstring_view winampClass) {
    const std::filesystem::path dir = WinampDir();
    const std::filesystem::path program = dir / L"winamp.exe";

    std::wstring commandLine = L'"' + program.wstring() + L'"';
    if (winampClass != kWinampClass) commandLine.append(L" /CLASS=").append(winampClass);

    STARTUPINFOW startup{.cb = sizeof(startup)};
    PROCESS_INFORMATION process{};
    if (!::CreateProcessW(program.c_str(), commandLine.data(), nullptr, nullptr, FALSE, 0, nullptr, dir.c_str(),
                          &startup, &process)) {
        return false;
    }
    ::CloseHandle(process.hThread);
    ::CloseHandle(process.hProcess);
    return true;
}

// Says whether the plugin took it.
[[nodiscard]] bool SendLink(HWND winamp, std::wstring_view link) {
    // We were started by a click, which lets us bring a window forward. That is passed on.
    DWORD processId = 0;
    ::GetWindowThreadProcessId(winamp, &processId);
    ::AllowSetForegroundWindow(processId);

    COPYDATASTRUCT copied{
        .dwData = kOpenLinkCommand,
        .cbData = static_cast<DWORD>(link.size() * sizeof(wchar_t)),
        .lpData = const_cast<wchar_t*>(link.data()),
    };
    DWORD_PTR answer = 0;
    return ::SendMessageTimeoutW(winamp, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&copied), SMTO_NORMAL, kAnswerMs,
                                 &answer) != 0 &&
           static_cast<LRESULT>(answer) == kLinkTaken;
}

// Winamp's main window exists well before the plugin is ready for a link, and may be on
// its way out, so this keeps at it until the plugin takes the link.
[[nodiscard]] bool Deliver(std::wstring_view winampClass, std::wstring_view link) {
    const std::wstring className(winampClass);
    bool started = false;
    const ULONGLONG giveUp = ::GetTickCount64() + kPatienceMs;
    do {
        if (const HWND winamp = ::FindWindowW(className.c_str(), nullptr)) {
            if (SendLink(winamp, link)) return true;
        } else if (!started) {
            if (!StartWinamp(winampClass)) return false;
            started = true;
        }
        ::Sleep(kRetryMs);
    } while (::GetTickCount64() < giveUp);
    return false;
}

} // namespace

} // namespace exodium

int WINAPI wWinMain(_In_ HINSTANCE, _In_opt_ HINSTANCE, _In_ PWSTR, _In_ int) {
    using namespace exodium;

    const OpenCommand command = ParseOpenCommand(::GetCommandLineW());
    const std::wstring_view winampClass = command.winampClass.empty() ? kWinampClass : command.winampClass;

    // Links clicked together take turns, or each of them would start a Winamp. Windows
    // lets go of the mutex when this process ends.
    ::WaitForSingleObject(::CreateMutexW(nullptr, FALSE, L"Local\\ExodiumOpen"), static_cast<DWORD>(kPatienceMs));

    if (Deliver(winampClass, command.link)) return 0;

    // No link is the Start menu's entry, or Win+R.
    const std::wstring what = command.link.empty() ? std::wstring(L"Exodium could not be opened.")
                                                   : L"This link could not be opened:\n\n" + std::wstring(command.link);
    const std::wstring message = what + L"\n\nWinamp could not be started, or Exodium isn't in it. If gen_browser.dll "
                                        L"was updated while Winamp was running, restart Winamp.";
    ::MessageBoxW(nullptr, message.c_str(), L"Exodium", MB_OK | MB_ICONERROR);
    return 1;
}
