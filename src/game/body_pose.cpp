#include "game/body_pose.h"

#include <cmath>
#include <cstring>
#include <mutex>

#include "core/config.h"
#include "core/log.h"
#include "game/body_set.h"
#include "game/engine.h"
#include "game/fingerprint.h"
#include "game/level.h"

namespace mecmp::game::body_pose {
namespace {

std::mutex g_m;
Sample g_latest; // under g_m
bool g_have = false;

// The offsets below are checked once; a mismatch only turns the full body off (the game's 23 bones are sent).
bool enabled() {
    static const bool ok = fingerprint::checkBodyPose();
    return ok;
}

// The bone set of the skeleton the recorder samples from (client thread only).
struct SetCache {
    uintptr_t skeleton = 0;
    uint32_t boneCount = 0;
    uint32_t count = 0;
    uint32_t idx[net::kMaxPoseBones] = {};
};
SetCache g_set;

bool readSet(uintptr_t skeleton) {
    const uint32_t boneCount = mem::readOr<uint32_t>(skeleton + Skel_BoneCount);
    if (skeleton == g_set.skeleton && boneCount == g_set.boneCount && g_set.count) return true;
    const uintptr_t parentsAt = mem::readOr<uintptr_t>(skeleton + Skel_Parents);
    int32_t parents[body_set::kMaxSkeletonBones];
    g_set = SetCache{};
    if (!parentsAt || boneCount == 0 || boneCount > body_set::kMaxSkeletonBones ||
        !mem::read(parentsAt, parents, boneCount * sizeof(int32_t)))
        return false;
    const uint32_t* targets = nullptr;
    const uint32_t targetCount = level::bodyTargets(targets);
    g_set.count = body_set::build(parents, boneCount, targets, targetCount, g_set.idx, net::kMaxPoseBones);
    if (!g_set.count) {
        MLOG("body: the skeleton %p (%u bones) does not give the full-body set; the game's bones are sent",
             reinterpret_cast<void*>(skeleton), boneCount);
        return false;
    }
    g_set.skeleton = skeleton;
    g_set.boneCount = boneCount;
    MLOG("body: full-body pose of %u bones taken from the skeleton (%u bones); runs keep the game's own", g_set.count,
         boneCount);
    return true;
}

} // namespace

void afterRecorderSample() {
    if (!config::get()->fullBodyPose || !enabled()) return;
    void* ch = reinterpret_cast<void* (*)()>(rva(Fn_LocalClientCharacter))();
    if (!ch) return;
    const auto comp = reinterpret_cast<uintptr_t>(getComponent(ch, TI_PamClientReplayRecordingComponent));
    if (!comp) return;
    // The recorder's newest keyframe: the pose below is the one it was just copied from.
    const uintptr_t rec = mem::readOr<uintptr_t>(comp + RecComp_Recorder);
    if (!rec) return;
    const uint64_t stride = mem::readOr<uint64_t>(rec + Rec_Stride);
    const uint32_t cap = mem::readOr<uint32_t>(rec + Rec_Capacity), head = mem::readOr<uint32_t>(rec + Rec_Head);
    const uintptr_t base = mem::readOr<uintptr_t>(rec + Rec_Base);
    if (!base || !cap || stride < 0x30) return;
    const float time = mem::readOr<float>(base + (head % cap) * stride);
    static float lastTime = -1.f; // client thread only
    if (time == lastTime) return; // no keyframe this frame
    const uintptr_t skeleton = mem::readOr<uintptr_t>(comp + RecComp_Skeleton);
    if (!skeleton || !readSet(skeleton)) return;
    const uintptr_t pose = mem::readOr<uintptr_t>(comp + RecComp_PoseHandle + PoseHandle_LocalPose);
    if (!pose) return;
    // The whole local pose up to the highest bone we send, in one read (the set is ascending).
    thread_local float all[body_set::kMaxSkeletonBones][8];
    const uint32_t upTo = g_set.idx[g_set.count - 1] + 1;
    if (!mem::read(pose, all, upTo * sizeof(all[0]))) return;
    thread_local Sample s;
    s.time = time;
    s.count = g_set.count;
    for (uint32_t i = 0; i < s.count; ++i) {
        s.idx[i] = static_cast<uint8_t>(g_set.idx[i]);
        memcpy(s.bones[i], all[g_set.idx[i]], sizeof(s.bones[i]));
        for (float v : s.bones[i])
            if (!std::isfinite(v)) return; // a level loading: no pose this time
    }
    lastTime = time;
    std::lock_guard<std::mutex> lock(g_m);
    g_latest = s;
    g_have = true;
}

bool latest(Sample& out) {
    std::lock_guard<std::mutex> lock(g_m);
    if (!g_have) return false;
    out = g_latest;
    return true;
}

} // namespace mecmp::game::body_pose
