#pragma once

#include "history.h"

#include <span>
#include <string>

namespace exodium {

// The page shown for kHistoryUrl: everything in the history, latest first and a day
// to a heading, with a box to search it. The X on a row sends the row's address back
// as a web message, which is what takes it out of the history.
[[nodiscard]] std::wstring BuildHistoryPageHtml(std::span<const HistoryEntry> entries);

} // namespace exodium
