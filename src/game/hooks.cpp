#include "game/hooks.h"

#include "core/log.h"
#include "game/engine.h"
#include "game/ghosts.h"
#include "game/level.h"

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
void* clientReplayFactory(void* creator, void* ci, void* a3, void* a4) {
    void* comp = g_orig[kClientReplay](creator, ci, a3, a4);
    if (comp && !ghosts::onClientReplayComp(comp)) level::onForeignClientReplayComp(comp);
    return comp;
}
void* clientCharacterFactory(void* creator, void* ci, void* a3, void* a4) {
    void* ent = g_orig[kClientCharacter](creator, ci, a3, a4);
    if (ent) ghosts::onClientCharacter(ent);
    return ent;
}

// Swaps a creator-table slot, chaining to whatever was there (the game's factory or another
// mod's wrapper around it).
bool installFactory(Factory f, uintptr_t slotVa, uintptr_t expectedVa, FactoryFn hook) {
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
    g_orig[f] = reinterpret_cast<FactoryFn>(current);
    if (!mem::patchPointer(slot, reinterpret_cast<uintptr_t>(hook), nullptr)) {
        MLOG("hook %s: VirtualProtect failed (%lu)", kNames[f], GetLastError());
        return false;
    }
    VLOG("hook %s: installed", kNames[f]);
    return true;
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

bool install(TickFn onTick) {
    bool ok = true;
    ok &= installFactory(kSpawner, Slot_ServerCharacterSpawnFactory, Fn_ServerCharacterSpawnFactory, &spawnerFactory);
    ok &= installFactory(kPamSpawner, Slot_PamServerCharacterSpawnFactory, Fn_PamServerCharacterSpawnFactory, &pamSpawnerFactory);
    ok &= installFactory(kClientReplay, Slot_ClientReplayPlayerFactory, Fn_ClientReplayPlayerFactory, &clientReplayFactory);
    ok &= installFactory(kClientCharacter, Slot_ClientCharacterFactory, Fn_ClientCharacterFactory, &clientCharacterFactory);
    const uintptr_t slot = rva(Slot_ServerWorldPostUpdate);
    uintptr_t current = 0;
    if (!mem::read(slot, current) || current != rva(Fn_ServerWorldPostUpdate)) {
        MLOG("hook tick: ServerGameWorld slot holds %p, expected %p; not hooking", reinterpret_cast<void*>(current),
             reinterpret_cast<void*>(rva(Fn_ServerWorldPostUpdate)));
        return false;
    }
    g_onTick = onTick;
    g_origPostUpdate = reinterpret_cast<PostUpdateFn>(current);
    if (!mem::patchPointer(slot, reinterpret_cast<uintptr_t>(&postUpdate), nullptr)) {
        MLOG("hook tick: VirtualProtect failed (%lu)", GetLastError());
        return false;
    }
    MLOG("hooks: %s", ok ? "all installed" : "some factory hooks missing (see above)");
    return true;
}

} // namespace mecmp::game::hooks
