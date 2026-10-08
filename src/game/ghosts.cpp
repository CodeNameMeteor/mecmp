#include "game/ghosts.h"

#include <atomic>
#include <cstring>
#include <mutex>
#include <vector>

#include "core/config.h"
#include "core/log.h"
#include "game/engine.h"
#include "game/level.h"
#include "game/models.h"
#include "net/net.h"

namespace mecmp::game::ghosts {
namespace {

constexpr uint32_t kFreshMs = 1500;     // a player counts as present while packets are this recent
constexpr uint32_t kHideAfterMs = 5000; // hide a ghost whose player stopped sending
constexpr DWORD kSpawnGapMs = 2000;     // at most one spawn per this interval (client-side matching)
constexpr DWORD kClaimWindowMs = 1500;  // client objects created this soon after our spawn are ours
constexpr uint64_t kSettleTicks = 90;   // ~3 s after a level starts before spawning

struct Ghost {
    uint32_t peer = 0;
    void* character = nullptr;  // server PamServerCharacterEntity
    void* spawner = nullptr;
    void* clientChar = nullptr; // client PamClientCharacterEntity
    void* clientComp = nullptr; // client PamClientReplayPlayerComponent
    DWORD spawnedAt = 0;
    DWORD clientCharAt = 0;
    int visibleCalls = 0;
    bool hidden = false;
    bool posing = false;
};

std::mutex g_m;               // guards g_ghosts (client hooks read/write the client fields)
std::vector<Ghost> g_ghosts;
std::atomic<uint32_t> g_pendingPeer{0};
std::atomic<DWORD> g_pendingAt{0};
std::atomic<uintptr_t> g_ghostCharData{0}; // the ghost blueprint's PamCharacterEntityData
DWORD g_lastSpawnAt = 0;
std::atomic<bool> g_paused{false};
std::atomic<bool> g_reqSpawn{false}, g_reqHide{false}, g_reqNextModel{false};
std::mutex g_reqModelMutex;
std::string g_reqModel; // set by the menu, applied on the game thread

bool claimWindowOpen() {
    const DWORD at = g_pendingAt.load();
    return at && GetTickCount() - at < kClaimWindowMs;
}

void setClientVisible(void* ent, bool visible) {
    const uintptr_t e = reinterpret_cast<uintptr_t>(ent);
    uintptr_t vt = 0, fn = 0;
    if (!mem::read(e, vt) || vt != rva(Vtbl_PamClientCharacterEntity) || !mem::read(vt + Vtbl_ClientSetVisible, fn)) return;
    reinterpret_cast<void (*)(void*, uint64_t)>(fn)(ent, visible ? 1 : 0);
}

bool clientCompAlive(void* comp) {
    uintptr_t vt = 0;
    return comp && mem::read(reinterpret_cast<uintptr_t>(comp), vt) && vt == rva(Vtbl_PamClientReplayPlayerComponent);
}

// Gives the ghost's server replay component a pose buffer with the bone setup real ghosts use.
// Buffers come from the component's own engine allocators, so its destructor frees them.
bool preparePose(uint8_t* c) {
    if (*reinterpret_cast<void**>(c + SRP_Pose)) return true;
    level::BoneSetup ref;
    if (!level::referenceSetup(ref)) return false;
    const uint32_t N = ref.count;
    uint8_t* S = c + SRP_Setup;
    if (!*reinterpret_cast<void**>(S + 0x10)) *reinterpret_cast<void**>(S + 0x08) = nullptr;
    if (!*reinterpret_cast<void**>(S + 0x78)) *reinterpret_cast<void**>(S + 0x90) = nullptr;
    auto* idx = static_cast<uint32_t*>(engineAlloc(reinterpret_cast<void**>(S + 0x08), N * 4, 4));
    const uint32_t flagCap = (N + 15) & ~15u;
    auto* flags = static_cast<uint8_t*>(engineAlloc(reinterpret_cast<void**>(S + 0x90), flagCap, 4));
    auto* pose = static_cast<float*>(engineAlloc(reinterpret_cast<void**>(c + SRP_PoseArena), N * 0x20, 16));
    if (!idx || !flags || !pose) return false;
    for (uint32_t i = 0; i < N; ++i) {
        idx[i] = ref.indices[i];
        flags[i] = ref.flags[i];
    }
    memset(pose, 0, N * 0x20);
    *reinterpret_cast<uint32_t**>(S + 0x10) = idx;
    *reinterpret_cast<uint32_t*>(S + 0x18) = N;
    *reinterpret_cast<uint8_t**>(S + 0x78) = flags;
    *reinterpret_cast<uint8_t**>(S + 0x80) = flags + N;
    *reinterpret_cast<uint8_t**>(S + 0x88) = flags + flagCap;
    *reinterpret_cast<float**>(c + SRP_Pose) = pose;
    *reinterpret_cast<float**>(c + SRP_PoseEnd) = pose + N * 8;
    *reinterpret_cast<float**>(c + SRP_PoseCap) = pose + N * 8;
    *reinterpret_cast<uint32_t*>(S) = N; // count last
    return true;
}

int trajectorySlot(uint8_t* c, uint32_t n) {
    const uint32_t* idx = *reinterpret_cast<uint32_t**>(c + SRP_Setup + 0x10);
    for (uint32_t i = 0; idx && i < n; ++i)
        if ((idx[i] & 0xff) == 1) return static_cast<int>(i); // AITrajectory: the world anchor
    return -1;
}

// Server side: the streamed pose into the ghost's replay component, replicated to clients.
void drive(Ghost& g, const net::PlayerState& st) {
    auto* c = static_cast<uint8_t*>(getComponent(g.character, TI_PamServerReplayPlayerComponent));
    if (!c || !preparePose(c)) return;
    const uint32_t N = *reinterpret_cast<uint32_t*>(c + SRP_Setup);
    auto* pose = *reinterpret_cast<float**>(c + SRP_Pose);
    if (!pose || N == 0 || N > kMaxBones) return;
    *reinterpret_cast<uint32_t*>(c + SRP_PlaybackState) = 0; // no server replay overwriting the pose
    const int traj = trajectorySlot(c, N);
    if (st.boneCount == N) {
        memcpy(pose, st.bones, N * 0x20);
        if (!g.posing) {
            g.posing = true;
            VLOG("ghost: player %08x now drives ghost %p with %u bones", g.peer, g.character, N);
        }
    }
    if (traj >= 0) {
        float* t = pose + traj * 8;
        t[0] = st.pos[0]; t[1] = st.pos[1]; t[2] = st.pos[2]; t[3] = 1.f;
        if (st.boneCount != N) { // no pose: at least face the right way
            t[4] = 0.f; t[5] = std::sin(st.yaw * 0.5f); t[6] = 0.f; t[7] = std::cos(st.yaw * 0.5f);
        }
    }
    setNetDirty(c + SRP_NetState, 2 | 4); // frames + pose
}

// Client side: a ghost client normally plays its own replay and parks the body at a fixed
// transform. Stop its playback, let it place the body from the trajectory bone, and pin that
// bone to the player's position (the client playback rewrites the received pose every frame).
void fixClient(Ghost& g, const net::PlayerState& st) {
    if (!clientCompAlive(g.clientComp)) return;
    const uintptr_t c = reinterpret_cast<uintptr_t>(g.clientComp);
    if (mem::readOr<uint32_t>(c + CRP_PlaybackState) != 0) poke<uint32_t>(c + CRP_PlaybackState, 0);
    if (mem::readOr<uint8_t>(c + CRP_PlaybackForced) != 0) poke<uint8_t>(c + CRP_PlaybackForced, 0);
    if (mem::readOr<uint8_t>(c + CRP_SkipRoot) != 0) poke<uint8_t>(c + CRP_SkipRoot, 0);
    const uintptr_t pose = mem::readOr<uintptr_t>(c + CRP_ReceivedPose);
    const uint32_t n = mem::readOr<uint32_t>(c + CRP_ExpectedBones);
    uint32_t idx[kMaxBones];
    const uintptr_t idxPtr = mem::readOr<uintptr_t>(c + CRP_Setup + 0x10);
    if (!pose || !n || n > kMaxBones || !idxPtr || !mem::read(idxPtr, idx, n * 4)) return;
    for (uint32_t i = 0; i < n; ++i) {
        if ((idx[i] & 0xff) != 1) continue;
        poke(pose + i * 0x20, st.pos, sizeof(st.pos));
        break;
    }
}

void* spawnGhost(uint32_t peer, const net::PlayerState& st) {
    void* spawner = level::ghostSpawner();
    if (!level::spawnerAlive(spawner)) return nullptr;
    const uintptr_t sd = mem::readOr<uintptr_t>(reinterpret_cast<uintptr_t>(spawner) + Spawner_Data);
    const uintptr_t sbp = sd ? mem::readOr<uintptr_t>(sd + ReferenceObjectData_Blueprint) : 0;
    if (sbp) g_ghostCharData = sbp + 0x60;
    SpawnInfo info;
    info.transform = transformFromYaw(st.pos, st.yaw);
    info.useTransform = 1;
    uint8_t* enabled = static_cast<uint8_t*>(spawner) + Spawner_Enabled;
    const uint8_t oldEnabled = *enabled;
    *enabled = 1; // the spawner refuses while disabled; enable it for this one call
    {   // register first: the client objects may be created while spawn() runs
        std::lock_guard<std::mutex> lock(g_m);
        Ghost g;
        g.peer = peer;
        g.spawner = spawner;
        g.spawnedAt = GetTickCount();
        g_ghosts.push_back(g);
    }
    g_pendingPeer = peer;
    g_pendingAt = GetTickCount();
    using SpawnFn = void* (*)(void*, SpawnInfo*);
    void* ch = (*reinterpret_cast<SpawnFn**>(spawner))[Vtbl_SpawnerSpawn / 8](spawner, &info);
    *enabled = oldEnabled;
    {
        std::lock_guard<std::mutex> lock(g_m);
        for (size_t i = 0; i < g_ghosts.size(); ++i)
            if (g_ghosts[i].peer == peer && !g_ghosts[i].character) {
                if (ch) g_ghosts[i].character = ch;
                else g_ghosts.erase(g_ghosts.begin() + static_cast<long>(i));
                break;
            }
    }
    if (!ch) {
        MLOG("ghost: the spawner returned no character");
        return nullptr;
    }
    if (const uintptr_t d = mem::readOr<uintptr_t>(reinterpret_cast<uintptr_t>(ch) + Character_Data)) g_ghostCharData = d;
    setTransform(ch, info.transform);
    std::string used;
    if (!models::apply(ch, config::get().model, &used) && !models::apply(ch, "RunVis", &used))
        MLOG("ghost: no model could be applied (the ghost may be invisible)");
    MLOG("ghost: spawned for player %08x as '%s'", peer, used.c_str());
    return ch;
}

void applyModelToAll(const std::string& model) {
    std::vector<void*> chars;
    {
        std::lock_guard<std::mutex> lock(g_m);
        for (auto& g : g_ghosts) chars.push_back(g.character);
    }
    for (void* ch : chars) models::apply(ch, model);
}

void handleRequests() {
    if (g_reqHide.exchange(false)) {
        g_paused = true;
        std::lock_guard<std::mutex> lock(g_m);
        for (auto& g : g_ghosts) {
            if (g.clientChar) setClientVisible(g.clientChar, false);
            g.hidden = true;
        }
        MLOG("ghosts: hidden, automatic spawning paused (F7 resumes)");
    }
    if (g_reqNextModel.exchange(false)) {
        const auto names = models::names();
        std::string& cur = config::mut().model;
        size_t at = 0;
        for (size_t i = 0; i < names.size(); ++i)
            if (names[i] == cur) at = i;
        void* probe = nullptr;
        {
            std::lock_guard<std::mutex> lock(g_m);
            if (!g_ghosts.empty()) probe = g_ghosts.front().character;
        }
        for (size_t step = 1; step <= names.size(); ++step) { // next model that actually applies
            const std::string& next = names[(at + step) % names.size()];
            if (!probe || models::apply(probe, next)) {
                cur = next;
                applyModelToAll(next);
                MLOG("ghosts: model '%s'", next.c_str());
                break;
            }
        }
    }
    {
        std::string want;
        {
            std::lock_guard<std::mutex> lock(g_reqModelMutex);
            want.swap(g_reqModel);
        }
        if (!want.empty()) {
            config::mut().model = want;
            applyModelToAll(want);
            MLOG("ghosts: model '%s'", want.c_str());
        }
    }
    if (g_reqSpawn.exchange(false)) {
        if (g_paused.exchange(false)) MLOG("ghosts: automatic spawning resumed");
        {
            std::lock_guard<std::mutex> lock(g_m);
            for (auto& g : g_ghosts) g.hidden = false, g.visibleCalls = 0; // show again (tick re-asserts)
            g_lastSpawnAt = 0;
        }
        applyModelToAll(config::get().model); // and refresh their look
    }
}
} // namespace

bool onClientReplayComp(void* comp) {
    if (!comp || !claimWindowOpen()) return false;
    const uint32_t peer = g_pendingPeer.load();
    std::lock_guard<std::mutex> lock(g_m);
    for (auto& g : g_ghosts)
        if (g.peer == peer && !g.clientComp) {
            g.clientComp = comp;
            return true;
        }
    return false;
}

void onClientCharacter(void* ent) {
    if (!ent || !claimWindowOpen()) return;
    const uintptr_t data = mem::readOr<uintptr_t>(reinterpret_cast<uintptr_t>(ent) + Character_Data);
    if (!data || data != g_ghostCharData.load()) return;
    const uint32_t peer = g_pendingPeer.load();
    {
        std::lock_guard<std::mutex> lock(g_m);
        for (auto& g : g_ghosts)
            if (g.peer == peer && !g.clientChar) {
                g.clientChar = ent;
                g.clientCharAt = GetTickCount();
                break;
            }
    }
    setClientVisible(ent, true);
}

void tick(uint64_t tick, uint64_t ticksInLevel) {
    (void)tick;
    handleRequests();
    const auto remotes = net::remotes(kHideAfterMs + 60000);

    // Forget ghosts the game destroyed or took back.
    {
        std::lock_guard<std::mutex> lock(g_m);
        for (size_t i = 0; i < g_ghosts.size();) {
            if (!g_ghosts[i].character || level::characterAlive(g_ghosts[i].character, g_ghosts[i].spawner)) { ++i; continue; }
            // The game sometimes takes a fresh ghost back from the city-ghost spawner right after a
            // level loads; the entity can live on, frozen. Hide it before forgetting it.
            if (g_ghosts[i].clientChar) setClientVisible(g_ghosts[i].clientChar, false);
            VLOG("ghost: ghost of player %08x is gone", g_ghosts[i].peer);
            g_ghosts.erase(g_ghosts.begin() + static_cast<long>(i));
        }
    }

    // Spawn a ghost for a present player that has none (one per kSpawnGapMs).
    const bool canSpawn = !g_paused && config::get().autoSpawn && ticksInLevel >= kSettleTicks &&
                          level::spawnerAlive(level::ghostSpawner()) && level::playerCharacter();
    if (canSpawn && GetTickCount() - g_lastSpawnAt >= kSpawnGapMs) {
        level::BoneSetup ref;
        for (const auto& r : remotes) {
            if (r.ageMs > kFreshMs) continue;
            bool has = false;
            {
                std::lock_guard<std::mutex> lock(g_m);
                for (auto& g : g_ghosts) has = has || g.peer == r.id;
            }
            if (has || !level::referenceSetup(ref)) continue;
            net::PlayerState st;
            uint32_t age = 0;
            if (!net::remoteState(r.id, st, age)) continue;
            g_lastSpawnAt = GetTickCount();
            spawnGhost(r.id, st);
            break;
        }
    }

    // Drive every ghost.
    std::vector<Ghost> ghosts;
    {
        std::lock_guard<std::mutex> lock(g_m);
        ghosts = g_ghosts;
    }
    static net::PlayerState st; // large; game thread only
    for (Ghost& g : ghosts) {
        if (!g.character) continue;
        uint32_t age = 0;
        const bool have = net::remoteState(g.peer, st, age);
        const bool present = have && age < kHideAfterMs && !g_paused;
        // Visibility: the client character starts hidden (the time-trial marker normally shows it);
        // show it a few times after creation and every 3 s after that, hide it when the player left.
        bool visibilityChanged = false;
        if (g.clientChar) {
            const DWORD since = GetTickCount() - g.clientCharAt;
            if (!present && !g.hidden) {
                setClientVisible(g.clientChar, false);
                g.hidden = true;
                visibilityChanged = true;
            } else if (present) {
                static const DWORD kAt[] = {300, 1000, 2500, 5000};
                const DWORD due = g.visibleCalls < 4 ? kAt[g.visibleCalls] : 5000u + 3000u * static_cast<DWORD>(g.visibleCalls - 3);
                if (g.hidden || since >= due) {
                    setClientVisible(g.clientChar, true);
                    ++g.visibleCalls;
                    g.hidden = false;
                    visibilityChanged = true;
                }
            }
        }
        if (have) {
            drive(g, st);
            fixClient(g, st);
        }
        std::lock_guard<std::mutex> lock(g_m); // write back what this tick changed
        for (auto& live : g_ghosts)
            if (live.character == g.character) {
                live.posing = g.posing;
                if (visibilityChanged) { live.hidden = g.hidden; live.visibleCalls = g.visibleCalls; }
            }
    }
}

void levelChanged() {
    std::lock_guard<std::mutex> lock(g_m);
    g_ghosts.clear();
    g_pendingAt = 0;
    g_lastSpawnAt = 0;
}

void requestSpawnOrRefresh() { g_reqSpawn = true; }
void requestHideAll() { g_reqHide = true; }
void requestNextModel() { g_reqNextModel = true; }
void requestModel(const std::string& name) {
    std::lock_guard<std::mutex> lock(g_reqModelMutex);
    g_reqModel = name;
}

std::string status() {
    std::lock_guard<std::mutex> lock(g_m);
    std::string s = std::to_string(g_ghosts.size()) + " ghost(s)";
    for (auto& g : g_ghosts) {
        char b[96];
        snprintf(b, sizeof(b), " [%08x%s%s]", g.peer, g.clientChar ? "" : " no-client", g.hidden ? " hidden" : "");
        s += b;
    }
    if (g_paused) s += ", paused";
    return s;
}

} // namespace mecmp::game::ghosts
