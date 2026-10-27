// Tests for game/replay_ring.h:
//   g++ -std=c++17 -Isrc tests/replay_ring_test.cpp -o replay_ring_test && ./replay_ring_test
#include <cstdio>
#include <vector>

#include "game/replay_ring.h"

using namespace mecmp::game::replay_ring;

static int g_failures = 0;
#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            std::printf("FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                 \
        }                                                                 \
    } while (0)

int main() {
    // The game's formula: (bones - 1) * 0x20 + 0x30, i.e. a 0x10 header and 0x20 per bone.
    CHECK(strideFor(1) == 0x30);
    CHECK(strideFor(23) == 0x2f0);  // the game's own recordings
    CHECK(strideFor(85) == 0xab0);  // MECMP's full-body recordings
    CHECK(kGuardStride == 0xc10);
    CHECK(bonesFor(strideFor(23)) == 23 && bonesFor(kGuardStride) == kGuardBones);
    CHECK(kGuardBones >= 85);

    // Which rings are grown.
    CHECK(needsGrow(strideFor(23), 1, 0x1000));
    CHECK(needsGrow(strideFor(85), 1, 0x1000));
    CHECK(!needsGrow(kGuardStride, 1, 0x1000));    // already big enough
    CHECK(!needsGrow(strideFor(23), 1, 0));        // not allocated
    CHECK(!needsGrow(strideFor(23), 0, 0x1000));   // no slots
    CHECK(!needsGrow(strideFor(23), kMaxSlots + 1, 0x1000));
    CHECK(!needsGrow(0x2f8, 1, 0x1000));           // not a stride the game makes
    CHECK(!needsGrow(0x10, 1, 0x1000));

    // Slots keep their contents at the new stride; the rest of each slot is zero.
    {
        const uint32_t slots = 3;
        const uint64_t from = strideFor(23);
        std::vector<uint8_t> old(static_cast<size_t>(from * slots));
        for (size_t i = 0; i < old.size(); ++i) old[i] = static_cast<uint8_t>(1 + i % 251);
        std::vector<uint8_t> grown(static_cast<size_t>(kGuardStride * slots), 0xEE);
        copySlots(grown.data(), old.data(), from, slots);
        bool same = true, zero = true;
        for (uint32_t s = 0; s < slots; ++s) {
            for (uint64_t b = 0; b < from; ++b) same = same && grown[s * kGuardStride + b] == old[s * from + b];
            for (uint64_t b = from; b < kGuardStride; ++b) zero = zero && grown[s * kGuardStride + b] == 0;
        }
        CHECK(same);
        CHECK(zero);
    }

    if (g_failures) {
        std::printf("%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("replay ring: all checks passed\n");
    return 0;
}
