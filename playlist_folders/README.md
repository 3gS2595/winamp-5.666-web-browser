# Playlist Folders

`ml_playlist_folders.dll`, a Winamp media library plugin of its own, unrelated to the
browser. It adds folders under Library > Playlists, to put saved playlists in, and folders
in folders. Winamp has no way of its own to do this: its Playlists list is flat, in Winamp
5.666 and in the 5.9 source alike (`playlists.xml` has no place for a folder).

The playlists stay ml_playlists' own: their files, their views, their menus, their keys,
Play, Send To, Rename, Delete, sorting, all as before. The plugin only moves their items
in the tree into its folders, and keeps which playlist is in which folder in a file of its
own. Without the plugin, the playlists are back directly under Playlists, as they were.

- **Making folders.** Right-click Playlists: *New Folder* (below ml_playlists' own items).
  Right-click a folder: *New Folder* makes one inside it. The new folder's name is typed
  in the tree, as in Explorer. `Ctrl+Shift+N` does the same on a folder, on Playlists, or
  on a playlist (a folder beside it).
- **Putting playlists in.** Drag a playlist onto a folder, or right-click it: *Move To
  Folder* lists Playlists and every folder, and *New Folder...* makes one beside the
  playlist and moves it in. Dragged onto Playlists, or onto a playlist in another folder,
  it goes there. Dragged among its neighbours it moves in the library's list, as before.
  Folders are dragged the same way, onto another folder or onto Playlists.
- **A folder's view.** Clicking a folder shows what is in it, folders first, with each
  one's songs and length, like the library's own Playlists view: double-click a playlist
  to play it (or enqueue, as the library is set), a folder to open it; Enter plays what is
  picked, Shift+Enter enqueues it; Play and Enqueue below play what is picked, or the
  whole folder with nothing picked; *New Folder* makes one in it. Right-click rows: Play,
  Enqueue, View Playlist / Open Folder, Move To, and for a folder Rename and Delete.
- **On a folder in the tree.** Right-click: Play, Enqueue (all its playlists, its folders'
  first), *New Playlist Here...* (ml_playlists' own New Playlist, the playlist made in the
  folder; `Shift+Insert`), New Folder, Move To, *Rename Folder* (`F2`), *Delete Folder*
  (`Del`). Enter plays the folder. Deleting a folder deletes no playlist: what was in it
  goes to the folder above (it asks first when there is something in it).
- **Order.** In each place, folders come first, in name order (as Explorer sorts them,
  numbers by value), then the playlists in the library's own order, so ml_playlists'
  dragging and *Sort playlists by* still order them. Two folders of one name in one place
  can't be; the second gets "(2)".
- **Kept.** Folders, which playlist is in which, and which folders are open, in
  `Plugins\ml\playlist_folders.txt` in Winamp's settings folder (UTF-8 text, written
  beside it and then put in its place). A playlist is known there by its file. Deleted
  playlists are forgotten. The library opening on the playlist it was on works inside
  folders too: the library saves a folder in `last_view` by its lasting name
  (`Playlists/PlaylistFolder3/My playlist`), which a rename doesn't change.

## How it works

ml_playlists makes one tree item for each saved playlist, directly under Playlists (id
3001): at start (ids 3002 and up, in its list's order), for a new playlist, and when it
moves one (it takes the item out and puts it back, at its new place in its list). It knows
its items only by their ids.

- **Its items, into folders.** The library window (the plugin header's
  `hwndLibraryParent`, gen_ml's main dialog) is subclassed. ml_playlists' messages to the
  library that make an item under Playlists (`ML_IPC_NAVCTRL_INSERTITEM`,
  `ML_IPC_TREEITEM_INSERTW`) are changed on their way: the parent becomes the playlist's
  folder, the place the one its order gives. An item moved by the plugin is taken out and
  put back, with all in it, keeping its id (and its picture, style and open state), so
  ml_playlists finds it as before. A move among siblings (a folder renamed, a playlist's
  new place in its folder) uses `ML_IPC_NAVITEM_MOVE`, which needs no taking out.
- **Which playlist an item is.** By what ml_playlists sends: an item it just took out
  coming back with the same name is the same playlist; at start its ids are 3002 plus the
  place in its list; otherwise the playlist of that name not in the tree yet. When it
  sorts its list it keeps its items and gives each the name of the playlist now in its
  place (`ML_IPC_TREEITEM_SETINFOW` for each, in order), so after a sort the n-th of those
  is the n-th playlist, and every item is put back in its folder.
- **Its messages from the library.** gen_ml loads ml_playlists before any other library
  plugin (its preload list), so it hears of its items first. The plugin swaps
  ml_playlists' `MessageProc` in its plugin header for one of its own, which passes
  everything on but: a drop of a playlist onto a folder (or into another folder), done by
  the plugin; the right-click menu on Playlists or a playlist, during which the library's
  skinned menu (`ML_IPC_TRACKSKINNEDPOPUPEX`) gets the plugin's items added at the bottom
  (ids from a range the menu doesn't use, taken out again after); and `Ctrl+Shift+N`.
  Plugins quit in the reverse of their loading, so the plugin puts the `MessageProc` back
  while ml_playlists is still there.
- **New Playlist Here** is ml_playlists' own New Playlist, from its menu on Playlists, the
  menu not shown (its item, `IDC_NEWPLAYLIST` 40059, given back at once); the playlist it
  then makes goes into the folder.
- **Folders** are the plugin's own items: ids `0x46440000` plus the folder's key,
  `NIS_HASCHILDREN | NIS_ALLOWEDIT`, no picture of their own, so the library draws its
  own open and shut folder. The folder view is a dialog skinned by the library as
  ml_playlists skins its Playlists view, its pictures the tree's own
  (`ML_IPC_IMAGELIST_GETREALINDEX` in the list's colours). ml_playlists can't take a
  folder out (it would, by its place under Playlists, when it has lost track of an item).

The plugin never writes ml_playlists' files or changes its list; `playlists.xml` stays as
ml_playlists writes it.

## Building and testing

```
msbuild playlist_folders\playlist_folders.vcxproj /p:Configuration=Release /p:Platform=Win32
msbuild playlist_folders\tests\playlist_folders_tests.vcxproj /p:Configuration=Release /p:Platform=Win32
bin\Win32\Release\playlist_folders_tests.exe
powershell -ExecutionPolicy Bypass -File tools\playlist-folders-test.ps1
powershell -ExecutionPolicy Bypass -File tools\playlist-folders-test.ps1 -RealPlaylists
```

The unit tests cover the folders and their file. The script runs the plugin in a scratch
copy of Winamp (copied once to `%TEMP%\playlist-folders-test`), on a desktop of its own,
with five playlists of silent songs in place of the library's: folders made, moved,
renamed and deleted, playlists moved and dropped, ml_playlists' own moves, sorting,
renaming and deleting, its New Playlist into a folder, the folder view and its Play, the
menus (driven with the keys), F2, and all of it again after a restart. The plugin takes
the script's commands as `WM_COPYDATA` (`dwData` `'PFT1'`) to its message-only window
(class `PlaylistFoldersHost`). `-RealPlaylists` does a shorter round with a copy of the
user's own saved playlists.

## Not tested

Dragging with a real mouse (the tests call the handlers the library calls on a drop),
`Ctrl+Shift+N` and `Shift+Insert` (the tests can't hold Ctrl or Shift down in Winamp), the
right-click menu on the folder view's rows, a modern skin, a language pack (the plugin
uses ml_playlists' ids, not its words), a library of thousands of playlists.

When a playlist is dragged over a folder directly beside it (both directly under
Playlists, or both in one folder), the library shows a line between items rather than the
folder lit, as it does for any item with the same parent; dropped on the folder's line,
the playlist goes into the folder all the same.
