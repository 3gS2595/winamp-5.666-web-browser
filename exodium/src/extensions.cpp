// The browser extensions in the user's profile: Bitwarden, fetched from the Chrome Web Store
// and kept up to date from there, and a little one of Exodium's own that tells Bitwarden's
// popup which tab it is for (bitwarden_popup.cpp). Claude's tabs have a profile of their own
// with no extensions in it (webview.cpp), so neither ever reaches a page of Claude's.
//
// WebView2 keeps the extensions a profile has been given, and loads each again from its
// folder whenever the browser process starts. Bitwarden's folder is always the same one,
// Plugins\exodium_data\extensions\bitwarden, so its data (the account, the vault) stays with
// it from one version to the next. A newer version is fetched and checked in the background
// and set aside beside it, and put in its place the next time Winamp starts, before the
// browser process does: its files can't be changed under an extension that is running.
// Only the very first version goes in at once, as nothing is using its folder yet.
//
// The store's packages are signed, and nothing is unpacked unless the signature made with
// Bitwarden's own key checks out (extension_package.h).
#include "browser.h"

#include "extension_package.h"
#include "gdiplus_headers.h"
#include "text_util.h"

#include <winhttp.h>

#include <atomic>
#include <fstream>
#include <mutex>
#include <sstream>
#include <system_error>

namespace exodium {

namespace {

using Microsoft::WRL::Callback;

// How long after start-up the store is first asked, so as not to slow Winamp's start, and
// how often after that. Without Bitwarden yet, it is asked at once.
constexpr DWORD kFirstLookMs = 60 * 1000;
constexpr DWORD kFirstInstallLookMs = 3 * 1000;
constexpr DWORD kLookEveryMs = 24 * 60 * 60 * 1000;
// A failed look is tried again sooner than a day.
constexpr DWORD kRetryMs = 60 * 60 * 1000;
constexpr std::size_t kMostAnswerBytes = 1 << 20;
constexpr std::size_t kMostPackageBytes = 256u << 20;
constexpr int kDownloadTries = 3;

constexpr wchar_t kBitwardenFolder[] = L"bitwarden";
constexpr wchar_t kStagedFolder[] = L"bitwarden.new";       // the next version, until Winamp starts again
constexpr wchar_t kUnpackingFolder[] = L"bitwarden.unpacking"; // the next version, while it is unpacked
constexpr wchar_t kOldFolder[] = L"bitwarden.old";          // the last version, while the next takes its place
constexpr wchar_t kTabHelperFolder[] = L"exodium_tab_helper";

// ---- Exodium's own extension ----

// Its key gives it the same id on every PC: kTabHelperId. Nothing is signed with it, so
// there is no private half to keep.
constexpr std::string_view kTabHelperManifest = R"({
   "manifest_version": 3,
   "name": "Exodium tab helper",
   "description": "Tells Bitwarden's popup in Exodium which tab it is for.",
   "version": "1.0",
   "permissions": [ "debugger" ],
   "key": "MIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIBCgKCAQEAqByt52TM09PhFLYEoTtPPzRfXyeROFIUVVOuXDNh39Z+dqpkNv+q+bfmrYRpUrNEBGmcgyWJbiJ5UcK9zQvyMhNzNLVh1u9eK6GeUv6QD9UsXOqsOHochyMmyf+vX77mKyz0oHqdkyMdmcCqD8Soih42GjOYcjZNb01MHgUt+K9Y13ZwGAX/NlAl022vTxPYBn9iV5pcFldWrTFwH9PJ4Em47KCvm+H5qftVPiRyS1UMr1O44hzxNac2yEH3Fo9tM4GY01hP5v9GvhCyQou6h7CEidaZjoNnNKFUoo9Svq6pqornxMNVi5hpnheexQeP6qKMTQQVk3Eq2jpIisI+twIDAQAB"
}
)";
// Opened in Bitwarden's popup first, with the DevTools target id of the tab after the '#'.
// The debugger permission is for getTargets alone, which attaches to nothing: it is the one
// thing that gives the id an extension's tabs API knows a tab by from the id DevTools does.
constexpr std::string_view kTabHelperPage =
    "<!doctype html><meta charset=\"utf-8\"><title>Bitwarden</title>"
    "<body style=\"background:#101010\"></body><script src=\"tab.js\"></script>\n";
constexpr std::string_view kTabHelperScript = R"(// Says which of the browser's tabs Exodium's tab is: the one with the DevTools target id
// after the '#'.
const target = decodeURIComponent(location.hash.slice(1));
chrome.debugger.getTargets(targets => {
  const tab = targets.find(candidate => candidate.id === target);
  chrome.webview.postMessage({ tabId: tab && typeof tab.tabId === "number" ? tab.tabId : null });
});
)";

[[nodiscard]] std::filesystem::path ExtensionsDir() {
    std::error_code ignored;
    const std::filesystem::path dir = DataDir() / L"extensions";
    std::filesystem::create_directories(dir, ignored);
    return dir;
}

[[nodiscard]] std::string ReadSmallFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

[[nodiscard]] std::optional<std::string> VersionIn(const std::filesystem::path& folder) {
    return ManifestVersion(ReadSmallFile(folder / L"manifest.json"));
}

// Writes the helper's files where they differ from what is there, as after an update of the
// plugin. WebView2 reads them again as the browser process starts.
void WriteTabHelper() {
    const std::filesystem::path folder = ExtensionsDir() / kTabHelperFolder;
    std::error_code ignored;
    std::filesystem::create_directories(folder, ignored);
    for (const auto& [name, contents] : {std::pair{L"manifest.json", kTabHelperManifest},
                                         std::pair{L"tab.html", kTabHelperPage},
                                         std::pair{L"tab.js", kTabHelperScript}}) {
        const std::filesystem::path path = folder / name;
        if (ReadSmallFile(path) == contents) continue;
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    }
}

// The version fetched last time takes the place of the one in use. Run before the browser
// process starts, which would be using the folder.
void PutStagedVersionInPlace() {
    const std::filesystem::path dir = ExtensionsDir();
    const std::filesystem::path current = dir / kBitwardenFolder, staged = dir / kStagedFolder, old = dir / kOldFolder;
    std::error_code error;
    std::filesystem::remove_all(old, error);
    std::filesystem::remove_all(dir / kUnpackingFolder, error); // left half done when Winamp last went
    if (!VersionIn(staged)) return;
    error.clear();
    if (std::filesystem::exists(current)) std::filesystem::rename(current, old, error);
    if (error) return; // something has it open; next time
    std::filesystem::rename(staged, current, error);
    if (error) {
        std::error_code ignored;
        std::filesystem::rename(old, current, ignored);
        return;
    }
    std::filesystem::remove_all(old, error);
}

// ---- fetching, on a thread of its own ----

// The message the thread posts to Winamp's main window once the first version is in place.
UINT InstalledMessage() {
    static const UINT message = ::RegisterWindowMessageW(L"Exodium.ExtensionInstalled");
    return message;
}

struct Fetcher {
    HANDLE thread = nullptr;
    HANDLE stop = nullptr;           // set as Winamp exits
    std::atomic<bool> wanted{false}; // Bitwarden is on in the settings
    std::mutex mutex;                // for request, which the UI thread closes to cut a download short
    HINTERNET request = nullptr;
    std::filesystem::path dir; // ExtensionsDir(), worked out on the UI thread
    HWND winamp = nullptr;
};
Fetcher g_fetcher;
bool g_started = false; // StartExtensions has run

struct InternetHandle {
    HINTERNET handle = nullptr;
    ~InternetHandle() {
        if (handle) ::WinHttpCloseHandle(handle);
    }
};

// What an https address answers, if it answers 200 with no more than `most` bytes.
[[nodiscard]] std::optional<Bytes> HttpsGet(const std::wstring& url, std::size_t most) {
    URL_COMPONENTS parts{.dwStructSize = sizeof(parts)};
    parts.dwHostNameLength = parts.dwUrlPathLength = parts.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!::WinHttpCrackUrl(url.c_str(), 0, 0, &parts) || parts.nScheme != INTERNET_SCHEME_HTTPS) return std::nullopt;
    const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    const std::wstring path = std::wstring(parts.lpszUrlPath, parts.dwUrlPathLength) +
                              std::wstring(parts.lpszExtraInfo ? parts.lpszExtraInfo : L"", parts.dwExtraInfoLength);

    InternetHandle session{::WinHttpOpen(L"Exodium", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                         WINHTTP_NO_PROXY_BYPASS, 0)};
    if (!session.handle) return std::nullopt;
    ::WinHttpSetTimeouts(session.handle, 15000, 15000, 30000, 60000);
    InternetHandle connection{::WinHttpConnect(session.handle, host.c_str(), parts.nPort, 0)};
    if (!connection.handle) return std::nullopt;
    InternetHandle request{::WinHttpOpenRequest(connection.handle, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                                WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)};
    if (!request.handle) return std::nullopt;
    {
        const std::scoped_lock lock(g_fetcher.mutex);
        if (::WaitForSingleObject(g_fetcher.stop, 0) == WAIT_OBJECT_0) return std::nullopt;
        g_fetcher.request = request.handle;
    }
    std::optional<Bytes> body;
    DWORD status = 0, size = sizeof(status);
    if (::WinHttpSendRequest(request.handle, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        ::WinHttpReceiveResponse(request.handle, nullptr) &&
        ::WinHttpQueryHeaders(request.handle, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                              WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX) &&
        status == 200) {
        Bytes bytes;
        std::uint8_t buffer[64 * 1024];
        for (;;) {
            DWORD read = 0;
            if (!::WinHttpReadData(request.handle, buffer, sizeof(buffer), &read)) break;
            if (read == 0) {
                body = std::move(bytes);
                break;
            }
            if (bytes.size() + read > most) break;
            bytes.insert(bytes.end(), buffer, buffer + read);
        }
    }
    const std::scoped_lock lock(g_fetcher.mutex);
    // StopExtensions closed it already, to cut it short.
    if (g_fetcher.request != request.handle) request.handle = nullptr;
    g_fetcher.request = nullptr;
    return body;
}

[[nodiscard]] bool Stopping() noexcept {
    return ::WaitForSingleObject(g_fetcher.stop, 0) == WAIT_OBJECT_0;
}

enum class Look { Done, Failed };

// Asks the store for Bitwarden's latest version, and fetches and unpacks it if it is newer
// than the one in use and the one waiting.
Look LookForNewVersion() {
    const std::filesystem::path& dir = g_fetcher.dir;
    const std::optional<std::string> current = VersionIn(dir / kBitwardenFolder);
    const std::optional<std::string> staged = VersionIn(dir / kStagedFolder);

    CoTaskMemString browserVersion;
    {
        LPWSTR version = nullptr;
        if (SUCCEEDED(::GetAvailableCoreWebView2BrowserVersionString(nullptr, &version))) browserVersion.reset(version);
    }
    const auto answer = HttpsGet(StoreUpdateCheckUrl(kBitwardenId, browserVersion ? browserVersion.get() : L"0"),
                                 kMostAnswerBytes);
    if (!answer) return Look::Failed;
    const auto release =
        ParseStoreUpdateCheck(std::string_view(reinterpret_cast<const char*>(answer->data()), answer->size()), kBitwardenId);
    if (!release) return Look::Failed;
    const std::string& have = staged && (!current || CompareVersions(*staged, *current) > 0) ? *staged : current.value_or("");
    if (!have.empty() && CompareVersions(release->version, have) <= 0) return Look::Done;

    std::optional<Bytes> package;
    for (int attempt = 0; attempt < kDownloadTries && !package && !Stopping(); ++attempt) {
        package = HttpsGet(FromUtf8(release->codebase), kMostPackageBytes);
    }
    if (!package) return Look::Failed;
    if (!release->sha256Hex.empty()) {
        const auto said = HexDecode(release->sha256Hex);
        const auto hash = Sha256({ByteSpan(*package)});
        if (!said || !hash || !std::equal(said->begin(), said->end(), hash->begin(), hash->end())) return Look::Failed;
    }

    const std::filesystem::path unpacking = dir / kUnpackingFolder;
    std::error_code error;
    std::filesystem::remove_all(unpacking, error);
    if (!UnpackStoreExtension(*package, kBitwardenId, unpacking)) return Look::Failed;
    if (Stopping()) return Look::Done; // put in place on the next look, or taken apart then

    if (!current) {
        // The first: nothing is using its folder yet, so it goes in at once.
        std::filesystem::rename(unpacking, dir / kBitwardenFolder, error);
        if (error) return Look::Failed;
        ::PostMessageW(g_fetcher.winamp, InstalledMessage(), 0, 0);
        return Look::Done;
    }
    std::filesystem::remove_all(dir / kStagedFolder, error);
    std::filesystem::rename(unpacking, dir / kStagedFolder, error);
    return error ? Look::Failed : Look::Done;
}

// Holds a reference to the plugin's DLL for as long as it runs, so that Winamp can't unload
// it from under the thread if it is still busy as Winamp exits.
DWORD WINAPI FetchThread(void* module) {
    const bool installed = VersionIn(g_fetcher.dir / kBitwardenFolder).has_value();
    DWORD wait = installed ? kFirstLookMs : kFirstInstallLookMs;
    while (::WaitForSingleObject(g_fetcher.stop, wait) == WAIT_TIMEOUT) {
        if (!g_fetcher.wanted) {
            wait = kRetryMs;
            continue;
        }
        wait = LookForNewVersion() == Look::Done ? kLookEveryMs : kRetryMs;
    }
    ::FreeLibraryAndExitThread(static_cast<HMODULE>(module), 0);
}

void StartFetching() {
    g_fetcher.wanted = true;
    if (g_fetcher.thread) return;
    g_fetcher.dir = ExtensionsDir();
    g_fetcher.winamp = g_app.winamp;
    if (!g_fetcher.stop) g_fetcher.stop = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HMODULE module = nullptr;
    if (!g_fetcher.stop ||
        !::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(&FetchThread), &module)) {
        return;
    }
    g_fetcher.thread = ::CreateThread(nullptr, 0, FetchThread, module, 0, nullptr);
    if (!g_fetcher.thread) ::FreeLibrary(module);
}

// ---- keeping the browser process up ----
//
// Bitwarden keeps an unlocked vault's key in the browser process's memory, and nowhere
// else unless told to: its lock is "on browser restart" to begin with. So the vault locks
// whenever that process ends, and WebView2 ends it as soon as the last webview goes, which
// is when every window of Exodium's has been closed, while Winamp goes on. Then the next
// login asked for brings Bitwarden's "Unlock your account" up instead of the logins.
//
// So while Bitwarden is on, a webview of the user's profile that is never shown keeps the
// process up until Winamp exits, as Chrome stays up while a window of its is open. It shows
// nothing (it stays on about:blank), is asked to use as little memory as it can, and is no
// tab: only the debugging port's tools see it, and they are shown their own tabs alone
// (exodium_mcp.mjs).

struct Keeper {
    UniqueWindow window; // hidden, its parent
    WebViewController controller;
    bool pending = false;
    int generation = 0; // goes up whenever it is let go, so that a late answer is let be
};
Keeper g_keeper;

void LetBrowserProcessGo() {
    ++g_keeper.generation;
    g_keeper.pending = false;
    g_keeper.controller.Close(); // before its parent, which would take it along
    g_keeper.window.reset();
}

void KeepBrowserProcessUp() {
    if (!g_app.settings.bitwarden || !g_app.extensions.profile || !g_app.environment || g_app.shutDown ||
        g_keeper.controller || g_keeper.pending) {
        return;
    }
    if (!g_keeper.window) {
        g_keeper.window.reset(::CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"STATIC", L"Exodium", WS_POPUP, 0,
                                                0, 1, 1, nullptr, nullptr, g_app.instance, nullptr));
        if (!g_keeper.window) return;
    }
    const int generation = ++g_keeper.generation;
    g_keeper.pending = true;
    const HRESULT started = g_app.environment->CreateCoreWebView2Controller(
        g_keeper.window.get(),
        Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
            [generation](HRESULT result, ICoreWebView2Controller* controller) -> HRESULT {
                if (generation != g_keeper.generation) {
                    if (controller) controller->Close();
                    return S_OK;
                }
                g_keeper.pending = false;
                if (FAILED(result) || !controller) {
                    LogFailure(L"Keeping the browser process up for Bitwarden", FAILED(result) ? result : E_FAIL);
                    return S_OK;
                }
                controller->put_IsVisible(FALSE);
                ComPtr<ICoreWebView2> webview;
                ComPtr<ICoreWebView2_19> webview19;
                if (SUCCEEDED(controller->get_CoreWebView2(&webview)) && webview && SUCCEEDED(webview.As(&webview19))) {
                    webview19->put_MemoryUsageTargetLevel(COREWEBVIEW2_MEMORY_USAGE_TARGET_LEVEL_LOW);
                }
                g_keeper.controller = WebViewController(controller);
                return S_OK;
            })
            .Get());
    if (FAILED(started)) {
        g_keeper.pending = false;
        LogFailure(L"Keeping the browser process up for Bitwarden", started);
    }
}

// ---- the extensions in the profile ----

[[nodiscard]] ComPtr<ICoreWebView2Profile7> ProfileOf(ICoreWebView2* webview) {
    ComPtr<ICoreWebView2_13> webview13;
    ComPtr<ICoreWebView2Profile> profile;
    ComPtr<ICoreWebView2Profile7> profile7;
    if (webview && SUCCEEDED(webview->QueryInterface(IID_PPV_ARGS(&webview13))) &&
        SUCCEEDED(webview13->get_Profile(&profile)) && profile) {
        profile.As(&profile7);
    }
    return profile7;
}

// Bitwarden's picture, at the size of the toolbar button's, from its folder. Read into
// memory first, so that the file isn't held open.
[[nodiscard]] UniqueIcon LoadBitwardenIcon() {
    const std::filesystem::path images = ExtensionsDir() / kBitwardenFolder / L"images";
    std::string png = ReadSmallFile(images / L"icon38.png");
    if (png.empty()) png = ReadSmallFile(images / L"icon19.png");
    if (png.empty()) return {};
    ComPtr<IStream> stream;
    stream.Attach(::SHCreateMemStream(reinterpret_cast<const BYTE*>(png.data()), static_cast<UINT>(png.size())));
    if (!stream) return {};
    Gdiplus::Bitmap source(stream.Get());
    if (source.GetLastStatus() != Gdiplus::Ok) return {};
    Gdiplus::Bitmap sized(kBitwardenIconSize, kBitwardenIconSize, PixelFormat32bppARGB);
    {
        Gdiplus::Graphics graphics(&sized);
        graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
        graphics.DrawImage(&source, 0, 0, kBitwardenIconSize, kBitwardenIconSize);
    }
    HICON icon = nullptr;
    if (sized.GetHICON(&icon) != Gdiplus::Ok) return {};
    return UniqueIcon(icon);
}

void SetEnabled(ICoreWebView2BrowserExtension* extension, bool enabled) {
    BOOL now = FALSE;
    if (!extension || FAILED(extension->get_IsEnabled(&now)) || (now != FALSE) == enabled) return;
    extension->Enable(enabled ? TRUE : FALSE,
                      Callback<ICoreWebView2BrowserExtensionEnableCompletedHandler>([](HRESULT result) -> HRESULT {
                          if (FAILED(result)) LogFailure(L"Turning an extension on or off", result);
                          RefreshBitwarden();
                          return S_OK;
                      }).Get());
}

void AddExtension(const std::filesystem::path& folder, bool isBitwarden) {
    ICoreWebView2Profile7* const profile = g_app.extensions.profile.Get();
    if (!profile) return;
    profile->AddBrowserExtension(
        folder.c_str(), Callback<ICoreWebView2ProfileAddBrowserExtensionCompletedHandler>(
                            [isBitwarden](HRESULT result, ICoreWebView2BrowserExtension* extension) -> HRESULT {
                                if (FAILED(result) || !extension) {
                                    LogFailure(L"Adding an extension", FAILED(result) ? result : E_FAIL);
                                    return S_OK;
                                }
                                if (!g_app.extensions.profile) return S_OK; // the browser process went meanwhile
                                if (isBitwarden) g_app.extensions.bitwarden = extension;
                                else g_app.extensions.tabHelper = extension;
                                RefreshBitwarden();
                                return S_OK;
                            })
                            .Get());
}

// Gives the profile the extensions the settings ask for, and turns them on or off. `fresh`:
// Bitwarden's folder has just been filled, so it is added even if the profile says it has
// it, as it may be one whose folder had gone.
void SortOutProfile(bool fresh = false) {
    ICoreWebView2Profile7* const profile = g_app.extensions.profile.Get();
    if (!profile) return;
    profile->GetBrowserExtensions(Callback<ICoreWebView2ProfileGetBrowserExtensionsCompletedHandler>(
                                      [fresh](HRESULT result, ICoreWebView2BrowserExtensionList* list) -> HRESULT {
                                          Extensions& extensions = g_app.extensions;
                                          if (FAILED(result) || !list || !extensions.profile) return S_OK;
                                          ComPtr<ICoreWebView2BrowserExtension> bitwarden, tabHelper;
                                          UINT count = 0;
                                          list->get_Count(&count);
                                          for (UINT i = 0; i < count; ++i) {
                                              ComPtr<ICoreWebView2BrowserExtension> extension;
                                              if (FAILED(list->GetValueAtIndex(i, &extension)) || !extension) continue;
                                              const auto id = ComString(extension.Get(), &ICoreWebView2BrowserExtension::get_Id);
                                              if (id == FromUtf8(kBitwardenId)) bitwarden = extension;
                                              if (id == FromUtf8(kTabHelperId)) tabHelper = extension;
                                          }
                                          extensions.bitwarden = bitwarden;
                                          extensions.tabHelper = tabHelper;

                                          const bool wanted = g_app.settings.bitwarden;
                                          const std::filesystem::path dir = ExtensionsDir();
                                          if (wanted && (fresh || !bitwarden) && VersionIn(dir / kBitwardenFolder)) {
                                              AddExtension(dir / kBitwardenFolder, true);
                                          }
                                          if (wanted && !tabHelper) AddExtension(dir / kTabHelperFolder, false);
                                          SetEnabled(bitwarden.Get(), wanted);
                                          SetEnabled(tabHelper.Get(), wanted);
                                          RefreshBitwarden();
                                          return S_OK;
                                      })
                                      .Get());
}

} // namespace

void StartExtensions() {
    PutStagedVersionInPlace();
    WriteTabHelper();
    g_started = true;
    if (g_app.settings.bitwarden) StartFetching();
}

void StopExtensions() {
    CloseBitwardenPopup();
    LetBrowserProcessGo();
    g_fetcher.wanted = false;
    if (g_fetcher.thread) {
        {
            const std::scoped_lock lock(g_fetcher.mutex);
            ::SetEvent(g_fetcher.stop);
            // Cuts a download short: the thread's call fails at once.
            if (g_fetcher.request) ::WinHttpCloseHandle(std::exchange(g_fetcher.request, nullptr));
        }
        // It holds the DLL until it is done, so it needn't be waited for long.
        ::WaitForSingleObject(g_fetcher.thread, 2000);
        ::CloseHandle(std::exchange(g_fetcher.thread, nullptr));
    }
    g_app.extensions = {};
}

void LoadExtensions(ICoreWebView2* webview) {
    if (g_app.extensions.profile || g_app.shutDown) return; // looked at already, in this browser process
    g_app.extensions.profile = ProfileOf(webview);
    SortOutProfile();
    KeepBrowserProcessUp();
}

void ForgetExtensions() {
    CloseBitwardenPopup();
    LetBrowserProcessGo(); // its webview went with the process
    g_app.extensions.profile.Reset();
    g_app.extensions.bitwarden.Reset();
    g_app.extensions.tabHelper.Reset();
    RefreshBitwarden();
}

void FollowExtensionSettings() {
    // Every change of setting comes here, a glow slider's each step of the way.
    static std::optional<bool> followed;
    if (followed == g_app.settings.bitwarden) return;
    followed = g_app.settings.bitwarden;
    // Not before StartExtensions: the settings are read, and followed, before it runs.
    if (g_app.settings.bitwarden && g_started) StartFetching();
    else g_fetcher.wanted = false;
    if (g_app.settings.bitwarden) KeepBrowserProcessUp();
    else LetBrowserProcessGo();
    SortOutProfile();
    RefreshBitwarden(); // which closes the popup if Bitwarden is off
}

bool OnExtensionsMessage(UINT msg) {
    if (msg == 0 || msg != InstalledMessage()) return false;
    if (!g_app.shutDown) SortOutProfile(true);
    return true;
}

bool BitwardenReady() noexcept {
    const Extensions& extensions = g_app.extensions;
    BOOL enabled = FALSE;
    return g_app.settings.bitwarden && extensions.bitwarden &&
           SUCCEEDED(extensions.bitwarden->get_IsEnabled(&enabled)) && enabled;
}

bool TabHelperReady() noexcept {
    const Extensions& extensions = g_app.extensions;
    BOOL enabled = FALSE;
    return extensions.tabHelper && SUCCEEDED(extensions.tabHelper->get_IsEnabled(&enabled)) && enabled;
}

void RefreshBitwarden() {
    if (BitwardenReady() && !g_app.extensions.bitwardenIcon) g_app.extensions.bitwardenIcon = LoadBitwardenIcon();
    if (!BitwardenReady()) CloseBitwardenPopup();
    RefreshBitwardenButtons();
}

std::wstring BitwardenPopupUrl(std::optional<int> tabId) {
    // uilocation=popup says it is the toolbar's popup, which Bitwarden closes after filling.
    std::wstring url = L"chrome-extension://" + FromUtf8(kBitwardenId) + L"/popup/index.html?uilocation=popup";
    if (tabId) url += L"#/tabs/vault?senderTabId=" + std::to_wstring(*tabId);
    return url;
}

bool IsBitwardenUnlockPopout(std::wstring_view url) {
    // As Bitwarden's own code names it: popup/index.html?uilocation=popout&singleActionPopout=auth_unlockExtension
    const std::wstring page = L"chrome-extension://" + FromUtf8(kBitwardenId) + L"/popup/index.html?";
    const std::wstring_view query = url.substr(0, url.find(L'#'));
    return query.starts_with(page) && query.contains(L"uilocation=popout") &&
           query.contains(L"singleActionPopout=auth_unlockExtension");
}

std::wstring TabHelperUrl(std::wstring_view targetId) {
    return L"chrome-extension://" + FromUtf8(kTabHelperId) + L"/tab.html#" + std::wstring(targetId);
}

bool IsTabHelperPage(std::wstring_view url) {
    const std::wstring prefix = L"chrome-extension://" + FromUtf8(kTabHelperId) + L"/";
    return url.starts_with(prefix);
}

} // namespace exodium
