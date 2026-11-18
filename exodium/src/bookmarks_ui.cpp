// Bookmarks in the browser: the star on the toolbar, which is filled while the page in
// view is a bookmark and marks or unmarks it when clicked, as Ctrl+D does; and the
// bookmarks page, where a bookmark is removed and another browser's are brought in. What
// a bookmark is, and what the page looks like, is bookmarks.h's to say.
//
// Another browser's bookmarks are read from the file it keeps them in, in its profile
// under the user's local application data. Only its first profile is looked at, the one
// it calls Default, and nothing of that browser's is changed.
#include "browser.h"

#include "text_util.h"

#include <shlobj.h>

#include <algorithm>
#include <system_error>

namespace exodium {

namespace {

// The browsers whose bookmarks can be brought in, and where each keeps its profiles.
struct OtherBrowser {
    BookmarkSource source;
    std::wstring_view folder; // under the user's local application data
};
constexpr OtherBrowser kOtherBrowsers[] = {
    {{L"chrome", L"Chrome"}, L"Google\\Chrome\\User Data"},
    {{L"edge", L"Microsoft Edge"}, L"Microsoft\\Edge\\User Data"},
    {{L"brave", L"Brave"}, L"BraveSoftware\\Brave-Browser\\User Data"},
    {{L"vivaldi", L"Vivaldi"}, L"Vivaldi\\User Data"},
};

// The file a browser keeps its bookmarks in; empty if Windows can't say where that would be.
[[nodiscard]] std::filesystem::path BookmarksFileOf(const OtherBrowser& browser) {
    CoTaskMemString local;
    if (FAILED(::SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, std::out_ptr(local))) || !local) return {};
    return std::filesystem::path(local.get()) / browser.folder / L"Default" / L"Bookmarks";
}

[[nodiscard]] bool HasBookmarksFile(const OtherBrowser& browser) {
    const std::filesystem::path file = BookmarksFileOf(browser);
    std::error_code error;
    return !file.empty() && std::filesystem::is_regular_file(file, error);
}

// Brings a browser's bookmarks in, and says how that went, as the page then shows.
[[nodiscard]] std::wstring Import(const OtherBrowser& browser) {
    const std::wstring name(browser.source.name);
    const std::optional<std::wstring> text = ReadTextFile(BookmarksFileOf(browser));
    const auto theirs = text ? ParseChromiumBookmarks(*text) : std::nullopt;
    if (!theirs) {
        LogLine(name + L"'s bookmarks couldn't be read.");
        return name + L"'s bookmarks couldn't be read.";
    }
    const std::size_t added = g_app.bookmarks.Merge(*theirs);
    if (added == 0) return name + L" has no bookmarks that aren't here already.";
    SaveBookmarks();
    RefreshBookmarkButtons();
    return std::to_wstring(added) + (added == 1 ? L" bookmark was" : L" bookmarks were") + L" brought in from " + name +
           L".";
}

} // namespace

void ToggleBookmark(BrowserWindow& win) {
    const Tab* const tab = ActiveTab(win);
    if (!tab) return;
    const std::wstring url = TabUrl(*tab);
    if (!g_app.bookmarks.Remove(url)) {
        const std::wstring_view title =
            tab->title == kNewTabTitle ? std::wstring_view() : std::wstring_view(tab->title);
        // Not every page can be one: the browser's own, and a tab that hasn't been anywhere.
        if (!g_app.bookmarks.Add(url, title, UnixNow())) return;
    }
    SaveBookmarks();
    RefreshBookmarkButtons();
}

void UpdateBookmarkButton(BrowserWindow& win) {
    if (!win.bookmarkButton) return;
    const Tab* const tab = ActiveTab(win);
    const bool marked = tab && g_app.bookmarks.Find(TabUrl(*tab)) != nullptr;
    const wchar_t* const glyph = marked ? kGlyphBookmarked : kGlyphBookmark;
    if (WindowText(win.bookmarkButton) != glyph) ::SetWindowTextW(win.bookmarkButton, glyph);
}

void RefreshBookmarkButtons() {
    for (const auto& win : g_app.windows) UpdateBookmarkButton(*win);
}

std::wstring BookmarksPageHtml() {
    std::vector<BookmarkSource> sources;
    for (const OtherBrowser& browser : kOtherBrowsers) {
        if (HasBookmarksFile(browser)) sources.push_back(browser.source);
    }
    return BuildBookmarksPageHtml(g_app.bookmarks.entries(), sources, std::exchange(g_app.bookmarksNotice, {}));
}

void OnBookmarksPageMessage(ICoreWebView2* sender, std::wstring_view message) {
    const std::size_t lineEnd = message.find(L'\n');
    if (lineEnd == std::wstring_view::npos) return;
    const std::wstring_view what = message.substr(0, lineEnd), about = message.substr(lineEnd + 1);
    if (what == kBookmarkPageRemove) {
        if (!g_app.bookmarks.Remove(about)) return;
        SaveBookmarks();
        RefreshBookmarkButtons();
    } else if (what == kBookmarkPageImport) {
        const auto browser =
            std::ranges::find(kOtherBrowsers, about, [](const OtherBrowser& other) { return other.source.id; });
        if (browser == std::ranges::end(kOtherBrowsers)) return;
        // The page is made again, with them on it and how it went said at the top.
        g_app.bookmarksNotice = Import(*browser);
        if (sender) sender->Reload();
    }
}

} // namespace exodium
