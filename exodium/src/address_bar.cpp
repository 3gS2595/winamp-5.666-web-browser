// The address bar, the list of suggestions under it, and the keyboard handling
// every control in a browser window shares. What the list holds is worked out in
// suggestions.cpp; this file shows it and acts on it.
#include "browser.h"

#include "text_util.h"

#include <commctrl.h>

#include <algorithm>

namespace exodium {

namespace {

constexpr UINT_PTR kControlSubclassId = 1;
constexpr UINT_PTR kSuggestSubclassId = 2;
constexpr int kSuggestRowPadding = 8;
constexpr int kSuggestFallbackRowHeight = 18;
constexpr int kSuggestBorder = 2;

// Control characters the address bar lets through as typing: what Ctrl+C, Ctrl+V,
// Ctrl+X and Ctrl+Z arrive as, and Backspace.
constexpr wchar_t kCharCopy = 0x03, kCharPaste = 0x16, kCharCut = 0x18, kCharUndo = 0x1A, kCharDelete = 0x7F;

[[nodiscard]] std::wstring UrlBoxText(const BrowserWindow& win) {
    return WindowText(win.urlEdit);
}

void SetUrlBoxText(BrowserWindow& win, const std::wstring& text) {
    win.settingUrl = true;
    ::SetWindowTextW(win.urlEdit, text.c_str());
    win.settingUrl = false;
}

[[nodiscard]] int SelectedSuggestion(const BrowserWindow& win) noexcept {
    return static_cast<int>(::SendMessageW(win.suggestList, LB_GETCURSEL, 0, 0));
}

void MoveCaretToEnd(HWND edit) noexcept {
    const int length = ::GetWindowTextLengthW(edit);
    ::SendMessageW(edit, EM_SETSEL, static_cast<WPARAM>(length), length);
    ::SendMessageW(edit, EM_SCROLLCARET, 0, 0);
}

void PositionSuggestions(BrowserWindow& win) {
    const RECT edit = WindowRectOf(win.urlEdit);
    POINT topLeft{edit.left, edit.bottom};
    ::ScreenToClient(win.client, &topLeft);
    int rowHeight = static_cast<int>(::SendMessageW(win.suggestList, LB_GETITEMHEIGHT, 0, 0));
    if (rowHeight <= 0) rowHeight = kSuggestFallbackRowHeight;
    const int rows = static_cast<int>(std::min(win.suggestions.size(), kMaxSuggestions));
    ::SetWindowPos(win.suggestList, HWND_TOP, topLeft.x, topLeft.y, Width(edit), rows * rowHeight + kSuggestBorder,
                   SWP_SHOWWINDOW);
}

// Puts the user's own text back in the address bar, along with the completion offered
// for it. The caret stays where the typing stopped.
void ShowUserText(BrowserWindow& win) {
    SetUrlBoxText(win, win.userText + win.inlineText);
    const std::size_t typed = win.userText.size();
    ::SendMessageW(win.urlEdit, EM_SETSEL, typed + win.inlineText.size(), static_cast<LPARAM>(typed));
    ::SendMessageW(win.urlEdit, EM_SCROLLCARET, 0, 0);
}

// Rebuilds the list for win.userText.
void UpdateSuggestions(BrowserWindow& win, bool allowCompletion) {
    SuggestionList list = BuildSuggestions(g_app.history.entries(), win.userText, allowCompletion, UnixNow());
    win.suggestions = std::move(list.rows);
    win.inlineText = std::move(list.inlineCompletion);

    ::SendMessageW(win.suggestList, LB_RESETCONTENT, 0, 0);
    if (win.suggestions.empty()) {
        HideSuggestions(win);
        return;
    }
    // The list draws its rows itself; all it holds for each is the row's number.
    for (std::size_t row = 0; row < win.suggestions.size(); ++row) {
        ::SendMessageW(win.suggestList, LB_ADDSTRING, 0, static_cast<LPARAM>(row));
    }
    ::SendMessageW(win.suggestList, LB_SETCURSEL, 0, 0);
    PositionSuggestions(win);
    win.suggestVisible = true;
    if (!win.inlineText.empty()) ShowUserText(win);
}

// Up and Down go round the list, showing each row in the address bar on the way.
void MoveSuggestSelection(BrowserWindow& win, int delta) {
    const int count = static_cast<int>(win.suggestions.size());
    if (!win.suggestVisible || count <= 0) return;
    const int selected = (std::max(SelectedSuggestion(win), 0) + delta + count) % count;
    ::SendMessageW(win.suggestList, LB_SETCURSEL, static_cast<WPARAM>(selected), 0);
    if (selected == 0) {
        ShowUserText(win);
        return;
    }
    SetUrlBoxText(win, win.suggestions[selected].text);
    MoveCaretToEnd(win.urlEdit);
}

// Once the caret is moved into text the list put in the box, a completion or a row
// being tried out, that text is the user's own, as if it had been typed.
void AdoptShownText(BrowserWindow& win) {
    if (!win.suggestVisible) return;
    std::wstring text = UrlBoxText(win);
    if (text == win.userText) return;
    const Selection selection = EditSelection(win.urlEdit);
    const bool onTopRow = SelectedSuggestion(win) <= 0;
    // The completion is still only an offer.
    if (onTopRow && selection.from == win.userText.size() && selection.to == text.size()) return;
    win.userText = std::move(text);
    UpdateSuggestions(win, false);
}

// Escape backs out one step at a time: from a row being tried out to the typed text,
// then out of the list and its completion, then back to the page's own address.
void EscapeAddressBar(BrowserWindow& win) {
    if (win.suggestVisible) {
        const bool tryingRow = SelectedSuggestion(win) > 0;
        const bool completed = !win.inlineText.empty();
        if (tryingRow) ::SendMessageW(win.suggestList, LB_SETCURSEL, 0, 0);
        else HideSuggestions(win);
        if (tryingRow || completed) ShowUserText(win);
        return;
    }
    win.editDirty = false;
    UpdateUrlBox(win);
    SelectAllText(win.urlEdit);
}

// `typed` says the address itself was asked for, which counts toward suggesting it.
void NavigateTo(BrowserWindow& win, const std::wstring& url, bool typed) {
    Tab* const tab = ActiveTab(win);
    if (!tab) return;
    win.editDirty = false;
    HideSuggestions(win);
    SetUrlBoxText(win, url);
    tab->typedNav = typed;
    if (tab->webview) {
        tab->webview->Navigate(url.c_str());
    } else {
        tab->pendingUrl = url;
        ::PostMessageW(win.client, kMsgInitWebView, 0, 0);
    }
}

LRESULT CALLBACK SuggestListProc(HWND list, UINT msg, WPARAM wParam, LPARAM lParam, UINT_PTR subclassId, DWORD_PTR) {
    switch (msg) {
    // The list never takes the keyboard from the address bar, which a list box does
    // when left to handle a click itself. A click picks the row under it and goes there.
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
    case WM_LBUTTONUP: {
        BrowserWindow* const win = WindowFromChild(list);
        const LRESULT hit = ::SendMessageW(list, LB_ITEMFROMPOINT, 0, lParam);
        if (!win || HIWORD(hit)) return 0; // outside the rows
        ::SendMessageW(list, LB_SETCURSEL, LOWORD(hit), 0);
        if (msg == WM_LBUTTONUP) NavigateFromUrlBox(*win);
        return 0;
    }
    case WM_NCDESTROY:
        ::RemoveWindowSubclass(list, SuggestListProc, subclassId);
        break;
    default:
        break;
    }
    return ::DefSubclassProc(list, msg, wParam, lParam);
}

// Key presses in the address bar itself. Says whether the key was dealt with; a
// caret movement is passed on first and reported through `result`.
bool OnUrlKeyDown(BrowserWindow& win, WPARAM key, LPARAM lParam, Modifiers mods, LRESULT& result) {
    const HWND edit = win.urlEdit;
    result = 0;
    if (mods.ctrl && !mods.alt && key == 'A') {
        SelectAllText(edit);
        return true;
    }
    if (win.suggestVisible && (key == VK_DOWN || key == VK_UP || key == VK_TAB)) {
        MoveSuggestSelection(win, key == VK_UP || (key == VK_TAB && mods.shift) ? -1 : 1);
        return true;
    }
    switch (key) {
    case VK_ESCAPE:
        EscapeAddressBar(win);
        return true;
    case VK_LEFT:
    case VK_RIGHT:
    case VK_HOME:
    case VK_END:
        result = ::DefSubclassProc(edit, WM_KEYDOWN, key, lParam);
        AdoptShownText(win);
        return true;
    default:
        return false;
    }
}

// Shared by the address bar and every button: each can have the keyboard, and the
// browser's shortcuts have to work from all of them.
LRESULT CALLBACK ControlProc(HWND control, UINT msg, WPARAM wParam, LPARAM lParam, UINT_PTR subclassId, DWORD_PTR) {
    BrowserWindow* const win = WindowFromChild(control);
    const bool isUrlEdit = win && control == win->urlEdit;

    switch (msg) {
    case WM_NCCALCSIZE:
        // Room for the one-pixel border drawn below.
        if (wParam) {
            auto* params = reinterpret_cast<NCCALCSIZE_PARAMS*>(lParam);
            ::InflateRect(&params->rgrc[0], -1, -1);
            return 0;
        }
        break;

    case WM_NCPAINT: {
        if (const WindowDc dc(control, WindowDc::Area::Whole); dc) {
            const RECT window = WindowRectOf(control);
            const RECT border{0, 0, Width(window), Height(window)};
            ::FrameRect(dc.get(), &border, g_app.theme.border.get());
        }
        return 0;
    }

    case WM_GETDLGCODE:
        return DLGC_WANTALLKEYS | DLGC_WANTCHARS;

    case WM_KEYDOWN: {
        const Modifiers mods = CurrentModifiers();
        if (HandleGlobalShortcut(win, wParam, mods)) return 0;
        if (win && mods.ctrl && wParam == 'L') {
            FocusAddressBar(*win);
            win->editDirty = true;
            return 0;
        }
        if (LRESULT result = 0; isUrlEdit && OnUrlKeyDown(*win, wParam, lParam, mods, result)) return result;
        if (win && wParam == VK_RETURN) {
            win->editDirty = false;
            NavigateFromUrlBox(*win);
            return 0;
        }
        break;
    }

    case WM_CHAR:
        // Enter already navigated on WM_KEYDOWN. The other control characters are what
        // shortcuts like Ctrl+L or Ctrl+T leave behind, and the edit would beep at them
        // or insert junk. Only copy, paste, cut, undo and Backspace go through. Testing
        // the character rather than the Ctrl key keeps AltGr-typed characters working.
        if ((wParam < L' ' || wParam == kCharDelete) && wParam != kCharCopy && wParam != kCharPaste &&
            wParam != kCharCut && wParam != kCharUndo && wParam != L'\b') {
            return 0;
        }
        // Ctrl+H leaves the same character behind as Backspace does.
        if (wParam == L'\b' && IsKeyDown('H')) return 0;
        // Only a character that was typed brings a completion with it, which the
        // edit's change notice has no way to tell from Backspace or a paste.
        if (isUrlEdit && wParam >= L' ') {
            win->typingChar = true;
            const LRESULT result = ::DefSubclassProc(control, msg, wParam, lParam);
            win->typingChar = false;
            return result;
        }
        break;

    case WM_LBUTTONDOWN:
        if (isUrlEdit) {
            if (::GetFocus() != control) win->urlClickSelect = true;
            const LRESULT result = ::DefSubclassProc(control, msg, wParam, lParam);
            AdoptShownText(*win);
            return result;
        }
        break;

    case WM_LBUTTONUP:
        // Clicking into the address bar selects the whole URL, as browsers do, unless
        // that click was itself a drag-select.
        if (isUrlEdit && win->urlClickSelect) {
            win->urlClickSelect = false;
            const LRESULT result = ::DefSubclassProc(control, msg, wParam, lParam);
            if (EditSelection(control).empty()) SelectAllText(control);
            return result;
        }
        break;

    case WM_NCDESTROY:
        ::RemoveWindowSubclass(control, ControlProc, subclassId);
        break;

    default:
        break;
    }
    return ::DefSubclassProc(control, msg, wParam, lParam);
}

} // namespace

void SubclassControl(HWND control) {
    if (control) ::SetWindowSubclass(control, ControlProc, kControlSubclassId, 0);
}

void CreateAddressBar(BrowserWindow& win) {
    const auto create = [&win](DWORD exStyle, const wchar_t* className, const wchar_t* text, DWORD style, int id) {
        const HWND control =
            ::CreateWindowExW(exStyle, className, text, WS_CHILD | style, 0, 0, 0, 0, win.client,
                              reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), g_app.instance, nullptr);
        ::SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(g_app.theme.font), TRUE);
        return control;
    };

    win.urlEdit = create(0, L"EDIT", kDefaultUrl, WS_VISIBLE | ES_AUTOHSCROLL, kIdUrl);
    SubclassControl(win.urlEdit);

    win.suggestList = create(WS_EX_TOOLWINDOW, L"LISTBOX", L"",
                             LBS_NOTIFY | LBS_NOINTEGRALHEIGHT | LBS_OWNERDRAWFIXED | WS_BORDER, kIdSuggest);
    ::SetWindowSubclass(win.suggestList, SuggestListProc, kSuggestSubclassId, 0);

    TEXTMETRICW metrics{};
    if (const WindowDc dc(win.suggestList); dc) {
        const SelectedObject selectedFont(dc.get(), g_app.theme.font);
        ::GetTextMetricsW(dc.get(), &metrics);
    }
    ::SendMessageW(win.suggestList, LB_SETITEMHEIGHT, 0, metrics.tmHeight + kSuggestRowPadding);
}

void FocusAddressBar(BrowserWindow& win) {
    ::SetFocus(win.urlEdit);
    SelectAllText(win.urlEdit);
}

bool IsEditingUrl(const BrowserWindow& win) noexcept {
    return ::GetFocus() == win.urlEdit && win.editDirty;
}

// A bar that has the keyboard with all of its text selected, as on a new tab, gets
// the new address selected as well, so that typing replaces it whenever the page
// gets round to changing it.
void ShowPageUrl(BrowserWindow& win, std::wstring_view url) {
    const Selection selection = EditSelection(win.urlEdit);
    const bool wholeSelected = ::GetFocus() == win.urlEdit && selection.from == 0 &&
                               static_cast<int>(selection.to) == ::GetWindowTextLengthW(win.urlEdit);
    SetUrlBoxText(win, std::wstring(url == kHistoryUrl ? std::wstring_view(kHistoryName) : url));
    if (wholeSelected) SelectAllText(win.urlEdit);
}

void UpdateUrlBox(BrowserWindow& win) {
    if (IsEditingUrl(win)) return;
    const Tab* tab = ActiveTab(win);
    if (!tab) return;
    // A tab whose webview isn't up yet shows where it is headed.
    ShowPageUrl(win, tab->webview ? SourceOf(tab->webview.Get()) : tab->pendingUrl);
}

void UpdateReloadButton(BrowserWindow& win) {
    if (!win.reloadButton) return;
    const Tab* tab = ActiveTab(win);
    ::SetWindowTextW(win.reloadButton, tab && tab->loading ? kGlyphStop : kGlyphReload);
}

void HideSuggestions(BrowserWindow& win) {
    if (win.suggestList) ::ShowWindow(win.suggestList, SW_HIDE);
    win.suggestVisible = false;
    win.inlineText.clear();
}

void OnUrlEdited(BrowserWindow& win) {
    win.editDirty = true;
    win.userText = UrlBoxText(win);
    // A completion only follows a character typed at the end of the text.
    const Selection selection = EditSelection(win.urlEdit);
    UpdateSuggestions(win, win.typingChar && selection.from == win.userText.size() && selection.empty());
}

void NavigateFromUrlBox(BrowserWindow& win) {
    if (!ActiveTab(win)) return;
    // With the list open it goes to the selected row. The top one stands for the text
    // in the box, so plain Enter takes whatever was completed.
    const int selected = win.suggestVisible ? SelectedSuggestion(win) : -1;
    if (selected >= 0 && selected < static_cast<int>(win.suggestions.size())) {
        const Suggestion picked = win.suggestions[selected];
        NavigateTo(win, picked.url, !picked.search);
        return;
    }
    const std::wstring input(Trim(UrlBoxText(win)));
    if (input.empty()) return;
    const ResolvedInput target = ResolveAddressInput(input);
    NavigateTo(win, target.url, !target.isSearch);
}

} // namespace exodium
