#pragma once
// The game's replay keyframe ring (one per replay player) and how replay_guard grows it. No game access, so the
// host-side test (tests/replay_ring_test.cpp) covers it.

#include <cstdint>
#include <cstring>

namespace mecmp::game::replay_ring {

// Bytes per keyframe slot for `bones` bones: a 0x10 header (time, flags), then 0x20 per bone (translation,
// rotation), as ReplayKeyframeRing_Alloc (0x143949780) computes it: (bones - 1) * 0x20 + 0x30.
constexpr uint64_t strideFor(uint32_t bones) { return (static_cast<uint64_t>(bones) - 1) * 0x20 + 0x30; }

// Rings are grown to room for this many bones: more than any recording the game (23) or MECMP (85) makes.
constexpr uint32_t kGuardBones = 96;
constexpr uint64_t kGuardStride = strideFor(kGuardBones);
// The game makes rings of one slot for playback; anything far beyond that is not a ring we understand.
constexpr uint32_t kMaxSlots = 1024;

// The bone count a slot of `stride` bytes has room for.
constexpr uint32_t bonesFor(uint64_t stride) { return static_cast<uint32_t>((stride - 0x10) / 0x20); }

// A ring the guard grows: allocated, laid out as the game makes them, and with less room than kGuardBones.
inline bool needsGrow(uint64_t stride, uint32_t slots, uintptr_t base) {
    return base != 0 && slots >= 1 && slots <= kMaxSlots && stride >= strideFor(1) && stride < kGuardStride &&
           (stride - 0x10) % 0x20 == 0;
}

// Copies every slot of a ring (`fromStride` bytes each) into a ring of kGuardStride slots, slot by slot (the game
// addresses slot i at base + i * stride). The rest of each new slot is zeroed.
inline void copySlots(uint8_t* to, const uint8_t* from, uint64_t fromStride, uint32_t slots) {
    for (uint32_t i = 0; i < slots; ++i) {
        std::memcpy(to + i * kGuardStride, from + i * fromStride, static_cast<size_t>(fromStride));
        std::memset(to + i * kGuardStride + fromStride, 0, static_cast<size_t>(kGuardStride - fromStride));
    }
}

} // namespace mecmp::game::replay_ring
