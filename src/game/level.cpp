#include "game/level.h"

#include <algorithm>
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

bool spawnerAliveUnlocked(void* spawner) {
    uintptr_t vt = 0;
    return spawner && mem::read(reinterpret_cast<uintptr_t>(spawner), vt) &&
           (vt == rva(Vtbl_PamServerCharacterSpawnEntity) || vt == rva(Vtbl_ServerCharacterSpawnEntity));
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

// A spawner whose data names the player's or the ghost blueprint. `pooledData`: data shared by the
// 15 pooled city replay-marker ghost spawners (only known after a memory scan; those are a worse
// choice than the level's own city-ghost spawner).
void considerSpawner(void* entity, uintptr_t data, bool fromScan) {
    const uintptr_t bp = data ? mem::readOr<uintptr_t>(data + ReferenceObjectData_Blueprint) : 0;
    std::string name;
    if (bp) mem::readCString(mem::readOr<uintptr_t>(bp + Asset_Name), name);
    if (name.empty()) return;
    models::noteTemplate(data, name);
    std::lock_guard<std::mutex> lock(g_m);
    if (name == kPlayerBlueprint) {
        if (!fromScan || !g_playerSpawner) g_playerSpawner = entity;
        VLOG("level: player spawner %p%s", entity, fromScan ? " (found by scan)" : "");
    } else if (name == kGhostBlueprint) {
        if (!fromScan || !g_ghostSpawner) g_ghostSpawner = entity; // live capture: the newest one is the city-ghost spawner
        VLOG("level: ghost spawner %p%s", entity, fromScan ? " (found by scan)" : "");
    }
}

void onSpawnerCreated(void* ci, void* entity) {
    if (!ci || !entity) return;
    considerSpawner(entity, mem::readOr<uintptr_t>(reinterpret_cast<uintptr_t>(ci) + CI_Data), false);
}

void onForeignClientReplayComp(void* comp) {
    std::lock_guard<std::mutex> lock(g_m);
    if (g_clientComps.size() >= 64) g_clientComps.erase(g_clientComps.begin());
    g_clientComps.push_back(comp);
}

bool complete() {
    bool haveRef = false;
    {
        std::lock_guard<std::mutex> lock(g_m);
        if (!spawnerAliveUnlocked(g_playerSpawner) || !spawnerAliveUnlocked(g_ghostSpawner)) return false;
        for (void* p : g_clientComps) haveRef = haveRef || clientCompAlive(reinterpret_cast<uintptr_t>(p));
    }
    return haveRef;
}

// The mod may be loaded after the level (and its spawners) were created: find the spawners and
// ghost client components by their vtables in the game's heap. Read-only and fault-tolerant.
void scanForMissing() {
    const uintptr_t vPam = rva(Vtbl_PamServerCharacterSpawnEntity), vSrv = rva(Vtbl_ServerCharacterSpawnEntity),
                    vComp = rva(Vtbl_PamClientReplayPlayerComponent);
    const DWORD start = GetTickCount();
    std::vector<uintptr_t> spawners, comps;
    std::vector<uint8_t> buf(4 << 20);
    uint64_t scanned = 0;
    MEMORY_BASIC_INFORMATION mbi{};
    for (uintptr_t addr = 0x10000; VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof(mbi)) == sizeof(mbi);
         addr = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize) {
        if (mbi.State != MEM_COMMIT || mbi.Type != MEM_PRIVATE || mbi.Protect != PAGE_READWRITE) continue;
        const uintptr_t base = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        for (size_t off = 0; off < mbi.RegionSize; off += buf.size()) {
            const size_t n = std::min(buf.size(), static_cast<size_t>(mbi.RegionSize - off));
            if (!mem::read(base + off, buf.data(), n)) continue;
            scanned += n;
            const auto* q = reinterpret_cast<const uintptr_t*>(buf.data());
            for (size_t i = 0; i < n / 8; i += 2) { // objects are 16-byte aligned
                const uintptr_t v = q[i];
                if (v == vPam || v == vSrv) { if (spawners.size() < 4096) spawners.push_back(base + off + i * 8); }
                else if (v == vComp) { if (comps.size() < 64) comps.push_back(base + off + i * 8); }
            }
        }
    }
    // Ghost spawners: prefer one whose data is unique (the level's city-ghost spawner) over the
    // pooled replay-marker spawners that share one data object.
    std::vector<std::pair<uintptr_t, uintptr_t>> ghosts; // entity, data
    for (uintptr_t e : spawners) {
        const uintptr_t data = mem::readOr<uintptr_t>(e + Spawner_Data);
        const uintptr_t bp = data ? mem::readOr<uintptr_t>(data + ReferenceObjectData_Blueprint) : 0;
        std::string name;
        if (!bp || !mem::readCString(mem::readOr<uintptr_t>(bp + Asset_Name), name)) continue;
        if (name == kGhostBlueprint) ghosts.push_back({e, data});
        else considerSpawner(reinterpret_cast<void*>(e), data, true);
    }
    std::sort(ghosts.begin(), ghosts.end(), [&](const auto& a, const auto& b) {
        auto count = [&](uintptr_t d) { return std::count_if(ghosts.begin(), ghosts.end(), [d](const auto& g) { return g.second == d; }); };
        return count(a.second) < count(b.second);
    });
    for (const auto& g : ghosts) considerSpawner(reinterpret_cast<void*>(g.first), g.second, true);
    for (uintptr_t c : comps) onForeignClientReplayComp(reinterpret_cast<void*>(c));
    MLOG("level: scanned %llu MB in %.1f s: %llu spawners (%llu ghost), %llu ghost clients; player spawner %s, ghost spawner %s",
         static_cast<unsigned long long>(scanned >> 20), (GetTickCount() - start) / 1000.0,
         static_cast<unsigned long long>(spawners.size()), static_cast<unsigned long long>(ghosts.size()),
         static_cast<unsigned long long>(comps.size()), playerSpawner() ? "found" : "missing", ghostSpawner() ? "found" : "missing");
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

bool characterExists(void* character, uintptr_t expectedData) {
    const uintptr_t ch = reinterpret_cast<uintptr_t>(character);
    uintptr_t vt = 0, data = 0;
    return ch && mem::read(ch, vt) && vt == rva(Vtbl_PamServerCharacterEntity) && mem::read(ch + Character_Data, data) &&
           (!expectedData || data == expectedData);
}

void* playerCharacter() {
    void* spawner = playerSpawner();
    if (!spawnerAlive(spawner)) return nullptr;
    const uintptr_t sp = reinterpret_cast<uintptr_t>(spawner);
    uintptr_t list[2] = {};
    if (!mem::read(sp + Spawner_Spawned, list, sizeof(list))) return nullptr;
    const uintptr_t begin = list[0], end = list[1];
    if (!begin || end <= begin || (end - begin) / 8 > 64) return nullptr;
    void* fallback = nullptr;
    for (uintptr_t it = end; it != begin;) { // newest first, prefer the one owned by a player
        it -= 8;
        void* c = reinterpret_cast<void*>(mem::readOr<uintptr_t>(it));
        if (!c || !isKindOf(c, TI_ServerCharacterEntity)) continue;
        if (mem::readOr<uintptr_t>(reinterpret_cast<uintptr_t>(c) + Character_Player)) return c;
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
