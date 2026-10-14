#include "game/players_map.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "core/config.h"
#include "core/log.h"
#include "game/engine.h"
#include "net/net.h"

namespace mecmp::game::players_map {
namespace {

// PamClientPresenceInfoManagerEntity slot array (PresenceInfoManager_assignSlot, _update).
constexpr size_t Mgr_Slots = 0x50;      // vector<Slot> begin / end (+0x58)
constexpr size_t Slot_Size = 0x50;
constexpr size_t Slot_Shown = 0x00;     // u8: the privacy lookup's answer; with Valid, makes the marker visible
constexpr size_t Slot_Priority = 0x04;  // int: assignSlot replaces a slot only for a higher priority
constexpr size_t Slot_OnlineId = 0x08;  // OnlineId {u64 id; char name[32]}
constexpr size_t Slot_Pos = 0x30;       // Vec4: the marker's world position
// The manager pool (vtable 0x141c74bb8): vector of {manager*, interval and timer} at +0x28 / +0x30.
constexpr size_t Pool_Entries = 0x28;
constexpr size_t Pool_EntrySize = 0x10;

constexpr int kPriority = 1000;                       // above any friend (the game uses 0)
constexpr uint64_t kIdTag = 0x4d45434d00000000ull;    // "MECM" in the top half: our made-up online ids
constexpr uint32_t kShowAfterMs = 5000;               // a player silent this long leaves the map

struct OnlineId {
    uint64_t id;
    char name[32];
};
static_assert(sizeof(OnlineId) == 0x28, "OnlineId");

using AssignSlotFn = void* (*)(void* manager, const OnlineId* id, const float* pos, int priority);

std::atomic<const char*> g_status{"not seen yet"};

bool ours(uint64_t id) { return (id & 0xffffffff00000000ull) == kIdTag; }

void hideSlot(uintptr_t slot) {
    const uint8_t no = 0;
    mem::write(slot + Slot_Shown, &no, 1);
    const int zero = 0;
    mem::write(slot + Slot_Priority, &zero, sizeof(zero));
    const OnlineId none{};
    mem::write(slot + Slot_OnlineId, &none, sizeof(none)); // free for the game (and for us) again
}

void updateManager(uintptr_t mgr, const std::vector<net::RemoteInfo>& players, bool enabled) {
    uintptr_t range[2] = {};
    if (!mem::read(mgr + Mgr_Slots, range, sizeof(range)) || range[1] <= range[0] ||
        (range[1] - range[0]) % Slot_Size || (range[1] - range[0]) / Slot_Size > 64)
        return;
    const size_t count = (range[1] - range[0]) / Slot_Size;
    std::vector<uint8_t> slots(count * Slot_Size);
    if (!mem::read(range[0], slots.data(), slots.size())) return;
    static size_t loggedCount = 0;
    if (count != loggedCount) {
        loggedCount = count;
        MLOG("map: the game's friend markers have %zu slots (players beyond that are not shown on the map)", count);
    }
    auto slotId = [&](size_t i) {
        uint64_t id = 0;
        memcpy(&id, &slots[i * Slot_Size + Slot_OnlineId], sizeof(id));
        return id;
    };

    // Players who left (or the feature was turned off): their slots are cleared and hidden.
    for (size_t i = 0; i < count; ++i) {
        const uint64_t id = slotId(i);
        if (!ours(id)) continue;
        bool present = false;
        for (const auto& p : players) present = present || (kIdTag | p.id) == id;
        if (!enabled || !present) hideSlot(range[0] + i * Slot_Size);
    }
    if (!enabled) return;

    static thread_local net::PlayerState st;
    for (const auto& p : players) {
        uint32_t age = 0;
        if (!net::remoteState(p.id, st, age) || age >= kShowAfterMs || !(st.flags & 1)) continue;
        const uint64_t id = kIdTag | p.id;
        uintptr_t slot = 0;
        for (size_t i = 0; i < count && !slot; ++i)
            if (slotId(i) == id) slot = range[0] + i * Slot_Size;
        const float pos[4] = {st.pos[0], st.pos[1], st.pos[2], 0.f};
        if (!slot) {
            // A slot of its own: the game spawns the marker prefab for it the first time.
            OnlineId oid{};
            oid.id = id;
            snprintf(oid.name, sizeof(oid.name), "%s", p.name.empty() ? "Runner" : p.name.c_str());
            auto assign = reinterpret_cast<AssignSlotFn>(rva(Fn_PresenceInfoManager_assignSlot));
            slot = reinterpret_cast<uintptr_t>(assign(reinterpret_cast<void*>(mgr), &oid, pos, kPriority));
            if (!slot) continue; // every slot holds someone of a higher or equal priority
            MLOG("map: player %08x ('%s') is on the map", p.id, oid.name);
        }
        mem::write(slot + Slot_Pos, pos, sizeof(pos));
        const uint8_t yes = 1;
        mem::write(slot + Slot_Shown, &yes, 1); // re-asserted every frame: the game's privacy lookup may clear it
    }
}

} // namespace

void beforeManagerUpdate(void* pool) {
    const uintptr_t pl = reinterpret_cast<uintptr_t>(pool);
    uintptr_t range[2] = {};
    if (!pl || !mem::read(pl + Pool_Entries, range, sizeof(range)) || range[1] <= range[0] ||
        (range[1] - range[0]) / Pool_EntrySize > 16)
        return;
    const size_t n = (range[1] - range[0]) / Pool_EntrySize;
    uintptr_t entries[16 * 2] = {};
    if (!mem::read(range[0], entries, n * Pool_EntrySize)) return;
    const bool enabled = config::get()->playersOnMap && net::running();
    const std::vector<net::RemoteInfo> players = enabled ? net::remotes(kShowAfterMs) : std::vector<net::RemoteInfo>{};
    for (size_t i = 0; i < n; ++i) {
        const uintptr_t mgr = entries[i * 2];
        uintptr_t vt = 0;
        if (!mgr || !mem::read(mgr, vt) || vt != rva(Vtbl_PamClientPresenceInfoManagerEntity)) continue;
        updateManager(mgr, players, enabled);
        g_status = enabled ? "on" : "off";
    }
}

const char* statusWord() { return g_status.load(); }

} // namespace mecmp::game::players_map
