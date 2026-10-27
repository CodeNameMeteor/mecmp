#include "game/hooks.h"

#include "core/log.h"
#include "core/prof.h"
#include "game/players_map.h"
#include "game/runs.h"
#include "game/engine.h"
#include "game/ghosts.h"
#include "game/player_audio.h"
#include "game/level.h"
#include "game/models.h"
#include "game/replay_guard.h"
#include "net/net.h"

#include <atomic>
#include <vector>

namespace mecmp::game::hooks {
namespace {

using FactoryFn = void* (*)(void* creator, void* ci, void* a3, void* a4);
enum Factory { kSpawner, kPamSpawner, kClientReplay, kClientRecording, kClientCharacter, kCustomize, kPresenceManager, kNamedChallenge, kChallengeMarker, kClientSpawner, kFactoryCount };
FactoryFn g_orig[kFactoryCount] = {};
const char* const kNames[kFactoryCount] = {"ServerCharacterSpawnEntity", "PamServerCharacterSpawnEntity",
                                           "PamClientReplayPlayerComponent", "PamClientReplayRecordingComponent", "PamClientCharacterEntity",
                                           "PamServerCustomizeProjectionCharacterEntity", "PamClientPresenceInfoManagerEntity",
                                           "PamServerNamedChallengeEntity", "PamServerChallengeMarkerEntity", "ClientCharacterSpawnEntity"};

void* spawnerFactory(void* creator, void* ci, void* a3, void* a4) {
    void* e = g_orig[kSpawner](creator, ci, a3, a4);
    level::onSpawnerCreated(ci, e);
    return e;
}
// Time-trial marker look pickers carry the echo look catalogue: noting them as they are created saves a
// memory scan per level. Only objects with the right vtable count (checked when they are read).
void* customizeFactory(void* creator, void* ci, void* a3, void* a4) {
    void* e = g_orig[kCustomize](creator, ci, a3, a4);
    models::onCustomizeEntity(e);
    return e;
}
// Named challenges (dashes, ...): their data names a finished run (game/runs.h).
void* namedChallengeFactory(void* creator, void* ci, void* a3, void* a4) {
    void* e = g_orig[kNamedChallenge](creator, ci, a3, a4);
    runs::onNamedChallengeEntity(e);
    return e;
}

// Time-trial markers: their name names a finished time trial (game/runs.h).
void* challengeMarkerFactory(void* creator, void* ci, void* a3, void* a4) {
    void* e = g_orig[kChallengeMarker](creator, ci, a3, a4);
    runs::onChallengeMarker(e);
    return e;
}

// The friend-marker manager (game/players_map.h): only logged, to see whether a level has one.
void* presenceManagerFactory(void* creator, void* ci, void* a3, void* a4) {
    void* e = g_orig[kPresenceManager](creator, ci, a3, a4);
    MLOG("map: the level created a friend-marker manager %p", e);
    return e;
}

// Client character spawners: their list decides which client character the map's echo replay takes (ghosts.h).
void* clientSpawnerFactory(void* creator, void* ci, void* a3, void* a4) {
    void* e = g_orig[kClientSpawner](creator, ci, a3, a4);
    if (e) level::onClientSpawnerCreated(e);
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
    runs::clientTick(); // cheap once registered
    replay_guard::beforeClientUpdate(self);
    ghosts::beforeClientReplayUpdate();
    g_origUpdateAll(self, info);
    if (player_audio::kEnabled) player_audio::tick();
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
    const bool ours = ghosts::spawnInProgress();
    if (ci) {
        level::patchPlayerData(mem::readOr<uintptr_t>(reinterpret_cast<uintptr_t>(ci) + CI_Data), ours);
        level::patchPlayerData(mem::readOr<uintptr_t>(reinterpret_cast<uintptr_t>(ci) + 0x10), ours);
    }
    void* comp = g_orig[kClientReplay](creator, ci, a3, a4);
    hookUpdaters();
    if (comp) {
        level::patchPlayerData(mem::readOr<uintptr_t>(reinterpret_cast<uintptr_t>(comp) + 0x10), ours);
        if (!ghosts::onClientReplayComp(comp)) level::onForeignClientReplayComp(comp);
    }
    return comp;
}

// The local player's recorder takes its bones from the data when it is built, so the data is patched before.
void* clientRecordingFactory(void* creator, void* ci, void* a3, void* a4) {
    if (ci) level::patchRecordingData(mem::readOr<uintptr_t>(reinterpret_cast<uintptr_t>(ci) + CI_Data));
    void* comp = g_orig[kClientRecording](creator, ci, a3, a4);
    if (comp) level::patchRecordingData(mem::readOr<uintptr_t>(reinterpret_cast<uintptr_t>(comp) + 0x10));
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
    // The original is stored before the slot is patched: a call that gets in between must reach it.
    g_orig[f] = reinterpret_cast<FactoryFn>(current);
    if (!mem::patchPointer(slot, reinterpret_cast<uintptr_t>(hook), nullptr)) {
        MLOG("hook %s: VirtualProtect failed (%lu)", kNames[f], GetLastError());
        return false;
    }
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
// PamUISystem's per-frame update: only noted (gameMenuOpen reads its pause controller). Game menus pause the
// game as they normally do, in a session too. (Earlier versions kept the world running under the pause menu
// and the map while in a session; players wanted the pause back.)
using UISystemUpdateFn = void (*)(void* uiSystem);
UISystemUpdateFn g_origUISystemUpdate = nullptr;

std::atomic<uintptr_t> g_uiSystem{0};

void uiSystemUpdate(void* uiSystem) {
    g_uiSystem = reinterpret_cast<uintptr_t>(uiSystem);
    g_origUISystemUpdate(uiSystem);
}

void installUIPauseHook(std::vector<Patch>& undo) {
    const uintptr_t slot = rva(Slot_PamUISystemUpdate);
    uintptr_t current = 0;
    if (!mem::read(slot, current) || current != rva(Fn_PamUISystemUpdate)) {
        MLOG("hook UI system: slot holds %p, expected %p; the mod cannot tell when a game menu is open", reinterpret_cast<void*>(current),
             reinterpret_cast<void*>(rva(Fn_PamUISystemUpdate)));
        return;
    }
    g_origUISystemUpdate = reinterpret_cast<UISystemUpdateFn>(current); // before the patch, as above
    if (!mem::patchPointer(slot, reinterpret_cast<uintptr_t>(&uiSystemUpdate), nullptr)) {
        MLOG("hook UI system: VirtualProtect failed (%lu)", GetLastError());
        return;
    }
    undo.push_back({slot, current});
    VLOG("hook UI system: installed");
}

// The friend markers' manager pool: our players get slots just before the game copies the slots into the
// markers (game/players_map.h). Optional: without it there are no players on the map.
using PoolUpdateFn = void (*)(void* pool, void* updateInfo);
PoolUpdateFn g_origPresencePoolUpdate = nullptr;

void presencePoolUpdate(void* pool, void* updateInfo) {
    players_map::beforeManagerUpdate(pool);
    g_origPresencePoolUpdate(pool, updateInfo);
}

void installPlayersMapHook(std::vector<Patch>& undo) {
    const uintptr_t slot = rva(Slot_PresenceInfoManagerPoolUpdate);
    uintptr_t current = 0;
    if (!mem::read(slot, current) || current != rva(Fn_PresenceInfoManagerPoolUpdate)) {
        MLOG("hook map markers: slot holds %p, expected %p; players are not shown on the map",
             reinterpret_cast<void*>(current), reinterpret_cast<void*>(rva(Fn_PresenceInfoManagerPoolUpdate)));
        return;
    }
    g_origPresencePoolUpdate = reinterpret_cast<PoolUpdateFn>(current);
    if (!mem::patchPointer(slot, reinterpret_cast<uintptr_t>(&presencePoolUpdate), nullptr)) {
        MLOG("hook map markers: VirtualProtect failed (%lu)", GetLastError());
        return;
    }
    undo.push_back({slot, current});
    MLOG("hook map markers: installed");
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
    installFactory(kClientRecording, Slot_ClientReplayRecordingFactory, Fn_ClientReplayRecordingFactory, &clientRecordingFactory, undo);
    const bool chars = installFactory(kClientCharacter, Slot_ClientCharacterFactory, Fn_ClientCharacterFactory, &clientCharacterFactory, undo);
    // Optional: without it the look catalogue is found by a memory scan instead.
    installFactory(kCustomize, Slot_CustomizeProjectionFactory, Fn_CustomizeProjectionCreate, &customizeFactory, undo);
    got.ghostClients = replay && chars;
    // Optional: without it the map's echo can take a ghost's client character (the ghost then stops for a moment).
    installFactory(kClientSpawner, Slot_ClientCharacterSpawnFactory, Fn_ClientCharacterSpawnFactory, &clientSpawnerFactory, undo);
    installUIPauseHook(undo);
    installPlayersMapHook(undo);
    installFactory(kPresenceManager, Slot_PresenceInfoManagerCreator, Fn_PresenceInfoManagerCreator, &presenceManagerFactory, undo);
    // Optional: without it a finished run is announced without its name.
    installFactory(kNamedChallenge, Slot_ServerNamedChallengeCreator, Fn_ServerNamedChallengeCreator, &namedChallengeFactory, undo);
    installFactory(kChallengeMarker, Slot_ServerChallengeMarkerCreator, Fn_ServerChallengeMarkerCreator, &challengeMarkerFactory, undo);
    // The tick hook goes in last: nothing reaches the mod's code before every other hook is in, and
    // if it fails, the factory hooks above are taken out again.
    if (!mem::patchPointer(slot, reinterpret_cast<uintptr_t>(&postUpdate), nullptr)) {
        MLOG("hook tick: VirtualProtect failed (%lu); undoing the other hooks", GetLastError());
        undoPatches(undo);
        return false;
    }
    got.tick = true;
    hookUpdaters(); // the vtable is in the game image, so this does not wait for a component
    if (out) *out = got;
    if (player_audio::kEnabled) player_audio::install();
    if (got.spawners && got.ghostClients) MLOG("hooks: all installed");
    else MLOG("hooks: %s%s", got.spawners ? "" : "spawner hooks missing (spawners are found by a memory scan instead); ",
              got.ghostClients ? "" : "ghost client hooks missing: remote players cannot be shown");
    return true;
}

bool gameMenuOpen() {
    const uintptr_t ui = g_uiSystem.load();
    uintptr_t controller = 0;
    int32_t counts[2] = {};
    if (!ui || !mem::read(ui + UISystem_PauseController, controller) || !controller ||
        !mem::read(controller + 8, counts, sizeof(counts)))
        return false;
    return counts[0] > 0 || counts[1] > 0;
}

} // namespace mecmp::game::hooks
