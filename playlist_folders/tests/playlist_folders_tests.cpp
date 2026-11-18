#include "../../tests/test_framework.h"

#include "model.h"

#include <string>
#include <vector>

using namespace folders;

namespace {

std::wstring Names(const Model& model, const std::vector<int>& keys) {
    std::wstring names;
    for (int key : keys) {
        if (!names.empty()) names += L", ";
        names += model.Find(key)->name;
    }
    return names;
}

Entry FolderEntry(std::wstring name) {
    Entry entry;
    entry.folder = true;
    entry.name = std::move(name);
    return entry;
}

Entry PlaylistEntry(std::size_t index) {
    Entry entry;
    entry.index = index;
    return entry;
}

} // namespace

TEST_CASE(FileKeyIsLowerCaseAndRelativeToThePlaylistsFolder) {
    const std::wstring dir = L"C:\\Users\\Me\\AppData\\Roaming\\Winamp\\Plugins\\ml\\playlists";
    EXPECT_EQ(FileKey(dir + L"\\plf1BE6.m3u8", dir), std::wstring(L"plf1be6.m3u8"));
    EXPECT_EQ(FileKey(L"c:/users/me/appdata/roaming/winamp/plugins/ml/playlists/PLF2.m3u8", dir + L"\\"),
              std::wstring(L"plf2.m3u8"));
    EXPECT_EQ(FileKey(L"D:\\Lists\\Party.m3u", dir), std::wstring(L"d:\\lists\\party.m3u"));
    // A folder of a longer name beginning the same is not the playlists folder.
    EXPECT_EQ(FileKey(dir + L"2\\x.m3u8", dir), FileKey(dir + L"2\\x.m3u8", L""));
}

TEST_CASE(NamesAreTidied) {
    EXPECT_EQ(CleanName(L"  Rock  "), std::wstring(L"Rock"));
    EXPECT_EQ(CleanName(L"A\tB\r\nC"), std::wstring(L"A B  C"));
    EXPECT_EQ(CleanName(L"   "), std::wstring());
}

TEST_CASE(FoldersSortAsExplorerSortsThem) {
    EXPECT(NameBefore(L"2000s", L"90s") == false);
    EXPECT(NameBefore(L"Mix 2", L"Mix 10"));
    EXPECT(NameBefore(L"apple", L"Banana"));
    Model model;
    model.Add(0, L"Rock");
    model.Add(0, L"chill");
    model.Add(0, L"Mix 10");
    model.Add(0, L"Mix 2");
    EXPECT_EQ(Names(model, model.ChildFolders(0)), std::wstring(L"chill, Mix 2, Mix 10, Rock"));
}

TEST_CASE(NewFoldersGetANameOfTheirOwnInTheirParent) {
    Model model;
    const int a = model.Add(0, L"New folder");
    const int b = model.Add(0, L"New folder");
    const int c = model.Add(0, L"new FOLDER");
    const int inside = model.Add(a, L"New folder");
    EXPECT_EQ(model.Find(a)->name, std::wstring(L"New folder"));
    EXPECT_EQ(model.Find(b)->name, std::wstring(L"New folder (2)"));
    EXPECT_EQ(model.Find(c)->name, std::wstring(L"new FOLDER (3)"));
    EXPECT_EQ(model.Find(inside)->name, std::wstring(L"New folder"));
    EXPECT_EQ(model.Add(0, L"  "), 0);
    EXPECT_EQ(model.Add(99, L"Orphan"), 0);
    EXPECT(model.Rename(b, L"new folder"));
    EXPECT_EQ(model.Find(b)->name, std::wstring(L"new folder (2)"));
    EXPECT(model.Rename(b, L"Rock"));
    EXPECT_EQ(model.Find(b)->name, std::wstring(L"Rock"));
    // Renaming to its own name, or the same but for case, keeps it.
    EXPECT(model.Rename(b, L"ROCK"));
    EXPECT_EQ(model.Find(b)->name, std::wstring(L"ROCK"));
    EXPECT(!model.Rename(b, L""));
}

TEST_CASE(PlaylistsArePlacedAndTakenOut) {
    Model model;
    const int rock = model.Add(0, L"Rock");
    model.Place(L"plf1.m3u8", rock);
    model.Place(L"plf2.m3u8", rock);
    model.Place(L"plf3.m3u8", 77); // no such folder: stays out
    EXPECT_EQ(model.FolderOf(L"plf1.m3u8"), rock);
    EXPECT_EQ(model.FolderOf(L"plf3.m3u8"), 0);
    EXPECT_EQ(model.PlaylistsIn(rock).size(), std::size_t{2});
    model.Place(L"plf1.m3u8", 0);
    EXPECT_EQ(model.FolderOf(L"plf1.m3u8"), 0);
    EXPECT_EQ(model.PlacedCount(), std::size_t{1});
}

TEST_CASE(RemovingAFolderGivesWhatWasInItToItsParent) {
    Model model;
    const int music = model.Add(0, L"Music");
    const int rock = model.Add(music, L"Rock");
    const int classic = model.Add(rock, L"Classic");
    const int samename = model.Add(music, L"Classic");
    model.Place(L"a.m3u8", rock);
    model.Place(L"b.m3u8", classic);
    model.Remove(rock);
    EXPECT(!model.Exists(rock));
    EXPECT_EQ(model.FolderOf(L"a.m3u8"), music);
    EXPECT_EQ(model.FolderOf(L"b.m3u8"), classic);
    EXPECT_EQ(model.Find(classic)->parent, music);
    // Two "Classic" now in Music: the one moved up gets a number.
    EXPECT_EQ(model.Find(samename)->name, std::wstring(L"Classic"));
    EXPECT_EQ(model.Find(classic)->name, std::wstring(L"Classic (2)"));
    model.Remove(music);
    EXPECT_EQ(model.FolderOf(L"a.m3u8"), 0);
    EXPECT_EQ(model.PlacedCount(), std::size_t{1}); // a.m3u8 is out of any folder now
    EXPECT_EQ(model.Find(classic)->parent, 0);
}

TEST_CASE(AFolderCantGoInsideItself) {
    Model model;
    const int a = model.Add(0, L"A");
    const int b = model.Add(a, L"B");
    const int c = model.Add(b, L"C");
    EXPECT(!model.MoveFolder(a, c));
    EXPECT(!model.MoveFolder(a, a));
    EXPECT(model.IsWithin(c, a));
    EXPECT(!model.IsWithin(a, c));
    EXPECT(model.MoveFolder(c, 0));
    EXPECT_EQ(model.Find(c)->parent, 0);
    EXPECT(model.MoveFolder(a, c));
    EXPECT(model.PathOf(b) == (std::vector<std::wstring>{L"C", L"A", L"B"}));
    EXPECT(model.TreeOrder() == (std::vector<int>{c, a, b}));
}

TEST_CASE(TheFileKeepsEverything) {
    Model model;
    const int music = model.Add(0, L"Music ünïcode");
    const int rock = model.Add(music, L"Rock");
    model.SetExpanded(music, true);
    model.Place(L"plf1be6.m3u8", rock);
    model.Place(L"d:\\lists\\party.m3u", music);
    const Model back = Model::Parse(model.Serialize());
    EXPECT_EQ(back.Folders().size(), std::size_t{2});
    EXPECT_EQ(back.Find(music)->name, std::wstring(L"Music ünïcode"));
    EXPECT(back.Find(music)->expanded);
    EXPECT(!back.Find(rock)->expanded);
    EXPECT_EQ(back.Find(rock)->parent, music);
    EXPECT_EQ(back.FolderOf(L"plf1be6.m3u8"), rock);
    EXPECT_EQ(back.FolderOf(L"d:\\lists\\party.m3u"), music);
    // Keys aren't reused after a reload.
    Model again = back;
    const int fresh = again.Add(0, L"Fresh");
    EXPECT(fresh > rock && fresh > music);
}

TEST_CASE(ABrokenFileIsReadAsFarAsItMakesSense) {
    const std::string text = "\xEF\xBB\xBF# comment\r\n"
                             "version\t1\n"
                             "folder\t1\t0\t1\tGood\n"
                             "folder\t2\t9\t0\tLost parent\n"
                             "folder\t3\t4\t0\tLoop A\n"
                             "folder\t4\t3\t0\tLoop B\n"
                             "folder\tx\t0\t0\tBad key\n"
                             "folder\t5\t0\t0\t\n"
                             "folder\t1\t0\t0\tSame key again\n"
                             "playlist\t1\tPLF9.m3u8\n"
                             "playlist\t42\tnowhere.m3u8\n"
                             "something else\n";
    const Model model = Model::Parse(text);
    EXPECT_EQ(model.Folders().size(), std::size_t{4});
    EXPECT_EQ(model.Find(1)->name, std::wstring(L"Good"));
    EXPECT_EQ(model.Find(2)->parent, 0);
    // One of the loop is moved to the top, so the other is inside it.
    EXPECT(model.Find(3)->parent == 0 || model.Find(4)->parent == 0);
    EXPECT_EQ(model.FolderOf(L"plf9.m3u8"), 1);
    EXPECT_EQ(model.FolderOf(L"nowhere.m3u8"), 0);
    EXPECT_EQ(model.TreeOrder().size(), std::size_t{4});
}

TEST_CASE(PlaylistsThatAreGoneAreForgotten) {
    Model model;
    const int a = model.Add(0, L"A");
    model.Place(L"kept.m3u8", a);
    model.Place(L"gone.m3u8", a);
    EXPECT(model.ForgetMissing([](std::wstring_view key) { return key == L"kept.m3u8"; }));
    EXPECT_EQ(model.FolderOf(L"kept.m3u8"), a);
    EXPECT_EQ(model.FolderOf(L"gone.m3u8"), 0);
    EXPECT(!model.ForgetMissing([](std::wstring_view) { return true; }));
}

TEST_CASE(ItemsGoInOrderFoldersFirst) {
    // Two folders, then playlists 1, 4, 7 of the library's list.
    const std::vector<Entry> existing = {FolderEntry(L"B"), FolderEntry(L"D"), PlaylistEntry(1), PlaylistEntry(4),
                                         PlaylistEntry(7)};
    EXPECT_EQ(InsertPosition(existing, FolderEntry(L"A")), std::size_t{0});
    EXPECT_EQ(InsertPosition(existing, FolderEntry(L"C")), std::size_t{1});
    EXPECT_EQ(InsertPosition(existing, FolderEntry(L"Z")), std::size_t{2});
    EXPECT_EQ(InsertPosition(existing, PlaylistEntry(0)), std::size_t{2});
    EXPECT_EQ(InsertPosition(existing, PlaylistEntry(5)), std::size_t{4});
    EXPECT_EQ(InsertPosition(existing, PlaylistEntry(9)), std::size_t{5});
    EXPECT_EQ(InsertPosition({}, PlaylistEntry(3)), std::size_t{0});
    // Out of order already: after the last one that belongs before it.
    const std::vector<Entry> muddled = {PlaylistEntry(5), PlaylistEntry(2), PlaylistEntry(8)};
    EXPECT_EQ(InsertPosition(muddled, PlaylistEntry(3)), std::size_t{2});
    EXPECT(EntryBefore(FolderEntry(L"Zzz"), PlaylistEntry(0)));
    EXPECT(!EntryBefore(PlaylistEntry(0), FolderEntry(L"A")));
}
