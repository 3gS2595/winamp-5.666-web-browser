#include "capture.h"

#include <audioclient.h>
#include <audioclientactivationparams.h>
#include <mmdeviceapi.h>
#include <tlhelp32.h>
#include <wrl/client.h>
#include <wrl/implements.h>

#include <utility>

namespace exodium {

using Microsoft::WRL::ComPtr;

namespace {

// How much sound Windows holds for the capture between wake-ups.
constexpr REFERENCE_TIME kBufferTicks = 200'000; // 20 ms
// How long to wait before trying again when Windows won't record, or stops recording,
// such as when the speakers in use change.
constexpr DWORD kRetryMs = 1000;

// Hears back from ActivateAudioInterfaceAsync, on a thread of Windows'.
class ActivationWaiter
    : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
                                          Microsoft::WRL::FtmBase, IActivateAudioInterfaceCompletionHandler> {
public:
    ActivationWaiter() : done_(::CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}
    ~ActivationWaiter() override {
        if (done_) ::CloseHandle(done_);
    }

    STDMETHOD(ActivateCompleted)(IActivateAudioInterfaceAsyncOperation* operation) override {
        HRESULT activated = E_FAIL;
        ComPtr<IUnknown> client;
        if (SUCCEEDED(operation->GetActivateResult(&activated, &client)) && SUCCEEDED(activated)) {
            client.As(&client_);
        }
        ::SetEvent(done_);
        return S_OK;
    }

    [[nodiscard]] HANDLE done() const noexcept { return done_; }
    [[nodiscard]] ComPtr<IAudioClient> client() const { return client_; }

private:
    HANDLE done_;
    ComPtr<IAudioClient> client_;
};

[[nodiscard]] ULONGLONG CreatedAt(HANDLE process) noexcept {
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!::GetProcessTimes(process, &created, &exited, &kernel, &user)) return 0;
    return (ULONGLONG{created.dwHighDateTime} << 32) | created.dwLowDateTime;
}

// The WebView2 browser process this process started, opened to wait on, or null while
// there is none. A process whose parent id is ours but that is older than us had a parent
// that went before we came and was given the same id, so it is none of ours. Should there
// ever be more than one, the first is taken.
HANDLE OpenWebViewBrowserProcess() noexcept {
    const DWORD self = ::GetCurrentProcessId();
    const ULONGLONG selfCreated = CreatedAt(::GetCurrentProcess());
    const HANDLE snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return nullptr;
    HANDLE found = nullptr;
    PROCESSENTRY32W entry{.dwSize = sizeof(entry)};
    for (BOOL more = ::Process32FirstW(snapshot, &entry); more && !found; more = ::Process32NextW(snapshot, &entry)) {
        if (entry.th32ParentProcessID != self || ::_wcsicmp(entry.szExeFile, L"msedgewebview2.exe") != 0) continue;
        const HANDLE process =
            ::OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
        if (!process) continue;
        if (CreatedAt(process) >= selfCreated) found = process;
        else ::CloseHandle(process);
    }
    ::CloseHandle(snapshot);
    return found;
}

// An audio client recording `target` and the processes it started, or none if Windows
// can't, or `stop` was set first.
ComPtr<IAudioClient> OpenProcessLoopback(DWORD target, HANDLE stop) {
    AUDIOCLIENT_ACTIVATION_PARAMS params{};
    params.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
    params.ProcessLoopbackParams.TargetProcessId = target;
    params.ProcessLoopbackParams.ProcessLoopbackMode = PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;
    PROPVARIANT blob{};
    blob.vt = VT_BLOB;
    blob.blob.cbSize = sizeof(params);
    blob.blob.pBlobData = reinterpret_cast<BYTE*>(&params);

    auto waiter = Microsoft::WRL::Make<ActivationWaiter>();
    if (!waiter || !waiter->done()) return nullptr;
    ComPtr<IActivateAudioInterfaceAsyncOperation> operation;
    if (FAILED(::ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, __uuidof(IAudioClient), &blob,
                                             waiter.Get(), &operation))) {
        return nullptr;
    }
    const HANDLE waits[] = {stop, waiter->done()};
    if (::WaitForMultipleObjects(2, waits, FALSE, INFINITE) != WAIT_OBJECT_0 + 1) return nullptr;
    return waiter->client();
}

} // namespace

std::int64_t TicksNow() noexcept {
    static const std::int64_t frequency = [] {
        LARGE_INTEGER f;
        ::QueryPerformanceFrequency(&f);
        return f.QuadPart;
    }();
    LARGE_INTEGER now;
    ::QueryPerformanceCounter(&now);
    // Split so the multiplication can't overflow, however long the computer has been up.
    return now.QuadPart / frequency * kTicksPerSecond + now.QuadPart % frequency * kTicksPerSecond / frequency;
}

void SoundCapture::Start(Give give, int gain) {
    Stop();
    give_ = std::move(give);
    gain_ = gain;
    stop_ = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!stop_) return;
    thread_ = ::CreateThread(nullptr, 0, ThreadMain, this, 0, nullptr);
}

void SoundCapture::Stop() noexcept {
    if (thread_) {
        ::SetEvent(stop_);
        ::WaitForSingleObject(thread_, INFINITE);
        ::CloseHandle(std::exchange(thread_, nullptr));
    }
    if (stop_) ::CloseHandle(std::exchange(stop_, nullptr));
    give_ = nullptr;
}

DWORD WINAPI SoundCapture::ThreadMain(void* self) {
    const bool com = SUCCEEDED(::CoInitializeEx(nullptr, COINIT_MULTITHREADED));
    static_cast<SoundCapture*>(self)->Run();
    if (com) ::CoUninitialize();
    return 0;
}

void SoundCapture::Run() {
    const HANDLE ready = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!ready) return;
    SoundBlocks blocks(gain_);

    // Each time round is one recording, from opening it until it stops working, the
    // browser process goes, or the capture is stopped.
    do {
        const HANDLE source = OpenWebViewBrowserProcess();
        if (!source) continue;
        struct Closer {
            HANDLE handle;
            ~Closer() { ::CloseHandle(handle); }
        } closeSource{source};
        const ComPtr<IAudioClient> client = OpenProcessLoopback(::GetProcessId(source), stop_);
        if (!client) continue;
        // Process loopback has no format of its own to offer; it gives what it is asked for.
        WAVEFORMATEX format{};
        format.wFormatTag = WAVE_FORMAT_PCM;
        format.nChannels = kSoundChannels;
        format.nSamplesPerSec = kSoundRate;
        format.wBitsPerSample = 16;
        format.nBlockAlign = static_cast<WORD>(format.nChannels * format.wBitsPerSample / 8);
        format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;
        ComPtr<IAudioCaptureClient> capture;
        if (FAILED(client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                      AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK, kBufferTicks,
                                      0, &format, nullptr)) ||
            FAILED(client->SetEventHandle(ready)) || FAILED(client->GetService(IID_PPV_ARGS(&capture))) ||
            FAILED(client->Start())) {
            continue;
        }
        blocks.Clear();

        bool working = true;
        const HANDLE waits[] = {stop_, ready, source};
        for (DWORD woke; working && (woke = ::WaitForMultipleObjects(3, waits, FALSE, kRetryMs)) != WAIT_OBJECT_0;) {
            if (woke == WAIT_OBJECT_0 + 2) break; // the browser process is gone
            UINT32 next = 0;
            while (working && (working = SUCCEEDED(capture->GetNextPacketSize(&next))) && next) {
                BYTE* data = nullptr;
                UINT32 frames = 0;
                DWORD flags = 0;
                UINT64 at = 0;
                if (!(working = SUCCEEDED(capture->GetBuffer(&data, &frames, &flags, nullptr, &at)))) break;
                const std::int64_t heardAt =
                    at ? static_cast<std::int64_t>(at) : TicksNow() - FramesToTicks(frames);
                if (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) blocks.Clear();
                const auto* samples =
                    (flags & AUDCLNT_BUFFERFLAGS_SILENT) ? nullptr : reinterpret_cast<const std::int16_t*>(data);
                blocks.Add(samples, frames, heardAt, give_);
                capture->ReleaseBuffer(frames);
            }
        }
        client->Stop();
    } while (::WaitForSingleObject(stop_, kRetryMs) == WAIT_TIMEOUT);

    ::CloseHandle(ready);
}

} // namespace exodium
