#include "game/level.h"

#include <mutex>
#include <string>
#include <vector>

#include "core/log.h"
#include "game/models.h"

namespace mecmp::game::level {
namespace {
std::mutex g_m;
void* g_playerSpawner = nullptr;
void* g_ghostSpawner = nullptr;
std::vector<void*> g_clientComps; // other ghosts' client replay components (reference setup source)

bool clientCompAlive(uintptr_t c) {
    uintptr_t vt = 0;
    return mem::read(c, vt) && vt == rva(Vtbl_PamClientReplayPlayerComponent);
}

bool readSetup(uintptr_t comp, BoneSetup& out) {
    const uintptr_t S = comp + CRP_Setup;
    uint32_t n = 0;
    uintptr_t idx = 0, flags = 0;
    if (!mem::read(S, n) || n == 0 || n > kMaxBones) return false;
    if (!mem::read(S + 0x10, idx) || !idx || !mem::read(S + 0x78, flags) || !flags) return false;
    if (!mem::read(idx, out.indices, n * 4) || !mem::read(flags, out.flags, n)) return false;
    out.count = n;
    return true;
}
} // namespace

void onSpawnerCreated(void* ci, void* entity) {
    if (!ci || !entity) return;
    const uintptr_t c = reinterpret_cast<uintptr_t>(ci);
    const uintptr_t data = mem::readOr<uintptr_t>(c + CI_Data);
    const uintptr_t bp = data ? mem::readOr<uintptr_t>(data + ReferenceObjectData_Blueprint) : 0;
    std::string name;
    if (bp) mem::readCString(mem::readOr<uintptr_t>(bp + Asset_Name), name);
    if (name.empty()) return;
    models::noteTemplate(data, name);
    std::lock_guard<std::mutex> lock(g_m);
    if (name == kPlayerBlueprint) {
        g_playerSpawner = entity;
        VLOG("level: player spawner %p", entity);
    } else if (name == kGhostBlueprint) {
        g_ghostSpawner = entity; // the newest one is the city-ghost spawner
        VLOG("level: ghost spawner %p", entity);
    }
}

void onForeignClientReplayComp(void* comp) {
    std::lock_guard<std::mutex> lock(g_m);
    if (g_clientComps.size() >= 64) g_clientComps.erase(g_clientComps.begin());
    g_clientComps.push_back(comp);
}

void reset() {
    {
        std::lock_guard<std::mutex> lock(g_m);
        g_playerSpawner = nullptr;
        g_ghostSpawner = nullptr;
        g_clientComps.clear();
    }
    models::reset();
}

uintptr_t serverWorld() { return mem::readOr<uintptr_t>(rva(kServerGameWorld)); }

void* playerSpawner() {
    std::lock_guard<std::mutex> lock(g_m);
    return g_playerSpawner;
}

void* ghostSpawner() {
    std::lock_guard<std::mutex> lock(g_m);
    return g_ghostSpawner;
}

bool spawnerAlive(void* spawner) {
    uintptr_t vt = 0;
    return spawner && mem::read(reinterpret_cast<uintptr_t>(spawner), vt) &&
           (vt == rva(Vtbl_PamServerCharacterSpawnEntity) || vt == rva(Vtbl_ServerCharacterSpawnEntity));
}

bool characterAlive(void* character, void* spawner) {
    const uintptr_t ch = reinterpret_cast<uintptr_t>(character);
    uintptr_t vt = 0, getComp = 0;
    if (!mem::read(ch, vt) || !inImage(vt)) return false;
    if (!mem::read(vt + Vtbl_GetComponent, getComp) || !inImage(getComp)) return false;
    if (!spawnerAlive(spawner)) return false;
    const uintptr_t sp = reinterpret_cast<uintptr_t>(spawner);
    uintptr_t b = 0, e = 0;
    if (!mem::read(sp + Spawner_Spawned, b) || !mem::read(sp + Spawner_Spawned + 8, e) || e < b || e - b > 0x800) return false;
    for (uintptr_t q = b; q < e; q += 8)
        if (mem::readOr<uintptr_t>(q) == ch) return true;
    return false;
}

void* playerCharacter() {
    void* spawner = playerSpawner();
    if (!spawnerAlive(spawner)) return nullptr;
    auto* sp = static_cast<uint8_t*>(spawner);
    void** begin = *reinterpret_cast<void***>(sp + Spawner_Spawned);
    void** end = *reinterpret_cast<void***>(sp + Spawner_Spawned + 8);
    if (!begin || end <= begin || end - begin > 64) return nullptr;
    void* fallback = nullptr;
    for (void** it = end; it != begin;) { // newest first, prefer the one owned by a player
        void* c = *--it;
        if (!c || !isKindOf(c, TI_ServerCharacterEntity)) continue;
        if (*reinterpret_cast<void**>(static_cast<uint8_t*>(c) + Character_Player)) return c;
        if (!fallback) fallback = c;
    }
    return fallback;
}

bool referenceSetup(BoneSetup& out) {
    std::vector<void*> comps;
    {
        std::lock_guard<std::mutex> lock(g_m);
        comps = g_clientComps;
    }
    for (void* p : comps) {
        const uintptr_t c = reinterpret_cast<uintptr_t>(p);
        if (clientCompAlive(c) && readSetup(c, out)) return true;
    }
    return false;
}

} // namespace mecmp::game::level
