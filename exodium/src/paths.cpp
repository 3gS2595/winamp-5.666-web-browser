#include "browser.h"

#include <system_error>

namespace exodium {

App g_app;

namespace {

// The folder the plugin's DLL is in.
std::filesystem::path ModuleDir() {
    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        const DWORD length = ::GetModuleFileNameW(g_app.instance, path.data(), static_cast<DWORD>(path.size()));
        if (length == 0) return {};
        if (length < path.size()) {
            path.resize(length);
            return std::filesystem::path(std::move(path)).parent_path();
        }
        path.resize(path.size() * 2); // it didn't fit
    }
}

std::filesystem::path EnsureDir(std::filesystem::path dir) {
    std::error_code error;
    std::filesystem::create_directories(dir, error);
    return dir;
}

} // namespace

const std::filesystem::path& DataDir() {
    static const std::filesystem::path dir = EnsureDir(ModuleDir() / L"exodium_data");
    return dir;
}

std::filesystem::path WebViewProfileDir() {
    return EnsureDir(DataDir() / L"webview_profile");
}

std::filesystem::path HistoryFile() {
    return DataDir() / L"history.txt";
}

std::filesystem::path ZoomFile() {
    return DataDir() / L"zoom.txt";
}

} // namespace exodium
