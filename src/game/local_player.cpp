#include "game/local_player.h"

#include <cmath>
#include <cstring>

#include "core/log.h"
#include "game/engine.h"
#include "game/level.h"

namespace mecmp::game::local_player {
namespace {

// Newest keyframe of the recorder ring: {f32 time; u32 flags; pad[8]; bone[n] {vec4 t; quat q}}.
bool capturePose(net::PlayerState& st) {
    void* ch = reinterpret_cast<void* (*)()>(rva(Fn_LocalClientCharacter))();
    if (!ch) return false;
    void* comp = getComponent(ch, TI_PamClientReplayRecordingComponent);
    if (!comp) return false;
    const uintptr_t rec = mem::readOr<uintptr_t>(reinterpret_cast<uintptr_t>(comp) + 0x48);
    if (!rec) return false;
    const uint64_t stride = mem::readOr<uint64_t>(rec + Rec_Stride);
    const uint32_t cap = mem::readOr<uint32_t>(rec + Rec_Capacity);
    const uintptr_t base = mem::readOr<uintptr_t>(rec + Rec_Base);
    const uint32_t head = mem::readOr<uint32_t>(rec + Rec_Head);
    if (!base || !cap || stride < 0x30 || (stride - 0x10) % 0x20 != 0) return false;
    const uint32_t n = static_cast<uint32_t>((stride - 0x10) / 0x20);
    if (n == 0 || n > net::kMaxPoseBones) return false;
    uint32_t best = head % cap;
    const uint32_t prev = (head + cap - 1) % cap;
    if (mem::readOr<float>(base + prev * stride) > mem::readOr<float>(base + best * stride)) best = prev;
    if (!mem::read(base + best * stride + 0x10, st.bones, n * 0x20)) return false;
    st.boneCount = n;
    return true;
}
} // namespace

bool capture(net::PlayerState& st) {
    void* player = level::playerCharacter();
    const LinearTransform* t = player ? entityTransform(player) : nullptr;
    if (!t) return false;
    st.pos[0] = t->trans[0];
    st.pos[1] = t->trans[1];
    st.pos[2] = t->trans[2];
    st.yaw = std::atan2(t->forward[0], t->forward[2]);
    st.flags = 1;
    st.boneCount = 0;
    capturePose(st);
    return true;
}

} // namespace mecmp::game::local_player
