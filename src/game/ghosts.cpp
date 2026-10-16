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
#include "net/pose_limits.h"

namespace mecmp::game::ghosts {
namespace {

constexpr uint32_t kFreshMs = 1500;     // a player counts as present while packets are this recent
constexpr uint32_t kHideAfterMs = 5000; // hide a ghost whose player stopped sending
// One spawn at a time: the client objects the game makes for a spawn are matched to it by time (the claim
// window below). The next spawn goes as soon as the previous ghost has both of its client objects, at least
// kMinSpawnGapMs after it; kSpawnGapMs is the most it waits (client objects that never came), and the wait
// after a failed spawn. With a fixed 2 s gap, a level load or a checkpoint reload took 2 s per player.
constexpr DWORD kSpawnGapMs = 2000;
constexpr DWORD kMinSpawnGapMs = 200;
constexpr DWORD kClaimWindowMs = 1500;  // client objects created this soon after our spawn are ours
constexpr uint64_t kSettleTicks = 90;   // ~3 s after a level starts before spawning
constexpr uint64_t kPlayerSettleTicks = 45; // ~1.5 s after the player's character appears (see tick)
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
    uintptr_t charData = 0;     // its PamCharacterEntityData when spawned: how it is recognised later
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
    std::string lookWanted;       // the look last asked for; a new choice is tried at once
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
std::atomic<uintptr_t> g_ghostCharData{0}; // the ghost blueprint's PamCharacterEntityData (of the current spawner)
// Every ghost character data seen this level. Two spawners can hold different data objects: our ghosts from
// the one before keep theirs, and their client characters must still count as ghosts.
std::atomic<uintptr_t> g_knownCharData[4] = {};
bool knownCharData(uintptr_t d) {
    if (!d) return false;
    if (d == g_ghostCharData.load()) return true;
    for (const auto& k : g_knownCharData)
        if (k.load() == d) return true;
    return false;
}
void rememberCharData(uintptr_t d) {
    if (!d || knownCharData(d)) return;
    for (auto& k : g_knownCharData) {
        uintptr_t empty = 0;
        if (k.compare_exchange_strong(empty, d)) return;
    }
    for (size_t i = 0; i + 1 < 4; ++i) g_knownCharData[i] = g_knownCharData[i + 1].load(); // full: drop the oldest
    g_knownCharData[3] = d;
}
std::atomic<uintptr_t> g_tiCharData{0};    // runtime TypeInfo of PamCharacterEntityData (set at startup)
// Tick cost (P-2/P-4), shown by F9: sum and max over the current 10 s window, in microseconds.
double g_costSum = 0, g_costMax = 0;    // game thread
uint32_t g_costCount = 0;
std::atomic<double> g_costShownAvg{0}, g_costShownMax{0}; // read by status() on the mod thread
std::atomic<uint32_t> g_costShownGhosts{0};
DWORD g_costWindowAt = 0;
DWORD g_lastSpawnAt = 0;
bool g_lastSpawnFailed = false; // the last spawn gave no character: wait the full gap before the next
// The ghost of the last spawn has both client objects: its claim window can close, and the next spawn go.
bool pendingClaimedUnlocked() {
    const uint32_t peer = g_pendingPeer.load();
    for (const auto& g : g_ghosts)
        if (g.peer == peer && g.character) return g.clientChar && g.clientComp;
    return true; // not there (the spawn failed, or the ghost is gone): nothing to wait for
}
void closeClaimIfDone(const Ghost& g) { // under g_m
    if (g.peer != g_pendingPeer.load() || !g.clientChar || !g.clientComp || !g_pendingAt.load()) return;
    VLOG("ghost: player %08x has its client objects %lu ms after the spawn", g.peer,
         static_cast<unsigned long>(GetTickCount() - g_pendingAt.load()));
    g_pendingAt = 0; // nothing the game creates from now on is taken for this ghost's
}
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
    return e && mem::read(e, vt) && vt == rva(Vtbl_PamClientCharacterEntity) && mem::read(e + Character_Data, data) &&
           (!g_ghostCharData.load() || knownCharData(data));
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

// The fields of a ghost's client replay component that are used every tick and every frame, taken
// with one read (each fault-tolerant read is a kernel call, so one large read beats several small ones).
struct ClientCompView {
    uint8_t raw[CRP_SkipRoot + 1];
    template <class T> T at(size_t off) const {
        T v;
        memcpy(&v, raw + off, sizeof(T));
        return v;
    }
    uintptr_t pose() const { return at<uintptr_t>(CRP_ReceivedPose); }
    uint32_t bones() const { return at<uint32_t>(CRP_ExpectedBones); }
    uintptr_t boneIndices() const { return at<uintptr_t>(CRP_Setup + 0x10); }
};

// Is the component still a client replay component of the same owner as when we claimed it (the memory
// was not reused)? Reads its fields in the same go.
bool readClientComp(void* comp, void* owner, ClientCompView& v) {
    const uintptr_t c = reinterpret_cast<uintptr_t>(comp);
    if (!c || !mem::read(c, v.raw, sizeof(v.raw)) || v.at<uintptr_t>(0) != rva(Vtbl_PamClientReplayPlayerComponent))
        return false;
    if (!owner) return true;
    const uintptr_t coll = v.at<uintptr_t>(Component_Collection);
    return coll && mem::readOr<uintptr_t>(coll) == reinterpret_cast<uintptr_t>(owner);
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
void drive(Ghost& g, const net::PlayerState& input) {
    net::PlayerState st = input;
    if (!net::limitPose(st)) return;
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
    thread_local ClientCompView v;
    if (!readClientComp(g.clientComp, g.clientChar, v)) return;
    const uintptr_t c = reinterpret_cast<uintptr_t>(g.clientComp);
    if (v.at<uint32_t>(CRP_PlaybackState) != 0) poke<uint32_t>(c + CRP_PlaybackState, 0);
    if (v.at<uint8_t>(CRP_PlaybackForced) != 0) poke<uint8_t>(c + CRP_PlaybackForced, 0);
    if (v.at<uint8_t>(CRP_SkipRoot) != 0) poke<uint8_t>(c + CRP_SkipRoot, 0);
    // Driven every rendered frame (renderFrame): the anchor is set there, for the frame's own moment.
    if (g.posing && GetTickCount() - g.posingSince > kReplicateMs) return;
    const uintptr_t pose = v.pose();
    const uint32_t n = v.bones();
    uint32_t idx[kMaxBones];
    const uintptr_t idxPtr = v.boneIndices();
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
    // One read covers the vtable, the data and the flag.
    thread_local uint8_t head[ClientChar_GhostShader + 1];
    if (!client || !mem::read(client, head, sizeof(head))) return;
    uintptr_t vt = 0, data = 0;
    memcpy(&vt, head, sizeof(vt));
    memcpy(&data, head + Character_Data, sizeof(data));
    const uint8_t current = head[ClientChar_GhostShader];
    if (vt != rva(Vtbl_PamClientCharacterEntity) || !knownCharData(data) || current == static_cast<uint8_t>(enabled))
        return;
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
            setGhostShader(ghost, key.rfind("echo:", 0) == 0 || applied.rfind("faith", 0) != 0);
            break;
        }
    }
    if (used) *used = key.rfind("echo:", 0) == 0 ? key : applied;
    return true;
}

// The look a player's ghost should have: the one they chose (sent in HELLO).
std::string desiredLook(uint32_t playerLook, uint8_t outfit) {
    if (playerLook || !outfit) return config::lookName(playerLook);
    return "faith:" + std::to_string(outfit); // the outfit changes (net::protocol::kWear... bits)
}

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

// How long to wait before trying a look that failed again: an echo the catalogue does not have is
// retried rarely (each attempt rebuilds the ghost's mesh), other looks sooner. A newly chosen look and a
// look list that just arrived (retryLooksNow) are tried at once.
DWORD lookRetryMs(const std::string& key) { return key.rfind("echo:", 0) == 0 ? 15000 : 2000; }

// The game asks this spawner for its *last* spawned character (ServerSpawnEntity_getLastSpawnedCharacter
// 0x143381080), e.g. for the pause menu's "Your Echo" preview, which shows the character the city-ghost
// spawner made at level load. Our ghosts come from the same spawner, so after a spawn ours was last and
// the preview showed our ghost (moving like the player, in their look; invisible as Faith). The list
// (+0x148/+0x150) is reordered so the game's own characters stay at the end, in their order; ours go
// first. Only pointers inside the list move.
void keepGameCharacterLast(void* spawner) {
    uintptr_t list[2] = {};
    const uintptr_t sp = reinterpret_cast<uintptr_t>(spawner);
    if (!sp || !mem::read(sp + Spawner_Spawned, list, sizeof(list)) || list[1] <= list[0] || (list[1] - list[0]) / 8 > 64)
        return;
    const size_t n = (list[1] - list[0]) / 8;
    std::vector<uintptr_t> items(n);
    if (!mem::read(list[0], items.data(), n * 8)) return;
    std::vector<uintptr_t> ours, theirs;
    {
        std::lock_guard<std::mutex> lock(g_m);
        for (uintptr_t c : items) {
            bool mine = false;
            for (const auto& g : g_ghosts) mine = mine || reinterpret_cast<uintptr_t>(g.character) == c;
            (mine ? ours : theirs).push_back(c);
        }
    }
    if (ours.empty() || theirs.empty()) return;
    std::vector<uintptr_t> order = ours;
    order.insert(order.end(), theirs.begin(), theirs.end());
    if (order != items) {
        poke(list[0], order.data(), n * 8);
        VLOG("ghost: spawner %p list reordered: the game's own character is last again", spawner);
    }
}

void* spawnGhost(uint32_t peer, const std::string& name, uint32_t look, uint8_t outfit, const net::PlayerState& st) {
    void* spawner = level::ghostSpawner();
    if (!level::spawnerAlive(spawner)) {
        MLOG("ghost: no ghost spawner to spawn %08x from (not in a level, or not found yet)", peer);
        return nullptr;
    }
    // The data the ghosts are recognised by; put back if this spawner gives no character (a failed spawn from
    // another spawner made every ghost look destroyed: they froze where they were).
    const uintptr_t dataBefore = g_ghostCharData.load();
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
    level::spawnResult(spawner, ch != nullptr);
    if (!ch) {
        g_ghostCharData = dataBefore;
        MLOG("ghost: the spawner returned no character for %08x", peer);
        return nullptr;
    }
    VLOG("ghost: spawned %p for %08x (look %08x, outfit %u)", ch, peer, look, outfit);
    if (const uintptr_t d = mem::readOr<uintptr_t>(reinterpret_cast<uintptr_t>(ch) + Character_Data)) {
        if (g_ghostCharData && g_ghostCharData != d)
            MLOG("ghost: ghost character data is %p, not %p as expected from the blueprint", reinterpret_cast<void*>(d),
                 reinterpret_cast<void*>(g_ghostCharData.load()));
        g_ghostCharData = d;
        rememberCharData(d);
        std::lock_guard<std::mutex> lock(g_m);
        for (auto& g : g_ghosts)
            if (g.character == ch) g.charData = d;
    }
    setTransform(ch, info.transform);
    keepGameCharacterLast(spawner);
    std::string used;
    const std::string key = desiredLook(look, outfit);
    const bool done = applyLook(ch, key, &used);
    // Visible at least; the wanted look is retried later. An outfit whose pieces are not known yet is
    // plain Faith meanwhile.
    if (!done && !(key.rfind("faith:", 0) == 0 && applyGhostModel(ch, "faith", &used))) applyGhostModel(ch, "RunVis", &used);
    {
        std::lock_guard<std::mutex> lock(g_m);
        for (auto& g : g_ghosts)
            if (g.character == ch) {
                g.lookKey = done ? key : std::string();
                g.lookWanted = key;
                g.lookTriedAt = GetTickCount();
            }
    }
    if (used.empty()) MLOG("ghost: no look could be applied (the ghost may be invisible)");
    uintptr_t list[2] = {};
    mem::read(reinterpret_cast<uintptr_t>(spawner) + Spawner_Spawned, list, sizeof(list));
    MLOG("ghost: spawned for player %08x as '%s' (character %p, spawner %p holds %llu, count %d)", peer, used.c_str(), ch,
         spawner, static_cast<unsigned long long>(list[1] >= list[0] ? (list[1] - list[0]) / 8 : 0),
         mem::readOr<int32_t>(reinterpret_cast<uintptr_t>(spawner) + 0xf4));
    return ch;
}

// Whether `ch` is still in its spawner's list of spawned characters (+0x148/+0x150).
const char* inSpawnerList(void* spawner, void* ch) {
    uintptr_t list[2] = {};
    if (!spawner || !mem::read(reinterpret_cast<uintptr_t>(spawner) + Spawner_Spawned, list, sizeof(list)) ||
        list[1] < list[0] || (list[1] - list[0]) / 8 > 256)
        return "spawner list unreadable";
    for (uintptr_t it = list[0]; it < list[1]; it += 8)
        if (mem::readOr<uintptr_t>(it) == reinterpret_cast<uintptr_t>(ch)) return "still in its spawner's list";
    return "not in its spawner's list";
}

// Diagnostic: why a ghost counts as gone (it is then spawned again).
void logLostGhost(const Ghost& g, uintptr_t expectedData) {
    const uintptr_t ch = reinterpret_cast<uintptr_t>(g.character);
    uintptr_t vt = 0, data = 0;
    const bool readable = mem::read(ch, vt);
    if (readable) mem::read(ch + Character_Data, data);
    const char* what = !readable                                     ? "memory unreadable"
                       : vt == rva(Vtbl_PamServerCharacterEntity) ? "character data changed"
                       : inImage(vt)                              ? "now another object"
                                                                  : "destroyed";
    MLOG("ghost: ghost of player %08x is gone %.1f s after spawning (%s: vtable %p, data %p, expected %p; %s)", g.peer,
         (GetTickCount() - g.spawnedAt) / 1000.0, what, reinterpret_cast<void*>(vt), reinterpret_cast<void*>(data),
         reinterpret_cast<void*>(expectedData), inSpawnerList(g.spawner, g.character));
}

// Forget every ghost's applied look: the tick applies the right one again.
void redoLooks() {
    std::lock_guard<std::mutex> lock(g_m);
    for (auto& g : g_ghosts) { g.lookKey.clear(); g.lookTriedAt = 0; }
}

// The look list just arrived: looks waiting for it are tried on the next ticks, not after the retry wait.
void retryLooksNow() {
    std::lock_guard<std::mutex> lock(g_m);
    for (auto& g : g_ghosts) g.lookWanted.clear();
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
            if (!owner) MLOG("ghost: claimed replay component %p without a known owner", comp);
            closeClaimIfDone(g);
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
                closeClaimIfDone(g);
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
    // Both of the game's client replay updaters run every frame and each calls this first. Writing before
    // both keeps the ghost smooth (skipping the second was visibly less smooth), and a write costs ~15 us.
    prof::Scope scope(prof::RenderPose);
    if (!level::serverWorld() || level::inFrontEnd()) return; // no ghosts outside a playable level
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
        {
            prof::Scope s(prof::RenderState);
            if (!net::remoteState(targets[i].peer, st, age) || age >= kHideAfterMs || !net::limitPose(st)) continue;
        }
        thread_local ClientCompView v;
        uint32_t idx[kMaxBones];
        uintptr_t pose = 0;
        uint32_t bones = 0;
        {
            prof::Scope s(prof::RenderRead);
            if (!readClientComp(targets[i].comp, targets[i].ch, v)) continue;
            pose = v.pose();
            bones = v.bones();
            const uintptr_t idxPtr = v.boneIndices();
            // Logged once per change: a ghost that stops being posed stays where it was (seen as "gone").
            thread_local uint32_t skipPeer = 0, skipCount = ~0u;
            const bool skip = !pose || bones != st.boneCount || !bones || bones > kMaxBones;
            if (skip && (skipPeer != targets[i].peer || skipCount != st.boneCount)) {
                static DWORD lastNote = 0; // a mismatch repeats every frame: one note per 10 s
                if (GetTickCount() - lastNote > 10000) {
                    lastNote = GetTickCount();
                    MLOG("ghost: player %08x is not posed (its ghost has %u bones, its packets %u)", targets[i].peer, bones,
                         st.boneCount);
                }
                skipPeer = targets[i].peer;
                skipCount = st.boneCount;
            } else if (!skip && skipPeer == targets[i].peer) {
                VLOG("ghost: player %08x posed again", targets[i].peer);
                skipPeer = 0;
                skipCount = ~0u;
            }
            if (skip || !idxPtr || !mem::read(idxPtr, idx, bones * 4)) continue;
        }
        float buf[kMaxBones][8];
        memcpy(buf, st.bones, bones * 0x20);
        for (uint32_t b = 0; b < bones; ++b)
            if ((idx[b] & 0xff) == 1) placeAnchor(&buf[0][0], static_cast<int>(b), st.pos); // the body's anchor
        prof::Scope s(prof::RenderWrite);
        poke(pose, buf, bones * 0x20);
    }
}
void setEnabled(bool enabled) { g_enabled = enabled; }

namespace {
// A remote player that is present now (packets newer than kHideAfterMs).
struct Present { uint32_t id; std::string name; uint32_t ageMs; uint32_t look; uint8_t outfit; };

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
    pick->lookWanted.clear();
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
    prof::Scope scope(prof::Ghosts);
    (void)tick;
    const double t0 = nowUs();
    handleRequests();
    std::vector<Present> players; // present now
    for (const auto& r : net::remotes(kHideAfterMs))
        if (r.ageMs < kHideAfterMs) players.push_back({r.id, r.name, r.ageMs, r.look, r.outfit});

    // Forget ghosts the game destroyed. The city-ghost spawner sometimes takes a ghost back (to put
    // its own city ghost there); the ghost entity lives on and keeps working, so keep driving it.
    {
        std::lock_guard<std::mutex> lock(g_m);
        const uintptr_t data = g_ghostCharData.load();
        for (size_t i = 0; i < g_ghosts.size();) {
            // Each ghost by its own data: a later spawn from another spawner may have changed the current one.
            const uintptr_t own = g_ghosts[i].charData ? g_ghosts[i].charData : data;
            if (!g_ghosts[i].character || level::characterExists(g_ghosts[i].character, own)) { ++i; continue; }
            logLostGhost(g_ghosts[i], own);
            g_ghosts.erase(g_ghosts.begin() + static_cast<long>(i));
        }
    }

    // The game deletes a ghost spawned in the first moments after the player's own character appears
    // (seen 0.6 s after it, every level load; the next spawn stays). Wait a little after that (1.5 s; a ghost the
    // game deletes anyway is found gone and spawned again).
    static void* lastPlayer = nullptr;
    static uint64_t playerSince = 0;
    void* const playerChar = level::playerCharacter();
    if (playerChar != lastPlayer) {
        VLOG("level: player character %p -> %p (%llu ticks into the level)", lastPlayer, playerChar,
             static_cast<unsigned long long>(ticksInLevel));
        lastPlayer = playerChar;
        playerSince = ticksInLevel;
    }
    const bool playerSettled = playerChar && ticksInLevel - playerSince >= kPlayerSettleTicks;
    if (playerSettled) { // the right spawner before any spawn (the hooks and scans may have changed it)
        const void* ours[kMaxGhosts];
        size_t k = 0;
        {
            std::lock_guard<std::mutex> lock(g_m);
            for (const auto& g : g_ghosts)
                if (g.character && k < kMaxGhosts) ours[k++] = g.character;
        }
        level::chooseGhostSpawner(ours, k);
    }

    // Every present player needs a ghost: hand them a free one, else spawn (one per kSpawnGapMs).
    // Never in the main menu: its "Your Echo" preview is spawned from a projection spawner there, and a
    // ghost of ours from it took the preview's place (and its look).
    const bool canSpawn = g_enabled && !g_paused && config::get()->autoSpawn && ticksInLevel >= kSettleTicks &&
                          !level::inFrontEnd() &&
                          level::spawnerAlive(level::ghostSpawner()) && playerSettled;
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
            {
                const DWORD since = GetTickCount() - g_lastSpawnAt;
                bool claimed = true;
                {
                    std::lock_guard<std::mutex> lock(g_m);
                    claimed = pendingClaimedUnlocked();
                }
                if (since < kMinSpawnGapMs || (since < kSpawnGapMs && (g_lastSpawnFailed || !claimed))) continue;
            }
            // Stamped before the lookup as well: with no reference yet, the lookup (up to 64 memory reads) is
            // not repeated at every tick.
            g_lastSpawnAt = GetTickCount();
            g_lastSpawnFailed = true; // until a spawn gives a character
            if (!level::referenceSetup(ref)) continue;
            net::PlayerState st;
            uint32_t age = 0;
            if (!net::remoteState(p.id, st, age)) continue;
            g_lastSpawnFailed = spawnGhost(p.id, p.name, p.look, p.outfit, st) == nullptr;
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
            uint8_t outfit = 0;
            for (const auto& p : players)
                if (p.id == g.peer) { look = p.look; outfit = p.outfit; }
            const std::string want = desiredLook(look, outfit);
            const DWORD nowTick = GetTickCount();
            const bool due = want != g.lookWanted || nowTick - g.lookTriedAt > lookRetryMs(want);
            if (want != g.lookKey && due && nowTick != lastLookApply) {
                lastLookApply = nowTick;
                g.lookTriedAt = nowTick;
                g.lookWanted = want;
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
            // The client character copies the definition's ghost-shader flag while it is being set up,
            // which can be after we claimed it (and after the look set it): keep it on what the look
            // needs. Otherwise a Faith ghost keeps the ghost shader and only its hair shows.
            if (g.clientChar) setGhostShader(g, g.ghostShader);
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
            if (lookChanged) {
                live->lookKey = g.lookKey;
                live->lookTriedAt = g.lookTriedAt;
                live->lookWanted = g.lookWanted;
            }
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
    g_lastSpawnFailed = false;
    g_ghostCharData = 0; // level data: the next level's ghost blueprint may use another object
    for (auto& k : g_knownCharData) k = 0;
}

void requestSpawnOrRefresh() { g_reqSpawn = true; }
void lookListArrived() { retryLooksNow(); }
void requestHideAll() { g_reqHide = true; }


uintptr_t clientCharacter(uint32_t peer) {
    std::lock_guard<std::mutex> lock(g_m);
    for (const auto& g : g_ghosts)
        if (g.peer == peer && !g.hidden && g.posing && !g_paused && clientCharIsGhost(g.clientChar))
            return reinterpret_cast<uintptr_t>(g.clientChar);
    return 0;
}
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
