#include "settings.h"

#include "text_util.h"

#include <algorithm>
#include <ranges>

namespace exodium {

namespace {

constexpr std::wstring_view kHomePageKey = L"home_page";
constexpr std::wstring_view kSearchEngineKey = L"search_engine";
constexpr std::wstring_view kCustomSearchUrlKey = L"custom_search_url";
constexpr std::wstring_view kCornerSkinKey = L"corner_skin";
constexpr std::wstring_view kGlowKey = L"glow";
constexpr std::wstring_view kGlowColorKey = L"glow_color";

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
    if (const auto skin = read(kCornerSkinKey)) {
        if (const CornerSkin* known = FindCornerSkin(Trim(*skin))) settings.cornerSkin = known->id;
    }
    if (const auto glow = read(kGlowKey)) {
        if (const GlowChoice* known = FindGlowChoice(Trim(*glow))) settings.glow = known->around;
    }
    for (const GlowNumber& number : kGlowNumbers) {
        const auto text = read(number.key);
        if (const auto value = text ? ParseNumber(*text) : std::nullopt) {
            settings.glowLook.*number.member = std::clamp(*value, number.least, number.most);
        }
    }
    if (const auto color = read(kGlowColorKey)) {
        if (const auto value = ParseColor(*color)) settings.glowLook.color = *value;
    }
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
        {kCornerSkinKey, settings.cornerSkin},
        {kGlowKey, std::wstring(GlowChoiceFor(settings.glow).id)},
        {kGlowColorKey, ColorText(settings.glowLook.color)},
    };
    for (const GlowNumber& number : kGlowNumbers) {
        entries.push_back({number.key, std::to_wstring(settings.glowLook.*number.member)});
    }
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

std::optional<int> ParseNumber(std::wstring_view text) noexcept {
    const std::wstring_view digits = Trim(text);
    if (digits.empty() || digits.size() > 9) return std::nullopt;
    const bool negative = digits.front() == L'-';
    int value = 0;
    for (const wchar_t digit : digits.substr(negative ? 1 : 0)) {
        if (digit < L'0' || digit > L'9') return std::nullopt;
        value = value * 10 + (digit - L'0');
    }
    if (negative && digits.size() == 1) return std::nullopt;
    return negative ? -value : value;
}

std::optional<std::uint32_t> ParseColor(std::wstring_view text) noexcept {
    std::wstring_view hex = Trim(text);
    if (!hex.empty() && hex.front() == L'#') hex.remove_prefix(1);
    if (hex.size() != 6) return std::nullopt;
    std::uint32_t color = 0;
    for (const wchar_t digit : hex) {
        int value = 0;
        if (digit >= L'0' && digit <= L'9') value = digit - L'0';
        else if (digit >= L'a' && digit <= L'f') value = digit - L'a' + 10;
        else if (digit >= L'A' && digit <= L'F') value = digit - L'A' + 10;
        else return std::nullopt;
        color = color << 4 | static_cast<std::uint32_t>(value);
    }
    return color;
}

std::wstring ColorText(std::uint32_t color) {
    constexpr wchar_t kDigits[] = L"0123456789abcdef";
    std::wstring text(6, L'0');
    for (int i = 5; i >= 0; --i, color >>= 4) text[static_cast<std::size_t>(i)] = kDigits[color & 0xF];
    return text;
}

const GlowChoice* FindGlowChoice(std::wstring_view id) noexcept {
    const auto found = std::ranges::find_if(kGlowChoices, [id](const GlowChoice& choice) {
        return EqualsIgnoreCase(choice.id, id);
    });
    return found == std::ranges::end(kGlowChoices) ? nullptr : &*found;
}

const GlowChoice& GlowChoiceFor(GlowAround around) noexcept {
    const auto found = std::ranges::find(kGlowChoices, around, &GlowChoice::around);
    return found == std::ranges::end(kGlowChoices) ? kGlowChoices[0] : *found;
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
