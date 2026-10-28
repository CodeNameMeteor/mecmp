// Tests for game/body_set.h:
//   g++ -std=c++17 -Isrc tests/body_set_test.cpp -o body_set_test && ./body_set_test
#include <cstdio>

#include "game/body_set.h"

using namespace mecmp::game::body_set;

static int g_failures = 0;
#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            std::printf("FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                 \
        }                                                                 \
    } while (0)

int main() {
    //   0 root
    //   1 <- 0, 2 <- 1, 3 <- 2   (a chain)
    //   4 <- 1, 5 <- 4           (a branch)
    //   6 <- 0                   (not asked for)
    const int32_t parents[] = {-1, 0, 1, 2, 1, 4, 0};
    uint32_t out[16] = {};

    // A target and its ancestors, ascending.
    {
        const uint32_t t[] = {3};
        CHECK(build(parents, 7, t, 1, out, 16) == 4);
        CHECK(out[0] == 0 && out[1] == 1 && out[2] == 2 && out[3] == 3);
    }
    // Shared ancestors only once; targets in any order.
    {
        const uint32_t t[] = {5, 3, 1};
        CHECK(build(parents, 7, t, 3, out, 16) == 6);
        CHECK(out[0] == 0 && out[1] == 1 && out[2] == 2 && out[3] == 3 && out[4] == 4 && out[5] == 5);
    }
    // A repeated target.
    {
        const uint32_t t[] = {2, 2};
        CHECK(build(parents, 7, t, 2, out, 16) == 3);
    }
    // Too many for `max`.
    {
        const uint32_t t[] = {5, 3};
        CHECK(build(parents, 7, t, 2, out, 5) == 0);
        CHECK(build(parents, 7, t, 2, out, 6) == 6);
    }
    // Out of range: a target, a parent, the skeleton size.
    {
        const uint32_t t[] = {7};
        CHECK(build(parents, 7, t, 1, out, 16) == 0);
        const int32_t badParent[] = {-1, 9};
        const uint32_t t1[] = {1};
        CHECK(build(badParent, 2, t1, 1, out, 16) == 0);
        CHECK(build(parents, 0, t1, 1, out, 16) == 0);
        CHECK(build(parents, kMaxSkeletonBones + 1, t1, 1, out, 16) == 0);
        CHECK(build(nullptr, 7, t1, 1, out, 16) == 0);
    }
    // A loop in the parents is not a skeleton (the walk stops at a bone already taken, so the set is still finite).
    {
        const int32_t loop[] = {1, 0};
        const uint32_t t[] = {0};
        CHECK(build(loop, 2, t, 1, out, 16) == 2);
    }

    if (g_failures) {
        std::printf("%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("body set: all checks passed\n");
    return 0;
}
