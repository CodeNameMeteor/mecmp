#include "game/hooks.h"

#include "core/log.h"
#include "game/engine.h"
#include "game/ghosts.h"
#include "game/level.h"

#include <atomic>
#include <vector>

namespace mecmp::game::hooks {
namespace {

using FactoryFn = void* (*)(void* creator, void* ci, void* a3, void* a4);
enum Factory { kSpawner, kPamSpawner, kClientReplay, kClientCharacter, kFactoryCount };
FactoryFn g_orig[kFactoryCount] = {};
const char* const kNames[kFactoryCount] = {"ServerCharacterSpawnEntity", "PamServerCharacterSpawnEntity",
                                           "PamClientReplayPlayerComponent", "PamClientCharacterEntity"};

void* spawnerFactory(void* creator, void* ci, void* a3, void* a4) {
    void* e = g_orig[kSpawner](creator, ci, a3, a4);
    level::onSpawnerCreated(ci, e);
    return e;
}
void* pamSpawnerFactory(void* creator, void* ci, void* a3, void* a4) {
    void* e = g_orig[kPamSpawner](creator, ci, a3, a4);
    level::onSpawnerCreated(ci, e);
    return e;
}
// Client replay updater: each of the (two) updater objects gets a copy of its vtable whose updateAll
// first writes our ghosts' poses, on the thread and at the moment the game reads them. Written from
// the render thread instead, the game sometimes read a pose half old and half new (jitter between
// two positions). The objects only exist once a client replay component was created.
using UpdateAllFn = void (*)(void* self, void* info);
UpdateAllFn g_origUpdateAll = nullptr;
uintptr_t g_updaterVtbl[16] = {};
std::atomic<bool> g_updaterHooked{false};

void updateAllHook(void* self, void* info) {
    ghosts::beforeClientReplayUpdate();
    g_origUpdateAll(self, info);
}

void hookUpdaters() {
    const uintptr_t vt = rva(Vtbl_ClientReplayUpdater);
    for (int i = 0; i < 2; ++i) {
        const uintptr_t obj = mem::readOr<uintptr_t>(rva(Data_ClientReplayUpdaters) + i * 8);
        if (!obj) continue;
        const uintptr_t cur = mem::readOr<uintptr_t>(obj);
        if (cur == reinterpret_cast<uintptr_t>(g_updaterVtbl)) continue; // done
        if (cur != vt) continue; // not the object this build expects: leave it
        if (!g_origUpdateAll) {
            if (!mem::read(vt, g_updaterVtbl, sizeof(g_updaterVtbl)) ||
                g_updaterVtbl[Vtbl_ClientReplayUpdateAll / 8] != rva(Fn_ClientReplayUpdateAll)) {
                static bool warned = false;
                if (!warned) MLOG("hook client replay updater: unexpected vtable; poses stay written per frame");
                warned = true;
                return;
            }
            g_origUpdateAll = reinterpret_cast<UpdateAllFn>(g_updaterVtbl[Vtbl_ClientReplayUpdateAll / 8]);
            g_updaterVtbl[Vtbl_ClientReplayUpdateAll / 8] = reinterpret_cast<uintptr_t>(&updateAllHook);
        }
        if (mem::patchPointer(obj, reinterpret_cast<uintptr_t>(g_updaterVtbl), nullptr)) {
            if (!g_updaterHooked.exchange(true)) MLOG("hook client replay updater: installed");
        }
    }
}

void* clientReplayFactory(void* creator, void* ci, void* a3, void* a4) {
    void* comp = g_orig[kClientReplay](creator, ci, a3, a4);
    hookUpdaters();
    if (comp && !ghosts::onClientReplayComp(comp)) level::onForeignClientReplayComp(comp);
    return comp;
}
void* clientCharacterFactory(void* creator, void* ci, void* a3, void* a4) {
    void* ent = g_orig[kClientCharacter](creator, ci, a3, a4);
    if (ent) ghosts::onClientCharacter(ent);
    return ent;
}

// A pointer we replaced, with what was there before: an install that fails part-way undoes these.
struct Patch { uintptr_t slot; uintptr_t original; };

// Swaps a creator-table slot, chaining to whatever was there (the game's factory or another
// mod's wrapper around it). A successful swap is recorded in `undo`.
bool installFactory(Factory f, uintptr_t slotVa, uintptr_t expectedVa, FactoryFn hook, std::vector<Patch>& undo) {
    const uintptr_t slot = rva(slotVa);
    uintptr_t current = 0;
    if (!mem::read(slot, current) || !current) {
        MLOG("hook %s: slot not readable", kNames[f]);
        return false;
    }
    if (current != rva(expectedVa)) {
        MEMORY_BASIC_INFORMATION mbi{};
        const bool exec = VirtualQuery(reinterpret_cast<void*>(current), &mbi, sizeof(mbi)) &&
                          (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY));
        if (!exec) {
            MLOG("hook %s: slot holds %p (not code); not hooking", kNames[f], reinterpret_cast<void*>(current));
            return false;
        }
        MLOG("hook %s: already redirected by another component; chaining in front of it", kNames[f]);
    }
    if (!mem::patchPointer(slot, reinterpret_cast<uintptr_t>(hook), nullptr)) {
        MLOG("hook %s: VirtualProtect failed (%lu)", kNames[f], GetLastError());
        return false;
    }
    g_orig[f] = reinterpret_cast<FactoryFn>(current);
    undo.push_back({slot, current});
    VLOG("hook %s: installed", kNames[f]);
    return true;
}

// Puts back every pointer an install changed, newest first.
void undoPatches(const std::vector<Patch>& undo) {
    for (auto it = undo.rbegin(); it != undo.rend(); ++it) mem::patchPointer(it->slot, it->original, nullptr);
}

using PostUpdateFn = void (*)(void* world, void* updateInfo);
PostUpdateFn g_origPostUpdate = nullptr;
TickFn g_onTick = nullptr;

void postUpdate(void* world, void* updateInfo) {
    g_origPostUpdate(world, updateInfo);
    if (world == reinterpret_cast<void*>(level::serverWorld())) {
        const float dt = updateInfo ? *reinterpret_cast<float*>(static_cast<uint8_t*>(updateInfo) + 0x10) : 0.f;
        g_onTick(world, dt);
    }
}
} // namespace

bool clientUpdateHooked() { return g_updaterHooked; }

bool install(TickFn onTick, Installed* out) {
    Installed got;
    if (out) *out = got;
    // Checks first, with nothing written: the tick hook is required, so if its slot is not what
    // this build expects, the game is left exactly as it was.
    const uintptr_t slot = rva(Slot_ServerWorldPostUpdate);
    uintptr_t current = 0;
    if (!mem::read(slot, current) || current != rva(Fn_ServerWorldPostUpdate)) {
        MLOG("hook tick: ServerGameWorld slot holds %p, expected %p; not hooking", reinterpret_cast<void*>(current),
             reinterpret_cast<void*>(rva(Fn_ServerWorldPostUpdate)));
        return false;
    }
    // Factory hooks only report objects as they are created; each group that fails costs a feature,
    // not stability (see Installed). Recorded so that a later failure can undo them.
    std::vector<Patch> undo;
    g_onTick = onTick;
    g_origPostUpdate = reinterpret_cast<PostUpdateFn>(current);
    const bool sp1 = installFactory(kSpawner, Slot_ServerCharacterSpawnFactory, Fn_ServerCharacterSpawnFactory, &spawnerFactory, undo);
    const bool sp2 = installFactory(kPamSpawner, Slot_PamServerCharacterSpawnFactory, Fn_PamServerCharacterSpawnFactory, &pamSpawnerFactory, undo);
    got.spawners = sp1 && sp2;
    const bool replay = installFactory(kClientReplay, Slot_ClientReplayPlayerFactory, Fn_ClientReplayPlayerFactory, &clientReplayFactory, undo);
    const bool chars = installFactory(kClientCharacter, Slot_ClientCharacterFactory, Fn_ClientCharacterFactory, &clientCharacterFactory, undo);
    got.ghostClients = replay && chars;
    // The tick hook goes in last: nothing reaches the mod's code before every other hook is in, and
    // if it fails, the factory hooks above are taken out again.
    if (!mem::patchPointer(slot, reinterpret_cast<uintptr_t>(&postUpdate), nullptr)) {
        MLOG("hook tick: VirtualProtect failed (%lu); undoing the other hooks", GetLastError());
        undoPatches(undo);
        return false;
    }
    got.tick = true;
    hookUpdaters(); // if a client replay component exists already
    if (out) *out = got;
    if (got.spawners && got.ghostClients) MLOG("hooks: all installed");
    else MLOG("hooks: %s%s", got.spawners ? "" : "spawner hooks missing (spawners are found by a memory scan instead); ",
              got.ghostClients ? "" : "ghost client hooks missing: remote players cannot be shown");
    return true;
}

} // namespace mecmp::game::hooks
