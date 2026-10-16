#include "game/runs.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "core/log.h"
#include "game/engine.h"
#include "net/net.h"
#include "net/protocol.h"

namespace mecmp::game::runs {
namespace proto = net::protocol;
namespace {

// The game's message managers (MessageManager_registerListener(manager, category, listener, 0xff)).
constexpr uintptr_t kClientGameContext = 0x142401cb0; // *ptr + 0x10 = client message manager
constexpr uintptr_t kServerGameContext = 0x14253d670; // *ptr + 0x10 = server message manager
constexpr uintptr_t Fn_RegisterListener = 0x142a3ad40;
constexpr uintptr_t Fn_Localize = 0x1436a8d40; // (char* out, u32 capacity, const char* id, void* args) -> ok
constexpr uintptr_t Vtbl_PamServerNamedChallengeEntity = 0x141c7ec70;

constexpr uint32_t kCategorySocialPlay = 0x54776fc6; // PamSocialPlayNamedChallenge*Message
constexpr uint32_t kTypeFinished = 0xd71d027f;       // ...FinishedMessage
constexpr uint32_t kTypeStarted = 0xc164590c;        // ...StartedMessage: +0x48 ContentId
constexpr uint32_t kTypeCancelled = 0x9a9a27c6;      // ...CancelledMessage (left the run or failed it)
// A player-made time trial: PamSocialPlayStartChallengeMessage / ...RestartChallengeMessage, +0x48 the trial's
// content id (the marker entity compares the same 24 bytes, PamServerChallengeMarkerEntity_onMessage).
constexpr uint32_t kTypeStartChallenge = 0x1cca484e, kTypeRestartChallenge = 0x72f865f9;
// Message type ids are the djb2-xor hash of the message class name (checked against these).
constexpr uint32_t kTypeChallengeFinished = 0x9b35497c; // PamSocialPlayChallengeFinishedMessage: a player-made time trial
// Its fields (seen in game, three finishes 3-4 s after starting): +0x48 the time trial's GUID, +0x60 u32 the
// time in hundredths of a second (139, 159, 249).
constexpr size_t Tt_Guid = 0x48, Tt_Time = 0x60;
// The trial's marker (PamServerChallengeMarkerEntity, vtable 0x141c7e3e8; its finish is vf28 0x143a88af7):
// +0x80 PamSharedContentId (the same 24 bytes as the message's +0x48), +0xb8 the trial's name (char*,
// what the game publishes as "contentName" while you play it).
constexpr uintptr_t Vtbl_PamServerChallengeMarkerEntity = 0x141c7e3e8;
constexpr size_t Marker_ContentId = 0x80, Marker_Name = 0xb8;
constexpr size_t kContentIdSize = 0x18;
constexpr size_t Msg_Category = 0x08, Msg_Type = 0x0c;
constexpr size_t Fin_ContentId = 0x48, Fin_Delta = 0x50, Fin_HadRecord = 0x54, Fin_Value = 0x58, Fin_NewRecord = 0x60;

constexpr size_t Ent_ChallengeData = 0x80;
constexpr size_t Data_Sid = 0x10, Data_MainStatCode = 0x58, Data_ContentId = 0x98;

// Our listener: an object whose vtable the message manager calls. Slot 0 = onMessage(listener, message);
// slot 1 = the listener's priority (asked once when it is registered).
struct Listener {
    void* const* vtbl;
};
void onMessage(Listener* self, const void* message);
uint16_t priority(Listener*) { return 0; }
void* const g_vtbl[2] = {reinterpret_cast<void*>(&onMessage), reinterpret_cast<void*>(&priority)};
Listener g_clientListener{g_vtbl}, g_serverListener{g_vtbl};
std::atomic<uintptr_t> g_clientRegistered{0}, g_serverRegistered{0}; // the manager each was registered with

std::mutex g_m;
std::vector<uintptr_t> g_entities; // live PamServerNamedChallengeEntity (checked by vtable before use)
std::vector<uintptr_t> g_markers;  // live PamServerChallengeMarkerEntity (time trials; checked likewise)

// Keeps a list of entities of one class: pushes `e`, first dropping the dead ones when it is full.
void track(std::vector<uintptr_t>& list, uintptr_t e, uintptr_t vtblVa) { // caller holds g_m
    if (list.size() >= 512) {
        std::vector<uintptr_t> live;
        for (uintptr_t x : list)
            if (mem::readOr<uintptr_t>(x) == rva(vtblVa)) live.push_back(x);
        list.swap(live);
        if (list.size() >= 512) return;
    }
    list.push_back(e);
}

// The name of the time trial with this content id (empty: its marker is not loaded or has no name).
// What kind of player-made challenge a content id is, from its marker's data (PamChallengeMarkerEntityData:
// +0x68 CheckpointOrder, +0x6c Time). A time trial has checkpoints (in order or any order); a "ReachThis" /
// Beat L.E. (I was here: reach this place) has none and is not a run against the clock. -1: no marker found.
constexpr size_t Ent_Data = 0x28, MarkerData_CheckpointOrder = 0x68, MarkerData_Time = 0x6c;
int checkpointOrder(const uint8_t* contentId) {
    std::lock_guard<std::mutex> lock(g_m);
    for (uintptr_t e : g_markers) {
        uint8_t head[Marker_ContentId + kContentIdSize] = {};
        if (!mem::read(e, head, sizeof(head))) continue;
        uintptr_t vt = 0, data = 0;
        memcpy(&vt, head, sizeof(vt));
        memcpy(&data, head + Ent_Data, sizeof(data));
        if (vt != rva(Vtbl_PamServerChallengeMarkerEntity) || memcmp(head + Marker_ContentId, contentId, kContentIdSize) != 0)
            continue;
        int32_t order = -1;
        if (!data || !mem::read(data + MarkerData_CheckpointOrder, order)) return -1;
        VLOG("runs: challenge marker %p: checkpoint order %d, time %.1f", reinterpret_cast<void*>(e), order,
             mem::readOr<float>(data + MarkerData_Time));
        return order;
    }
    return -1;
}

std::string timeTrialName(const uint8_t* contentId) {
    std::lock_guard<std::mutex> lock(g_m);
    for (uintptr_t e : g_markers) {
        uint8_t head[Marker_Name + 8] = {};
        if (!mem::read(e, head, sizeof(head))) continue;
        uintptr_t vt = 0, name = 0;
        memcpy(&vt, head, sizeof(vt));
        if (vt != rva(Vtbl_PamServerChallengeMarkerEntity) || memcmp(head + Marker_ContentId, contentId, kContentIdSize) != 0)
            continue;
        memcpy(&name, head + Marker_Name, sizeof(name));
        std::string s;
        if (name && mem::readUtf8String(name, s, 96)) return s; // a player wrote it: any language
    }
    return {};
}
struct Recent { uint32_t content; int64_t value; uint32_t at; };
Recent g_recent{0, 0, 0}; // the last finish reported: the same one from the other manager is a repeat

bool registerOn(uintptr_t contextGlobal, Listener& l, std::atomic<uintptr_t>& done) {
    const uintptr_t ctx = mem::readOr<uintptr_t>(rva(contextGlobal));
    if (!ctx) return false;
    const uintptr_t manager = ctx + 0x10;
    if (done.load() == manager) return true;
    using RegisterFn = char (*)(uintptr_t manager, uint32_t category, Listener* listener, uint32_t filter);
    reinterpret_cast<RegisterFn>(rva(Fn_RegisterListener))(manager, kCategorySocialPlay, &l, 0xff);
    done = manager;
    VLOG("runs: listening for finished runs on the %s message manager",
         &l == &g_clientListener ? "client" : "server");
    return true;
}

std::string readString(uintptr_t addr, size_t max = 96) {
    std::string s;
    if (addr && mem::readCString(addr, s, max)) return s;
    return {};
}

// The run's localised name ("The View"), else its id with the prefix cut ("THEVIEW02").
std::string localise(const std::string& sid) {
    if (sid.empty()) return {};
    char buf[512] = {};
    void* noArgs = nullptr;
    using LocalizeFn = bool (*)(char* out, uint32_t capacity, const char* id, void* args);
    const bool ok = reinterpret_cast<LocalizeFn>(rva(Fn_Localize))(buf, sizeof(buf), sid.c_str(), &noArgs);
    buf[sizeof(buf) - 1] = 0;
    std::string text = buf;
    bool printable = !text.empty();
    for (unsigned char c : text) printable = printable && (c >= 0x20);
    if (ok && printable && text != sid) return text;
    std::string s = sid;
    for (const char* prefix : {"ID_NC_", "ID_BILLBOARD_", "ID_"})
        if (s.rfind(prefix, 0) == 0) { s = s.substr(strlen(prefix)); break; }
    return s;
}

// The run's kind, from its main stat's code ("ch_rrt_tv2_time" = a dash, "Runner's Route").
uint8_t kindOf(const std::string& statCode) {
    struct K { const char* prefix; uint8_t kind; };
    static const K kinds[] = {{"ch_rrt_", proto::kRunDash},      {"ch_dlvry_", proto::kRunDelivery},
                              {"ch_gnd_", proto::kRunGridNode},  {"ch_hbb_", proto::kRunBillboard},
                              {"ch_mgd_", proto::kRunMetaGrid},  {"ch_aslt_", proto::kRunAssault}};
    for (const auto& k : kinds)
        if (statCode.rfind(k.prefix, 0) == 0) return k.kind;
    return proto::kRunOther;
}

// The challenge data with this ContentId, from the level's named challenge entities.
uintptr_t challengeData(uint32_t contentId) {
    std::lock_guard<std::mutex> lock(g_m);
    for (size_t i = 0; i < g_entities.size();) {
        const uintptr_t e = g_entities[i];
        uintptr_t head[Ent_ChallengeData / 8 + 1] = {};
        if (!mem::read(e, head, sizeof(head)) || head[0] != rva(Vtbl_PamServerNamedChallengeEntity)) {
            g_entities.erase(g_entities.begin() + static_cast<long>(i)); // gone
            continue;
        }
        const uintptr_t data = head[Ent_ChallengeData / 8];
        if (data && mem::readOr<uint32_t>(data + Data_ContentId) == contentId) return data;
        ++i;
    }
    return 0;
}

// The run we are in now, for the others' name tags (net::setActivity). Ends with its finish, a cancel, the
// next run, a level change, or after kActivityMaxMs (a time trial left without finishing sends nothing we
// know of yet).
constexpr DWORD kActivityMaxMs = 30 * 60 * 1000;
uint8_t g_activeKind = proto::kActivityNone; // under g_m
DWORD g_activeAt = 0;

void beginActivity(uint8_t kind, const std::string& title) {
    {
        std::lock_guard<std::mutex> lock(g_m);
        g_activeKind = kind;
        g_activeAt = GetTickCount();
    }
    net::setActivity(kind, GetTickCount(), title);
    MLOG("runs: started the %s '%s'", proto::runKindName(kind), title.c_str());
}

void endActivity(const char* why) {
    {
        std::lock_guard<std::mutex> lock(g_m);
        if (g_activeKind == proto::kActivityNone) return;
        g_activeKind = proto::kActivityNone;
    }
    net::setActivity(proto::kActivityNone, 0, {});
    VLOG("runs: the run ended (%s)", why);
}

void onStarted(const uint8_t* m) {
    uint32_t content = 0;
    memcpy(&content, m + 0x48, 4);
    const uintptr_t data = challengeData(content);
    const std::string sid = data ? readString(mem::readOr<uintptr_t>(data + Data_Sid)) : std::string();
    const std::string stat = data ? readString(mem::readOr<uintptr_t>(data + Data_MainStatCode)) : std::string();
    VLOG("runs: started content %08x ('%s', stat '%s')", content, sid.c_str(), stat.c_str());
    const uint8_t kind = kindOf(stat);
    if (kind == proto::kRunBillboard || kind == proto::kRunAssault || (!stat.empty() && stat.find("_time") == std::string::npos))
        return; // not against the clock
    beginActivity(kind, localise(sid));
}

void onFinished(const uint8_t* m) {
    endActivity("finished");
    uint32_t content = 0;
    int32_t delta = 0;
    int64_t best = 0;
    memcpy(&content, m + Fin_ContentId, 4);
    memcpy(&delta, m + Fin_Delta, 4);
    memcpy(&best, m + Fin_Value, 8);
    const bool hadRecord = m[Fin_HadRecord] != 0, newRecord = m[Fin_NewRecord] != 0;
    // The message carries a record and this run's difference to it, in hundredths of a second (seen in game:
    // 2386 + 1177 and 2507 + 1056 for one 35.63 s run, one message per stat set). With a new record (or the
    // first run) the value is already this run's time and the difference is new - old (negative).
    const int64_t runCs = newRecord || !hadRecord ? best : best + delta;
    const int64_t valueMs = runCs * 10;
    {
        std::lock_guard<std::mutex> lock(g_m);
        const uint32_t now = GetTickCount();
        // One run sends one message per stat set; the first (set 0, the player's own best) is the one shown.
        if (g_recent.content == content && g_recent.value == runCs && now - g_recent.at < 5000) return;
        g_recent = {content, runCs, now};
    }
    const uintptr_t data = challengeData(content);
    const std::string sid = data ? readString(mem::readOr<uintptr_t>(data + Data_Sid)) : std::string();
    const std::string stat = data ? readString(mem::readOr<uintptr_t>(data + Data_MainStatCode)) : std::string();
    uint32_t index = 0;
    memcpy(&index, m + 0x4c, 4);
    MLOG("runs: finished content %08x ('%s', stat '%s'), set %u: record %lld, difference %d, had record %d, new record %d",
         content, sid.c_str(), stat.c_str(), index, static_cast<long long>(best), delta, hadRecord, newRecord);
    const uint8_t kind = kindOf(stat);
    // Only runs against the clock: a billboard hack's "time stamp" is when it was done, not how long.
    if (kind == proto::kRunBillboard || kind == proto::kRunAssault || (!stat.empty() && stat.find("_time") == std::string::npos)) {
        MLOG("runs: not a timed run; not announced");
        return;
    }
    if (runCs <= 0) return;
    proto::Result r;
    r.contentId = content;
    r.kind = kind;
    r.flags = static_cast<uint8_t>((newRecord ? proto::kResultNewRecord : 0) | (hadRecord ? proto::kResultHadRecord : 0));
    r.value = valueMs;
    r.delta = newRecord && hadRecord && delta < 0 ? static_cast<int64_t>(-delta) * 10 : 0; // how much faster
    r.title = localise(sid);
    MLOG("runs: you finished the %s '%s' in %s%s", proto::runKindName(kind), r.title.c_str(),
         proto::formatRunTime(valueMs).c_str(), newRecord ? " (new record)" : "");
    net::sendResult(r);
}

// Research: what the other messages of this category hold (a player-made time trial's finish is one of
// them). Each type is dumped the first few times it is seen.
void onTimeTrialFinished(const uint8_t* m) {
    endActivity("time trial finished");
    if (checkpointOrder(m + Tt_Guid) == 0) { // a ReachThis reached: no time to announce
        MLOG("runs: reached a ReachThis (not a timed run)");
        return;
    }
    uint32_t cs = 0;
    memcpy(&cs, m + Tt_Time, 4);
    uint32_t guidHead = 0;
    memcpy(&guidHead, m + Tt_Guid, 4);
    MLOG("runs: finished a time trial (%08x...) in %u hundredths", guidHead, cs);
    if (cs == 0 || cs > 360000) return; // not a time (an hour at most)
    {
        std::lock_guard<std::mutex> lock(g_m);
        const uint32_t now = GetTickCount();
        if (g_recent.content == guidHead && g_recent.value == cs && now - g_recent.at < 5000) return;
        g_recent = {guidHead, cs, now};
    }
    proto::Result r;
    r.contentId = guidHead;
    r.kind = proto::kRunTimeTrial;
    r.value = static_cast<int64_t>(cs) * 10;
    r.title = timeTrialName(m + Tt_Guid);
    MLOG("runs: you finished the time trial '%s' in %s", r.title.c_str(), proto::formatRunTime(r.value).c_str());
    net::sendResult(r);
}

// Every PamSocialPlay*Message class in the game (strings in the image), by type id (djb2-xor of the class name),
// so the verbose log names what it sees: which messages opening, closing, starting and leaving a run send.
struct MessageName { uint32_t type; const char* name; };
constexpr MessageName kMessageNames[] = {
    {0xa390e6be, "RunnerKitOpened"},
    {0x85ebbf13, "EchoCustomizationChanged"},
    {0x9a0dfb2b, "RemoveSharedContent"},
    {0xaa327a65, "InventoryChanged"},
    {0xd71d027f, "NamedChallengeFinished"},
    {0x855bb873, "OnShowMap"},
    {0x5b61f030, "OnHideMap"},
    {0xfc61a69a, "PinSharedContent"},
    {0xa3118cf7, "PermissionRequest"},
    {0x5a464701, "UnpinSharedContent"},
    {0x36154959, "FireEvent"},
    {0xf1c3cee5, "PermissionResponse"},
    {0x33d42557, "SocialTeleport"},
    {0xe7d40012, "BlockedListUpdated"},
    {0x606b598c, "NamedChallengeTeleport"},
    {0xfc74c3ca, "Control"},
    {0x052d5907, "BlockedUsers"},
    {0xa64f89ee, "NamedChallengeStatsUploaded"},
    {0xa78a38f4, "NamedChallengeBookmarkStatusRequest"},
    {0x1846085d, "UgcFilterOptionChanged"},
    {0x9b35497c, "ChallengeFinished"},
    {0xe6fd16cc, "NamedChallengeBookmarkStatusResult"},
    {0x68f236ee, "FriendsList"},
    {0x924d6bd9, "DownloadSharedContent"},
    {0x1cca484e, "StartChallenge"},
    {0xb7633230, "NamedChallengeActivated"},
    {0x72f865f9, "RestartChallenge"},
    {0x57d92a5d, "RequestHackableBillboardLeader"},
    {0x320071b1, "MapMarkerUpdates"},
    {0x82ad8c5d, "RefreshHackableBillboardLeaderCache"},
    {0xc164590c, "NamedChallengeStarted"},
    {0xf33d51ff, "HackableBillboardsLeader"},
    {0x9a9a27c6, "NamedChallengeCancelled"},
    {0xd089d3e2, "FetchSharedContents"},
    {0x955f5cbd, "NamedChallengeUploadReplay"}
};
const char* messageName(uint32_t type) {
    for (const MessageName& n : kMessageNames)
        if (n.type == type) return n.name;
    return "?";
}

void dumpMessage(const uint8_t* m, uint32_t type) {
    static uint32_t types[32] = {};
    static int counts[32] = {};
    int slot = -1;
    for (int i = 0; i < 32 && slot < 0; ++i)
        if (types[i] == type || types[i] == 0) { types[i] = type; slot = i; }
    // Every one by name (cheap); the first few of each type with their bytes (research).
    VLOG("runs: social-play %s (%08x)", messageName(type), type);
    if (slot < 0 || counts[slot]++ >= 4) return;
    uint8_t raw[0x60] = {};
    if (!mem::read(reinterpret_cast<uintptr_t>(m) + 0x30, raw, sizeof(raw))) return;
    char hex[sizeof(raw) * 3 + 1] = {};
    for (size_t i = 0; i < sizeof(raw); ++i) snprintf(hex + i * 3, 4, "%02x ", raw[i]);
    uintptr_t vt = 0;
    memcpy(&vt, m, sizeof(vt));
    VLOG("runs: social-play message %s (%08x, vtable %p), bytes from +0x30: %s", messageName(type), type, reinterpret_cast<void*>(vt), hex);
}

void onMessage(Listener* self, const void* message) {
    (void)self;
    const auto* m = static_cast<const uint8_t*>(message);
    uint32_t category = 0, type = 0;
    memcpy(&category, m + Msg_Category, 4);
    memcpy(&type, m + Msg_Type, 4);
    if (category != kCategorySocialPlay) return;
    dumpMessage(m, type);
    if (type == kTypeFinished) onFinished(m);
    else if (type == kTypeChallengeFinished) onTimeTrialFinished(m);
    else if (type == kTypeStarted) onStarted(m);
    else if (type == kTypeCancelled) endActivity("cancelled");
    else if (type == kTypeStartChallenge || type == kTypeRestartChallenge) {
        // Both managers may dispatch it: the same trial again within a moment is one start.
        static uint32_t lastHead = 0;
        static DWORD lastAt = 0;
        uint32_t head = 0;
        memcpy(&head, m + Tt_Guid, 4);
        const DWORD now = GetTickCount();
        if (type == kTypeStartChallenge && head == lastHead && now - lastAt < 1000) return;
        lastHead = head;
        lastAt = now;
        // A ReachThis (Beat L.E.) is opened with the same message as a time trial, but it is not a run:
        // counting it kept the player "in a time trial" (no teleports, the run shown above their head) after
        // they closed it, until the next run or 30 minutes.
        if (checkpointOrder(m + Tt_Guid) == 0) {
            VLOG("runs: a ReachThis was opened (not a run)");
            return;
        }
        beginActivity(proto::kRunTimeTrial, timeTrialName(m + Tt_Guid));
    }
}

} // namespace

void onNamedChallengeEntity(void* entity) {
    if (!entity) return;
    std::lock_guard<std::mutex> lock(g_m);
    track(g_entities, reinterpret_cast<uintptr_t>(entity), Vtbl_PamServerNamedChallengeEntity);
}

void onChallengeMarker(void* entity) {
    if (!entity) return;
    std::lock_guard<std::mutex> lock(g_m);
    track(g_markers, reinterpret_cast<uintptr_t>(entity), Vtbl_PamServerChallengeMarkerEntity);
}

void clientTick() { registerOn(kClientGameContext, g_clientListener, g_clientRegistered); }
void serverTick() {
    registerOn(kServerGameContext, g_serverListener, g_serverRegistered);
    bool stale = false;
    {
        std::lock_guard<std::mutex> lock(g_m);
        stale = g_activeKind != proto::kActivityNone && GetTickCount() - g_activeAt > kActivityMaxMs;
    }
    if (stale) endActivity("too long");
}

void levelChanged() { endActivity("level changed"); }


} // namespace mecmp::game::runs
