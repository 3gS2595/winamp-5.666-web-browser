// The bookmarks page: see BuildBookmarksPageHtml in bookmarks.h.
#include "bookmarks.h"

#include "text_util.h"

namespace exodium {

namespace {

constexpr std::wstring_view kPageHead = LR"html(<!doctype html>
<html><head><meta charset="utf-8"><title>Bookmarks</title><link rel="icon" href="data:,">
<style>
body { background: #000; color: #fff; font: 13px "Segoe UI", sans-serif; margin: 0 auto; max-width: 900px; padding: 16px; }
#q { box-sizing: border-box; width: 100%; padding: 6px 8px; background: #000; color: #fff; border: 1px solid #3c3c3c; font: inherit; outline: none; }
#bar { display: flex; align-items: center; gap: 10px; margin: 10px 0 6px; min-height: 26px; color: #969696; }
#bar button { background: #101010; border: 1px solid #3c3c3c; color: #fff; font: inherit; padding: 3px 10px; cursor: pointer; }
#bar button:hover { background: #202020; }
p { color: #969696; }
.row { display: flex; align-items: center; gap: 10px; padding: 4px 8px; }
.row:hover { background: #202020; }
.row span, .row a { white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
.row a { color: #fff; text-decoration: none; }
.row a:hover { text-decoration: underline; }
.host { flex: 1; min-width: 60px; color: #8ab4f8; }
.row button { flex: none; background: none; border: 0; color: #969696; font: inherit; cursor: pointer; visibility: hidden; }
.row:hover button { visibility: visible; }
.row button:hover { color: #fff; }
</style></head><body>
<input id="q" type="search" placeholder="Search bookmarks" autofocus>
<div id="bar"></div>
<div id="list"></div>
<script>
const bookmarks = [)html";

constexpr std::wstring_view kSources = L"];\nconst sources = [";
constexpr std::wstring_view kNotice = L"];\nconst notice = ";

constexpr std::wstring_view kPageTail = LR"html(;
const list = document.getElementById('list'), q = document.getElementById('q'), bar = document.getElementById('bar');
function add(parent, tag, cls, text) {
  const e = document.createElement(tag);
  e.className = cls; e.textContent = text;
  parent.append(e);
  return e;
}
function host(url) { try { return new URL(url).hostname; } catch (e) { return ''; } }
for (const [id, name] of sources) {
  add(bar, 'button', '', 'Import from ' + name).onclick = () => chrome.webview.postMessage('import\n' + id);
}
if (notice) add(bar, 'span', '', notice);
function show() {
  const words = q.value.toLowerCase().split(/\s+/).filter(Boolean);
  list.textContent = '';
  for (const bookmark of bookmarks) {
    const [url, title] = bookmark;
    const text = (title + ' ' + url).toLowerCase();
    if (!words.every(w => text.includes(w))) continue;
    const row = add(list, 'div', 'row', '');
    const link = add(row, 'a', '', title || url);
    link.href = url; link.title = url;
    add(row, 'span', 'host', host(url));
    const remove = add(row, 'button', '', '✕');
    remove.title = 'Remove this bookmark';
    remove.onclick = () => {
      chrome.webview.postMessage('remove\n' + url);
      bookmarks.splice(bookmarks.indexOf(bookmark), 1);
      show();
    };
  }
  if (!list.firstChild) {
    add(list, 'p', '', bookmarks.length ? 'No bookmark matches.'
      : 'No bookmarks yet. Ctrl+D, or the star on the toolbar, bookmarks the page in view.');
  }
}
q.oninput = show;
show();
</script></body></html>)html";

} // namespace

std::wstring BuildBookmarksPageHtml(std::span<const Bookmark> bookmarks, std::span<const BookmarkSource> sources,
                                    std::wstring_view notice) {
    std::wstring html(kPageHead);
    for (const Bookmark& bookmark : bookmarks) {
        html.append(1, L'[').append(JsStringLiteral(bookmark.url));
        html.append(1, L',').append(JsStringLiteral(bookmark.title)).append(L"],\n");
    }
    html += kSources;
    for (const BookmarkSource& source : sources) {
        html.append(1, L'[').append(JsStringLiteral(source.id));
        html.append(1, L',').append(JsStringLiteral(source.name)).append(L"],");
    }
    html += kNotice;
    html += JsStringLiteral(notice);
    html += kPageTail;
    return html;
}

} // namespace exodium
