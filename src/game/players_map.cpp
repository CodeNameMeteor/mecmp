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

constexpr int kPriority = 1000;          // above any friend (the game uses 0)
constexpr uint32_t kShowAfterMs = 5000;  // a player silent this long leaves the map

struct OnlineId {
    uint64_t id;
    char name[32];
};
static_assert(sizeof(OnlineId) == 0x28, "OnlineId");

using AssignSlotFn = void* (*)(void* manager, const OnlineId* id, const float* pos, int priority);

std::atomic<const char*> g_status{"not seen yet"};

// The local player's own online id: the OnlineId at *0x14257cc68 + 0x30, which the game publishes its own
// position under (PresenceInfoManager_publishOwnWorldPosition). 0 while offline.
constexpr uintptr_t kOwnPresence = 0x14257cc68;

bool ownOnlineId(OnlineId& out) {
    const uintptr_t p = mem::readOr<uintptr_t>(rva(kOwnPresence));
    if (!p || !mem::read(p + 0x30, &out, sizeof(out)) || !out.id) return false;
    out.name[sizeof(out.name) - 1] = 0;
    return out.name[0] != 0;
}

// Online ids the mod put on the map (client game thread only): only these are ever cleared.
std::vector<uint64_t> g_placed;

bool placed(uint64_t id) {
    for (uint64_t x : g_placed)
        if (x == id) return true;
    return false;
}

void hideSlot(uintptr_t slot) {
    const uint8_t no = 0;
    mem::write(slot + Slot_Shown, &no, 1);
    const int zero = 0;
    mem::write(slot + Slot_Priority, &zero, sizeof(zero));
    const OnlineId none{};
    mem::write(slot + Slot_OnlineId, &none, sizeof(none)); // free for the game (and for us) again
}

// The profile a player is shown under: the one they share (HELLO), or ours for the solo-test mirror.
// The marker's icon is the profile's Runner Tag, which the game loads by online id: without a real id
// it stays empty, so players who do not share one get no marker.
bool profileOf(const net::RemoteInfo& p, OnlineId& out) {
    out = OnlineId{};
    if (p.loopback) return ownOnlineId(out);
    if (!p.onlineId || p.profileName.empty()) return false;
    out.id = p.onlineId;
    snprintf(out.name, sizeof(out.name), "%s", p.profileName.c_str());
    return true;
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
        MLOG("map: the game's friend markers have %llu slots (players beyond that are not shown on the map)",
             static_cast<unsigned long long>(count));
    }
    auto slotId = [&](size_t i) {
        uint64_t id = 0;
        memcpy(&id, &slots[i * Slot_Size + Slot_OnlineId], sizeof(id));
        return id;
    };

    // Who should be on the map now.
    struct Want { const net::RemoteInfo* p; OnlineId oid; };
    std::vector<Want> want;
    if (enabled)
        for (const auto& p : players) {
            Want w{&p, {}};
            if (profileOf(p, w.oid)) want.push_back(w);
        }

    // Players who left, stopped sharing or the feature was turned off: their slots are cleared and hidden.
    for (size_t i = 0; i < count; ++i) {
        const uint64_t id = slotId(i);
        if (!id || !placed(id)) continue;
        bool keep = false;
        for (const auto& w : want) keep = keep || w.oid.id == id;
        if (!keep) {
            hideSlot(range[0] + i * Slot_Size);
            for (size_t k = 0; k < g_placed.size(); ++k)
                if (g_placed[k] == id) { g_placed.erase(g_placed.begin() + static_cast<long>(k)); break; }
        }
    }

    static thread_local net::PlayerState st;
    for (const auto& w : want) {
        uint32_t age = 0;
        if (!net::remoteState(w.p->id, st, age) || age >= kShowAfterMs || !(st.flags & 1)) continue;
        uintptr_t slot = 0;
        for (size_t i = 0; i < count && !slot; ++i)
            if (slotId(i) == w.oid.id) slot = range[0] + i * Slot_Size;
        const float pos[4] = {st.pos[0], st.pos[1], st.pos[2], 0.f};
        if (!slot || !placed(w.oid.id)) {
            // A slot of its own (or the friend slot the game already gave them, raised to our priority): the
            // game spawns the marker prefab for a slot the first time it is used.
            auto assign = reinterpret_cast<AssignSlotFn>(rva(Fn_PresenceInfoManager_assignSlot));
            slot = reinterpret_cast<uintptr_t>(assign(reinterpret_cast<void*>(mgr), &w.oid, pos, kPriority));
            if (!slot) continue; // every slot holds someone of a higher or equal priority
            g_placed.push_back(w.oid.id);
            MLOG("map: player %08x is on the map%s", w.p->id, w.p->loopback ? " (solo test: under your own profile)" : "");
        }
        mem::write(slot + Slot_Pos, pos, sizeof(pos));
        const uint8_t yes = 1;
        mem::write(slot + Slot_Shown, &yes, 1); // re-asserted every frame: the game's privacy lookup may clear it
    }
}

} // namespace

void beforeManagerUpdate(void* pool) {
    const uintptr_t pl = reinterpret_cast<uintptr_t>(pool);
    static bool first = true;
    if (first) {
        first = false;
        VLOG("map: the game updates its friend markers (pool %p)", pool);
    }
    uintptr_t range[2] = {};
    if (!pl || !mem::read(pl + Pool_Entries, range, sizeof(range)) || range[1] <= range[0] ||
        (range[1] - range[0]) / Pool_EntrySize > 16)
        return;
    const size_t n = (range[1] - range[0]) / Pool_EntrySize;
    uintptr_t entries[16 * 2] = {};
    if (!mem::read(range[0], entries, n * Pool_EntrySize)) return;
    static size_t loggedN = static_cast<size_t>(-1);
    if (n != loggedN) { // what the pool holds (verbose log)
        loggedN = n;
        std::string what;
        for (size_t i = 0; i < n; ++i) {
            uintptr_t vt = 0;
            mem::read(entries[i * 2], vt);
            char one[64];
            snprintf(one, sizeof(one), " %p (vtable %p)", reinterpret_cast<void*>(entries[i * 2]), reinterpret_cast<void*>(vt));
            what += one;
        }
        VLOG("map: the friend-marker pool %p holds %llu manager(s):%s", pool, static_cast<unsigned long long>(n), what.c_str());
    }
    const bool enabled = config::get()->playersOnMap && net::running();
    const std::vector<net::RemoteInfo> players = enabled ? net::remotes(kShowAfterMs) : std::vector<net::RemoteInfo>{};
    for (size_t i = 0; i < n; ++i) {
        const uintptr_t mgr = entries[i * 2];
        uintptr_t vt = 0;
        if (!mgr || !mem::read(mgr, vt) || vt != rva(Vtbl_PamClientPresenceInfoManagerEntity)) continue;
        static uintptr_t lastMgr = 0;
        if (mgr != lastMgr) { // a new level: its slots start empty
            lastMgr = mgr;
            g_placed.clear();
        }
        updateManager(mgr, players, enabled);
        g_status = enabled ? "on" : "off";
    }
}

const char* statusWord() { return g_status.load(); }

void publishOwnProfile() {
    static uint64_t sent = 1; // something that is never an id, so the first call always sets it
    OnlineId own{};
    const bool share = config::get()->shareProfile && ownOnlineId(own);
    const uint64_t id = share ? own.id : 0;
    if (id == sent) return;
    sent = id;
    net::setOnlineProfile(id, own.name);
    MLOG(id ? "map: your EA profile is shared with the session (your Runner Tag shows on their maps)"
            : "map: your EA profile is not shared (offline, or share_profile=0)");
}

} // namespace mecmp::game::players_map
