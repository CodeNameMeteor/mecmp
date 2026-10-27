#include "game/replay_guard.h"

#include <atomic>
#include <vector>

#include "core/log.h"
#include "game/engine.h"
#include "game/fingerprint.h"
#include "game/level.h"
#include "game/replay_ring.h"

namespace mecmp::game::replay_guard {
namespace {

// The allocator calls and the offsets the game uses are checked once; a mismatch only turns the guard off.
bool enabled() {
    static const bool ok = fingerprint::checkReplayGuard();
    return ok;
}

std::atomic<uint32_t> g_grown{0};

// Grows the keyframe ring of the replay player's playback object at `pb` to room for kGuardBones, keeping its
// keyframes. Runs on the thread that loads recordings into this playback and ticks it, so nothing reads the ring
// while it is swapped. The new ring comes from the ring's own allocator and the old one goes back to it, as
// ReplayKeyframeRing_Alloc / _Free do.
void guard(uintptr_t pb, const char* side) {
    uint64_t ring[4] = {}; // stride, slots (u32), base, allocator
    if (!mem::read(pb + Playback_Ring, ring, sizeof(ring))) return;
    const uint64_t stride = ring[Ring_Stride / 8];
    const uint32_t slots = static_cast<uint32_t>(ring[Ring_Slots / 8]);
    const uintptr_t base = ring[Ring_Base / 8], arena = ring[Ring_Arena / 8];
    if (!replay_ring::needsGrow(stride, slots, base)) return;
    // The ring's allocator is the playback's own (ReplayPlayback_LoadRecording passes +0x168): anything else is a
    // layout we do not know, and is left alone.
    if (!arena || mem::readOr<uintptr_t>(pb + Playback_Arena) != arena) return;
    std::vector<uint8_t> old(static_cast<size_t>(stride * slots));
    if (!mem::read(base, old.data(), old.size())) return;
    using AllocFn = void* (*)(uintptr_t arena, uint64_t size, uint64_t align);
    using FreeFn = void (*)(uintptr_t arena, uintptr_t memory);
    auto* fresh = static_cast<uint8_t*>(
        reinterpret_cast<AllocFn>(rva(Fn_Arena_allocAligned))(arena, replay_ring::kGuardStride * slots, 0x10));
    if (!fresh) return;
    replay_ring::copySlots(fresh, old.data(), stride, slots);
    poke<uint64_t>(pb + Playback_Ring + Ring_Stride, replay_ring::kGuardStride);
    poke<uintptr_t>(pb + Playback_Ring + Ring_Base, reinterpret_cast<uintptr_t>(fresh));
    reinterpret_cast<FreeFn>(rva(Fn_Arena_free))(arena, base);
    const uint32_t n = ++g_grown;
    if (n <= 20 || n % 100 == 0)
        VLOG("replay: %s replay player %p now has room for %u bones (had %u; %u grown)", side, reinterpret_cast<void*>(pb),
             replay_ring::kGuardBones, replay_ring::bonesFor(stride), n);
}

} // namespace

void serverTick() {
    if (!enabled()) return;
    for (void* sp : level::ghostSpawnerCandidates()) {
        uintptr_t list[2] = {};
        if (!mem::read(reinterpret_cast<uintptr_t>(sp) + Spawner_Spawned, list, sizeof(list)) || list[1] < list[0] ||
            (list[1] - list[0]) / 8 > 64)
            continue;
        const size_t n = (list[1] - list[0]) / 8;
        uintptr_t items[64] = {};
        if (!n || !mem::read(list[0], items, n * 8)) continue;
        for (size_t i = 0; i < n; ++i) {
            void* ch = reinterpret_cast<void*>(items[i]);
            if (!level::characterExists(ch, 0)) continue; // a live PamServerCharacterEntity
            void* comp = getComponent(ch, TI_PamServerReplayPlayerComponent);
            if (comp) guard(reinterpret_cast<uintptr_t>(comp) + SRP_Playback, "server");
        }
    }
}

void beforeClientUpdate(void* updater) {
    if (!updater || !enabled()) return;
    uintptr_t list[2] = {};
    if (!mem::read(reinterpret_cast<uintptr_t>(updater) + ClientReplayUpdater_List, list, sizeof(list)) ||
        list[1] < list[0] || (list[1] - list[0]) / 8 > 256)
        return;
    const size_t n = (list[1] - list[0]) / 8;
    uintptr_t items[256];
    if (!n || !mem::read(list[0], items, n * 8)) return;
    const uintptr_t vt = rva(Vtbl_PamClientReplayPlayerComponent);
    for (size_t i = 0; i < n; ++i)
        if (items[i] && mem::readOr<uintptr_t>(items[i]) == vt) guard(items[i] + CRP_Playback, "client");
}

} // namespace mecmp::game::replay_guard
