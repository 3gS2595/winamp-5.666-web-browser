#include "test_framework.h"

#include "command_line.h"

using namespace exodium;

TEST_CASE(FindsTheLinkAfterTheProgramName) {
    EXPECT_EQ(ParseOpenCommand(LR"("C:\Win amp\exodium_open.exe" "https://example.com/a?b=c")").link,
              L"https://example.com/a?b=c");
    EXPECT_EQ(ParseOpenCommand(LR"(exodium_open.exe https://example.com/)").link, L"https://example.com/");
    EXPECT_EQ(ParseOpenCommand(LR"("C:\Win amp\exodium_open.exe" "C:\My Pages\a page.html")").link,
              LR"(C:\My Pages\a page.html)");
}

TEST_CASE(NoLinkIsAskingForTheBrowser) {
    EXPECT_EQ(ParseOpenCommand(LR"("C:\Win amp\exodium_open.exe")").link, L"");
    EXPECT_EQ(ParseOpenCommand(LR"(exodium_open.exe)").link, L"");
    EXPECT_EQ(ParseOpenCommand(LR"(exodium_open.exe   )").link, L"");
    EXPECT_EQ(ParseOpenCommand(L"").link, L"");
}

TEST_CASE(ReadsTheWinampClass) {
    const OpenCommand both = ParseOpenCommand(LR"(open.exe --class=Scratch "https://example.com/")");
    EXPECT_EQ(both.winampClass, L"Scratch");
    EXPECT_EQ(both.link, L"https://example.com/");

    const OpenCommand alone = ParseOpenCommand(LR"(open.exe --class=Scratch)");
    EXPECT_EQ(alone.winampClass, L"Scratch");
    EXPECT_EQ(alone.link, L"");

    EXPECT_EQ(ParseOpenCommand(LR"(open.exe "https://example.com/")").winampClass, L"");
}

TEST_CASE(AQuoteInALinkDoesNotEndIt) {
    EXPECT_EQ(ParseOpenCommand(LR"("open.exe" "https://example.com/" --class=Evil "x")").link,
              LR"(https://example.com/" --class=Evil "x)");
    EXPECT_EQ(ParseOpenCommand(LR"("open.exe" "https://example.com/" --class=Evil "x")").winampClass, L"");
}

TEST_CASE(ALinkCannotPoseAsTheOption) {
    const OpenCommand command = ParseOpenCommand(LR"("open.exe" "--class=Evil https://example.com/")");
    EXPECT_EQ(command.winampClass, L"");
    EXPECT_EQ(command.link, L"--class=Evil https://example.com/");
}
