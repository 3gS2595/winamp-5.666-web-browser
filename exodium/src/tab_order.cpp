#include "tab_order.h"

#include <algorithm>

namespace exodium {

TabSelection SelectRange(const TabSelection& current, int anchor, int index, bool add) {
    const int count = static_cast<int>(current.size());
    TabSelection selected = add ? current : TabSelection(current.size(), false);
    const int first = std::clamp(std::min(anchor, index), 0, count);
    const int last = std::clamp(std::max(anchor, index), -1, count - 1);
    for (int i = first; i <= last; ++i) selected[i] = true;
    return selected;
}

int NearestWanted(const std::vector<bool>& wanted, int from) {
    const int count = static_cast<int>(wanted.size());
    if (count == 0) return -1;
    from = std::clamp(from, 0, count - 1);
    for (int i = from; i >= 0; --i) {
        if (wanted[i]) return i;
    }
    for (int i = from + 1; i < count; ++i) {
        if (wanted[i]) return i;
    }
    return -1;
}

int LinkTabPlace(const std::vector<int>& openers, int active, bool foreground) {
    const int count = static_cast<int>(openers.size());
    if (active < 0 || active >= count) return count;
    if (foreground) return active + 1;
    // The active tab and every tab opened from it, or from one of those, as far as they
    // run on unbroken after it.
    std::vector<bool> family(openers.size(), false);
    family[active] = true;
    int last = active;
    for (int i = active + 1; i < count; ++i) {
        const int opener = openers[i];
        if (opener < 0 || opener >= count || !family[opener]) break;
        family[i] = true;
        last = i;
    }
    return last + 1;
}

bool SwitchForgetsOpeners(int fromId, int fromOpener, int toId, int toOpener) noexcept {
    return toOpener != fromOpener && toOpener != fromId && fromOpener != toId;
}

std::vector<int> GatheredOrder(const std::vector<bool>& moving, int insertAt) {
    std::vector<int> order, gathered;
    for (int i = 0; i < static_cast<int>(moving.size()); ++i) (moving[i] ? gathered : order).push_back(i);
    insertAt = std::clamp(insertAt, 0, static_cast<int>(order.size()));
    order.insert(order.begin() + insertAt, gathered.begin(), gathered.end());
    return order;
}

} // namespace exodium
