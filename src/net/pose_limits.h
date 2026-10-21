#pragma once
#include "net/net.h"
#include <algorithm>
#include <cmath>

namespace mecmp::net {
// MEC's replay pose stores translation.xyz, a homogeneous weight, and a
// quaternion. It has no peer-controlled scale vector. Never import a weight
// or quaternion magnitude as an implicit body scale.
constexpr float kClientBoneReach = 2.f;
inline bool limitPose(PlayerState& state, float reach = kClientBoneReach) {
    if (state.boneCount > kMaxPoseBones || !std::isfinite(state.yaw)) return false;
    for (float v : state.pos) if (!std::isfinite(v) || std::fabs(v) > 100000.f) return false;
    // Keep interpolation's angle wrapping bounded even for a forged huge yaw.
    state.yaw = std::remainder(state.yaw, 6.28318530718f);
    state.flags &= kStateInLevel | kStateTyping;
    reach = std::isfinite(reach) ? std::clamp(reach, .5f, kClientBoneReach) : kClientBoneReach;
    for (uint32_t b = 0; b < state.boneCount; ++b) {
        auto& bone = state.bones[b];
        double norm = 0;
        for (int j = 0; j < 8; ++j) if (!std::isfinite(bone[j])) return false;
        for (int j = 4; j < 8; ++j) norm += static_cast<double>(bone[j]) * bone[j];
        if (norm < 1.e-12 || norm > 1.e12) return false;
        const float inv = static_cast<float>(1. / std::sqrt(norm));
        for (int j = 4; j < 8; ++j) bone[j] *= inv;
        bone[3] = 1.f;
        const uint8_t bone_idx = state.boneIdx[b];
        if (bone_idx == 1) { // AITrajectory is the world anchor, not a limb.
            for (int j = 0; j < 3; ++j) bone[j] = state.pos[j];
            continue;
        }
        const double length = std::sqrt(static_cast<double>(bone[0]) * bone[0] +
            static_cast<double>(bone[1]) * bone[1] + static_cast<double>(bone[2]) * bone[2]);
        // Hips (bone 4) are the animated body root above AITrajectory. Its offset
        // changes during parkour; give it room without freeing limb lengths.
        const float bound = bone_idx == 4 ? kClientBoneReach : reach;
        if (length > bound) for (int j = 0; j < 3; ++j) bone[j] *= static_cast<float>(bound / length);
    }
    return true;
}
}
