#pragma once
#include <cmath>
#include <cstdint>

namespace mecmp::game::door_policy {
// All operations are serialized by the adapter's registry mutex.
struct Lifecycle {
    uint64_t ticket = 0;
    unsigned retiring = 0;
    void changed() { ++ticket; }
    void beginDestroy() {
        ++ticket;
        ++retiring;
    }
    void endDestroy() {
        ++ticket;
        --retiring;
    }
    bool accepts(uint64_t observed) const { return !retiring && observed == ticket; }
};
inline bool allowOpening(bool inRun) {
    return !inRun;
}
inline bool publishPulse(bool value, bool ownWrite, bool enabled, bool inRun) {
    return value && !ownWrite && enabled && allowOpening(inRun);
}
inline bool bootstrapNeeded(uintptr_t loadedWorld, uintptr_t currentWorld) {
    return loadedWorld != 0 && loadedWorld == currentWorld;
}
struct Application {
    static constexpr unsigned kMaxAttempts = 3;
    static constexpr uint64_t kRetryMs = 3000;
    unsigned attempts = 0;
    uint64_t last = 0;
    bool done = false;
    bool due(uint64_t now) const { return !done && attempts < kMaxAttempts && (!attempts || now - last >= kRetryMs); }
    void attempted(uint64_t now) {
        ++attempts;
        last = now;
    }
    void opened() { done = true; }
    bool exhausted() const { return !done && attempts >= kMaxAttempts; }
};

// Conservative stock-door bounds about the initial placement. The first physics
// hit must lie on its panel plane, rather than merely being close to its pivot.
inline bool matchesHit(const float mat[16], const float eye[3], const float hit[3], const float normal[3]) {
    float local[3] = {}, range = 0, alignment = 0;
    for (int i = 0; i < 3; ++i) {
        if (!std::isfinite(eye[i]) || !std::isfinite(hit[i]) || !std::isfinite(normal[i]))
            return false;
        const float d = hit[i] - eye[i];
        range += d * d;
        alignment += normal[i] * mat[8 + i];
        for (int j = 0; j < 3; ++j)
            local[j] += (hit[i] - mat[12 + i]) * mat[4 * j + i];
    }
    return range <= 9.f && range >= .01f && std::fabs(alignment) > .9f && std::fabs(local[0]) <= 1.1f &&
           local[1] >= -.15f && local[1] <= 2.7f && std::fabs(local[2]) <= .12f;
}
} // namespace mecmp::game::door_policy
