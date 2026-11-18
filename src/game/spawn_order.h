#pragma once
// Order of a client character spawner's list: our ghosts' characters first, the game's own last (ghosts.cpp, 3.3). The
// map's echo takes the last character of its spawner's list, so it must be the game's.

#include <algorithm>
#include <cstdint>
#include <vector>

namespace mecmp::game {

// True, with the wanted order in `order`, when the list holds characters of ours and of the game's and the last one is
// ours. Both groups keep their relative order. False: nothing to change (the game's is last already, or the list is
// all ours or all the game's).
inline bool gameCharactersLast(const std::vector<uintptr_t>& items, const std::vector<uintptr_t>& mine,
                               std::vector<uintptr_t>& order) {
    std::vector<uintptr_t> ours, theirs;
    for (uintptr_t it : items) (std::find(mine.begin(), mine.end(), it) != mine.end() ? ours : theirs).push_back(it);
    if (ours.empty() || theirs.empty()) return false;
    if (std::find(ours.begin(), ours.end(), items.back()) == ours.end()) return false;
    order = ours;
    order.insert(order.end(), theirs.begin(), theirs.end());
    return true;
}

} // namespace mecmp::game
