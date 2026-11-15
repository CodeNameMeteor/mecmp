// Tests for game/spawn_order.h:
//   g++ -std=c++17 -Isrc tests/spawn_order_test.cpp -o spawn_order_test && ./spawn_order_test
#include <cstdio>

#include "game/spawn_order.h"

using namespace mecmp::game;
using V = std::vector<uintptr_t>;

static int g_failures = 0;
#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            std::printf("FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                 \
        }                                                                 \
    } while (0)

int main() {
    V order;
    // The game's echo, then our ghost added after it: ours first, the game's last.
    CHECK(gameCharactersLast({1, 100}, {100}, order) && order == V({100, 1}));
    // Several of each keep their order inside the group.
    CHECK(gameCharactersLast({1, 2, 100, 101}, {100, 101}, order) && order == V({100, 101, 1, 2}));
    CHECK(gameCharactersLast({1, 100, 2, 101}, {100, 101}, order) && order == V({100, 101, 1, 2}));
    // The game's is last already: nothing to do.
    CHECK(!gameCharactersLast({100, 1}, {100}, order));
    CHECK(!gameCharactersLast({100, 1, 2}, {100}, order));
    // All ours, all the game's, empty, none of ours known.
    CHECK(!gameCharactersLast({100, 101}, {100, 101}, order));
    CHECK(!gameCharactersLast({1, 2}, {100}, order));
    CHECK(!gameCharactersLast({}, {100}, order));
    CHECK(!gameCharactersLast({1, 2}, {}, order));
    if (g_failures == 0) std::printf("spawn_order_test: all passed\n");
    return g_failures ? 1 : 0;
}
