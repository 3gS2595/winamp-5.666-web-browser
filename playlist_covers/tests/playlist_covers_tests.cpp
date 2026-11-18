#include "../../tests/test_framework.h"

#include "art_scale.h"
#include "covers_model.h"
#include "layout.h"
#include "playlist_file.h"
#include "settings.h"
#include "tag_cache.h"

#include <windows.h>

#include <cstring>
#include <string>
#include <vector>

using namespace covers;

namespace {

Track Song(std::wstring file, std::wstring title, std::wstring artist, std::wstring album, std::wstring albumArtist = L"") {
    Track track;
    track.file = std::move(file);
    track.tags = Tags{std::move(title), std::move(artist), std::move(album), std::move(albumArtist)};
    return track;
}

Track Untagged(std::wstring file, std::wstring listTitle) {
    Track track;
    track.file = std::move(file);
    track.listTitle = std::move(listTitle);
    return track;
}

std::wstring Names(const std::vector<Group>& groups) {
    std::wstring names;
    for (const Group& group : groups) {
        if (!names.empty()) names += L", ";
        names += group.name;
    }
    return names;
}

} // namespace

// ---- Grouping

TEST_CASE(SongsOfAnAlbumShareACover) {
    const std::vector<Track> tracks{
        Song(L"C:\\Music\\III\\01.mp3", L"Freed Pig", L"Sebadoh", L"III"),
        Song(L"C:\\Music\\mp-3\\single.mp3", L"Superstar", L"Lupe Fiasco", L""),
        Song(L"C:\\Music\\III\\02.mp3", L"Total Peace", L"Sebadoh", L"III"),
    };
    const auto groups = GroupTracks(tracks, GroupOrder::kPlaylist);
    EXPECT_EQ(groups.size(), size_t{2});
    EXPECT_EQ(groups[0].name, std::wstring(L"III"));
    EXPECT_EQ(groups[0].artist, std::wstring(L"Sebadoh"));
    EXPECT((groups[0].tracks == std::vector<int>{0, 2}));
    EXPECT(!groups[0].single);
    // A song without an album is a cover of its own, named after the song.
    EXPECT_EQ(groups[1].name, std::wstring(L"Superstar"));
    EXPECT_EQ(groups[1].artist, std::wstring(L"Lupe Fiasco"));
    EXPECT(groups[1].single);
}

TEST_CASE(AlbumNamesAreMatchedWithoutCase) {
    const std::vector<Track> tracks{
        Song(L"C:\\a\\1.mp3", L"One", L"X", L"Breaker"),
        Song(L"C:\\a\\2.mp3", L"Two", L"X", L"BREAKER"),
    };
    EXPECT_EQ(GroupTracks(tracks, GroupOrder::kPlaylist).size(), size_t{1});
}

TEST_CASE(SameAlbumNameInOtherFoldersIsAnotherAlbum) {
    const std::vector<Track> tracks{
        Song(L"C:\\a\\1.mp3", L"One", L"Artist A", L"Greatest Hits"),
        Song(L"C:\\b\\1.mp3", L"Uno", L"Artist B", L"Greatest Hits"),
    };
    EXPECT_EQ(GroupTracks(tracks, GroupOrder::kPlaylist).size(), size_t{2});
}

TEST_CASE(AnAlbumArtistHoldsAnAlbumTogetherAcrossFolders) {
    const std::vector<Track> tracks{
        Song(L"C:\\cd1\\1.mp3", L"One", L"Guest", L"Box Set", L"The Band"),
        Song(L"C:\\cd2\\1.mp3", L"Two", L"Other", L"Box Set", L"The Band"),
    };
    const auto groups = GroupTracks(tracks, GroupOrder::kPlaylist);
    EXPECT_EQ(groups.size(), size_t{1});
    EXPECT_EQ(groups[0].artist, std::wstring(L"The Band"));
}

TEST_CASE(AnAlbumOfManyArtistsIsVarious) {
    const std::vector<Track> tracks{
        Song(L"C:\\nts\\1.mp3", L"One", L"MC A", L"funk.BR"),
        Song(L"C:\\nts\\2.mp3", L"Two", L"MC B", L"funk.BR"),
    };
    EXPECT_EQ(GroupTracks(tracks, GroupOrder::kPlaylist)[0].artist, std::wstring(kVariousArtists));
}

TEST_CASE(TheSameFileTwiceIsOneCover) {
    const std::vector<Track> tracks{Untagged(L"C:\\x.mp3", L"A - B"), Untagged(L"C:\\X.MP3", L"A - B")};
    const auto groups = GroupTracks(tracks, GroupOrder::kPlaylist);
    EXPECT_EQ(groups.size(), size_t{1});
    EXPECT_EQ(groups[0].tracks.size(), size_t{2});
}

TEST_CASE(CoversCanBeArrangedByArtistOrAlbum) {
    const std::vector<Track> tracks{
        Song(L"C:\\1\\a.mp3", L"t", L"Zed", L"Alpha"),
        Song(L"C:\\2\\a.mp3", L"t", L"Abba", L"Omega"),
        Song(L"C:\\3\\a.mp3", L"t", L"", L"Disc 10"),
        Song(L"C:\\4\\a.mp3", L"t", L"", L"Disc 2"),
    };
    EXPECT_EQ(Names(GroupTracks(tracks, GroupOrder::kPlaylist)), std::wstring(L"Alpha, Omega, Disc 10, Disc 2"));
    // No artist goes last; numbers in names go by their value.
    EXPECT_EQ(Names(GroupTracks(tracks, GroupOrder::kArtist)), std::wstring(L"Omega, Alpha, Disc 2, Disc 10"));
    EXPECT_EQ(Names(GroupTracks(tracks, GroupOrder::kAlbum)), std::wstring(L"Alpha, Disc 2, Disc 10, Omega"));
}

// ---- What the list shows

TEST_CASE(UntaggedSongsAreTakenApartFromThePlaylistTitle) {
    const Track track = Untagged(L"C:\\m\\Green Day - Good Riddance.mp3", L"Green Day - Good Riddance");
    EXPECT_EQ(TitleOf(track), std::wstring(L"Good Riddance"));
    EXPECT_EQ(ArtistOf(track), std::wstring(L"Green Day"));
    EXPECT_EQ(AlbumOf(track), std::wstring());
}

TEST_CASE(ATitleTagIsNotTakenApart) {
    Track track = Song(L"C:\\a.mp3", L"Holding On - Original Mix", L"", L"");
    track.listTitle = L"Holding On - Original Mix";
    EXPECT_EQ(TitleOf(track), std::wstring(L"Holding On - Original Mix"));
    EXPECT_EQ(ArtistOf(track), std::wstring());
}

TEST_CASE(ASongWithNothingIsNamedAfterItsFile) {
    const Track track = Untagged(L"C:\\Music\\track01.mp3", L"");
    EXPECT_EQ(TitleOf(track), std::wstring(L"track01"));
    EXPECT_EQ(TitleOf(Untagged(L"C:\\b.mp3", L"Just a name")), std::wstring(L"Just a name"));
}

TEST_CASE(PathsComeApart) {
    EXPECT_EQ(std::wstring(FolderOf(L"C:\\a\\b.mp3")), std::wstring(L"C:\\a"));
    EXPECT_EQ(std::wstring(FolderOf(L"b.mp3")), std::wstring());
    EXPECT_EQ(std::wstring(FileStem(L"C:\\a\\b.c.mp3")), std::wstring(L"b.c"));
    EXPECT_EQ(std::wstring(FileStem(L"C:/a/.hidden")), std::wstring(L".hidden"));
}

TEST_CASE(TagsAreCleaned) {
    EXPECT_EQ(CleanTag(L"  Two\tLines\r\nHere  "), std::wstring(L"Two Lines  Here"));
    EXPECT_EQ(CleanTag(L" \t "), std::wstring());
}

// ---- Search

TEST_CASE(SearchNeedsEveryWordAnywhere) {
    const Track track = Song(L"C:\\Music\\Sebadoh\\III\\01 Freed Pig.mp3", L"Freed Pig", L"Sebadoh", L"III");
    const std::wstring text = SearchText(track);
    EXPECT(MatchesAll(text, SearchWords(L"sebadoh PIG")));
    EXPECT(MatchesAll(text, SearchWords(L"  freed  ")));
    EXPECT(MatchesAll(text, SearchWords(L"")));
    EXPECT(!MatchesAll(text, SearchWords(L"sebadoh dinosaur")));
    EXPECT_EQ(SearchWords(L" a  bC ").size(), size_t{2});
    EXPECT_EQ(SearchWords(L" a  bC ")[1], std::wstring(L"bc"));
}

// ---- Layout

// Measured on the library's Audio view in its "medium icons" mode, with a 17 pixel font:
// covers 94 pixels apart across and 111 down, the first 2 pixels in from the corner.
TEST_CASE(TheGridIsLaidOutAsTheLibrarysAlbumView) {
    const Grid grid = MakeGrid(670, 90, 17);
    EXPECT_EQ(grid.cellWidth, 94);
    EXPECT_EQ(grid.cellHeight, 111);
    EXPECT_EQ(grid.columns, 7); // what is left over stays at the right
    EXPECT((grid.Art(0) == Box{2, 2, 92, 92}));
    EXPECT((grid.Art(1) == Box{96, 2, 186, 92}));
    EXPECT((grid.Art(7) == Box{2, 113, 92, 203}));
    // Its name, centred in the library's own rectangle for it, as wide as the cover.
    EXPECT((grid.Name(0) == Box{2, 85, 92, 111}));
    // A picked one is filled but for the bottom 4 pixels.
    EXPECT((grid.Highlight(0) == Box{0, 0, 94, 107}));
    EXPECT_EQ(MakeGrid(50, 90, 17).columns, 1);
}

TEST_CASE(CoversOnlyLeaveTheNamesOut) {
    const Grid grid = MakeGrid(670, 90, 0);
    EXPECT_EQ(grid.cellHeight, 94);
    EXPECT((grid.Highlight(0) == Box{0, 0, 94, 94}));
}

TEST_CASE(TheLibrarysViewModesGiveItsCoverSizes) {
    EXPECT_EQ(ArtSizeOfViewMode(0), 60);
    EXPECT_EQ(ArtSizeOfViewMode(1), 90);
    EXPECT_EQ(ArtSizeOfViewMode(2), 120);
    EXPECT_EQ(ArtSizeOfViewMode(3), 60);
    EXPECT_EQ(ArtSizeOfViewMode(4), 90);
    EXPECT_EQ(ArtSizeOfViewMode(5), 120);
    EXPECT_EQ(ArtSizeOfViewMode(6), 180);
    EXPECT_EQ(ArtSizeOfViewMode(42), 90);
}

TEST_CASE(TilesAreFoundUnderThePointer) {
    const Grid grid = MakeGrid(500, 90, 17);
    EXPECT_EQ(grid.TileAt(10, 100, 10), 1);
    EXPECT_EQ(grid.TileAt(1, 100, 10), -1);   // there is no second tile
    EXPECT_EQ(grid.TileAt(10, 490, 10), -1);  // right of the last column
    EXPECT_EQ(grid.TileAt(10, 0, 0), 0);      // the cells start at the corner
    EXPECT_EQ(grid.TileAt(10, 1, 111), grid.columns);
}

TEST_CASE(OnlyTheRowsInSightAreDrawn) {
    const Grid grid = MakeGrid(500, 90, 17);
    int first = 0, last = 0;
    grid.VisibleRange(100, 0, 111, first, last);
    EXPECT_EQ(first, 0);
    EXPECT_EQ(last, grid.columns - 1); // the second row starts below
    grid.VisibleRange(100, 0, 112, first, last);
    EXPECT_EQ(last, 2 * grid.columns - 1); // and now a pixel of it is in sight
    grid.VisibleRange(100, 10 * 111, 11 * 111, first, last);
    EXPECT_EQ(first, 10 * grid.columns);
    grid.VisibleRange(3, 0, 10000, first, last);
    EXPECT_EQ(last, 2);
    EXPECT_EQ(ClampOffset(-5, 1000, 100), 0);
    EXPECT_EQ(ClampOffset(5000, 1000, 100), 900);
    EXPECT_EQ(ClampOffset(50, 80, 100), 0);
}

// The library's own files, as gen_ml writes them.
TEST_CASE(TheAudioViewsLookIsReadFromTheLibrarysSettings) {
    wchar_t temp[MAX_PATH] = {};
    ::GetTempPathW(MAX_PATH, temp);
    const std::wstring folder = std::wstring(temp) + L"playlist_covers_tests_" + std::to_wstring(::GetCurrentProcessId());
    ::CreateDirectoryW(folder.c_str(), nullptr);
    ::CreateDirectoryW((folder + L"\\ml").c_str(), nullptr);
    ::CreateDirectoryW((folder + L"\\ml\\views").c_str(), nullptr);
    const auto write = [](const std::wstring& path, const char* text) {
        const HANDLE file = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        DWORD written = 0;
        ::WriteFile(file, text, static_cast<DWORD>(std::strlen(text)), &written, nullptr);
        ::CloseHandle(file);
    };
    write(folder + L"\\gen_ml.ini",
          "[gen_ml_config]\r\nquery1_name_utf8=Video\r\nquery1_meta=metV.vmd\r\nquery2_name_utf8=Audio\r\nquery2_meta=metA.vmd\r\n");
    write(folder + L"\\ml\\views\\metA.vmd", "[gen_ml_config]\r\nalbumartviewmode=6\r\nalbumarticonmode=1\r\n");
    const AudioViewLook look = ReadAudioViewLook(folder);
    EXPECT_EQ(look.coverSize, 180);
    EXPECT(!look.names);
    // Nothing to go by: the library's own default, medium covers with names.
    const AudioViewLook none = ReadAudioViewLook(folder + L"\\nowhere");
    EXPECT_EQ(none.coverSize, 90);
    EXPECT(none.names);
    for (const wchar_t* file : {L"\\ml\\views\\metA.vmd", L"\\gen_ml.ini"}) ::DeleteFileW((folder + file).c_str());
    for (const wchar_t* dir : {L"\\ml\\views", L"\\ml", L""}) ::RemoveDirectoryW((folder + dir).c_str());
}

TEST_CASE(TheCoversTakeTheTopOfTheListsPlace) {
    const Box area{10, 20, 410, 1020};
    const Split split = SplitArea(area, 450, 70);
    EXPECT((split.covers == Box{10, 20, 410, 470}));
    EXPECT((split.list == Box{10, 470, 410, 1020}));
    // Each keeps the least it is given.
    EXPECT_EQ(SplitArea(area, 10, 70).covers.Height(), 70);
    EXPECT_EQ(SplitArea(area, 990, 70).list.Height(), 70);
    // Without room for both, the list has it all.
    const Split cramped = SplitArea({0, 0, 100, 120}, 450, 70);
    EXPECT_EQ(cramped.covers.Height(), 0);
    EXPECT((cramped.list == Box{0, 0, 100, 120}));
    EXPECT_EQ(ShareFor(area, 20 + 500), 500);
    EXPECT_EQ(ShareFor(area, 0), 100);
}

// ---- Saved playlists

TEST_CASE(PlaylistFilesAreRead) {
    const std::string text =
        "\xEF\xBB\xBF#EXTM3U\r\n"
        "#EXTINF:351,(Fran\xC3\xA7ois K) Deep And Sexy - Omato Grosso\r\n"
        "\\Users\\lucius\\Music\\a.mp3\r\n"
        "\r\n"
        "C:\\Music\\b.mp3\r\n"
        "#EXTINF:-1,Radio\n"
        "http://example.com/stream\n"
        "sub\\c.mp3\n";
    const auto entries = ParsePlaylist(text, L"D:\\Lists", false);
    EXPECT_EQ(entries.size(), size_t{4});
    EXPECT_EQ(entries[0].file, std::wstring(L"D:\\Users\\lucius\\Music\\a.mp3")); // on the playlist's drive
    EXPECT_EQ(entries[0].title, std::wstring(L"(Fran\u00e7ois K) Deep And Sexy - Omato Grosso"));
    EXPECT_EQ(entries[0].seconds, 351);
    EXPECT_EQ(entries[1].file, std::wstring(L"C:\\Music\\b.mp3"));
    EXPECT_EQ(entries[1].title, std::wstring()); // an #EXTINF is for the next file only
    EXPECT_EQ(entries[2].file, std::wstring(L"http://example.com/stream"));
    EXPECT_EQ(entries[3].file, std::wstring(L"D:\\Lists\\sub\\c.mp3")); // beside the playlist
}

TEST_CASE(RowsAreMatchedToTheFilesSongs) {
    const std::vector<std::wstring> file{L"A", L"B", L"A", L"C"};
    EXPECT((MapRows(file, file) == std::vector<int>{0, 1, 2, 3}));
    // Edited and not saved: one taken out, one moved, one added.
    const std::vector<std::wstring> rows{L"C", L"A", L"New", L"A"};
    EXPECT((MapRows(rows, file) == std::vector<int>{3, 0, -1, 2}));
}

TEST_CASE(TheLibrarysRowNumbersAreLeftOut) {
    EXPECT_EQ(std::wstring(WithoutNumber(L"12. 01 Red 1", 12)), std::wstring(L"01 Red 1"));
    EXPECT_EQ(std::wstring(WithoutNumber(L"3. Title", 4)), std::wstring(L"3. Title")); // not its number
    EXPECT_EQ(std::wstring(WithoutNumber(L"Title", 1)), std::wstring(L"Title"));
}

// ---- Covers

TEST_CASE(ScalingDownAveragesThePixels) {
    const std::vector<std::uint32_t> square{0xFF000000, 0xFFFFFFFF, 0xFFFFFFFF, 0xFF000000};
    const auto one = ScaleToSquare(square.data(), 2, 2, 1);
    EXPECT_EQ(one.size(), size_t{1});
    const std::uint32_t pixel = one[0];
    EXPECT_EQ(pixel >> 24, 0xFFu);
    EXPECT(((pixel >> 16) & 0xFF) >= 127 && ((pixel >> 16) & 0xFF) <= 128);
}

TEST_CASE(AWidePictureLosesItsSides) {
    // Red, green, green, red: the middle square is the green.
    const std::vector<std::uint32_t> wide{0xFFFF0000, 0xFF00FF00, 0xFF00FF00, 0xFFFF0000,
                                          0xFFFF0000, 0xFF00FF00, 0xFF00FF00, 0xFFFF0000};
    const auto square = ScaleToSquare(wide.data(), 4, 2, 2);
    EXPECT_EQ(square.size(), size_t{4});
    for (const std::uint32_t pixel : square) EXPECT_EQ(pixel, 0xFF00FF00u);
}

TEST_CASE(ScalingUpKeepsTheColour) {
    const std::uint32_t blue = 0xFF0000FF;
    const auto big = ScaleToSquare(&blue, 1, 1, 3);
    EXPECT_EQ(big.size(), size_t{9});
    for (const std::uint32_t pixel : big) EXPECT_EQ(pixel, blue);
    EXPECT(ScaleToSquare(nullptr, 1, 1, 3).empty());
    EXPECT(ScaleToSquare(&blue, 1, 1, 0).empty());
}

TEST_CASE(SeeThroughPixelsStayPremultiplied) {
    const std::vector<std::uint32_t> half{0x80808080, 0x00000000};
    const auto one = ScaleToSquare(half.data(), 2, 1, 1);
    const std::uint32_t pixel = one[0];
    EXPECT(((pixel >> 16) & 0xFF) <= (pixel >> 24));
}

// ---- Tag cache

TEST_CASE(TheTagCacheReadsBackWhatItWrote) {
    TagCache cache;
    cache[L"c:\\music\\b\u00e4r.mp3"] = {0x1D9A2B3C4D5E6F70, 12345, Tags{L"T\u00eftle", L"Art\tist", L"Al\nbum", L""}, true};
    cache[L"c:\\x.m4a"] = {1, 2, Tags{}, false};
    const TagCache back = ParseTagCache(SerializeTagCache(cache, 100));
    EXPECT_EQ(back.size(), size_t{2});
    const CachedTags& entry = back.at(L"c:\\music\\b\u00e4r.mp3");
    EXPECT_EQ(entry.written, std::uint64_t{0x1D9A2B3C4D5E6F70});
    EXPECT_EQ(entry.size, std::uint64_t{12345});
    EXPECT_EQ(entry.tags.title, std::wstring(L"T\u00eftle"));
    EXPECT_EQ(entry.tags.artist, std::wstring(L"Art ist")); // the separators can't be in a field
    EXPECT_EQ(entry.tags.album, std::wstring(L"Al bum"));
    EXPECT(!entry.used);
}

TEST_CASE(TheTagCacheKeepsThisSessionsFilesOverTheLimit) {
    TagCache cache;
    cache[L"old"] = {1, 1, Tags{}, false};
    cache[L"new"] = {1, 1, Tags{}, true};
    const TagCache back = ParseTagCache(SerializeTagCache(cache, 1));
    EXPECT_EQ(back.size(), size_t{1});
    EXPECT(back.contains(L"new"));
}

TEST_CASE(TheTagCacheSkipsWhatItCannotRead) {
    EXPECT(ParseTagCache("something else\nx\t1\t2\ta\tb\tc\td\n").empty());
    const TagCache back = ParseTagCache("playlist_covers tags 1\r\nbad line\nx\tzz\t2\ta\tb\tc\td\ny\t1\t2\ta\tb\tc\td\r\n");
    EXPECT_EQ(back.size(), size_t{1});
    EXPECT_EQ(back.at(L"y").tags.albumArtist, std::wstring(L"d"));
}
