# Exodium

A tabbed web browser that lives inside Winamp. It is a Winamp general purpose plugin
(`gen_browser.dll`) that puts Microsoft's WebView2 in one of Winamp's own skinned windows.

- Tabs that can be reordered, dragged between windows, and torn off into windows of their own
- A speaker on each tab whose page is making sound, as in Chrome
- An address bar that completes and suggests from the history, the way Chrome's does
- A history page (`exodium://history`), per-site zoom, reopening of closed tabs, fullscreen video
- Windows that snap (dragged by the title bar to the screen's edges and corners, or with
  Win+arrow keys), maximize, minimize and have their own taskbar buttons
- Ironwork ornaments on each window's corners, bulging out past the frame. The one on
  the top right has the buttons that minimize, maximize and close the window

A second, unrelated plugin lives beside it: `gen_llama.dll` plays Winamp's llama sound
at start-up when the splash screen is enabled.

## Keyboard

| Keys | |
| --- | --- |
| Ctrl+T / Ctrl+W | New tab / close tab |
| Ctrl+Shift+T | Reopen the last closed tab |
| Ctrl+Tab / Ctrl+Shift+Tab | Next / previous tab |
| Ctrl+N | New window |
| Ctrl+Shift+W, Alt+F4 | Close window |
| Ctrl+L | Address bar |
| Ctrl+H | History |
| Alt+Left / Alt+Right | Back / forward |
| Ctrl+0 | Put the site back to its normal zoom |

A tab is also closed by middle-clicking it.

## Building

Requirements: Visual Studio 2026 with the C++ desktop workload (toolset v145), and the
WebView2 NuGet package, which Visual Studio restores into `packages\`.

Open `exodium.slnx`, or build from a command prompt:

```
msbuild exodium\exodium.vcxproj /p:Configuration=Release /p:Platform=Win32
```

Classic Winamp is a 32-bit program, so **Win32 is the build to use**. x64 builds as well,
for 64-bit hosts.

Everything is built into one tree, wherever the build is started from:

```
bin\<Platform>\<Configuration>\gen_browser.dll
bin\<Platform>\<Configuration>\WebView2Loader.dll
```

## Installing

With Winamp closed, copy `gen_browser.dll` and `WebView2Loader.dll` into Winamp's
`Plugins` folder. The browser is then in Winamp's main menu and its Windows menu as
*Exodium*. It needs the WebView2 runtime, which Windows 10 and 11 normally have.

The plugin keeps its data in `Plugins\exodium_data`: `history.txt`, `zoom.txt` and the
browser profile (cookies, sign-ins, cache). The window's place is kept in `winamp.ini`.

## Testing

Unit tests cover everything that needs neither a window nor a webview: addresses, the
history, suggestions, zoom, and the files they are kept in.

```
msbuild tests\exodium_tests.vcxproj /p:Configuration=Release /p:Platform=Win32
bin\Win32\Release\exodium_tests.exe
```

The rest is covered by a smoke test that runs the built plugin in a scratch copy of
Winamp, beside the one in use and without touching it, and works it by window messages:
pages, tabs, dragging tabs between windows, pages that open tabs and windows of their
own, minimizing and maximizing, the corner ornaments and their buttons, closing, and
what is on disk after Winamp exits.

```
powershell -ExecutionPolicy Bypass -File tools\smoke-test.ps1
```

Keyboard shortcuts are the one thing neither covers; those are tried by hand.

## Layout

```
build\Common.props        compiler and linker settings shared by every project
exodium\src\              the browser plugin
exodium\res\              the corner ornaments' pictures, built into the plugin
exodium\third_party\      Winamp SDK headers, as they came
gen_llama\                the llama sound plugin
tests\                    unit tests and the small runner they use
tools\smoke-test.ps1      the smoke test
docs\                     design notes for individual features
```

Inside `exodium\src`:

| File | |
| --- | --- |
| `plugin.cpp` | What Winamp calls: start-up, preferences, exit |
| `browser.h` | The model (`App`, `BrowserWindow`, `Tab`) and every function shared between the files below |
| `browser_window.cpp` | Making and closing windows, their window procedure, shortcuts |
| `frame.cpp`, `embed_frame.cpp` | Winamp's frame around a window: taskbar button, snapping, menu entries, saved place |
| `layout.cpp`, `drawing.cpp` | Where the controls go and how they are painted |
| `ornament.cpp` | The ornaments on a window's corners, and the minimize, maximize and close buttons on the top right one |
| `tabs.cpp`, `tab_drag.cpp` | Opening, closing, reopening, moving and dragging tabs |
| `address_bar.cpp` | The address bar, its suggestion list, and the keyboard handling all controls share |
| `webview.cpp`, `webview_events.cpp` | Creating webviews and reacting to what they report |
| `fullscreen.cpp`, `zoom.cpp` | Fullscreen video, per-site zoom |
| `suggestions.cpp`, `history.cpp`, `history_page.cpp`, `site_zoom.cpp`, `url_util.cpp`, `text_util.cpp` | Logic with no window in it; this is what the unit tests build |
| `win32_util.h` | Owning wrappers for window, GDI and COM handles |

## Conventions

- C++23, warning level 4, and no warnings.
- Everything runs on Winamp's UI thread; nothing is locked.
- Handles are owned by RAII types (`UniqueWindow`, `UniqueIcon`, `WebViewController`, ...),
  never closed by hand.
- Webview events find their tab by id each time they fire, because the tab may have moved
  to another window or been closed in the meantime.
- Work that would destroy a window or webview from inside its own event is posted to the
  window's client instead (`ClientMessage` in `browser.h`).
- `std::format` is kept out of the plugin (it is fine in the tests): it brings about
  190 KB of formatting tables into a DLL whose code is otherwise about 200 KB.
- Formatting is `clang-format` with the `.clang-format` at the root.
