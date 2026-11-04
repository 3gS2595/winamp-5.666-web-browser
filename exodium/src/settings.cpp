#include "settings.h"

#include "text_util.h"

#include <algorithm>
#include <ranges>

namespace exodium {

namespace {

constexpr std::wstring_view kHomePageKey = L"home_page";
constexpr std::wstring_view kSearchEngineKey = L"search_engine";
constexpr std::wstring_view kCustomSearchUrlKey = L"custom_search_url";

} // namespace

Settings ReadSettings(const SettingReader& read) {
    Settings settings;
    if (const auto home = read(kHomePageKey); home && !Trim(*home).empty())
        settings.homePage = NormalizeHomePage(*home);
    if (const auto engine = read(kSearchEngineKey)) {
        const std::wstring_view id = Trim(*engine);
        if (const SearchEngine* known = FindSearchEngine(id)) settings.searchEngine = known->id;
        else if (EqualsIgnoreCase(id, kCustomSearchId)) settings.searchEngine = kCustomSearchId;
    }
    if (const auto custom = read(kCustomSearchUrlKey)) settings.customSearchUrl = NormalizeSearchUrl(*custom);
    for (const FlagSetting& flag : kFlagSettings) {
        const auto text = read(flag.key);
        if (const auto on = text ? ParseFlag(*text) : std::nullopt) settings.*flag.member = *on;
    }
    return settings;
}

std::vector<SettingEntry> SettingEntries(const Settings& settings) {
    std::vector<SettingEntry> entries{
        {kHomePageKey, settings.homePage},
        {kSearchEngineKey, settings.searchEngine},
        {kCustomSearchUrlKey, settings.customSearchUrl},
    };
    for (const FlagSetting& flag : kFlagSettings) {
        entries.push_back({flag.key, settings.*flag.member ? L"1" : L"0"});
    }
    return entries;
}

std::vector<SettingEntry> ChangedEntries(const Settings& before, const Settings& after) {
    std::vector<SettingEntry> changed;
    // Both lists have every setting, in the same order.
    for (auto&& [was, now] : std::views::zip(SettingEntries(before), SettingEntries(after))) {
        if (was.value != now.value) changed.push_back(std::move(now));
    }
    return changed;
}

std::optional<bool> ParseFlag(std::wstring_view text) noexcept {
    const std::wstring_view word = Trim(text);
    for (const std::wstring_view on : {L"1", L"true", L"yes", L"on"}) {
        if (EqualsIgnoreCase(word, on)) return true;
    }
    for (const std::wstring_view off : {L"0", L"false", L"no", L"off"}) {
        if (EqualsIgnoreCase(word, off)) return false;
    }
    return std::nullopt;
}

std::wstring NormalizeHomePage(std::wstring_view typed) {
    const std::wstring_view text = Trim(typed);
    if (text.empty()) return kDefaultUrl;
    return ResolveAddressInput(text).url;
}

} // namespace exodium
