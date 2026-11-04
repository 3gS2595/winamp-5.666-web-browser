#include "test_framework.h"

#include "session_history.h"
#include "url_util.h"

#include <string>
#include <string_view>
#include <vector>

using namespace exodium;

namespace {

// A history of the given addresses, each titled after its own, with `current` in view.
[[nodiscard]] SessionHistory Of(const std::vector<std::wstring>& urls, int current) {
    SessionHistory history;
    for (const std::wstring& url : urls) {
        history.entries.push_back(
            {.id = static_cast<int>(history.entries.size()) + 10, .url = url, .title = L"T " + url});
    }
    history.current = current;
    return history;
}

// A copy written as its addresses, with the shown one in brackets, so that a failure
// shows what differs.
[[nodiscard]] std::wstring Show(const std::optional<HistoryCopy>& copy) {
    if (!copy) return L"(none)";
    std::wstring text;
    for (std::size_t i = 0; i < copy->entries.size(); ++i) {
        if (i) text += L' ';
        text += i == copy->shown ? L"[" + copy->entries[i].url + L"]" : copy->entries[i].url;
    }
    return text;
}

} // namespace

TEST_CASE(ReadsTheProtocolsNavigationHistory) {
    // As Page.getNavigationHistory answers, keys in the order it gives them.
    const auto history = ParseSessionHistory(
        LR"({"currentIndex":1,"entries":[{"id":5,"title":"First","transitionType":"typed","url":"https://a.example/","userTypedURL":""},)"
        LR"({"id":8,"title":"Second \"two\" \u00e9\ud83d\ude00","transitionType":"link","url":"https://b.example/?q=1&r=\/x","userTypedURL":"x"}]})");
    EXPECT(history.has_value());
    if (!history) return;
    EXPECT_EQ(history->current, 1);
    EXPECT_EQ(history->entries.size(), 2u);
    EXPECT_EQ(history->entries[0].id, 5);
    EXPECT_EQ(history->entries[0].url, L"https://a.example/");
    EXPECT_EQ(history->entries[0].title, L"First");
    EXPECT_EQ(history->entries[1].id, 8);
    EXPECT_EQ(history->entries[1].url, L"https://b.example/?q=1&r=/x");
    EXPECT_EQ(history->entries[1].title, L"Second \"two\" \u00E9\U0001F600");
}

TEST_CASE(ReadingTheHistoryToleratesSpaceAndOtherValues) {
    const auto history = ParseSessionHistory(
        L" { \"extra\" : [ 1, -2.5e3, true, false, null, {\"deep\": [\"x\"]} ] ,\n"
        L"   \"entries\" : [ { \"url\" : \"about:blank\" , \"id\" : 1 } ] , \"currentIndex\" : 0 } ");
    EXPECT(history.has_value());
    if (!history) return;
    EXPECT_EQ(history->entries.size(), 1u);
    EXPECT_EQ(history->entries[0].url, L"about:blank");
    EXPECT_EQ(history->entries[0].title, L"");
}

TEST_CASE(RefusesWhatIsNoNavigationHistory) {
    EXPECT(!ParseSessionHistory(L"").has_value());
    EXPECT(!ParseSessionHistory(L"{}").has_value());
    EXPECT(!ParseSessionHistory(L"[]").has_value());
    EXPECT(!ParseSessionHistory(LR"({"currentIndex":0})").has_value());
    EXPECT(!ParseSessionHistory(LR"({"currentIndex":0,"entries":[]})").has_value()); // nothing in view
    EXPECT(!ParseSessionHistory(LR"({"currentIndex":1,"entries":[{"id":1,"url":"a"}]})").has_value());
    EXPECT(!ParseSessionHistory(LR"({"currentIndex":-1,"entries":[{"id":1,"url":"a"}]})").has_value());
    EXPECT(!ParseSessionHistory(LR"({"currentIndex":0.5,"entries":[{"id":1,"url":"a"}]})").has_value());
    EXPECT(!ParseSessionHistory(LR"({"currentIndex":0,"entries":[{"id":1,"url":"a})").has_value()); // never closed
    EXPECT(
        !ParseSessionHistory(LR"({"currentIndex":0,"entries":[{"id":1,"url":"\q"}]})").has_value()); // no such escape
    EXPECT(!ParseSessionHistory(LR"({"currentIndex":0,"entries":[{"id":1,"url":"\u12"}]})").has_value());
    EXPECT(!ParseSessionHistory(LR"({"currentIndex":0,"entries":[{"id":1,"url":"a"}]} x)").has_value());
    EXPECT(!ParseSessionHistory(LR"({"currentIndex":0,"entries":[{"id":"1","url":"a"}]})").has_value());
    EXPECT(!ParseSessionHistory(LR"({"currentIndex":99999999999,"entries":[{"id":1,"url":"a"}]})").has_value());
    // Nesting past any real answer is refused rather than followed.
    EXPECT(!ParseSessionHistory(L"{\"x\":" + std::wstring(200, L'[') + std::wstring(200, L']') +
                                L",\"currentIndex\":0,\"entries\":[{\"id\":1,\"url\":\"a\"}]}")
                .has_value());
}

TEST_CASE(BackCopiesEverythingAndShowsTheEntryBefore) {
    EXPECT_EQ(Show(PlanHistoryCopy(Of({L"a", L"b", L"c", L"d"}, 2), false)), L"a [b] c d");
    EXPECT_EQ(Show(PlanHistoryCopy(Of({L"a", L"b"}, 1), false)), L"[a] b");
}

TEST_CASE(ForwardShowsTheEntryAfter) {
    EXPECT_EQ(Show(PlanHistoryCopy(Of({L"a", L"b", L"c", L"d"}, 1), true)), L"a b [c] d");
    EXPECT_EQ(Show(PlanHistoryCopy(Of({L"a", L"b"}, 0), true)), L"a [b]");
}

TEST_CASE(NothingIsCopiedWithNowhereToGo) {
    EXPECT_EQ(Show(PlanHistoryCopy(Of({L"a", L"b"}, 0), false)), L"(none)");
    EXPECT_EQ(Show(PlanHistoryCopy(Of({L"a", L"b"}, 1), true)), L"(none)");
    EXPECT_EQ(Show(PlanHistoryCopy(Of({L"a"}, 0), true)), L"(none)");
    EXPECT_EQ(Show(PlanHistoryCopy(SessionHistory{}, false)), L"(none)");
}

TEST_CASE(TheParkedPageIsNeitherShownNorCopied) {
    // A reopened tab has the page it was put away on ahead of it, and Forward doesn't lead there.
    EXPECT_EQ(Show(PlanHistoryCopy(Of({L"a", L"b", kParkedUrl}, 1), true)), L"(none)");
    EXPECT_EQ(Show(PlanHistoryCopy(Of({L"a", L"b", kParkedUrl}, 1), false)), L"[a] b");
}

TEST_CASE(EntriesThatCantBeMadeAgainAreSkipped) {
    EXPECT_EQ(Show(PlanHistoryCopy(Of({L"a", L"blob:https://a/1", L"b"}, 2), false)), L"[a] b");
    EXPECT_EQ(Show(PlanHistoryCopy(Of({L"a", L"JavaScript:x", L"b", L"c"}, 0), true)), L"a [b] c");
    EXPECT_EQ(Show(PlanHistoryCopy(Of({L"blob:x", L"a"}, 1), false)), L"(none)");
    EXPECT(CanCopyEntry(L"https://exodium.invalid/history"));
    EXPECT(CanCopyEntry(L"file:///C:/page.html"));
    EXPECT(CanCopyEntry(L"about:blank"));
    EXPECT(!CanCopyEntry(L""));
    EXPECT(!CanCopyEntry(kParkedUrl));
}

TEST_CASE(AnEntryWithTheAddressBeforeItIsTakenAsThatOne) {
    // pushState can leave the same address twice in a row; going to it again makes no new entry.
    EXPECT_EQ(Show(PlanHistoryCopy(Of({L"a", L"b", L"b", L"c"}, 3), false)), L"a [b] c");
    EXPECT_EQ(Show(PlanHistoryCopy(Of({L"a", L"b", L"b", L"c"}, 1), true)), L"a [b] c");
    // Fragments are other addresses, and stay.
    EXPECT_EQ(Show(PlanHistoryCopy(Of({L"a", L"a#x", L"a#y"}, 2), false)), L"a [a#x] a#y");
    // Apart, they are separate entries.
    EXPECT_EQ(Show(PlanHistoryCopy(Of({L"a", L"b", L"a"}, 2), false)), L"a [b] a");
}

TEST_CASE(TheCopyKeepsIdsAndTitles) {
    const auto copy = PlanHistoryCopy(Of({L"a", L"b"}, 1), false);
    EXPECT(copy.has_value());
    if (!copy) return;
    EXPECT_EQ(copy->entries[0].title, L"T a");
    EXPECT_EQ(copy->entries[1].id, 11);
}

TEST_CASE(FindsTheShownEntryAmongThoseMade) {
    const auto copy = PlanHistoryCopy(Of({L"a", L"b", L"c", L"b"}, 2), false);
    EXPECT(copy.has_value());
    if (!copy) return;
    EXPECT_EQ(Show(copy), L"a [b] c b");
    // All made: in its own place, though its address turns up again later.
    EXPECT_EQ(FindShownEntry(Of({L"a", L"b", L"c", L"b"}, 3), *copy).value_or(99), 1u);
    // One the webview wouldn't make: the nearest with its address.
    EXPECT_EQ(FindShownEntry(Of({L"b", L"c", L"b"}, 2), *copy).value_or(99), 0u);
    EXPECT_EQ(FindShownEntry(Of({L"a", L"c"}, 1), *copy).value_or(99), 99u);
    // Written differently, but all there.
    EXPECT_EQ(FindShownEntry(Of({L"A", L"B", L"C", L"B2"}, 3), *copy).value_or(99), 1u);
}

TEST_CASE(ClicksOpenWhereChromeOpensThem) {
    EXPECT(WhereClickOpens(false, false, false) == OpenIn::kSameTab);
    EXPECT(WhereClickOpens(false, true, false) == OpenIn::kBackgroundTab);
    EXPECT(WhereClickOpens(false, true, true) == OpenIn::kForegroundTab);
    EXPECT(WhereClickOpens(true, false, false) == OpenIn::kBackgroundTab);
    EXPECT(WhereClickOpens(true, false, true) == OpenIn::kForegroundTab);
    EXPECT(WhereClickOpens(true, true, false) == OpenIn::kBackgroundTab);
    EXPECT(WhereClickOpens(false, false, true) == OpenIn::kNewWindow);
}

TEST_CASE(KnowsThePluginsOwnPages) {
    EXPECT(IsPluginPage(kHistoryUrl));
    EXPECT(IsPluginPage(L"HTTPS://EXODIUM.INVALID/other"));
    EXPECT(!IsPluginPage(L"https://exodium.invalid.example.com/"));
    EXPECT(!IsPluginPage(L"https://example.com/https://exodium.invalid/"));
}

TEST_CASE(TheStandInPageCarriesTheTitleSafely) {
    const std::wstring html = StandInPageHtml(L"Tom & Jerry </title><script>x()</script>");
    EXPECT(html.contains(L"<title>Tom &amp; Jerry &lt;/title&gt;&lt;script&gt;x()&lt;/script&gt;</title>"));
    EXPECT(html.starts_with(L"<!doctype html>"));
    // No icon is fetched from the site for it.
    EXPECT(html.contains(L"<link rel=\"icon\" href=\"data:,\">"));
}
