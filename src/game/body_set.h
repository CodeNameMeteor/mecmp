#pragma once
// The skeleton bones MECMP sends for the full body: the target bones and all their ancestors, in ascending order,
// as the game's ReplaySetup_Build (0x143948610) makes a recorder's bone setup from its RecordingBones. No game
// access, so the host-side test (tests/body_set_test.cpp) covers it.

#include <cstdint>

namespace mecmp::game::body_set {

constexpr uint32_t kMaxSkeletonBones = 256; // the pose packets name bones with one byte

// `parents[b]` is bone b's parent (negative: a root), for `boneCount` bones. Writes the targets and every ancestor
// to `out` in ascending order without repeats and returns how many. 0 when a target or a parent is out of range,
// the skeleton is too big, the parents loop, or the set has more than `max` bones.
inline uint32_t build(const int32_t* parents, uint32_t boneCount, const uint32_t* targets, uint32_t targetCount,
                      uint32_t* out, uint32_t max) {
    if (!parents || boneCount == 0 || boneCount > kMaxSkeletonBones) return 0;
    bool in[kMaxSkeletonBones] = {};
    for (uint32_t t = 0; t < targetCount; ++t) {
        if (targets[t] >= boneCount) return 0;
        int32_t b = static_cast<int32_t>(targets[t]);
        for (uint32_t steps = 0; b >= 0 && !in[b]; ++steps) {
            if (steps > boneCount) return 0; // a loop in the parents: not a skeleton
            in[b] = true;
            const int32_t p = parents[b];
            if (p >= static_cast<int32_t>(boneCount)) return 0;
            b = p;
        }
    }
    uint32_t n = 0;
    for (uint32_t b = 0; b < boneCount; ++b)
        if (in[b]) {
            if (n == max) return 0;
            out[n++] = b;
        }
    return n;
}

} // namespace mecmp::game::body_set
