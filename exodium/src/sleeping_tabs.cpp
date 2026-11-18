// Tabs left out of view are put to sleep.
//
// A tab that isn't in view still has its page, in a process of its own that keeps
// everything the page holds: measured on 2026-11-09, after a day's use, the browser had 24
// such processes and 4.9 GB between them. A tab that has been out of view for two hours is
// put to sleep instead: its webview is closed, which ends that process and frees all of it
// at once, and the tab stays in the strip with its title and icon. Looked at again, it is
// given a new webview and the pages its Back and Forward led to, as a tab is that comes
// back when Winamp starts (session.cpp), and its page loads afresh. This is what Chrome's
// Memory Saver does.
//
// What a sleeping page loses is whatever it held that isn't in its address: where it was
// scrolled to, what a script had worked out. So a tab is left awake while there is a sign
// that it is in use:
//
// - The tab in view of each window, on the screen or not.
// - One whose page is making sound, or is still loading, or has a download going.
// - One with something typed into a form that hasn't been sent, or text in an editor: its
//   page is asked before it is put to sleep (kHasTypedText). A page that doesn't answer
//   stays awake.
// - Claude Code's tabs, which its session is working in.
//
// Settings::sleepTabs turns it off (the checkbox on Exodium's page in Winamp's
// Preferences), and sleep_tabs_minutes in winamp.ini says how long a tab is left; see
// plugin.cpp. Closing a webview works with the debugging port open too, where suspending
// one (TrySuspend) would leave a page that doesn't answer the tools.
#include "browser.h"

#include <wrl/event.h>

#include <string_view>

namespace exodium {

namespace {

using Microsoft::WRL::Callback;

// How often tabs are looked at. With EXODIUM_SLEEP_TABS_MS set, as the smoke test sets
// it, that is how long a tab is left out of view, and they are looked at every second.
constexpr UINT kLookEveryMs = 60 * 1000;
constexpr UINT kTestLookEveryMs = 1000;
constexpr wchar_t kTestWaitVariable[] = L"EXODIUM_SLEEP_TABS_MS";

// Asked of a page before it is put to sleep: whether anything typed into it would be lost.
// A text box that holds something other than what the page gave it, or an editor with text
// in it. Only the page itself is looked at, not the frames in it.
constexpr wchar_t kHasTypedText[] = LR"js((() => {
  try {
    const boxes = 'textarea, input:not([type]), input[type=text], input[type=search], input[type=email], ' +
                  'input[type=url], input[type=tel], input[type=number]';
    for (const box of document.querySelectorAll(boxes)) {
      if (box.value !== '' && box.value !== box.defaultValue) return true;
    }
    const editors = '[contenteditable=""], [contenteditable="true"], [contenteditable="plaintext-only"]';
    for (const editor of document.querySelectorAll(editors)) {
      if (editor.textContent.trim() !== '') return true;
    }
    return false;
  } catch (e) {
    return true;
  }
})())js";

[[nodiscard]] bool SleepingIsOn() noexcept {
    return g_app.settings.sleepTabs && g_app.sleepTabsAfterMs > 0 && !g_app.shutDown;
}

// Whether the tab at `index` is the one in view of a window that is on the screen.
[[nodiscard]] bool IsInView(const BrowserWindow& win, int index) noexcept {
    return index == win.active && win.frame && ::IsWindowVisible(win.frame) && !win.minimized;
}

// Whether a tab that has been out of view long enough may be put to sleep.
[[nodiscard]] bool MaySleep(const BrowserWindow& win, const Tab& tab, int index) {
    if (!tab.controller || !tab.webview || win.closing) return false;
    // The tab in view of a window stays as it is even while the window is out of sight.
    if (index == win.active || (win.isFullscreen && tab.id == win.fullscreenTabId)) return false;
    if (tab.busy() || tab.historyCopy || tab.copyingHistory || tab.parked || tab.askingToSleep) return false;
    // One with its DevTools open is being worked on, and they would lose the page they are for.
    if (tab.devtools.open) return false;
    // A private tab's session ends when the last private webview goes, and its sign-ins with it.
    if (tab.playingAudio || tab.forClaude || tab.isPrivate || g_app.drag.tabId == tab.id) return false;
    return !TabHasDownloadsGoing(tab);
}

void SleepNow(Tab& tab) {
    PutTabToSleep(tab);
    if (tab.button) ::InvalidateRect(tab.button.get(), nullptr, FALSE);
}

// The page's answer to kHasTypedText, which is JSON: true, false, or nothing it could say.
void OnPageAnswered(int tabId, HRESULT result, LPCWSTR json) {
    const TabLocation at = FindTab(tabId);
    if (!at) return;
    Tab& tab = at.tab();
    tab.askingToSleep = false;
    if (!SleepingIsOn()) return;
    const bool nothingTyped = SUCCEEDED(result) && json && std::wstring_view(json) == L"false";
    if (!nothingTyped) {
        // Left awake, and asked again once a quarter of the wait has gone by.
        const ULONGLONG now = ::GetTickCount64(), leftToWait = g_app.sleepTabsAfterMs / 4 * 3;
        tab.seenAt = now > leftToWait ? now - leftToWait : 0;
        return;
    }
    // It may have been looked at, or started something, while its page was asked.
    if (MaySleep(*at.window, tab, at.index)) SleepNow(tab);
}

void AskThenSleep(Tab& tab) {
    // A page whose process died has nothing left to lose.
    if (tab.crashed) {
        SleepNow(tab);
        return;
    }
    const int tabId = tab.id;
    tab.askingToSleep = true;
    const HRESULT asked = tab.webview->ExecuteScript(
        kHasTypedText,
        Callback<ICoreWebView2ExecuteScriptCompletedHandler>([tabId](HRESULT result, LPCWSTR json) -> HRESULT {
            OnPageAnswered(tabId, result, json);
            return S_OK;
        }).Get());
    if (FAILED(asked)) OnPageAnswered(tabId, asked, nullptr);
}

void LookForTabsToPutToSleep() {
    const ULONGLONG now = ::GetTickCount64();
    const bool on = SleepingIsOn();
    for (const auto& win : g_app.windows) {
        for (int i = 0; i < static_cast<int>(win->tabs.size()); ++i) {
            Tab& tab = win->tabs[i];
            if (IsInView(*win, i)) {
                tab.seenAt = now;
            } else if (on && now - tab.seenAt >= g_app.sleepTabsAfterMs && MaySleep(*win, tab, i)) {
                AskThenSleep(tab);
            }
        }
    }
}

} // namespace

void PutTabToSleep(Tab& tab) {
    tab.CloseWebView();
    if (!tab.lastUrl.empty()) tab.pendingUrl = tab.lastUrl;
    tab.loading = tab.typedNav = tab.parked = tab.crashed = tab.askingToSleep = false;
    tab.typedNavId = 0;
    ForgetTabZoom(tab); // a new webview starts out at the normal size
    // Where Back and Forward led went with the webview; its next one makes them again.
    GiveTabPages(tab, HistoryCopy(tab.pages));
}

void StartSleepingTabs() {
    if (g_app.windows.empty() || !g_app.windows.front()->client) return;
    UINT lookEvery = kLookEveryMs;
    wchar_t text[16] = {};
    const DWORD length = ::GetEnvironmentVariableW(kTestWaitVariable, text, static_cast<DWORD>(std::size(text)));
    if (length > 0 && length < std::size(text)) {
        if (const auto wait = ParseNumber(text); wait && *wait > 0) {
            g_app.sleepTabsAfterMs = static_cast<ULONGLONG>(*wait);
            lookEvery = kTestLookEveryMs;
        }
    }
    ::SetTimer(g_app.windows.front()->client, kSleepTimer, lookEvery, nullptr);
}

bool OnSleepTimer(UINT_PTR timer) {
    if (timer != kSleepTimer) return false;
    LookForTabsToPutToSleep();
    return true;
}

} // namespace exodium
