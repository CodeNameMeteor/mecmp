#include "game/level.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "core/log.h"
#include "game/models.h"

namespace mecmp::game::level {
namespace {
std::mutex g_m;
void* g_playerSpawner = nullptr;
void* g_ghostSpawner = nullptr;
std::vector<void*> g_clientComps; // other ghosts' client replay components (reference setup source)
std::vector<void*> g_ghostCands;  // every ghost-blueprint spawner seen (hooks and scans), for chooseGhostSpawner
// chooseGhostSpawner has picked g_ghostSpawner. A spawner the game creates later is only a candidate then:
// made current at once (as before), it was what the "keep the current one on a tie" rule compared against,
// so a tie went to another spawner than the one our ghosts came from.
bool g_ghostChosen = false;
// Spawners that gave no character several times in a row are not chosen again (until the level changes).
std::map<void*, int> g_spawnFails;
// By address and data: the game reuses memory, and a later spawner at the same address is another one.
std::vector<std::pair<void*, uintptr_t>> g_badSpawners;
constexpr int kMaxSpawnFails = 3;

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
    // A scan may only fill a gap, never replace a live spawner (a hook's newest capture always wins). A held
    // spawner that is no longer alive is a gap: the game destroys and rebuilds them when a district streams
    // out and in, and keeping the dead pointer left the mod with no spawner (no ghosts, "found" in the log).
    if (name == kPlayerBlueprint) {
        if (!fromScan || !g_playerSpawner || !spawnerAliveUnlocked(g_playerSpawner)) g_playerSpawner = entity;
        VLOG("level: player spawner %p%s", entity, fromScan ? " (found by scan)" : "");
    } else if (name == kGhostBlueprint) {
        // live capture: the newest one, until chooseGhostSpawner has looked at all of them (after that, only
        // when the chosen one is gone; otherwise the next choice decides)
        const bool gone = !g_ghostSpawner || !spawnerAliveUnlocked(g_ghostSpawner);
        if (gone) g_ghostChosen = false;
        if ((!fromScan && !g_ghostChosen) || gone) g_ghostSpawner = entity;
        if (g_ghostCands.size() < 64 && std::find(g_ghostCands.begin(), g_ghostCands.end(), entity) == g_ghostCands.end())
            g_ghostCands.push_back(entity);
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
    // How many spawners share each one's data, counted before sorting: counting inside the comparator read the
    // vector while std::sort was moving its elements, which can make the order inconsistent.
    std::map<uintptr_t, size_t> shared;
    for (const auto& g : ghosts) ++shared[g.second];
    std::stable_sort(ghosts.begin(), ghosts.end(),
                     [&](const auto& a, const auto& b) { return shared[a.second] < shared[b.second]; });
    for (const auto& g : ghosts) considerSpawner(reinterpret_cast<void*>(g.first), g.second, true);
    for (uintptr_t c : comps) onForeignClientReplayComp(reinterpret_cast<void*>(c));
    MLOG("level: scanned %llu MB in %.1f s: %llu spawners (%llu ghost), %llu ghost clients; player spawner %s, ghost spawner %s",
         static_cast<unsigned long long>(scanned >> 20), (GetTickCount() - start) / 1000.0,
         static_cast<unsigned long long>(spawners.size()), static_cast<unsigned long long>(ghosts.size()),
         static_cast<unsigned long long>(comps.size()), spawnerAlive(playerSpawner()) ? "found" : "missing",
         spawnerAlive(ghostSpawner()) ? "found" : "missing");
}

// Which of the ghost spawners is the level's city-ghost spawner (the one our ghosts are made from)?
// The newest one is not always it: when the mod is loaded before the level, the hooks see the pooled
// replay-marker spawners and others created after it, and ghosts made from one of those stood still after
// half a second. The city-ghost spawner has its own data object (the pooled ones share one) and holds the
// game's own city ghost: a character that is not one of ours (`ours`). Ours do not count: a ghost made from
// the wrong spawner would make it look like the right one.
void chooseGhostSpawner(const void* const* ours, size_t oursCount) {
    static DWORD lastAt = 0; // game thread only
    const DWORD now = GetTickCount();
    if (now - lastAt < 500) return;
    lastAt = now;
    std::vector<void*> cands;
    void* current = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_m);
        cands = g_ghostCands;
        current = g_ghostSpawner;
        for (const auto& bad : g_badSpawners)
            if (mem::readOr<uintptr_t>(reinterpret_cast<uintptr_t>(bad.first) + Spawner_Data) == bad.second)
                cands.erase(std::remove(cands.begin(), cands.end(), bad.first), cands.end());
    }
    struct Info { void* spawner; uintptr_t data; size_t holds; };
    std::vector<Info> alive;
    for (void* c : cands) {
        if (!spawnerAliveUnlocked(c)) continue;
        const uintptr_t s = reinterpret_cast<uintptr_t>(c);
        uintptr_t v[2] = {};
        size_t holds = 0; // characters in its list that are not ours
        if (mem::read(s + Spawner_Spawned, v, sizeof(v)) && v[1] >= v[0] && v[1] - v[0] <= 0x800) {
            const size_t count = (v[1] - v[0]) / 8;
            uintptr_t items[64] = {};
            if (count <= 64 && (count == 0 || mem::read(v[0], items, count * 8))) {
                for (size_t i = 0; i < count; ++i) {
                    bool mine = false;
                    for (size_t k = 0; k < oursCount; ++k) mine = mine || reinterpret_cast<uintptr_t>(ours[k]) == items[i];
                    if (!mine) ++holds;
                }
            } else {
                holds = count; // too many to compare: it holds something
            }
        }
        alive.push_back({c, mem::readOr<uintptr_t>(s + Spawner_Data), holds});
    }
    if (alive.empty()) return;
    void* best = nullptr;
    int bestScore = -1, currentScore = -1;
    for (const Info& a : alive) {
        size_t shared = 0;
        for (const Info& b : alive) shared += b.data == a.data ? 1 : 0;
        const bool own = shared == 1;
        const int score = own ? (a.holds ? 6 : 3) : (a.holds ? 1 : 0);
        if (a.spawner == current) currentScore = score;
        if (score >= bestScore) { // a tie among the others goes to the newer one
            best = a.spawner;
            bestScore = score;
        }
    }
    // The current one stays unless another is strictly better (or it is gone). A time trial brings a spawner of
    // its own that scores the same while the trial runs: switching on a tie flipped between the two every few
    // seconds, and ghosts made from the trial's spawner go with it when the trial ends.
    std::lock_guard<std::mutex> lock(g_m);
    if (best && best != current && bestScore > currentScore) {
        MLOG("level: ghost spawner %p chosen among %llu (score %d; was %p)", best, static_cast<unsigned long long>(alive.size()),
             bestScore, current);
        g_ghostSpawner = best;
    }
    g_ghostChosen = g_ghostSpawner != nullptr;
}

void spawnResult(void* spawner, bool gotCharacter) {
    std::lock_guard<std::mutex> lock(g_m);
    if (gotCharacter) {
        g_spawnFails.erase(spawner);
        return;
    }
    if (++g_spawnFails[spawner] < kMaxSpawnFails) return;
    g_spawnFails.erase(spawner);
    const std::pair<void*, uintptr_t> bad{spawner, mem::readOr<uintptr_t>(reinterpret_cast<uintptr_t>(spawner) + Spawner_Data)};
    if (std::find(g_badSpawners.begin(), g_badSpawners.end(), bad) == g_badSpawners.end() && g_badSpawners.size() < 64)
        g_badSpawners.push_back(bad);
    MLOG("level: ghost spawner %p gave no character %d times in a row; another one is chosen", spawner, kMaxSpawnFails);
    if (g_ghostSpawner == spawner) {
        g_ghostSpawner = nullptr; // the next choice (before the next spawn) takes the best of the others
        g_ghostChosen = false;
    }
}

void reset() {
    {
        std::lock_guard<std::mutex> lock(g_m);
        g_playerSpawner = nullptr;
        g_ghostSpawner = nullptr;
        g_clientComps.clear();
        g_ghostCands.clear();
        g_ghostChosen = false;
        g_spawnFails.clear();
        g_badSpawners.clear();
    }
    models::reset();
}

// A global in the game image, so always readable: a plain load (this runs several times a frame).
uintptr_t serverWorld() { return *reinterpret_cast<const volatile uintptr_t*>(rva(kServerGameWorld)); }

std::string levelName() {
    const uintptr_t world = serverWorld();
    const uintptr_t lvl = world ? mem::readOr<uintptr_t>(world + World_Level) : 0;
    const uintptr_t data = lvl ? mem::readOr<uintptr_t>(lvl + Level_Data) : 0;
    std::string name;
    if (data) mem::readCString(mem::readOr<uintptr_t>(data + Asset_Name), name);
    return name;
}

bool inFrontEnd() {
    static std::atomic<uintptr_t> cachedWorld{0};
    static std::atomic<bool> cached{false};
    const uintptr_t world = serverWorld();
    if (!world) return false;
    if (world != cachedWorld.load()) {
        const std::string n = levelName();
        if (n.empty()) return false; // not readable yet: ask again next time
        std::string lower = n;
        for (auto& c : lower) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
        cached = lower.find("frontend") != std::string::npos;
        cachedWorld = world;
    }
    return cached.load();
}

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
    uintptr_t head[Character_Data / 8 + 1] = {}; // the vtable and the data in one read
    return ch && mem::read(ch, head, sizeof(head)) && head[0] == rva(Vtbl_PamServerCharacterEntity) &&
           (!expectedData || head[Character_Data / 8] == expectedData);
}

void* playerCharacter() {
    void* spawner = playerSpawner();
    if (!spawner) return nullptr;
    // The spawner's vtable (is it still a spawner?) and its list of characters, in one read.
    uintptr_t head[Spawner_Spawned / 8 + 2];
    if (!mem::read(reinterpret_cast<uintptr_t>(spawner), head, sizeof(head)) ||
        (head[0] != rva(Vtbl_PamServerCharacterSpawnEntity) && head[0] != rva(Vtbl_ServerCharacterSpawnEntity)))
        return nullptr;
    const uintptr_t begin = head[Spawner_Spawned / 8], end = head[Spawner_Spawned / 8 + 1];
    if (!begin || end <= begin || (end - begin) / 8 > 64) return nullptr;
    uintptr_t items[64];
    const size_t count = (end - begin) / 8;
    if (!mem::read(begin, items, count * 8)) return nullptr; // the whole list in one read
    void* fallback = nullptr;
    for (size_t i = count; i-- > 0;) { // newest first, prefer the one owned by a player
        void* c = reinterpret_cast<void*>(items[i]);
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
