# Playlist Covers

`gen_playlist_covers.dll`, a Winamp general purpose plugin of its own, unrelated to the
browser. It puts album covers above the songs of each saved playlist in the Media Library
(Library > Playlists), the way the library's Audio view shows its albums above its songs.

The playlist view stays the library's own (ml_playlists'): its list, its Play, Send-To,
+, −, Sel, Misc, Manage Playlist and Save buttons, dragging songs to reorder them, all as
before. The covers take the top of the place its list had, and the list the rest.

- **Covers.** An album's songs go under one cover: the same album tag and album artist,
  or, without an album artist, the same album tag in the same folder. Any other song is a
  cover of its own. The pictures are Winamp's own album art: the one in the file, or a
  `folder.jpg` and the like beside it. A wide picture (a video thumbnail) shows its middle
  square.
- **The Audio view's look.** The covers look as the library's Audio view shows its albums
  (ml_local's album art icon view): the size it is set to (its `albumartviewmode`: 60, 90,
  120 or 180 pixels), two pixels round each, packed from the top left with nothing between
  them, the name on one line below unless it shows icons only, a picked one filled with the
  list's picked colour, the library's own llama for one without a picture, and the
  library's own skinned scroll bar. The size follows the Audio view's unless one is picked
  in the right-click menu.
- **Picking.** A click on a cover picks its songs in the list (and only those) and brings
  the first into sight; Ctrl+click adds a cover's songs or takes them out; Shift+click
  picks the covers between. A cover with songs picked in the list shows as picked, however
  they were picked. The library's buttons then act on them as on any songs picked.
- **Playing.** A double click on a cover plays its songs, through the library's own Play.
  The cover of the song playing has a frame and a play mark.
- **Keys.** The arrows, Page Up/Down, Home and End move from cover to cover, picking each.
  The library's own keys (Enter to play, Delete to remove...) act on the songs picked.
- **Right click.** Play, Enqueue, Select its songs, Open folder, Show the playing song;
  the cover size (as in the Audio view, or the library's small, medium, large or extra
  large; Ctrl+wheel goes through them); the order: as in the playlist, by artist or by
  album.
- **Room.** The bar under the covers can be dragged to give them more or less of the view.

The covers follow the playlist's file: when the playlist is saved (its Save button, or the
library saving it on leaving it), they are read again. A playlist edited and not yet saved
is matched to the file by the songs' titles, so a cover picks the right rows; songs added
and not yet saved have no cover until then.

Tags and covers are read on threads of their own, so Winamp never waits for a file. The
tags are kept in `Plugins\playlist_covers_tags.txt` in Winamp's settings folder, so a long
playlist is grouped at once the next time (by file, for as long as its size and write time
stay the same). Covers are read for the ones in sight and near them, and about 40 MB of
them are kept, shared by all the playlists. Settings are in `winamp.ini` under
`[PlaylistCovers]`: `cover_size` (0: as the Audio view), `covers_share` (per thousand of
the view's list area) and `order`. The Audio view's own are read from `gen_ml.ini` (the
view named Audio, `queryN_meta`) and that view's file in `ml\views`.

The covers are a window of their own inside the pane, skinned by the library
(`ML_IPC_SKINWINDOW`, `SKINNEDWND_TYPE_SCROLLWND`, sent to the library's main window, the
playlist view's parent). The skinned scroll bar keeps its own copy of the scroll state, so
each change is followed by `IPC_ML_SKINNEDSCROLLWND_UPDATEBARS` to it; `DISABLENOSCROLL`
keeps the bar there when nothing scrolls. The llama is ml_local.dll's PNG resource 169,
decoded with WIC and tinted with the text colour as ml_local tints it.

## How it finds the view

ml_playlists makes a dialog for each playlist picked in the library's tree. The plugin
watches windows being made on Winamp's thread (`SetWinEventHook`, `EVENT_OBJECT_CREATE`)
and looks at each dialog once it is done: a playlist view has a `TITLE` property (the
playlist's name, set by ml_playlists) and a list view with id 1012. The name is looked up in
Winamp's list of saved playlists (`api_playlists`, in `System\playlist.w5s`) for the file.
The covers are a child window of the dialog; the list is subclassed, and wherever
ml_playlists puts it (`WM_WINDOWPOSCHANGING`), it is put below the covers instead.
Clicking a cover sets the list's rows' selection (`LVM_SETITEMSTATE`); Play and Enqueue
are ml_playlists' own buttons (ids 1003 and 1004).

## Building and testing

```
msbuild playlist_covers\playlist_covers.vcxproj /p:Configuration=Release /p:Platform=Win32
msbuild playlist_covers\tests\playlist_covers_tests.vcxproj /p:Configuration=Release /p:Platform=Win32
bin\Win32\Release\playlist_covers_tests.exe
powershell -ExecutionPolicy Bypass -File tools\playlist-covers-test.ps1
```

The last runs the plugin in a scratch copy of Winamp, on a desktop of its own. In the copy
it replaces the library's playlists with two of silent songs it makes with ffmpeg
(`C:\ffmpeg\bin\ffmpeg.exe`), opens the library on the first, and checks the covers through
the pane's probe message (`WM_APP + 9`, see `Pane::Probe`) and the library's tree. With
`-MusicFolder <folder>` the playlist is that folder's songs instead; nothing is played, and
it times the tags and covers.

To install, copy `bin\Win32\Release\gen_playlist_covers.dll` into Winamp's `Plugins`
folder and restart Winamp.

## Where things are

```
src\plugin.cpp        the plugin: finding the playlist views, Winamp's messages
src\pane.cpp          the covers in a playlist view: placing, drawing, mouse, keys, menu
src\store.cpp         the tags and covers read so far, shared by every view
src\loader.cpp        the threads that read tags and covers
src\covers_model.cpp  the songs and how they make covers
src\playlist_file.cpp a saved playlist's file, and matching the list's rows to it
src\layout.cpp        the grid, the scroll bar, the split with the list
src\tag_cache.cpp     the tags kept between sessions
src\art_scale.cpp     a cover cut square and scaled
src\draw.cpp          drawing in the skin's colours
src\no_art.cpp        the library's llama, for a cover without a picture
src\wasabi.h          the Winamp services used: album art, metadata, memory, playlists
tests\                unit tests for the parts without a window
```

The Wasabi interfaces are declared here from Winamp's source (`Src/Agave`, `Src/Wasabi`,
`Src/playlist`), and ml_playlists' control ids and `TITLE` property from
`Src/Plugins/Library/ml_playlists`; the installed 5.666 ml_playlists.dll was checked to have
the same.
