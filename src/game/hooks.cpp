#include "game/hooks.h"

#include "core/log.h"
#include "game/engine.h"
#include "game/ghosts.h"
#include "game/level.h"
#include "net/net.h"
#include "MinHook.h"

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
// Client replay updater: the updateAll slot of its vtable (shared by the two updater objects) first
// writes our ghosts' poses, on the thread and at the moment the game reads them. Written from
// the render thread instead, the game sometimes read a pose half old and half new (jitter between
// two positions). The updater objects are created with the first client replay component.
using UpdateAllFn = void (*)(void* self, void* info);
UpdateAllFn g_origUpdateAll = nullptr;
std::atomic<bool> g_updaterHooked{false};

void updateAllHook(void* self, void* info) {
    ghosts::beforeClientReplayUpdate();
    g_origUpdateAll(self, info);
}

// Swaps the updateAll slot of the updater vtable. Both updater objects share that vtable, so one
// patched slot covers both; the slot is replaced in place (the other hooks work the same way), so
// the rest of the table is untouched and no copy of it has to be kept up to date.
void hookUpdaters() {
    if (g_updaterHooked) return;
    const uintptr_t slot = rva(Vtbl_ClientReplayUpdater) + Vtbl_ClientReplayUpdateAll;
    uintptr_t current = 0;
    if (!mem::read(slot, current)) return;
    if (current != rva(Fn_ClientReplayUpdateAll)) {
        static bool warned = false;
        if (!warned) MLOG("hook client replay updater: slot holds %p, expected %p; poses stay written per frame",
                          reinterpret_cast<void*>(current), reinterpret_cast<void*>(rva(Fn_ClientReplayUpdateAll)));
        warned = true;
        return;
    }
    g_origUpdateAll = reinterpret_cast<UpdateAllFn>(current);
    uintptr_t old = 0;
    if (mem::patchPointer(slot, reinterpret_cast<uintptr_t>(&updateAllHook), &old)) {
        g_updaterHooked = true;
        MLOG("hook client replay updater: installed");
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
using PauseConsumerFn = void (*)(const void* controller, void* client);
PauseConsumerFn g_origPauseConsumer = nullptr;

void pauseConsumer(const void* controller, void* client) {
    if (controller && net::running() && (net::hosting() || net::connectedToHost())) {
        struct ControllerView { uintptr_t vtable; int32_t requests, fullRequests; } view{};
        static_assert(sizeof(ControllerView) == 0x10, "UI pause consumer read layout");
        if (mem::read(reinterpret_cast<uintptr_t>(controller), view.vtable)) {
            // The native consumer reads only these counts; retain native references for offline resume.
            g_origPauseConsumer(&view, client);
            return;
        }
    }
    g_origPauseConsumer(controller, client);
}

void installPauseConsumer() {
    const uintptr_t target = rva(Fn_UIPauseConsumer);
    uint8_t bytes[9]{};
    const uint8_t expected[] = {0x48, 0x85, 0xd2, 0x0f, 0x84, 0x43, 0x01, 0x00, 0x00};
    if (!mem::read(target, bytes, sizeof(bytes)) || std::memcmp(bytes, expected, sizeof(bytes)) != 0) {
        MLOG("hook UI pause: unexpected native consumer; not hooking");
        return;
    }
    MH_STATUS status = MH_Initialize();
    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) {
        MLOG("hook UI pause: initialize failed (%s)", MH_StatusToString(status));
        return;
    }
    status = MH_CreateHook(reinterpret_cast<void*>(target), reinterpret_cast<void*>(&pauseConsumer),
                           reinterpret_cast<void**>(&g_origPauseConsumer));
    if (status != MH_OK) {
        MLOG("hook UI pause: create failed (%s)", MH_StatusToString(status));
        return;
    }
    status = MH_EnableHook(reinterpret_cast<void*>(target));
    if (status != MH_OK) {
        MH_RemoveHook(reinterpret_cast<void*>(target));
        g_origPauseConsumer = nullptr;
        MLOG("hook UI pause: enable failed (%s)", MH_StatusToString(status));
        return;
    }
    MLOG("hook UI pause: installed (native unpaused state while multiplayer is active)");
}



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
    installPauseConsumer();
    hookUpdaters(); // the vtable is in the game image, so this does not wait for a component
    if (out) *out = got;
    if (got.spawners && got.ghostClients) MLOG("hooks: all installed");
    else MLOG("hooks: %s%s", got.spawners ? "" : "spawner hooks missing (spawners are found by a memory scan instead); ",
              got.ghostClients ? "" : "ghost client hooks missing: remote players cannot be shown");
    return true;
}

} // namespace mecmp::game::hooks
