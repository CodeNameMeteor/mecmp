#include "game/local_player.h"

#include <cmath>
#include <cstring>

#include "core/log.h"
#include "game/engine.h"
#include "game/ghosts.h"
#include "game/level.h"

namespace mecmp::game::local_player {
namespace {

// Newest keyframe of the recorder ring: {f32 time; u32 flags; pad[8]; bone[n] {vec4 t; quat q}}.
// `keyTime` = the keyframe's own time (recorder seconds).
bool capturePose(net::PlayerState& st, float& keyTime) {
    void* ch = reinterpret_cast<void* (*)()>(rva(Fn_LocalClientCharacter))();
    if (!ch) return false;
    void* comp = getComponent(ch, TI_PamClientReplayRecordingComponent);
    if (!comp) return false;
    const uintptr_t rec = mem::readOr<uintptr_t>(reinterpret_cast<uintptr_t>(comp) + 0x48);
    if (!rec) return false;
    // The ring's stride, capacity, base and head in one read.
    uint8_t ring[Rec_Head + 4 - Rec_Stride];
    if (!mem::read(rec + Rec_Stride, ring, sizeof(ring))) return false;
    uint64_t stride = 0;
    uint32_t cap = 0, head = 0;
    uintptr_t base = 0;
    memcpy(&stride, ring, sizeof(stride));
    memcpy(&cap, ring + (Rec_Capacity - Rec_Stride), sizeof(cap));
    memcpy(&base, ring + (Rec_Base - Rec_Stride), sizeof(base));
    memcpy(&head, ring + (Rec_Head - Rec_Stride), sizeof(head));
    if (!base || !cap || stride < 0x30 || (stride - 0x10) % 0x20 != 0) return false;
    const uint32_t n = static_cast<uint32_t>((stride - 0x10) / 0x20);
    if (n == 0 || n > net::kMaxPoseBones) return false;
    // The recorder's bone setup names the skeleton bone of each slot. Read again only when the recorder changes.
    static uintptr_t setupOf = 0;
    static level::BoneSetup setup;
    if (rec != setupOf || setup.count != n) {
        setupOf = rec;
        setup.count = 0;
        if (!level::readSetupAt(rec + Rec_Setup, setup) || setup.count != n) return false;
        for (uint32_t i = 0; i < n; ++i)
            if (setup.indices[i] > 255) return false;
        const bool synced = level::noteRecorderSetup(setup);
        MLOG("capture: local recorder has %u bones (%s)", n, synced ? "full sync set" : "vanilla set, the mod loaded after the level");
    }
    uint32_t best = head % cap;
    const uint32_t prev = (head + cap - 1) % cap;
    if (mem::readOr<float>(base + prev * stride) > mem::readOr<float>(base + best * stride)) best = prev;
    // The game thread of the client writes this ring while we read it: read the keyframe's time before
    // and after the bones, and drop the read if the slot was rewritten in between (a torn pose).
    const uintptr_t slot = base + best * stride;
    float before = 0, after = 0;
    if (!mem::read(slot, before) || !mem::read(slot + 0x10, st.bones, n * 0x20) || !mem::read(slot, after) || before != after)
        return false;
    keyTime = before;
    for (uint32_t i = 0; i < n; ++i) st.boneIdx[i] = static_cast<uint8_t>(setup.indices[i]);
    ghosts::noteOwnPose(st.bones, st.boneIdx, n);
    st.boneCount = n;
    return true;
}
} // namespace

bool capture(net::PlayerState& st) {
    void* player = level::playerCharacter();
    LinearTransform xf;
    if (!player || !readEntityTransform(player, xf)) return false;
    const LinearTransform* t = &xf;
    st.pos[0] = t->trans[0];
    st.pos[1] = t->trans[1];
    st.pos[2] = t->trans[2];
    st.yaw = std::atan2(t->forward[0], t->forward[2]);
    st.flags = 1;
    st.boneCount = 0;
    float keyTime = 0;
    const bool posed = capturePose(st, keyTime) && st.boneCount > 1;
    // The recorder writes keyframes less often than the server ticks: a keyframe already sent is not
    // sent again with a new time (that made motion stop and jump). Nothing new: nothing to publish.
    static float lastKeyTime = -1.f;
    if (posed && keyTime == lastKeyTime) return false;
    if (posed) {
        // Bone slot 1 is the AITrajectory bone: the world anchor the game itself places ghosts by.
        // Using it (instead of the entity position) keeps body and root consistent (no height offset).
        st.pos[0] = st.bones[1][0];
        st.pos[1] = st.bones[1][1];
        st.pos[2] = st.bones[1][2];
    }
    // While a level loads the transform and the recorder can hold garbage: never send it.
    for (float v : st.pos)
        if (!std::isfinite(v) || std::fabs(v) > 100000.f) return false;
    for (uint32_t b = 0; b < st.boneCount; ++b)
        for (float v : st.bones[b])
            if (!std::isfinite(v) || std::fabs(v) > 100000.f) {
                st.boneCount = 0; // keep the position, drop the pose
                break;
            }
    static const double freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return static_cast<double>(f.QuadPart); }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    const uint32_t now = static_cast<uint32_t>(static_cast<uint64_t>(static_cast<double>(c.QuadPart) * 1000.0 / freq));
    // Time each pose by its keyframe's own time, so receivers see the true spacing between poses
    // (reading them on server ticks adds up to a tick of uneven delay). The keyframe clock is used as
    // it is (receivers follow slow drift, see protocol::DelayEstimator); it is only re-anchored to the
    // real clock when it jumps (level start, rewinds: backwards or more than 1 s forward).
    static uint32_t lastMs = 0;
    static double anchorMs = 0; // keyframe time 0 on our clock
    uint32_t ms = now;
    if (posed) {
        const float step = keyTime - lastKeyTime;
        if (lastKeyTime < 0.f || step <= 0.f || step > 1.f) anchorMs = static_cast<double>(now) - keyTime * 1000.0;
        ms = static_cast<uint32_t>(static_cast<int64_t>(anchorMs + keyTime * 1000.0));
        lastKeyTime = keyTime;
    }
    if (static_cast<int32_t>(ms - lastMs) <= 0) ms = lastMs + 1; // strictly increasing for receivers
    lastMs = ms;
    st.timeMs = ms;
    return true;
}

} // namespace mecmp::game::local_player
