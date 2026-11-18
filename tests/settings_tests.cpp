#include "test_framework.h"

#include "settings.h"

#include <map>
#include <set>

using namespace exodium;

namespace {

using Section = std::map<std::wstring, std::wstring, std::less<>>;

// A section of winamp.ini, read the way the plugin reads it.
Settings Read(const Section& section) {
    return ReadSettings([&section](std::wstring_view key) -> std::optional<std::wstring> {
        const auto found = section.find(key);
        if (found == section.end()) return std::nullopt;
        return found->second;
    });
}

Section Written(const Settings& settings) {
    Section section;
    for (const SettingEntry& entry : SettingEntries(settings)) section.emplace(entry.key, entry.value);
    return section;
}

} // namespace

TEST_CASE(SettingsStartAsTheBrowserAlwaysWas) {
    const Settings settings;
    EXPECT_EQ(settings.homePage, L"https://google.com/");
    EXPECT_EQ(settings.searchEngine, L"google");
    EXPECT_EQ(settings.customSearchUrl, L"");
    EXPECT_EQ(settings.cornerSkin, L"iron");
    EXPECT(settings.tabCloseButtons);
    EXPECT(Read({}) == settings);
}

TEST_CASE(SettingsAreReadFromTheSection) {
    const Settings settings = Read({
        {L"home_page", L"example.com"},
        {L"search_engine", L"DuckDuckGo"},
        {L"custom_search_url", L"kagi.com/search?q=%s"},
        {L"tab_close_buttons", L"0"},
        {L"corner_skin", L" Venom "},
        {L"remote_debugging_port", L"9333"},
    });
    EXPECT_EQ(settings.homePage, L"https://example.com");
    EXPECT_EQ(settings.searchEngine, L"duckduckgo");
    EXPECT_EQ(settings.customSearchUrl, L"https://kagi.com/search?q=%s");
    EXPECT_EQ(settings.cornerSkin, L"venom");
    EXPECT(!settings.tabCloseButtons);

    EXPECT_EQ(Read({{L"search_engine", L"Custom"}}).searchEngine, L"custom");
}

TEST_CASE(SettingsThatMakeNoSenseKeepTheirDefaults) {
    const Settings settings = Read({
        {L"home_page", L"   "},
        {L"search_engine", L"altavista"},
        {L"tab_close_buttons", L"sometimes"},
        {L"corner_skin", L"chrome"},
    });
    EXPECT(settings == Settings{});
}

TEST_CASE(SettingsWrittenAreReadBackTheSame) {
    Settings settings;
    settings.homePage = L"https://example.com/start";
    settings.searchEngine = std::wstring(kCustomSearchId);
    settings.customSearchUrl = L"https://kagi.com/search?q=%s";
    settings.tabCloseButtons = false;
    settings.cornerSkin = L"venom";
    EXPECT(Read(Written(settings)) == settings);
    EXPECT_EQ(Written(settings).at(L"tab_close_buttons"), L"0");
    EXPECT_EQ(Written(Settings{}).at(L"tab_close_buttons"), L"1");
    EXPECT_EQ(Written(settings).at(L"corner_skin"), L"venom");
    EXPECT_EQ(Written(Settings{}).at(L"corner_skin"), L"iron");
}

TEST_CASE(OnlyWhatChangedIsWritten) {
    const Settings before;
    Settings after = before;
    EXPECT(ChangedEntries(before, after).empty());

    after.searchEngine = L"bing";
    const std::vector<SettingEntry> changed = ChangedEntries(before, after);
    EXPECT_EQ(changed.size(), std::size_t{1});
    if (!changed.empty()) {
        EXPECT_EQ(std::wstring(changed[0].key), L"search_engine");
        EXPECT_EQ(changed[0].value, L"bing");
    }
}

// Each setting that is on or off has a key of its own, and is written and read back
// under it with nothing else.
TEST_CASE(EveryFlagIsKeptUnderItsOwnKey) {
    std::set<std::wstring_view> keys;
    for (const FlagSetting& flag : kFlagSettings) {
        EXPECT(keys.insert(flag.key).second);
        Settings flipped;
        flipped.*flag.member = !(flipped.*flag.member);
        const std::vector<SettingEntry> changed = ChangedEntries(Settings{}, flipped);
        EXPECT_EQ(changed.size(), std::size_t{1});
        if (!changed.empty()) EXPECT(changed[0].key == flag.key);
        EXPECT(Read(Written(flipped)) == flipped);
    }
}

TEST_CASE(FlagsAreReadAsWinampAndPeopleWriteThem) {
    for (const std::wstring_view on : {L"1", L"true", L"Yes", L" ON "}) EXPECT(ParseFlag(on) == true);
    for (const std::wstring_view off : {L"0", L"FALSE", L"no", L"off"}) EXPECT(ParseFlag(off) == false);
    for (const std::wstring_view neither : {L"", L"2", L"maybe", L"10"}) EXPECT(!ParseFlag(neither));
}

TEST_CASE(HomePagesAreTakenAsTheAddressBarTakesThem) {
    EXPECT_EQ(NormalizeHomePage(L"example.com"), L"https://example.com");
    EXPECT_EQ(NormalizeHomePage(L"  https://example.com/a?b=c  "), L"https://example.com/a?b=c");
    EXPECT_EQ(NormalizeHomePage(L"about:blank"), L"about:blank");
    EXPECT_EQ(NormalizeHomePage(L"file:///C:/start.html"), L"file:///C:/start.html");
    EXPECT_EQ(NormalizeHomePage(L"exodium://history"), kHistoryUrl);
    EXPECT_EQ(NormalizeHomePage(L""), L"https://google.com/");
    EXPECT_EQ(NormalizeHomePage(L" \t "), L"https://google.com/");
    // Words are a search, as they would be in the address bar.
    EXPECT_EQ(NormalizeHomePage(L"good news"), L"https://www.google.com/search?q=good%20news");
}

// The glow goes round all of Winamp unless the page says otherwise, and is kept by the id of
// the choice, read in any case.
TEST_CASE(TheGlowIsKeptByItsChoice) {
    EXPECT(Settings{}.glow == GlowAround::Winamp);
    EXPECT_EQ(Written(Settings{}).at(L"glow"), L"winamp");
    EXPECT(Read({{L"glow", L" Exodium "}}).glow == GlowAround::Exodium);
    EXPECT(Read({{L"glow", L"off"}}).glow == GlowAround::Nothing);
    EXPECT(Read({{L"glow", L"sometimes"}}).glow == GlowAround::Winamp);
    for (const GlowChoice& choice : kGlowChoices) {
        Settings settings;
        settings.glow = choice.around;
        EXPECT(Read(Written(settings)) == settings);
        EXPECT(&GlowChoiceFor(choice.around) == &choice);
        EXPECT(!choice.name.empty());
    }
}

// The glow's look is kept as numbers, each in its range, and its colour as RRGGBB.
TEST_CASE(TheGlowsLookIsKeptAsNumbers) {
    const Section written = Written(Settings{});
    EXPECT_EQ(written.at(L"glow_size"), L"60");
    EXPECT_EQ(written.at(L"glow_opacity"), L"100");
    EXPECT_EQ(written.at(L"glow_spread"), L"0");
    EXPECT_EQ(written.at(L"glow_color"), L"000000");

    const Settings read = Read({{L"glow_size", L" 120 "}, {L"glow_opacity", L"55"}, {L"glow_spread", L"30"},
                                {L"glow_color", L"#7F00ff"}});
    EXPECT_EQ(read.glowLook.size, 120);
    EXPECT_EQ(read.glowLook.opacity, 55);
    EXPECT_EQ(read.glowLook.spread, 30);
    EXPECT(read.glowLook.color == 0x7F00FFu);
    EXPECT(Read(Written(read)) == read);
    EXPECT_EQ(Written(read).at(L"glow_color"), L"7f00ff");

    // Out of range, to the nearest end; nonsense, the default.
    const Settings odd = Read({{L"glow_size", L"5000"}, {L"glow_opacity", L"-3"}, {L"glow_spread", L"big"},
                               {L"glow_color", L"red"}});
    EXPECT_EQ(odd.glowLook.size, kGlowSizeMost);
    EXPECT_EQ(odd.glowLook.opacity, 0);
    EXPECT_EQ(odd.glowLook.spread, 0);
    EXPECT(odd.glowLook.color == 0u);
    EXPECT(!ParseColor(L"12345"));
    EXPECT(!ParseNumber(L"-"));
    EXPECT(ParseNumber(L"-7") == -7);
}
