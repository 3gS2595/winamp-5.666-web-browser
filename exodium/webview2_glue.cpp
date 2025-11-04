// webview2_glue.cpp (diagnostic version)

#include <windows.h>
#include <wrl.h>
#include "WebView2.h"
using Microsoft::WRL::ComPtr;

extern HWND g_hostWnd;
static ComPtr<ICoreWebView2Controller> g_controller;
static ComPtr<ICoreWebView2> g_webview;

static void ShowHR(LPCWSTR where, HRESULT hr) {
    wchar_t buf[256];
    wsprintf(buf, L"%s failed: 0x%08X", where, (unsigned)hr);
    MessageBoxW(NULL, buf, L"WebView2 init", MB_ICONERROR | MB_OK);
}

extern "C" void __cdecl webview2_resize(HWND host) {
    if (!g_controller) return;
    RECT r; GetClientRect(host, &r);
    g_controller->put_Bounds(r);
}

extern "C" int __cdecl init_webview2() {
    // Quick checks to help catch the common “missing loader/runtime”
    HMODULE hLoader = LoadLibraryW(L"WebView2Loader.dll");
    if (!hLoader) {
        MessageBoxW(NULL, L"Cannot load WebView2Loader.dll (x86). Make sure it is next to the plugin or winamp.exe.",
            L"WebView2 init", MB_ICONERROR | MB_OK);
        return 1;
    }
    FreeLibrary(hLoader);

    // Create environment (Evergreen) and controller bound to our child HWND.
    HRESULT hrEnv = CreateCoreWebView2EnvironmentWithOptions(
        nullptr, L".\\wv2_userdata", nullptr,
        Microsoft::WRL::Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [](HRESULT hr, ICoreWebView2Environment* env) -> HRESULT {
                if (FAILED(hr) || !env) { ShowHR(L"CreateEnvironment", hr); return hr ? hr : E_FAIL; }

                HRESULT hrCtl = env->CreateCoreWebView2Controller(
                    g_hostWnd,
                    Microsoft::WRL::Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                        [](HRESULT hr2, ICoreWebView2Controller* controller) -> HRESULT {
                            if (FAILED(hr2) || !controller) { ShowHR(L"CreateController", hr2); return hr2 ? hr2 : E_FAIL; }

                            g_controller = controller;
                            g_controller->get_CoreWebView2(&g_webview);

                            // Ensure visible and sized
                            g_controller->put_IsVisible(TRUE);
                            RECT r; GetClientRect(g_hostWnd, &r);
                            g_controller->put_Bounds(r);

                            // Try a very lightweight page first
                            if (g_webview) {
                                g_webview->Navigate(L"https://example.org/");
                                MessageBoxW(NULL, L"WebView2 controller created OK.\nNavigating to example.org ...",
                                    L"WebView2 init", MB_OK);
                            }
                            return S_OK;
                        }).Get());
                if (FAILED(hrCtl)) ShowHR(L"CreateController (sync)", hrCtl);
                return S_OK;
            }).Get());

    if (FAILED(hrEnv)) { ShowHR(L"CreateEnvironment (sync)", hrEnv); return 1; }
    return 0;
}

extern "C" void __cdecl shutdown_webview2() {
    g_webview.Reset();
    if (g_controller) {
        g_controller->Close();
        g_controller.Reset();
    }
}
