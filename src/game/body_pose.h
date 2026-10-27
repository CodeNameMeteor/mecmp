#pragma once
// The local player's full-body pose, read where the game's replay recorder samples it. The recorder keeps the
// game's own bones (23): it also records the player's time-trial runs, shared time trials and echo, and those must
// stay playable on the game's ghosts (an 85-bone run T-posed them and corrupted the heap, 2026-10-27). Each time the
// recorder takes a keyframe, MECMP copies its own bone set (body_set.h: the full-body targets and their ancestors)
// from the same full-skeleton pose the recorder copies its bones from.

#include <cstdint>

#include "net/net.h"

namespace mecmp::game::body_pose {

struct Sample {
    float time = -1.f;      // the recorder keyframe's time this pose belongs to (recorder seconds)
    uint32_t count = 0;     // bones
    uint8_t idx[net::kMaxPoseBones] = {};      // skeleton bone of each slot, ascending
    float bones[net::kMaxPoseBones][8] = {};   // {vec4 translation, quat rotation} per slot, as the recorder stores them
};

// Client thread, right after the game's recording components sampled (hooks.cpp, their updater's updateAll).
void afterRecorderSample();
// The newest full-body pose. False when there is none (off, the build check failed, nothing sampled yet). Any thread.
bool latest(Sample& out);

} // namespace mecmp::game::body_pose
