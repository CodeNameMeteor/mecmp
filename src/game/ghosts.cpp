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
#include "game/motion_stats.h"
#include "game/pose_math.h"
#include <map>
#include "net/net.h"

namespace mecmp::game::ghosts {
namespace {

constexpr uint32_t kFreshMs = 1500;     // a player counts as present while packets are this recent
constexpr uint32_t kHideAfterMs = 5000; // hide a ghost whose player stopped sending
constexpr DWORD kSpawnGapMs = 2000;     // at most one spawn per this interval (client-side matching)
constexpr DWORD kClaimWindowMs = 1500;  // client objects created this soon after our spawn are ours
constexpr uint64_t kSettleTicks = 90;   // ~3 s after a level starts before spawning
// Ghost entities are never destroyed by the mod (no known safe despawn call): a ghost whose player
// left is hidden and handed to the next player that needs one. So the level holds at most as many
// ghosts as players were ever there at the same time, and never more than this.
constexpr size_t kMaxGhosts = 15;
// Hiding the client character leaves its hair drawn (a separate mesh setVisible does not reach).
// So a ghost being hidden is also moved this far below where it was, for long enough to replicate.
constexpr float kParkDepth = 1000.f;
constexpr DWORD kParkMs = 1500;
// Once a ghost's client side is driven every rendered frame (renderFrame), the server stops
// replicating its pose after this long: replication only arrives once per server tick with an older
// pose and would make the ghost step back and forth. Until then (and without a client component)
// replication is what shows the pose.
constexpr DWORD kReplicateMs = 2000;
std::atomic<uint32_t> g_updaterCalls{0}; // client replay updater calls (status: per second)

bool setClientVisible(void* ent, bool visible);

struct Ghost {
    uint32_t peer = 0;          // player driving it (a free ghost keeps its last player's id)
    std::string name;           // that player's name: a reconnecting player gets their old ghost back
    void* character = nullptr;  // server PamServerCharacterEntity
    void* spawner = nullptr;
    void* clientChar = nullptr; // client PamClientCharacterEntity
    void* clientComp = nullptr; // client PamClientReplayPlayerComponent
    DWORD spawnedAt = 0;
    DWORD clientCharAt = 0;
    int visibleCalls = 0;
    bool hidden = false;
    bool posing = false;
    bool ghostShader = true;
    DWORD posingSince = 0;        // when the current player started driving it
    float lastPos[3] = {0, 0, 0}; // where it was last driven
    std::string lookKey;          // look applied now ("faith", "echo:<hash>" or a model name); empty = redo
    DWORD lookTriedAt = 0;
    DWORD parkUntil = 0;          // while hidden: keep it parked below lastPos until then
};

// Starts hiding a ghost: client character hidden, and parked below its last position.
void startHiding(Ghost& g) {
    if (g.clientChar) setClientVisible(g.clientChar, false);
    g.hidden = true;
    g.parkUntil = GetTickCount() + kParkMs;
    // Back to server-driven: the park position must replicate (and be pinned on the client side).
    // Left client-driven, renderFrame skips the hidden ghost and the server no longer replicates,
    // so the body never moved down and its hair stayed drawn where the player was.
    g.posing = false;
    g.posingSince = 0;
}

bool parking(const Ghost& g) { return g.hidden && g.parkUntil && static_cast<int32_t>(GetTickCount() - g.parkUntil) < 0; }

net::PlayerState parkedState(const Ghost& g) {
    net::PlayerState p; // no pose: only the trajectory bone (the body's anchor) moves
    p.pos[0] = g.lastPos[0];
    p.pos[1] = g.lastPos[1] - kParkDepth;
    p.pos[2] = g.lastPos[2];
    return p;
}

std::mutex g_m;               // guards g_ghosts (client hooks read/write the client fields)
std::vector<Ghost> g_ghosts;
std::atomic<uint32_t> g_pendingPeer{0};
std::atomic<DWORD> g_pendingAt{0};
std::atomic<uintptr_t> g_ghostCharData{0}; // the ghost blueprint's PamCharacterEntityData
std::atomic<uintptr_t> g_tiCharData{0};    // runtime TypeInfo of PamCharacterEntityData (set at startup)
// Tick cost (P-2/P-4), shown by F9: sum and max over the current 10 s window, in microseconds.
double g_costSum = 0, g_costMax = 0;    // game thread
uint32_t g_costCount = 0;
std::atomic<double> g_costShownAvg{0}, g_costShownMax{0}; // read by status() on the mod thread
std::atomic<uint32_t> g_costShownGhosts{0};
DWORD g_costWindowAt = 0;
DWORD g_lastSpawnAt = 0;
std::atomic<bool> g_paused{false};
std::atomic<bool> g_enabled{true};
std::mutex g_motionM;                  // guards g_motion (written by the game thread, read by status())
std::map<uint32_t, MotionStats> g_motion; // per player: what the ghost was driven with, tick to tick
std::atomic<bool> g_reqSpawn{false}, g_reqHide{false};

bool claimWindowOpen() {
    const DWORD at = g_pendingAt.load();
    return at && GetTickCount() - at < kClaimWindowMs;
}

// A client character that is (still) a ghost: right class and the ghost blueprint's data. Checked
// before every call, so a client object the game destroyed and whose memory went to something else
// is not called into. (It does not close the window in which the client thread destroys the object
// while the server thread uses it; see C-2 in docs/ARCHITECTURE.md.)
bool clientCharIsGhost(void* ent) {
    const uintptr_t e = reinterpret_cast<uintptr_t>(ent);
    uintptr_t vt = 0, data = 0;
    const uintptr_t want = g_ghostCharData.load();
    return e && mem::read(e, vt) && vt == rva(Vtbl_PamClientCharacterEntity) && mem::read(e + Character_Data, data) &&
           (!want || data == want);
}

// The entity a component belongs to: Component +0x08 = the owner's component collection, whose
// first entry is the owning entity (fb::Component ctor 0x14315f9e0).
uintptr_t componentOwner(uintptr_t comp) {
    const uintptr_t coll = mem::readOr<uintptr_t>(comp + Component_Collection);
    return coll ? mem::readOr<uintptr_t>(coll) : 0;
}

bool setClientVisible(void* ent, bool visible) {
    if (!clientCharIsGhost(ent)) return false;
    uintptr_t vt = 0, fn = 0;
    if (!mem::read(reinterpret_cast<uintptr_t>(ent), vt) || !mem::read(vt + Vtbl_ClientSetVisible, fn) || !inImage(fn)) return false;
    reinterpret_cast<void (*)(void*, uint64_t)>(fn)(ent, visible ? 1 : 0);
    return true;
}

bool clientCompAlive(void* comp, void* owner) {
    const uintptr_t c = reinterpret_cast<uintptr_t>(comp);
    uintptr_t vt = 0;
    if (!c || !mem::read(c, vt) || vt != rva(Vtbl_PamClientReplayPlayerComponent)) return false;
    // Same owner as when we claimed it (when the owner is known): the memory was not reused.
    return !owner || componentOwner(c) == reinterpret_cast<uintptr_t>(owner);
}

// Is `data` a PamCharacterEntityData? Calls the object's getType (vtable slot 0) only after checking
// that the vtable and the function are in the game image.
bool isCharacterData(uintptr_t data) {
    const uintptr_t ti = g_tiCharData.load();
    uintptr_t vt = 0, getType = 0;
    if (!ti || !data || !mem::read(data, vt) || !inImage(vt) || !mem::read(vt, getType) || !inImage(getType)) return false;
    return isKindOfRuntime(reinterpret_cast<void*>(data), ti);
}

// The ghost blueprint's character data, needed before the first spawn to recognise the client
// character (it may be created while spawn() runs). Candidates: ObjectBlueprint::Object (the field
// is at +0x40 in the type dump; PC data containers can sit 8 bytes further), and the object stored
// right after the blueprint in its partition (+0x60, what earlier versions assumed). Only a
// candidate that really is a PamCharacterEntityData is used.
uintptr_t blueprintCharacterData(uintptr_t bp) {
    const uintptr_t candidates[] = {mem::readOr<uintptr_t>(bp + ObjectBlueprint_Object),
                                    mem::readOr<uintptr_t>(bp + ObjectBlueprint_Object + 8), bp + 0x60};
    for (uintptr_t c : candidates)
        if (isCharacterData(c)) return c;
    return 0;
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

// Puts the trajectory bone (the body's anchor) of a pose buffer (8 floats per bone) at `pos`. The
// game places the body from bones 0 and 1 together, and something else reads bone 1 alone
// (pose_math.h): bone 0 is folded into bone 1, so both give `pos`.
void placeAnchor(float* pose, int traj, const float pos[3]) {
    float* t = pose + traj * 8;
    if (traj == 1) {
        foldBone0(pose, t, pos);
    } else { // not the layout the placement code expects: plain position
        t[0] = pos[0]; t[1] = pos[1]; t[2] = pos[2]; t[3] = 1.f;
    }
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
            g.posingSince = GetTickCount();
            VLOG("ghost: player %08x now drives ghost %p with %u bones", g.peer, g.character, N);
        }
    }
    if (traj >= 0) {
        float* t = pose + traj * 8;
        if (st.boneCount != N) { // no pose: at least face the right way
            t[4] = 0.f; t[5] = std::sin(st.yaw * 0.5f); t[6] = 0.f; t[7] = std::cos(st.yaw * 0.5f);
        }
        placeAnchor(pose, traj, st.pos);
    }
    const bool clientDriven = g.posing && g.clientComp && GetTickCount() - g.posingSince > kReplicateMs;
    if (!clientDriven) setNetDirty(c + SRP_NetState, 2 | 4); // frames + pose
}

// Client side: a ghost client normally plays its own replay and parks the body at a fixed
// transform. Stop its playback, let it place the body from the trajectory bone, and pin that
// bone to the player's position (the client playback rewrites the received pose every frame).
void fixClient(Ghost& g, const net::PlayerState& st) {
    if (!clientCompAlive(g.clientComp, g.clientChar)) return;
    const uintptr_t c = reinterpret_cast<uintptr_t>(g.clientComp);
    if (mem::readOr<uint32_t>(c + CRP_PlaybackState) != 0) poke<uint32_t>(c + CRP_PlaybackState, 0);
    if (mem::readOr<uint8_t>(c + CRP_PlaybackForced) != 0) poke<uint8_t>(c + CRP_PlaybackForced, 0);
    if (mem::readOr<uint8_t>(c + CRP_SkipRoot) != 0) poke<uint8_t>(c + CRP_SkipRoot, 0);
    // Driven every rendered frame (renderFrame): the anchor is set there, for the frame's own moment.
    if (g.posing && GetTickCount() - g.posingSince > kReplicateMs) return;
    const uintptr_t pose = mem::readOr<uintptr_t>(c + CRP_ReceivedPose);
    const uint32_t n = mem::readOr<uint32_t>(c + CRP_ExpectedBones);
    uint32_t idx[kMaxBones];
    const uintptr_t idxPtr = mem::readOr<uintptr_t>(c + CRP_Setup + 0x10);
    if (!pose || !n || n > kMaxBones || !idxPtr || !mem::read(idxPtr, idx, n * 4)) return;
    for (uint32_t i = 0; i < n; ++i) {
        if ((idx[i] & 0xff) != 1) continue;
        float bones[2][8];
        if (i != 1 || !mem::read(pose, bones, sizeof(bones))) { poke(pose + i * 0x20, st.pos, sizeof(st.pos)); break; }
        foldBone0(bones[0], bones[1], st.pos);
        poke(pose, bones, sizeof(bones));
        break;
    }
}

void setGhostShader(Ghost& ghost, bool enabled) {
    ghost.ghostShader = enabled;
    const uintptr_t client = reinterpret_cast<uintptr_t>(ghost.clientChar);
    uint8_t current = 0;
    if (!client || mem::readOr<uintptr_t>(client) != rva(Vtbl_PamClientCharacterEntity) ||
        mem::readOr<uintptr_t>(client + Character_Data) != g_ghostCharData.load() ||
        !mem::read(client + ClientChar_GhostShader, current) || current == static_cast<uint8_t>(enabled)) return;
    // Character creation caches the definition's shader flag in each client entity.
    poke<uint8_t>(client + ClientChar_GhostShader, enabled ? 1 : 0);
}
bool applyGhostModel(void* character, const std::string& key, std::string* used) {
    std::string applied;
    const bool ok = key.rfind("echo:", 0) == 0
        ? models::applyEcho(character, config::lookHash(key))
        : models::apply(character, key, &applied);
    if (!ok) return false;
    {
        std::lock_guard<std::mutex> lock(g_m);
        for (auto& ghost : g_ghosts) if (ghost.character == character) {
            setGhostShader(ghost, key.rfind("echo:", 0) == 0 || applied != "faith");
            break;
        }
    }
    if (used) *used = key.rfind("echo:", 0) == 0 ? key : applied;
    return true;
}

// The look a player's ghost should have: the one they chose (sent in HELLO).
std::string desiredLook(uint32_t playerLook) { return config::lookName(playerLook); }

// Applies a look key to a ghost. False when it could not be applied, and then the ghost keeps the look
// it has: a look that is not known yet (an echo the catalogue has not loaded) is simply tried again
// later. Only spawning falls back to the default red ghost (see spawnGhost).
bool applyLook(void* ch, const std::string& key, std::string* used = nullptr) {
    if (key.rfind("echo:", 0) == 0) {
        const uint32_t hash = config::lookHash(key);
        // A style that cannot render here (e.g. picked in an older version) would leave the ghost
        // invisible: show the plain ghost instead, and count it as done (no retries).
        if (!models::echoRenders(hash) && !models::echoes()->empty()) {
            if (!applyGhostModel(ch, "RunVis", used)) return false;
            if (used) *used = "RunVis (their style is not available in free roam)";
            return true;
        }
        // The catalogue is not loaded yet, or the look is unknown to it: keep the ghost's look, and the
        // caller retries later (it does not reapply RunVis each time: that rebuilds the mesh).
        if (!applyGhostModel(ch, key, used)) return false;
        if (used) *used = key;
        return true;
    }
    return applyGhostModel(ch, key, used);
}

// How long to wait before trying a look again: an echo the catalogue does not have is retried rarely
// (each attempt rebuilds the ghost's mesh), other looks sooner.
DWORD lookRetryMs(const std::string& key) { return key.rfind("echo:", 0) == 0 ? 15000 : 2000; }

void* spawnGhost(uint32_t peer, const std::string& name, uint32_t look, const net::PlayerState& st) {
    void* spawner = level::ghostSpawner();
    if (!level::spawnerAlive(spawner)) return nullptr;
    const uintptr_t sd = mem::readOr<uintptr_t>(reinterpret_cast<uintptr_t>(spawner) + Spawner_Data);
    const uintptr_t sbp = sd ? mem::readOr<uintptr_t>(sd + ReferenceObjectData_Blueprint) : 0;
    if (const uintptr_t d = sbp ? blueprintCharacterData(sbp) : 0) {
        g_ghostCharData = d;
    } else if (sbp && !g_ghostCharData) {
        g_ghostCharData = sbp + 0x60; // unverified (what earlier versions used); corrected after the spawn
        MLOG("ghost: could not verify the ghost blueprint's character data; using the old guess");
    }
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
        g.name = name;
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
    if (const uintptr_t d = mem::readOr<uintptr_t>(reinterpret_cast<uintptr_t>(ch) + Character_Data)) {
        if (g_ghostCharData && g_ghostCharData != d)
            MLOG("ghost: ghost character data is %p, not %p as expected from the blueprint", reinterpret_cast<void*>(d),
                 reinterpret_cast<void*>(g_ghostCharData.load()));
        g_ghostCharData = d;
    }
    setTransform(ch, info.transform);
    std::string used;
    const std::string key = desiredLook(look);
    const bool done = applyLook(ch, key, &used);
    if (!done) applyGhostModel(ch, "RunVis", &used); // visible at least; the wanted look is retried later
    {
        std::lock_guard<std::mutex> lock(g_m);
        for (auto& g : g_ghosts)
            if (g.character == ch) { g.lookKey = done ? key : std::string(); g.lookTriedAt = GetTickCount(); }
    }
    if (used.empty()) MLOG("ghost: no look could be applied (the ghost may be invisible)");
    MLOG("ghost: spawned for player %08x as '%s'", peer, used.c_str());
    return ch;
}

// Forget every ghost's applied look: the tick applies the right one again.
void redoLooks() {
    std::lock_guard<std::mutex> lock(g_m);
    for (auto& g : g_ghosts) { g.lookKey.clear(); g.lookTriedAt = 0; }
}

void handleRequests() {
    if (g_reqHide.exchange(false)) {
        g_paused = true;
        std::lock_guard<std::mutex> lock(g_m);
        for (auto& g : g_ghosts) startHiding(g);
        MLOG("ghosts: hidden, automatic spawning paused (F7 resumes)");
    }
    if (g_reqSpawn.exchange(false)) {
        if (g_paused.exchange(false)) MLOG("ghosts: automatic spawning resumed");
        {
            std::lock_guard<std::mutex> lock(g_m);
            for (auto& g : g_ghosts) g.hidden = false, g.visibleCalls = 0; // show again (tick re-asserts)
            g_lastSpawnAt = 0;
        }
        redoLooks(); // and refresh their look
    }
}
} // namespace

bool onClientReplayComp(void* comp) {
    if (!comp || !claimWindowOpen()) return false;
    // Only a component whose owner is a ghost character (B-3). The owner is normally known when the
    // factory returns; if it is not, fall back to the claim window alone, as before.
    const uintptr_t owner = componentOwner(reinterpret_cast<uintptr_t>(comp));
    if (owner && !clientCharIsGhost(reinterpret_cast<void*>(owner))) return false;
    const uint32_t peer = g_pendingPeer.load();
    std::lock_guard<std::mutex> lock(g_m);
    for (auto& g : g_ghosts)
        if (g.peer == peer && !g.clientComp) {
            if (owner && g.clientChar && reinterpret_cast<uintptr_t>(g.clientChar) != owner) continue;
            g.clientComp = comp;
            if (!owner) VLOG("ghost: claimed replay component %p without a known owner", comp);
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
                setGhostShader(g, g.ghostShader);
                break;
            }
    }
    setClientVisible(ent, true);
}

void setCharacterDataType(uintptr_t runtimeTypeInfo) { g_tiCharData = runtimeTypeInfo; }

void beforeClientReplayUpdate() {
    ++g_updaterCalls;
    renderFrame();
}

void renderFrame() {
    struct Target { uint32_t peer; void* comp; void* ch; };
    Target targets[kMaxGhosts];
    size_t n = 0;
    {
        std::lock_guard<std::mutex> lock(g_m);
        for (const auto& g : g_ghosts)
            if (n < kMaxGhosts && g.posing && !g.hidden && g.clientComp && !g_paused &&
                GetTickCount() - g.posingSince > kReplicateMs)
                targets[n++] = {g.peer, g.clientComp, g.clientChar};
    }
    thread_local net::PlayerState st; // large; render thread or client update thread
    for (size_t i = 0; i < n; ++i) {
        uint32_t age = 0;
        if (!net::remoteState(targets[i].peer, st, age) || age >= kHideAfterMs) continue;
        if (!clientCompAlive(targets[i].comp, targets[i].ch)) continue;
        const uintptr_t c = reinterpret_cast<uintptr_t>(targets[i].comp);
        const uintptr_t pose = mem::readOr<uintptr_t>(c + CRP_ReceivedPose);
        const uint32_t bones = mem::readOr<uint32_t>(c + CRP_ExpectedBones);
        uint32_t idx[kMaxBones];
        const uintptr_t idxPtr = mem::readOr<uintptr_t>(c + CRP_Setup + 0x10);
        if (!pose || bones != st.boneCount || !bones || bones > kMaxBones || !idxPtr || !mem::read(idxPtr, idx, bones * 4))
            continue;
        float buf[kMaxBones][8];
        memcpy(buf, st.bones, bones * 0x20);
        for (uint32_t b = 0; b < bones; ++b)
            if ((idx[b] & 0xff) == 1) placeAnchor(&buf[0][0], static_cast<int>(b), st.pos); // the body's anchor
        poke(pose, buf, bones * 0x20);
    }
}
void setEnabled(bool enabled) { g_enabled = enabled; }

namespace {
// A remote player that is present now (packets newer than kHideAfterMs).
struct Present { uint32_t id; std::string name; uint32_t ageMs; uint32_t look; };

bool isPresent(const std::vector<Present>& present, uint32_t id) {
    for (const auto& p : present)
        if (p.id == id) return true;
    return false;
}

// Hands a free ghost (its player is gone) to `p`. Prefers the ghost this player had before, by name,
// so a reconnecting player (new session id) gets their old ghost back. Caller holds g_m.
bool reuseFreeGhost(const Present& p, const std::vector<Present>& present) {
    Ghost* pick = nullptr;
    for (auto& g : g_ghosts) {
        if (!g.character || isPresent(present, g.peer)) continue;
        if (!pick || (!p.name.empty() && g.name == p.name && pick->name != p.name)) pick = &g;
    }
    if (!pick) return false;
    MLOG("ghost: player %08x takes over the ghost of %08x%s", p.id, pick->peer,
         !p.name.empty() && pick->name == p.name ? " (same name: reconnected)" : "");
    pick->peer = p.id;
    pick->name = p.name;
    pick->posing = false;
    pick->posingSince = 0;
    pick->lookKey.clear(); // the new player's own look
    pick->lookTriedAt = 0;
    pick->visibleCalls = 0;
    pick->hidden = true;               // the tick shows it again on its next pass
    pick->clientCharAt = GetTickCount();
    return true;
}

void recordCost(double us, size_t ghosts) {
    g_costSum += us;
    g_costCount += 1;
    if (us > g_costMax) g_costMax = us;
    const DWORD now = GetTickCount();
    if (now - g_costWindowAt >= 10000) {
        g_costShownAvg = g_costCount ? g_costSum / g_costCount : 0;
        g_costShownMax = g_costMax;
        g_costShownGhosts = static_cast<uint32_t>(ghosts);
        g_costSum = g_costMax = 0;
        g_costCount = 0;
        g_costWindowAt = now;
    }
}

double nowUs() {
    static const LARGE_INTEGER f = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return x; }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) * 1e6 / static_cast<double>(f.QuadPart);
}
} // namespace

void tick(uint64_t tick, uint64_t ticksInLevel) {
    (void)tick;
    const double t0 = nowUs();
    handleRequests();
    std::vector<Present> players; // present now
    for (const auto& r : net::remotes(kHideAfterMs))
        if (r.ageMs < kHideAfterMs) players.push_back({r.id, r.name, r.ageMs, r.look});

    // Forget ghosts the game destroyed. The city-ghost spawner sometimes takes a ghost back (to put
    // its own city ghost there); the ghost entity lives on and keeps working, so keep driving it.
    {
        std::lock_guard<std::mutex> lock(g_m);
        const uintptr_t data = g_ghostCharData.load();
        for (size_t i = 0; i < g_ghosts.size();) {
            if (!g_ghosts[i].character || level::characterExists(g_ghosts[i].character, data)) { ++i; continue; }
            VLOG("ghost: ghost of player %08x is gone", g_ghosts[i].peer);
            g_ghosts.erase(g_ghosts.begin() + static_cast<long>(i));
        }
    }

    // Every present player needs a ghost: hand them a free one, else spawn (one per kSpawnGapMs).
    const bool canSpawn = g_enabled && !g_paused && config::get()->autoSpawn && ticksInLevel >= kSettleTicks &&
                          level::spawnerAlive(level::ghostSpawner()) && level::playerCharacter();
    if (canSpawn) {
        level::BoneSetup ref;
        for (const auto& p : players) {
            if (p.ageMs > kFreshMs) continue;
            size_t count = 0;
            {
                std::lock_guard<std::mutex> lock(g_m);
                bool has = false;
                for (auto& g : g_ghosts) has = has || g.peer == p.id;
                if (has || reuseFreeGhost(p, players)) continue;
                count = g_ghosts.size();
            }
            if (count >= kMaxGhosts) {
                static DWORD lastWarn = 0;
                if (GetTickCount() - lastWarn > 30000) {
                    lastWarn = GetTickCount();
                    MLOG("ghost: %llu ghosts already; player %08x gets none", static_cast<unsigned long long>(count), p.id);
                }
                continue;
            }
            if (GetTickCount() - g_lastSpawnAt < kSpawnGapMs || !level::referenceSetup(ref)) continue;
            net::PlayerState st;
            uint32_t age = 0;
            if (!net::remoteState(p.id, st, age)) continue;
            g_lastSpawnAt = GetTickCount();
            spawnGhost(p.id, p.name, p.look, st);
        }
    }

    // Drive the ghosts of present players; hide the others once (a hidden ghost costs nothing).
    std::vector<Ghost> ghosts;
    {
        std::lock_guard<std::mutex> lock(g_m);
        ghosts = g_ghosts;
    }
    static net::PlayerState st; // large; game thread only
    for (size_t i = 0; i < ghosts.size(); ++i) {
        Ghost& g = ghosts[i];
        if (!g.character) continue;
        const bool present = !g_paused && isPresent(players, g.peer);
        if (!present && g.hidden && !parking(g)) continue;
        uint32_t age = 0;
        const bool have = present && net::remoteState(g.peer, st, age);
        // Visibility: the client character starts hidden (the time-trial marker normally shows it);
        // show it a few times after creation and every 3 s after that, hide it when the player left.
        bool visibilityChanged = false;
        if (!present) {
            if (!g.hidden) {
                startHiding(g);
                visibilityChanged = true;
            }
            static net::PlayerState park; // large; game thread only
            park = parkedState(g);
            drive(g, park);
            fixClient(g, park);
        } else if (g.clientChar) {
            const DWORD since = GetTickCount() - g.clientCharAt;
            {
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
        // The look: the player's own choice or our override; applied again when it changes. At most one
        // apply per tick (it rebuilds meshes), and a failed one is retried after 2 s.
        bool lookChanged = false;
        static DWORD lastLookApply = 0;
        if (present) {
            uint32_t look = 0;
            for (const auto& p : players)
                if (p.id == g.peer) look = p.look;
            const std::string want = desiredLook(look);
            const DWORD nowTick = GetTickCount();
            if (want != g.lookKey && nowTick - g.lookTriedAt > lookRetryMs(want) && nowTick != lastLookApply) {
                lastLookApply = nowTick;
                g.lookTriedAt = nowTick;
                std::string used;
                if (applyLook(g.character, want, &used)) {
                    g.lookKey = want;
                    MLOG("ghost: player %08x now looks like '%s'", g.peer, used.c_str());
                }
                lookChanged = true;
            }
        }
        if (have) {
            {
                std::lock_guard<std::mutex> lock(g_motionM);
                g_motion[g.peer].add(st);
            }
            drive(g, st);
            fixClient(g, st);
            memcpy(g.lastPos, st.pos, sizeof(g.lastPos));
            g.parkUntil = 0;
        }
        // Write back what this tick changed. The list only changes on this thread (spawn/erase/reuse)
        // or by the client hooks filling in client pointers, so the entry is normally still at i.
        std::lock_guard<std::mutex> lock(g_m);
        Ghost* live = i < g_ghosts.size() && g_ghosts[i].character == g.character ? &g_ghosts[i] : nullptr;
        for (size_t k = 0; !live && k < g_ghosts.size(); ++k)
            if (g_ghosts[k].character == g.character) live = &g_ghosts[k];
        if (live && live->peer == g.peer) {
            live->posing = g.posing;
            live->posingSince = g.posingSince;
            if (lookChanged) { live->lookKey = g.lookKey; live->lookTriedAt = g.lookTriedAt; }
            memcpy(live->lastPos, g.lastPos, sizeof(live->lastPos));
            if (visibilityChanged) { live->hidden = g.hidden; live->visibleCalls = g.visibleCalls; live->parkUntil = g.parkUntil; }
            else if (have) live->parkUntil = 0;
        }
    }
    recordCost(nowUs() - t0, ghosts.size());
}

void levelChanged() {
    std::lock_guard<std::mutex> lock(g_m);
    g_ghosts.clear();
    g_pendingAt = 0;
    g_lastSpawnAt = 0;
    g_ghostCharData = 0; // level data: the next level's ghost blueprint may use another object
}

void requestSpawnOrRefresh() { g_reqSpawn = true; }
void requestHideAll() { g_reqHide = true; }


std::vector<uint32_t> shownPeers() {
    std::vector<uint32_t> v;
    std::lock_guard<std::mutex> lock(g_m);
    for (const auto& g : g_ghosts)
        if (g.character && g.clientChar && !g.hidden && g.posing && !g_paused) v.push_back(g.peer);
    return v;
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
    {
        std::lock_guard<std::mutex> mlock(g_motionM);
        for (auto it = g_motion.begin(); it != g_motion.end();) {
            bool alive = false;
            for (auto& g : g_ghosts) alive = alive || g.peer == it->first;
            if (!alive) { it = g_motion.erase(it); continue; }
            char id[16];
            snprintf(id, sizeof(id), "%08x", it->first);
            s += std::string(" | ghost of ") + id + " " + it->second.report();
            ++it;
        }
    }
    {
        // status() can be called from any thread (F9, the menu): the sample state is guarded.
        static std::mutex rateM;
        static DWORD lastAt = 0;
        static uint32_t lastCalls = 0;
        std::lock_guard<std::mutex> rate(rateM);
        const DWORD now = GetTickCount();
        const uint32_t calls = g_updaterCalls.load();
        if (lastAt && now > lastAt) {
            char u[96];
            snprintf(u, sizeof(u), ", poses written by the client update %.0f/s", (calls - lastCalls) * 1000.0 / (now - lastAt));
            s += u;
        }
        lastAt = now;
        lastCalls = calls;
    }
    char cost[96];
    snprintf(cost, sizeof(cost), ", ghost tick %.0f us avg / %.0f us max (%u ghosts)", g_costShownAvg.load(),
             g_costShownMax.load(), g_costShownGhosts.load());
    s += cost;
    return s;
}

} // namespace mecmp::game::ghosts
