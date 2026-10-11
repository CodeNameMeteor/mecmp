#pragma once
// Developer diagnostics for jitter: the largest change between consecutive ticks of a player state
// (position, bone translations, bone rotations), since the last report. Game thread only.

#include <cmath>
#include <cstdio>
#include <string>

#include "net/net.h"

namespace mecmp::game {

struct MotionStats {
    bool have = false;
    float lastPos[3] = {};
    float last[net::kMaxPoseBones][8] = {};
    uint32_t lastCount = 0;
    float maxPos = 0, maxT = 0, maxQ = 0;
    uint32_t samples = 0, poseChanges = 0;

    void add(const net::PlayerState& s) {
        if (have) {
            float d = 0;
            for (int i = 0; i < 3; ++i) d = std::fmax(d, std::fabs(s.pos[i] - lastPos[i]));
            maxPos = std::fmax(maxPos, d);
            bool changed = false;
            if (s.boneCount == lastCount)
                for (uint32_t b = 0; b < s.boneCount; ++b) {
                    for (int j = 0; j < 3; ++j) {
                        if (b == 1) continue; // the trajectory bone is the position, counted above
                        const float dt = std::fabs(s.bones[b][j] - last[b][j]);
                        maxT = std::fmax(maxT, dt);
                        changed = changed || dt > 0;
                    }
                    for (int j = 4; j < 8; ++j) {
                        const float dq = std::fabs(s.bones[b][j] - last[b][j]);
                        maxQ = std::fmax(maxQ, dq);
                        changed = changed || dq > 0;
                    }
                }
            poseChanges += changed;
            ++samples;
        }
        have = true;
        for (int i = 0; i < 3; ++i) lastPos[i] = s.pos[i];
        lastCount = s.boneCount;
        for (uint32_t b = 0; b < s.boneCount && b < net::kMaxPoseBones; ++b)
            for (int j = 0; j < 8; ++j) last[b][j] = s.bones[b][j];
    }

    // "step max: position 0.4 mm, bone translation 1.2 mm, rotation 0.0031; pose changed in 28/30 ticks"; resets.
    std::string report() {
        char b[160];
        snprintf(b, sizeof(b), "step max: position %.1f mm, bone translation %.1f mm, rotation %.4f; pose changed in %u/%u ticks",
                 maxPos * 1000.f, maxT * 1000.f, maxQ, poseChanges, samples);
        maxPos = maxT = maxQ = 0;
        samples = poseChanges = 0;
        return b;
    }
};

} // namespace mecmp::game
