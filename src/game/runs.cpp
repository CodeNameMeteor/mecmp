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
constexpr uint32_t kTypeStarted = 0xc164590c;        // ...StartedMessage (logged only)
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

void onFinished(const uint8_t* m) {
    uint32_t content = 0, delta = 0;
    int64_t value = 0;
    memcpy(&content, m + Fin_ContentId, 4);
    memcpy(&delta, m + Fin_Delta, 4);
    memcpy(&value, m + Fin_Value, 8);
    const bool hadRecord = m[Fin_HadRecord] != 0, newRecord = m[Fin_NewRecord] != 0;
    {
        std::lock_guard<std::mutex> lock(g_m);
        const uint32_t now = GetTickCount();
        if (g_recent.content == content && g_recent.value == value && now - g_recent.at < 5000) return; // repeat
        g_recent = {content, value, now};
    }
    const uintptr_t data = challengeData(content);
    const std::string sid = data ? readString(mem::readOr<uintptr_t>(data + Data_Sid)) : std::string();
    const std::string stat = data ? readString(mem::readOr<uintptr_t>(data + Data_MainStatCode)) : std::string();
    uint32_t index = 0;
    memcpy(&index, m + 0x4c, 4);
    MLOG("runs: finished content %08x ('%s', stat '%s'), set %u: value %lld, delta %d, had record %d, new record %d",
         content, sid.c_str(), stat.c_str(), index, static_cast<long long>(value), static_cast<int32_t>(delta), hadRecord,
         newRecord);
    const uint8_t kind = kindOf(stat);
    // Only runs against the clock: a billboard hack's "time stamp" is when it was done, not how long.
    if (kind == proto::kRunBillboard || kind == proto::kRunAssault || (!stat.empty() && stat.find("_time") == std::string::npos)) {
        MLOG("runs: not a timed run; not announced");
        return;
    }
    proto::Result r;
    r.contentId = content;
    r.kind = kind;
    r.flags = static_cast<uint8_t>((newRecord ? proto::kResultNewRecord : 0) | (hadRecord ? proto::kResultHadRecord : 0));
    r.value = value;
    r.delta = hadRecord ? static_cast<int64_t>(static_cast<int32_t>(delta) < 0 ? -static_cast<int32_t>(delta) : delta) : 0;
    r.title = localise(sid);
    MLOG("runs: you finished the %s '%s' in %s%s", proto::runKindName(kind), r.title.c_str(),
         proto::formatRunTime(value).c_str(), newRecord ? " (new record)" : "");
    net::sendResult(r);
}

// Research: what the other messages of this category hold (a player-made time trial's finish is one of
// them). Each type is dumped the first few times it is seen.
void dumpMessage(const uint8_t* m, uint32_t type) {
    static uint32_t types[32] = {};
    static int counts[32] = {};
    int slot = -1;
    for (int i = 0; i < 32 && slot < 0; ++i)
        if (types[i] == type || types[i] == 0) { types[i] = type; slot = i; }
    if (slot < 0 || counts[slot]++ >= 4) return;
    uint8_t raw[0x60] = {};
    if (!mem::read(reinterpret_cast<uintptr_t>(m) + 0x30, raw, sizeof(raw))) return;
    char hex[sizeof(raw) * 3 + 1] = {};
    for (size_t i = 0; i < sizeof(raw); ++i) snprintf(hex + i * 3, 4, "%02x ", raw[i]);
    uintptr_t vt = 0;
    memcpy(&vt, m, sizeof(vt));
    MLOG("runs: social-play message type %08x (vtable %p), bytes from +0x30: %s", type, reinterpret_cast<void*>(vt), hex);
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
    else if (type == kTypeStarted) {
        uint32_t content = 0;
        memcpy(&content, m + 0x48, 4);
        VLOG("runs: started content %08x", content);
    }
}

} // namespace

void onNamedChallengeEntity(void* entity) {
    if (!entity) return;
    std::lock_guard<std::mutex> lock(g_m);
    if (g_entities.size() >= 512) { // drop the ones that are gone (levels come and go; nothing else clears them)
        std::vector<uintptr_t> live;
        for (uintptr_t e : g_entities)
            if (mem::readOr<uintptr_t>(e) == rva(Vtbl_PamServerNamedChallengeEntity)) live.push_back(e);
        g_entities.swap(live);
        if (g_entities.size() >= 512) return;
    }
    g_entities.push_back(reinterpret_cast<uintptr_t>(entity));
}

void clientTick() { registerOn(kClientGameContext, g_clientListener, g_clientRegistered); }
void serverTick() { registerOn(kServerGameContext, g_serverListener, g_serverRegistered); }


} // namespace mecmp::game::runs
