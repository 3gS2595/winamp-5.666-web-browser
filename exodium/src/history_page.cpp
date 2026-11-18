#include "history_page.h"

#include "text_util.h"

#include <string_view>

namespace exodium {

namespace {

constexpr std::wstring_view kPageHead = LR"html(<!doctype html>
<html><head><meta charset="utf-8"><title>History</title><link rel="icon" href="data:,">
<style>
body { background: #000; color: #fff; font: 13px "Segoe UI", sans-serif; margin: 0 auto; max-width: 900px; padding: 16px; }
#q { box-sizing: border-box; width: 100%; padding: 6px 8px; background: #000; color: #fff; border: 1px solid #3c3c3c; font: inherit; outline: none; }
h2 { margin: 18px 0 4px; font-size: 13px; font-weight: normal; color: #969696; }
p { color: #969696; }
.row { display: flex; align-items: center; gap: 10px; padding: 4px 8px; }
.row:hover { background: #202020; }
.row span, .row a { white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
.time { flex: none; width: 48px; color: #969696; }
.row a { color: #fff; text-decoration: none; }
.row a:hover { text-decoration: underline; }
.host { flex: 1; min-width: 60px; color: #8ab4f8; }
.row button { flex: none; background: none; border: 0; color: #969696; font: inherit; cursor: pointer; visibility: hidden; }
.row:hover button { visibility: visible; }
.row button:hover { color: #fff; }
#more { height: 1px; }
</style></head><body>
<input id="q" type="search" placeholder="Search history" autofocus>
<div id="list"></div>
<div id="more"></div>
<script>
const entries = [)html";

// The history can hold many thousands of pages, so the list is made a stretch at a time:
// the first of them at once, and more whenever the end of the list scrolls into sight.
constexpr std::wstring_view kPageTail = LR"html(];
const list = document.getElementById('list'), q = document.getElementById('q'), more = document.getElementById('more');
const stretch = 200;
let words = [], next = 0, heading = null, shown = 0;
function add(parent, tag, cls, text) {
  const e = document.createElement(tag);
  e.className = cls; e.textContent = text;
  parent.append(e);
  return e;
}
function host(url) { try { return new URL(url).hostname; } catch (e) { return ''; } }
function addRow(entry) {
  const [url, title, when] = entry;
  const at = when ? new Date(when * 1000) : null;
  const day = at ? at.toLocaleDateString([], { weekday: 'long', year: 'numeric', month: 'long', day: 'numeric' }) : 'Earlier';
  if (day !== heading) add(list, 'h2', '', heading = day);
  const row = add(list, 'div', 'row', '');
  add(row, 'span', 'time', at ? at.toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' }) : '');
  const link = add(row, 'a', '', title || url);
  link.href = url; link.title = url;
  add(row, 'span', 'host', host(url));
  const remove = add(row, 'button', '', '✕');
  remove.title = 'Remove from history';
  remove.onclick = () => {
    chrome.webview.postMessage(url);
    entries.splice(entries.indexOf(entry), 1);
    show(shown - 1);
  };
}
// Adds rows until `count` more are there, or the history has no more that match.
function fill(count) {
  for (const until = shown + count; next < entries.length && shown < until; ++next) {
    const entry = entries[next];
    if (words.length) {
      const text = (entry[1] + ' ' + entry[0]).toLowerCase();
      if (!words.every(w => text.includes(w))) continue;
    }
    addRow(entry);
    ++shown;
  }
}
// Makes the list again from the top, with `count` rows to begin with.
function show(count) {
  words = q.value.toLowerCase().split(/\s+/).filter(Boolean);
  list.textContent = '';
  next = 0; heading = null; shown = 0;
  fill(Math.max(count, stretch));
  if (!shown) add(list, 'p', '', entries.length ? 'Nothing in the history matches.' : 'The history is empty.');
}
// The end of the list is in sight, or nearly: more of it, until it isn't.
function fillInSight() {
  fill(stretch);
  if (next < entries.length && more.getBoundingClientRect().top < innerHeight + 600) requestAnimationFrame(fillInSight);
}
new IntersectionObserver(seen => { if (seen.some(s => s.isIntersecting)) fillInSight(); }, { rootMargin: '600px' }).observe(more);
q.oninput = () => show(stretch);
show(stretch);
</script></body></html>)html";

} // namespace

std::wstring BuildHistoryPageHtml(std::span<const HistoryEntry> entries) {
    std::wstring html(kPageHead);
    for (const HistoryEntry& entry : entries) {
        html.append(1, L'[').append(JsStringLiteral(entry.url));
        html.append(1, L',').append(JsStringLiteral(entry.title));
        html.append(1, L',').append(std::to_wstring(entry.lastVisit)).append(L"],\n");
    }
    html += kPageTail;
    return html;
}

} // namespace exodium
