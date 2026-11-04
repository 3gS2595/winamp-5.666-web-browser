// Exodium's entry in the Start menu: a shortcut to exodium_open.exe, which opens a window
// on the home page, and starts Winamp first if it isn't running. It is ticked on and off
// on Exodium's page in Winamp's Preferences (settings_page.cpp), and
// tools\register-browser.ps1 makes the same shortcut, so whether it is there is read from
// the disk each time rather than kept anywhere.
#include "browser.h"

#include "link_ipc.h"

#include <shlobj.h>

#include <algorithm>
#include <iterator>
#include <string>
#include <string_view>
#include <system_error>

namespace exodium {

namespace {

constexpr wchar_t kShortcutName[] = L"Exodium.lnk";
// The entry's tooltip. register-browser.ps1 describes the browser to Windows the same way.
constexpr wchar_t kDescription[] = L"A tabbed web browser that lives inside Winamp";
// A folder to put the shortcut in instead of the Start menu. The smoke test names one, so
// that it can tick the checkbox without touching the Start menu of whoever runs it.
constexpr wchar_t kFolderVariable[] = L"EXODIUM_START_MENU_FOLDER";

[[nodiscard]] std::filesystem::path StartMenuFolder() {
    if (const DWORD size = ::GetEnvironmentVariableW(kFolderVariable, nullptr, 0); size > 1) {
        std::wstring folder(size, L'\0');
        folder.resize(::GetEnvironmentVariableW(kFolderVariable, folder.data(), size));
        return folder;
    }
    // The current user's Programs folder, which is all the Start menu needs. The one for
    // every user of the PC would need an administrator.
    CoTaskMemString folder;
    if (FAILED(::SHGetKnownFolderPath(FOLDERID_Programs, KF_FLAG_DEFAULT, nullptr, std::out_ptr(folder)))) return {};
    return folder.get();
}

// A Winamp started under a window class of its own, as a second copy of it is, gets an
// entry that leads to it rather than to the Winamp in everyday use.
[[nodiscard]] std::wstring ShortcutArguments() {
    wchar_t name[256];
    const int length = ::GetClassNameW(g_app.winamp, name, static_cast<int>(std::size(name)));
    const std::wstring_view winampClass(name, static_cast<std::size_t>(std::max(length, 0)));
    if (winampClass.empty() || winampClass == kWinampClass) return {};
    return std::wstring(L"--class=").append(winampClass);
}

} // namespace

std::filesystem::path StartMenuShortcut() {
    std::filesystem::path folder = StartMenuFolder();
    return folder.empty() ? folder : folder / kShortcutName;
}

bool HasStartMenuShortcut() {
    const std::filesystem::path shortcut = StartMenuShortcut();
    std::error_code error;
    return !shortcut.empty() && std::filesystem::is_regular_file(shortcut, error);
}

HRESULT AddStartMenuShortcut() {
    const std::filesystem::path shortcut = StartMenuShortcut();
    if (shortcut.empty()) return HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND);
    // An entry that leads nowhere is worse than none.
    const std::filesystem::path program = OpenerProgram();
    std::error_code error;
    if (!std::filesystem::is_regular_file(program, error)) return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
    const std::wstring arguments = ShortcutArguments();

    // It has no icon of its own, so it shows exodium_open.exe's, and follows it when the
    // program is updated. Nor does it name an AppUserModelID: the browser's windows are
    // Winamp's, and group with it on the taskbar.
    ComPtr<IShellLinkW> link;
    ComPtr<IPersistFile> file;
    HRESULT result = ::CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link));
    if (SUCCEEDED(result)) result = link->SetPath(program.c_str());
    if (SUCCEEDED(result)) result = link->SetArguments(arguments.c_str());
    if (SUCCEEDED(result)) result = link->SetWorkingDirectory(program.parent_path().c_str());
    if (SUCCEEDED(result)) result = link->SetDescription(kDescription);
    if (SUCCEEDED(result)) result = link.As(&file);
    if (FAILED(result)) return result;

    const bool existed = HasStartMenuShortcut();
    // The Start menu's folder is always there; a test's may not be.
    std::filesystem::create_directories(shortcut.parent_path(), error);
    result = file->Save(shortcut.c_str(), TRUE);
    if (FAILED(result)) return result;
    ::SHChangeNotify(existed ? SHCNE_UPDATEITEM : SHCNE_CREATE, SHCNF_PATHW, shortcut.c_str(), nullptr);
    return S_OK;
}

HRESULT RemoveStartMenuShortcut() {
    const std::filesystem::path shortcut = StartMenuShortcut();
    if (shortcut.empty()) return S_OK;
    if (!::DeleteFileW(shortcut.c_str())) {
        const DWORD error = ::GetLastError();
        return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ? S_OK : HRESULT_FROM_WIN32(error);
    }
    ::SHChangeNotify(SHCNE_DELETE, SHCNF_PATHW, shortcut.c_str(), nullptr);
    return S_OK;
}

} // namespace exodium
