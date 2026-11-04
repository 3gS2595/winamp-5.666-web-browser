// Downloads. WebView2 puts up a dialog of its own for each download, and while it is open
// the browser's shortcuts stop working: it takes the window over. Here WebView2 is told
// each download has been seen to, which keeps that dialog shut, and the download is
// followed in a panel of the browser's own instead (downloads_panel.cpp), which never
// takes the keyboard.
//
// The files still go where they always went, the profile's download folder, without a
// prompt. A type of file that can harm the computer, such as a program or a script, waits
// in the panel for the user to keep it or delete it, as it waited in WebView2's dialog
// (see OnSaveFileSecurityCheck). Claude Code's tabs are left as they were: Playwright,
// which drives them over the debugging port, has its own way with downloads.
#include "browser.h"

#include <shellapi.h>
#include <shlobj.h>
#include <wrl/event.h>

#include <algorithm>
#include <filesystem>
#include <ranges>

namespace exodium {

namespace {

using Microsoft::WRL::Callback;

// How many finished downloads the list keeps, besides those still going.
constexpr std::size_t kFinishedKept = 20;
// How often the downloads still going are looked at; see Poll.
constexpr UINT kPollMs = 500;
UINT_PTR g_pollTimer = 0;

[[nodiscard]] bool IsFinished(const Download& download) noexcept {
    return download.phase == DownloadPhase::Completed || download.phase == DownloadPhase::Canceled ||
           download.phase == DownloadPhase::Failed;
}

[[nodiscard]] DownloadFailure FailureFrom(COREWEBVIEW2_DOWNLOAD_INTERRUPT_REASON reason) noexcept {
    switch (reason) {
    case COREWEBVIEW2_DOWNLOAD_INTERRUPT_REASON_FILE_ACCESS_DENIED:
        return DownloadFailure::AccessDenied;
    case COREWEBVIEW2_DOWNLOAD_INTERRUPT_REASON_FILE_NO_SPACE:
        return DownloadFailure::DiskFull;
    case COREWEBVIEW2_DOWNLOAD_INTERRUPT_REASON_FILE_NAME_TOO_LONG:
        return DownloadFailure::PathTooLong;
    case COREWEBVIEW2_DOWNLOAD_INTERRUPT_REASON_FILE_TOO_LARGE:
        return DownloadFailure::FileTooLarge;
    case COREWEBVIEW2_DOWNLOAD_INTERRUPT_REASON_FILE_MALICIOUS:
        return DownloadFailure::Virus;
    case COREWEBVIEW2_DOWNLOAD_INTERRUPT_REASON_FILE_TRANSIENT_ERROR:
        return DownloadFailure::SystemBusy;
    case COREWEBVIEW2_DOWNLOAD_INTERRUPT_REASON_FILE_BLOCKED_BY_POLICY:
        return DownloadFailure::Blocked;
    case COREWEBVIEW2_DOWNLOAD_INTERRUPT_REASON_FILE_SECURITY_CHECK_FAILED:
        return DownloadFailure::VirusScanFailed;
    case COREWEBVIEW2_DOWNLOAD_INTERRUPT_REASON_FILE_TOO_SHORT:
    case COREWEBVIEW2_DOWNLOAD_INTERRUPT_REASON_FILE_HASH_MISMATCH:
    case COREWEBVIEW2_DOWNLOAD_INTERRUPT_REASON_SERVER_CONTENT_LENGTH_MISMATCH:
        return DownloadFailure::Incomplete;
    case COREWEBVIEW2_DOWNLOAD_INTERRUPT_REASON_NETWORK_FAILED:
    case COREWEBVIEW2_DOWNLOAD_INTERRUPT_REASON_NETWORK_TIMEOUT:
    case COREWEBVIEW2_DOWNLOAD_INTERRUPT_REASON_NETWORK_DISCONNECTED:
    case COREWEBVIEW2_DOWNLOAD_INTERRUPT_REASON_NETWORK_INVALID_REQUEST:
        return DownloadFailure::Network;
    case COREWEBVIEW2_DOWNLOAD_INTERRUPT_REASON_NETWORK_SERVER_DOWN:
        return DownloadFailure::ServerUnreachable;
    case COREWEBVIEW2_DOWNLOAD_INTERRUPT_REASON_SERVER_FAILED:
    case COREWEBVIEW2_DOWNLOAD_INTERRUPT_REASON_SERVER_NO_RANGE:
    case COREWEBVIEW2_DOWNLOAD_INTERRUPT_REASON_SERVER_UNEXPECTED_RESPONSE:
    case COREWEBVIEW2_DOWNLOAD_INTERRUPT_REASON_SERVER_CROSS_ORIGIN_REDIRECT:
        return DownloadFailure::Server;
    case COREWEBVIEW2_DOWNLOAD_INTERRUPT_REASON_SERVER_BAD_CONTENT:
        return DownloadFailure::NoFile;
    case COREWEBVIEW2_DOWNLOAD_INTERRUPT_REASON_SERVER_UNAUTHORIZED:
        return DownloadFailure::NeedsAuthorization;
    case COREWEBVIEW2_DOWNLOAD_INTERRUPT_REASON_SERVER_CERTIFICATE_PROBLEM:
        return DownloadFailure::BadCertificate;
    case COREWEBVIEW2_DOWNLOAD_INTERRUPT_REASON_SERVER_FORBIDDEN:
        return DownloadFailure::Forbidden;
    case COREWEBVIEW2_DOWNLOAD_INTERRUPT_REASON_USER_SHUTDOWN:
        return DownloadFailure::Shutdown;
    case COREWEBVIEW2_DOWNLOAD_INTERRUPT_REASON_DOWNLOAD_PROCESS_CRASHED:
        return DownloadFailure::Crashed;
    default:
        return DownloadFailure::Unknown;
    }
}

[[nodiscard]] bool FileExists(const std::wstring& path) {
    const DWORD attributes = ::GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

[[nodiscard]] std::wstring FolderOf(const std::wstring& path) {
    return std::filesystem::path(path).parent_path().wstring();
}

// The icon Windows shows for files of this name's type. Found by the name alone, since the
// file may not be there yet.
[[nodiscard]] UniqueIcon FileTypeIcon(const std::wstring& name) {
    SHFILEINFOW info{};
    if (!::SHGetFileInfoW(name.c_str(), FILE_ATTRIBUTE_NORMAL, &info, sizeof(info),
                          SHGFI_ICON | SHGFI_SMALLICON | SHGFI_USEFILEATTRIBUTES)) {
        return {};
    }
    return UniqueIcon(info.hIcon);
}

// Reads what the operation says of its download now.
void Update(Download& download) {
    ICoreWebView2DownloadOperation* const operation = download.operation.Get();
    if (!operation) return;
    INT64 received = 0, total = 0;
    if (SUCCEEDED(operation->get_BytesReceived(&received))) download.received = received;
    if (SUCCEEDED(operation->get_TotalBytesToReceive(&total))) download.total = total;
    if (auto path = ComString(operation, &ICoreWebView2DownloadOperation::get_ResultFilePath); path && !path->empty()) {
        download.path = std::move(*path);
    }
    BOOL canResume = FALSE;
    if (SUCCEEDED(operation->get_CanResume(&canResume))) download.canResume = canResume != FALSE;
    download.speed.Add(::GetTickCount64(), download.received);

    COREWEBVIEW2_DOWNLOAD_STATE state = COREWEBVIEW2_DOWNLOAD_STATE_IN_PROGRESS;
    COREWEBVIEW2_DOWNLOAD_INTERRUPT_REASON reason = COREWEBVIEW2_DOWNLOAD_INTERRUPT_REASON_NONE;
    if (FAILED(operation->get_State(&state))) return;
    operation->get_InterruptReason(&reason);
    switch (state) {
    case COREWEBVIEW2_DOWNLOAD_STATE_IN_PROGRESS:
        // One just paused goes on for a moment before it stops.
        download.phase = download.paused ? DownloadPhase::Paused : DownloadPhase::InProgress;
        break;
    case COREWEBVIEW2_DOWNLOAD_STATE_COMPLETED:
        download.phase = DownloadPhase::Completed;
        download.paused = false;
        break;
    case COREWEBVIEW2_DOWNLOAD_STATE_INTERRUPTED:
        if (download.paused || reason == COREWEBVIEW2_DOWNLOAD_INTERRUPT_REASON_USER_PAUSED) {
            download.phase = DownloadPhase::Paused;
        } else if (reason == COREWEBVIEW2_DOWNLOAD_INTERRUPT_REASON_USER_CANCELED) {
            download.phase = DownloadPhase::Canceled;
        } else {
            download.phase = DownloadPhase::Failed;
            download.failure = FailureFrom(reason);
        }
        break;
    }
}

// What a COM object is known by: the same pointer whichever of its interfaces it came as.
[[nodiscard]] ComPtr<IUnknown> Identity(IUnknown* object) {
    ComPtr<IUnknown> identity;
    if (object) object->QueryInterface(IID_PPV_ARGS(&identity));
    return identity;
}

[[nodiscard]] bool HasDownloadsGoing(IUnknown* webview) {
    return webview && std::ranges::any_of(g_app.downloads, [webview](const Download& download) {
               return !IsFinished(download) && download.source.Get() == webview;
           });
}

// Closes the kept webviews whose downloads are all done. Never from inside an event of a
// webview's own.
void ReleaseKeptWebViews() {
    std::erase_if(g_app.downloadKeepers,
                  [](const Tab& kept) { return !HasDownloadsGoing(Identity(kept.webview.Get()).Get()); });
}

// Stops hearing from a download's operation, and lets go of it.
void Unfollow(Download& download) noexcept {
    if (!download.operation) return;
    download.operation->remove_BytesReceivedChanged(download.bytesToken);
    download.operation->remove_StateChanged(download.stateToken);
    download.operation.Reset();
}

// The oldest finished downloads go once there are more of them than the list keeps.
void TrimFinished() {
    std::size_t finished = 0;
    for (auto it = g_app.downloads.begin(); it != g_app.downloads.end();) {
        if (IsFinished(*it) && ++finished > kFinishedKept) {
            Unfollow(*it);
            it = g_app.downloads.erase(it);
        } else {
            ++it;
        }
    }
}

// Every handler is given the download's id and finds it afresh: it may have been cleared
// from the list in the meantime.
void OnDownloadChanged(int downloadId, DownloadsChange change) {
    Download* const download = FindDownload(downloadId);
    if (!download) return;
    Update(*download);
    RefreshDownloadsUi(change);
}

void StopPolling() noexcept {
    if (g_pollTimer) ::KillTimer(nullptr, std::exchange(g_pollTimer, 0));
}

// The downloads still going are asked how they are every so often as well, in case word
// of a change didn't come; and the webviews kept for them are let go of once they are done.
void CALLBACK Poll(HWND, UINT, UINT_PTR, DWORD) {
    bool going = false, changed = false, progressed = false;
    for (Download& download : g_app.downloads) {
        if (IsFinished(download) || !download.operation) continue;
        const DownloadPhase phase = download.phase;
        const std::int64_t received = download.received;
        Update(download);
        changed = changed || download.phase != phase;
        progressed = progressed || download.received != received;
        going = going || !IsFinished(download);
    }
    ReleaseKeptWebViews();
    if (!going) StopPolling();
    if (changed) RefreshDownloadsUi(DownloadsChange::State);
    else if (progressed) RefreshDownloadsUi(DownloadsChange::Progress);
}

void StartPolling() noexcept {
    if (!g_pollTimer) g_pollTimer = ::SetTimer(nullptr, 0, kPollMs, Poll);
}

void OnDownloadStarting(int tabId, ICoreWebView2* sender, ICoreWebView2DownloadStartingEventArgs* args) {
    const TabLocation at = FindTab(tabId);
    if (!args || (at && at.tab().forClaude)) return;
    ComPtr<ICoreWebView2DownloadOperation> operation;
    if (FAILED(args->get_DownloadOperation(&operation)) || !operation) return;

    Download added;
    added.id = g_app.nextDownloadId++;
    added.operation = operation;
    added.source = Identity(sender);
    added.path = ComString(args, &ICoreWebView2DownloadStartingEventArgs::get_ResultFilePath).value_or(std::wstring());
    added.name = FileNameOf(added.path);
    if (added.name.empty()) added.name = L"download";
    args->put_Handled(TRUE);
    added.icon = FileTypeIcon(added.name);
    const int downloadId = added.id;
    operation->add_BytesReceivedChanged(
        Callback<ICoreWebView2BytesReceivedChangedEventHandler>([downloadId](ICoreWebView2DownloadOperation*,
                                                                             IUnknown*) -> HRESULT {
            OnDownloadChanged(downloadId, DownloadsChange::Progress);
            return S_OK;
        }).Get(),
        &added.bytesToken);
    operation->add_StateChanged(
        Callback<ICoreWebView2StateChangedEventHandler>([downloadId](ICoreWebView2DownloadOperation*,
                                                                     IUnknown*) -> HRESULT {
            OnDownloadChanged(downloadId, DownloadsChange::State);
            return S_OK;
        }).Get(),
        &added.stateToken);
    Update(added);
    g_app.downloads.insert(g_app.downloads.begin(), std::move(added));
    TrimFinished();
    StartPolling();
    RefreshDownloadsUi(DownloadsChange::List);

    // The panel comes up in the window the download was started from, where the user is.
    if (at) ShowDownloadsPanel(*at.window, true);
}

// WebView2 checks every download's type just after it starts. It would hold a program, an
// installer or a script until the user kept it in its own dialog, which a download it was
// told has been seen to is left out of; with a handler of the host's it blocks it outright
// instead. So that question is asked here: such a file isn't saved until the user keeps
// it, or deletes it, in the panel. Any other type is left to WebView2's check, which lets
// it through.
void OnSaveFileSecurityCheck(int tabId, ICoreWebView2SaveFileSecurityCheckStartingEventArgs* args) {
    if (!args) return;
    const auto path = ComString(args, &ICoreWebView2SaveFileSecurityCheckStartingEventArgs::get_FilePath);
    if (!path || !CanHarmComputer(FileNameOf(*path))) return;
    // The newest download going to that file. Any other save is left to WebView2.
    const auto found = std::ranges::find_if(g_app.downloads, [&path](const Download& download) {
        return download.path == *path && !IsFinished(download) && !download.keepDeferral;
    });
    if (found == g_app.downloads.end()) return;
    ComPtr<ICoreWebView2Deferral> deferral;
    if (FAILED(args->GetDeferral(&deferral)) || !deferral) return;
    found->keepCheck = args;
    found->keepDeferral = std::move(deferral);
    RefreshDownloadsUi(DownloadsChange::State);
    if (const TabLocation at = FindTab(tabId)) ShowDownloadsPanel(*at.window, true);
}

// Lets a held download go on, whichever way the user answered.
void Settle(Download& download) {
    const ComPtr<ICoreWebView2Deferral> deferral = std::move(download.keepDeferral);
    download.keepCheck.Reset();
    if (deferral) deferral->Complete();
}

} // namespace

void WireTabDownloads(int tabId) {
    const TabLocation at = FindTab(tabId);
    if (!at || !at.tab().webview || at.tab().forClaude) return;
    // Without the security check to answer, the downloads are left to WebView2 altogether:
    // with a download handler of any kind, it blocks a file that can harm the computer
    // rather than ask whether to keep it, and the question couldn't be asked here.
    ComPtr<ICoreWebView2_4> webview4;
    ComPtr<ICoreWebView2_26> webview26;
    if (FAILED(at.tab().webview.As(&webview4)) || FAILED(at.tab().webview.As(&webview26))) return;
    // The handlers live as long as the webview does, like the tab's others.
    webview4->add_DownloadStarting(
        Callback<ICoreWebView2DownloadStartingEventHandler>([tabId](ICoreWebView2* sender,
                                                                    ICoreWebView2DownloadStartingEventArgs* args)
                                                                -> HRESULT {
            OnDownloadStarting(tabId, sender, args);
            return S_OK;
        }).Get(),
        nullptr);
    webview26->add_SaveFileSecurityCheckStarting(
        Callback<ICoreWebView2SaveFileSecurityCheckStartingEventHandler>(
            [tabId](ICoreWebView2*, ICoreWebView2SaveFileSecurityCheckStartingEventArgs* args) -> HRESULT {
                OnSaveFileSecurityCheck(tabId, args);
                return S_OK;
            })
            .Get(),
        nullptr);
}

// Kept in the first window, which is there until Winamp exits, as a closed tab's webview
// is kept in its own. It is out of sight and quiet, which its page is told.
void KeepWebViewForDownloads(Tab& tab) {
    if (g_app.shutDown || g_app.windows.empty() || !tab.controller || !tab.webview) return;
    if (!HasDownloadsGoing(Identity(tab.webview.Get()).Get())) return;
    const HWND holder = g_app.windows.front()->client;
    if (!holder) return;
    Tab& kept = g_app.downloadKeepers.emplace_back();
    kept.controller = std::move(tab.controller);
    kept.webview = std::move(tab.webview);
    tab.playingAudio = false;
    kept.controller->put_IsVisible(FALSE);
    kept.controller->put_ParentWindow(holder);
    SetTabMuted(kept, true);
}

Download* FindDownload(int downloadId) noexcept {
    const auto found = std::ranges::find(g_app.downloads, downloadId, &Download::id);
    return found == g_app.downloads.end() ? nullptr : &*found;
}

DownloadFacts FactsOf(const Download& download) {
    DownloadFacts facts{.phase = download.phase,
                        .failure = download.failure,
                        .received = download.received,
                        .total = download.total,
                        .fileGone = download.fileGone,
                        .held = download.keepDeferral != nullptr};
    if (download.phase == DownloadPhase::InProgress) {
        facts.bytesPerSecond = download.speed.BytesPerSecond(::GetTickCount64());
    }
    return facts;
}

void CheckDownloadedFiles() {
    for (Download& download : g_app.downloads) {
        if (download.phase == DownloadPhase::Completed) download.fileGone = !FileExists(download.path);
    }
}

void OpenDownload(int downloadId) {
    const Download* const download = FindDownload(downloadId);
    if (!download || download->phase != DownloadPhase::Completed || !FileExists(download->path)) return;
    // The click was the user's, on a window of ours, which lets the program that opens the
    // file come to the front.
    ::AllowSetForegroundWindow(ASFW_ANY);
    ::ShellExecuteW(nullptr, nullptr, download->path.c_str(), nullptr, FolderOf(download->path).c_str(), SW_SHOWNORMAL);
}

void ShowDownloadInFolder(int downloadId) {
    const Download* const download = FindDownload(downloadId);
    if (!download) return;
    ::AllowSetForegroundWindow(ASFW_ANY);
    if (FileExists(download->path)) {
        if (const PIDLIST_ABSOLUTE item = ::ILCreateFromPathW(download->path.c_str())) {
            const HRESULT shown = ::SHOpenFolderAndSelectItems(item, 0, nullptr, 0);
            ::ILFree(item);
            if (SUCCEEDED(shown)) return;
        }
    }
    // Gone, or not to be picked out: its folder, at least.
    const std::wstring folder = FolderOf(download->path);
    if (!folder.empty()) ::ShellExecuteW(nullptr, L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

// The folder the latest download went to, which is where they all go.
void OpenDownloadsFolder() {
    if (g_app.downloads.empty()) return;
    const std::wstring folder = FolderOf(g_app.downloads.front().path);
    if (folder.empty()) return;
    ::AllowSetForegroundWindow(ASFW_ANY);
    ::ShellExecuteW(nullptr, L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

// The panel shows a pause or a resume at once; the operation's own word that it has
// stopped or started again follows.
void PauseDownload(int downloadId) {
    Download* const download = FindDownload(downloadId);
    if (!download || !download->operation || download->phase != DownloadPhase::InProgress) return;
    download->paused = true;
    if (FAILED(download->operation->Pause())) download->paused = false;
    if (download->paused) download->phase = DownloadPhase::Paused;
    RefreshDownloadsUi(DownloadsChange::State);
}

void ResumeDownload(int downloadId) {
    Download* const download = FindDownload(downloadId);
    if (!download || !download->operation) return;
    if (download->phase != DownloadPhase::Paused &&
        !(download->phase == DownloadPhase::Failed && download->canResume)) {
        return;
    }
    if (FAILED(download->operation->Resume())) return;
    download->paused = false;
    download->speed = {}; // the time it stood still isn't to slow its speed down
    download->phase = DownloadPhase::InProgress;
    StartPolling();
    RefreshDownloadsUi(DownloadsChange::State);
}

void CancelDownload(int downloadId) {
    Download* const download = FindDownload(downloadId);
    if (!download || !download->operation || IsFinished(*download)) return;
    download->paused = false;
    download->operation->Cancel();
    RefreshDownloadsUi(DownloadsChange::State);
}

void KeepDownload(int downloadId) {
    Download* const download = FindDownload(downloadId);
    if (!download || !download->keepCheck) return;
    download->keepCheck->put_SuppressDefaultPolicy(TRUE); // saved, with nothing more asked
    Settle(*download);
    RefreshDownloadsUi(DownloadsChange::State);
}

void DeleteDownload(int downloadId) {
    Download* const download = FindDownload(downloadId);
    if (!download || !download->keepCheck) return;
    download->keepCheck->put_CancelSave(TRUE);
    Settle(*download);
    RefreshDownloadsUi(DownloadsChange::State);
}

void ClearFinishedDownloads() {
    for (Download& download : g_app.downloads) {
        if (IsFinished(download)) Unfollow(download);
    }
    std::erase_if(g_app.downloads, IsFinished);
    ReleaseKeptWebViews();
    RefreshDownloadsUi(DownloadsChange::List);
}

void AbandonDownloads() {
    StopPolling();
    bool stopped = false;
    for (Download& download : g_app.downloads) {
        if (!IsFinished(download)) {
            download.phase = DownloadPhase::Failed;
            download.failure = DownloadFailure::Crashed;
            download.paused = download.canResume = false;
            stopped = true;
        }
        // Its operation went with the process, and any question it was held on with it.
        download.keepCheck.Reset();
        download.keepDeferral.Reset();
        Unfollow(download);
    }
    g_app.downloadKeepers.clear(); // as did their webviews
    if (stopped) RefreshDownloadsUi(DownloadsChange::State);
}

void StopFollowingDownloads() {
    StopPolling();
    for (Download& download : g_app.downloads) {
        // One still held isn't kept: the user didn't say to, and WebView2 isn't to be left
        // waiting for an answer as it shuts down.
        if (download.keepCheck) {
            download.keepCheck->put_CancelSave(TRUE);
            Settle(download);
        }
        Unfollow(download);
    }
    g_app.downloads.clear();
    g_app.downloadKeepers.clear();
}

} // namespace exodium
