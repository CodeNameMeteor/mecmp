#pragma once
// Moving a pose between bone sets by skeleton bone index (no engine dependencies, tested in tests/pose_test.cpp).
#include <cstdint>
#include <cstring>

namespace mecmp::game {

// Copies each `src` bone into the `dst` slot with the same skeleton bone index (low 8 bits; the skeleton has
// fewer than 256 bones). Slots with no source stay as they are, except a slot with no rotation at all (a zeroed
// buffer): a zero quaternion collapses the bone, so it becomes the identity. Returns the slots written.
inline uint32_t remapPose(float dst[][8], const uint32_t* dstIdx, uint32_t dstN, const float src[][8],
                          const uint8_t* srcIdx, uint32_t srcN) {
    int16_t slotOf[256];
    memset(slotOf, 0xff, sizeof(slotOf));
    for (uint32_t s = srcN; s-- > 0;) slotOf[srcIdx[s]] = static_cast<int16_t>(s); // the first bone with an index wins
    static const float identity[8] = {0, 0, 0, 1, 0, 0, 0, 1};
    uint32_t written = 0;
    for (uint32_t d = 0; d < dstN; ++d) {
        const int16_t s = slotOf[dstIdx[d] & 0xff];
        if (s >= 0) {
            memcpy(dst[d], src[s], 0x20);
            ++written;
        } else if (dst[d][4] == 0.f && dst[d][5] == 0.f && dst[d][6] == 0.f && dst[d][7] == 0.f) {
            memcpy(dst[d], identity, sizeof(identity));
        }
    }
    return written;
}

} // namespace mecmp::game
