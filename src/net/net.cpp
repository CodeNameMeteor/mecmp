// See net.h.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include "net/net.h"

#include <atomic>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <algorithm>
#include <deque>
#include <random>
#include <thread>
#include <cstdio>
#include <cstdlib>

#include "core/config.h"
#include "core/log.h"
#include "net/host_guard.h"
#include "net/joincode.h"
#include "net/protocol.h"
#include "net/pose_limits.h"
#include "net/summon_policy.h"
#include "net/upnp.h"

namespace mecmp::net {
namespace {
namespace proto = protocol;

constexpr int kSnapCount = 16;
constexpr double kInterpDelayMs = 120.0; // remote players are shown this far behind their own clock
constexpr size_t kMaxPeers = 16;
constexpr size_t kMaxRemotes = 64;        // player entries kept at once (a session has at most kMaxPeers players)
constexpr DWORD kRemoteForgetMs = 120000;
constexpr int kMaxNameLogs = 3; // entries silent this long are dropped (the ghosts use 65 s)

double nowMs() {
    static const LARGE_INTEGER f = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return x; }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) * 1000.0 / static_cast<double>(f.QuadPart);
}

struct Snap { PlayerState s; double t = 0; }; // t = sender's capture time
// One of a player's marker cells (see PING in protocol.h).
struct PingCell {
    bool have = false;
    proto::Ping ping;
    DWORD arrivedAt = 0; // when this marker appeared: the flash and the notice go by it
    DWORD setAt = 0;     // when it was last placed: its lifetime counts from here
    uint64_t order = 0;  // ours only: grows with every placement
    std::vector<ReactionMark> reactions; // quick answers to it; cleared when a new marker takes the cell
    bool live(DWORD now) const { return have && now - setAt < proto::pingLifeMs(ping.life); }
};
struct Remote {
    uint32_t id = 0;
    sockaddr_in addr{};
    std::string name;
    uint32_t look = 0;
    uint8_t outfit = 0;
    proto::OnlineProfile profile; // shared by them (HELLO), for map markers
    bool loopback = false;
    uint32_t lastSeq = 0;
    bool haveSeq = false;
    DWORD lastTick = 0;  // last STATE (player data)
    DWORD lastHeard = 0; // last packet of any kind (a player who only sent a HELLO yet is still there)
    proto::Decoder decoder;
    // Sender clock -> our clock: the smallest (arrival - sent) seen = one-way delay + clock offset.
    // Replaying on the sender's timestamps keeps movement smooth when packets arrive in bursts.
    double minDelta = 0;
    bool haveDelta = false;
    proto::DelayEstimator delay; // minDelta over the last 2 s
    uint32_t lastTime = 0;
    int nameLogs = 0;         // name changes logged for this player (at most kMaxNameLogs)
    proto::ClockUnwrap clock; // sender's timestamps without the 49-day wrap
    proto::LossWindow loss;
    proto::RecentIds<128> audioSeen;
    DWORD audioWindow = 0;
    int audioBurst = 0;
    proto::RecentIds<16> chatSeen; // message ids already shown (repeats are dropped)
    DWORD chatBurstAt = 0;       // flood limit
    int chatBurst = 0;
    // Their world markers.
    proto::RecentIds<16> pingSeen;
    PingCell pings[proto::kPingCells];
    DWORD pingBurstAt = 0; // flood limit
    int pingBurst = 0;
    proto::RecentIds<16> reactSeen; // quick answers already shown
    proto::RecentIds<8> resultSeen; // finished runs already shown
    DWORD resultAt = 0;             // flood limit: one run result per kResultGapMs
    uint32_t stateFlags = 0;        // the newest STATE's flags (kStateTyping, ...)
    proto::Activity activity;       // their run now (ACTIVITY), as of activityAt
    DWORD activityAt = 0;
    DWORD reactBurstAt = 0;         // flood limit
    int reactBurst = 0;
    proto::RecentIds<16> summonSeen;       // summons already handled
    proto::RecentIds<16> summonCancelSeen; // withdrawals already handled
    DWORD summonBurstAt = 0;               // flood limit
    int summonBurst = 0;
    DWORD lastSummonAt = 0;                // the last summon of theirs that reached us (summon::kMinGapMs)
    bool haveSummon = false;
    bool asksVisit = false;                // their HELLO: ask before going to them (kHelloAskVisit)
    proto::RecentIds<16> visitSeen;        // VISIT packets already handled (id and kind)
    DWORD visitBurstAt = 0;                // flood limit
    int visitBurst = 0;
    Snap snaps[kSnapCount];
    int head = 0, fill = 0;
};

// Session roles. HOST: accepts joiners and forwards every player's packets to all the others,
// so joiners only ever talk to the host (only the host needs a reachable port). JOIN: sends to
// the host only.
enum class Role { Host, Join };
Role g_role = Role::Host;

struct Member {
    sockaddr_in addr{};
    DWORD lastSeen = 0;
    uint32_t session = 0;     // the session id this joiner speaks with (see handlePacket)
    proto::Decoder poseDecoder;
    guard::Bucket bucket;     // how fast it may send (net/host_guard.h)
};

std::mutex g_m;
std::deque<RemoteAudio> g_audio;
std::map<uint32_t, Remote> g_remotes;     // by session id
std::vector<sockaddr_in> g_sendTo;        // JOIN: the host. HOST: kept in sync with g_members
std::vector<Member> g_members;            // HOST: connected joiners
uint8_t g_cookieKey[16] = {};            // random per start(): the admission cookies it hands out are only good for this session
// HOST: session ids of joiners who went silent. Only their own address may use the id again for kSessionGraceMs.
struct Reserved { uint32_t session; sockaddr_in addr; DWORD until; };
std::vector<Reserved> g_reserved;        // guarded by g_m
constexpr DWORD kSessionGraceMs = 60000;
sockaddr_in g_hostAddr{};                 // JOIN
std::atomic<bool> g_heardFromHost{false};
std::string g_joinCode, g_lanCode;
std::atomic<uint32_t> g_generation{0}; // bumped on every start(); stale host setups don't publish
PlayerState g_local;
bool g_haveLocal = false;
DWORD g_localAt = 0; // when g_local was published
std::string g_name;
std::atomic<uint32_t> g_look{0};
std::atomic<uint8_t> g_outfit{0};

std::atomic<bool> g_run{false};
std::thread g_thread;
SOCKET g_sock = INVALID_SOCKET;
uint32_t g_session = 0;
std::atomic<uint32_t> g_hostSession{0}; // JOIN: the host's session id, from its HELLO (0 until heard)

std::atomic<bool> g_hostAllowsMembers{false}; // JOIN: what the host's HELLO said about the same
std::string g_serverName;                // JOIN: the dedicated server's name (empty: a player hosts); under g_m
float g_loopbackOffset = 3.0f;
uint16_t g_port = 47800;
// Router port mapping (host). Map and unmap calls are serialised, and an unmap is skipped when a
// newer session wants the same port mapped again (re-hosting reuses the mapping).
std::mutex g_routerM;
uint16_t g_routerMapped = 0; // port we mapped (0 = none)
std::atomic<uint16_t> g_routerWanted{0}; // port the current session wants mapped (0 = none); not under g_routerM

bool routerMap(uint16_t port, upnp::Result* out) {
    std::lock_guard<std::mutex> lock(g_routerM);
    if (g_routerWanted != port) return false; // the session changed meanwhile
    upnp::Result r = upnp::mapUdpPort(port, "MECMP", 3600);
    if (r.mapped) g_routerMapped = port;
    if (out) *out = r;
    return r.mapped;
}

void routerUnmap() {
    std::lock_guard<std::mutex> lock(g_routerM);
    if (!g_routerMapped || g_routerWanted == g_routerMapped) return;
    upnp::unmapUdpPort(g_routerMapped);
    MLOG("net: router port %u closed", g_routerMapped);
    g_routerMapped = 0;
}

std::atomic<uint64_t> g_sent{0}, g_recv{0}, g_dropped{0}, g_bytesSent{0}, g_relayed{0};

// Messages caused by other players' packets (joins, leaves, names, new session ids) share a budget:
// 20 at once, then one every 3 s, so a flood of fake players cannot fill the log (S-9).
// Called with g_m held or from the network thread only.
int g_peerLogTokens = 20;
DWORD g_peerLogRefill = 0;
uint32_t g_peerLogSuppressed = 0;
bool peerLogAllowed() {
    const DWORD now = GetTickCount();
    if (!g_peerLogRefill) g_peerLogRefill = now;
    while (now - g_peerLogRefill >= 3000 && g_peerLogTokens < 20) { ++g_peerLogTokens; g_peerLogRefill += 3000; }
    if (g_peerLogTokens >= 20) g_peerLogRefill = now;
    if (g_peerLogTokens <= 0) { ++g_peerLogSuppressed; return false; }
    --g_peerLogTokens;
    if (g_peerLogSuppressed) {
        MLOG("net: (%u messages about other players were not logged)", g_peerLogSuppressed);
        g_peerLogSuppressed = 0;
    }
    return true;
}

// The log names other players by address, which is personal: without verbose logging only the first two
// numbers are written (verbose logs are for bug reports, which need the whole address).
std::string addrToString(const sockaddr_in& a) {
    char ip[64] = {};
    if (log::verbose) {
        inet_ntop(AF_INET, &a.sin_addr, ip, sizeof(ip));
    } else {
        const unsigned char* b = reinterpret_cast<const unsigned char*>(&a.sin_addr);
        snprintf(ip, sizeof(ip), "%u.%u.x.x", b[0], b[1]);
    }
    return std::string(ip) + ":" + std::to_string(ntohs(a.sin_port));
}

bool sameAddr(const sockaddr_in& a, const sockaddr_in& b) {
    return a.sin_addr.s_addr == b.sin_addr.s_addr && a.sin_port == b.sin_port;
}

bool parsePeer(const std::string& text, sockaddr_in& out) {
    std::string s = text;
    while (!s.empty() && (s.back() == ' ' || s.back() == '\r' || s.back() == '\n')) s.pop_back();
    if (s.empty()) return false;
    std::string host = s, port = "47800";
    const size_t colon = s.rfind(':');
    if (colon != std::string::npos) { host = s.substr(0, colon); port = s.substr(colon + 1); }
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* res = nullptr;
    if (getaddrinfo(host.c_str(), port.c_str(), &hints, &res) != 0 || !res) return false;
    memcpy(&out, res->ai_addr, sizeof(sockaddr_in));
    freeaddrinfo(res);
    return true;
}

void blend(const PlayerState& a, const PlayerState& b, float k, PlayerState& out) {
    out = b;
    for (int i = 0; i < 3; ++i) out.pos[i] = a.pos[i] + (b.pos[i] - a.pos[i]) * k;
    float dy = b.yaw - a.yaw;
    while (dy > 3.14159265f) dy -= 6.2831853f;
    while (dy < -3.14159265f) dy += 6.2831853f;
    out.yaw = a.yaw + dy * k;
    if (a.boneCount != b.boneCount) return;
    for (uint32_t i = 0; i < b.boneCount; ++i) {
        const float* pa = a.bones[i];
        const float* pb = b.bones[i];
        float* po = out.bones[i];
        for (int j = 0; j < 3; ++j) po[j] = pa[j] + (pb[j] - pa[j]) * k;
        const float dot = pa[4] * pb[4] + pa[5] * pb[5] + pa[6] * pb[6] + pa[7] * pb[7];
        const float sb = dot < 0 ? -1.f : 1.f;
        float q[4], len = 0;
        for (int j = 0; j < 4; ++j) { q[j] = pa[4 + j] + (sb * pb[4 + j] - pa[4 + j]) * k; len += q[j] * q[j]; }
        len = len > 1e-12f ? 1.f / std::sqrt(len) : 0.f;
        for (int j = 0; j < 4; ++j) po[4 + j] = len ? q[j] * len : pb[4 + j];
    }
}

void sendAll(const char* buf, size_t len) {
    std::vector<sockaddr_in> to;
    {
        std::lock_guard<std::mutex> lock(g_m);
        to = g_sendTo;
    }
    static DWORD lastFailNote = 0;
    static uint64_t failures = 0;
    for (const auto& a : to) {
        if (sendto(g_sock, buf, static_cast<int>(len), 0, reinterpret_cast<const sockaddr*>(&a), sizeof(a)) ==
            static_cast<int>(len)) {
            ++g_sent;
            g_bytesSent += len;
            continue;
        }
        // A failed send is an error the beta needs to see: at most one note per 10 s, with the count.
        ++failures;
        if (GetTickCount() - lastFailNote > 10000) {
            lastFailNote = GetTickCount();
            MLOG("net: sending to %s failed (error %d; %llu failure(s) so far)", addrToString(a).c_str(), WSAGetLastError(),
                 static_cast<unsigned long long>(failures));
        }
    }
}

proto::Header header(uint16_t type) {
    static uint32_t seq = 0;
    return proto::Header{proto::kMagic, proto::kProtocol, type, g_session, ++seq};
}

std::atomic<bool> g_typing{false};

void sendState() {
    PlayerState s;
    {
        std::lock_guard<std::mutex> lock(g_m);
        // Nothing captured for a second (main menu, loading): send nothing, so the others hide our ghost
        // instead of keeping a frozen copy of where we were "live".
        if (!g_haveLocal || g_sendTo.empty() || GetTickCount() - g_localAt > 1000) return;
        s = g_local;
    }
    if (g_typing.load()) s.flags |= kStateTyping;
    static proto::Encoder enc;
    char buf[proto::kMaxPacket];
    sendAll(buf, proto::writeState(buf, header(proto::kTypeState), s, enc));
}

proto::OnlineProfile g_profile; // ours, under g_m

void sendHello() {
    char buf[proto::kMaxPacket];
    std::string name;
    {
        std::lock_guard<std::mutex> lock(g_m);
        name = g_name;
    }
    uint8_t flags = 0;
    if (g_role == Role::Host) {
        flags = proto::kHelloHost;
        if (config::get()->membersMayHardSummon) flags = static_cast<uint8_t>(flags | proto::kHelloMembersHard);
    }
    if (config::get()->askBeforeVisit) flags = static_cast<uint8_t>(flags | proto::kHelloAskVisit);
    proto::OnlineProfile profile;
    {
        std::lock_guard<std::mutex> lock(g_m);
        profile = g_profile;
    }
    sendAll(buf, proto::writeHello(buf, header(proto::kTypeHello), name.c_str(), g_look.load(), flags, g_outfit.load(), &profile));
}

// Player names are shown in the menu and written to the log: keep bytes that can be shown, drop control
// characters (a name must not be able to forge log lines). UTF-8 bytes (0x80 and up) are kept.
std::string cleanName(const char* raw, size_t len) {
    std::string s;
    for (size_t i = 0; i < len && raw[i]; ++i) {
        const unsigned char c = static_cast<unsigned char>(raw[i]);
        if (c >= 0x20 && c != 0x7f) s.push_back(static_cast<char>(c));
    }
    return s;
}

// --- Chat -----------------------------------------------------------------------------------------
constexpr size_t kChatKeep = 50;       // lines kept for the overlay
constexpr int kChatSends = 3;          // each message goes out this many times (one per send cycle)
constexpr int kChatBurst = 5;          // at most this many messages from one player...
constexpr DWORD kChatBurstMs = 5000;   // ...in this long; more are dropped
std::deque<ChatLine> g_chat;           // guarded by g_m
struct OutChat { uint32_t id; std::string text; int left; };
std::vector<OutChat> g_outChat;        // guarded by g_m
uint32_t g_chatId = 0;

void addChatLine(ChatLine line) { // caller holds g_m
    g_chat.push_back(std::move(line));
    while (g_chat.size() > kChatKeep) g_chat.pop_front();
}

void receiveChat(Remote& r, bool own, const char* payload, size_t len) { // caller holds g_m
    uint32_t id = 0;
    std::string text;
    if (!proto::readChat(payload, len, id, text)) { ++g_dropped; return; }
    if (own) return; // solo test: our own message, already shown when we sent it
    if (!r.chatSeen.add(id)) return; // a repeat of a message we already showed
    const DWORD now = GetTickCount();
    if (now - r.chatBurstAt > kChatBurstMs) { r.chatBurstAt = now; r.chatBurst = 0; }
    if (++r.chatBurst > kChatBurst) {
        if (r.chatBurst == kChatBurst + 1 && peerLogAllowed()) MLOG("net: player %08x sends chat too fast; dropping some", r.id);
        return;
    }
    addChatLine({r.name.empty() ? std::string("Runner") : r.name, text, now, false});
    VLOG("chat: %s: %s", r.name.c_str(), text.c_str());
}

void sendPendingChat() {
    std::vector<std::pair<uint32_t, std::string>> out;
    {
        std::lock_guard<std::mutex> lock(g_m);
        if (g_sendTo.empty()) { g_outChat.clear(); return; }
        for (auto& m : g_outChat) {
            out.emplace_back(m.id, m.text);
            --m.left;
        }
        g_outChat.erase(std::remove_if(g_outChat.begin(), g_outChat.end(), [](const OutChat& m) { return m.left <= 0; }),
                        g_outChat.end());
    }
    char buf[proto::kMaxPacket];
    for (const auto& m : out) sendAll(buf, proto::writeChat(buf, header(proto::kTypeChat), m.first, m.second));
}

// --- Markers (PING) and other small packets sent a few times -------------------------------------
// One-off packets go out like chat: in kChatSends consecutive send cycles, since UDP may drop one.
struct OutBurst { uint16_t type; std::vector<char> payload; int left; };
std::vector<OutBurst> g_outBurst; // guarded by g_m
uint32_t g_burstId = 0;           // guarded by g_m
// At most this many marker changes from one player per kChatBurstMs. A player may place one every
// 0.3 s (game/ping_point.cpp), which is 17 in that time.
constexpr int kPingBurst = 20;
// The same marker placed again on the same spot this soon after it appeared is the same event: it
// moves and lasts longer, but does not flash or get announced again.
constexpr DWORD kPingRepeatMs = 2000;
PingCell g_myPings[proto::kPingCells]; // guarded by g_m
uint64_t g_pingOrder = 0;              // guarded by g_m

proto::PingCellView pingView(const PingCell& c, DWORD now) {
    proto::PingCellView v;
    v.live = c.live(now);
    v.glyph = proto::pingGlyph(c.ping.flags);
    v.label = c.ping.label;
    memcpy(v.pos, c.ping.pos, sizeof(v.pos));
    v.order = c.order;
    return v;
}

void putPing(PingCell& c, const proto::Ping& p, DWORD now, uint64_t order) {
    // The same marker on exactly the same spot is its owner giving it more time (renewPing): not new either.
    const bool again = proto::samePingMarker(pingView(c, now), proto::pingGlyph(p.flags), p.label, p.pos) &&
                       (now - c.arrivedAt < kPingRepeatMs || memcmp(c.ping.pos, p.pos, sizeof(p.pos)) == 0);
    // The answers under a marker go with the slightest move of it, not only with a new marker.
    const bool moved = memcmp(c.ping.pos, p.pos, sizeof(p.pos)) != 0;
    c.have = true;
    c.ping = p;
    c.setAt = now;
    c.order = order;
    if (!again) c.arrivedAt = now;
    if (!again || moved) c.reactions.clear();
}

uint32_t nextBurstId() { // caller holds g_m
    uint32_t id = (g_session << 16) ^ ++g_burstId;
    if (!id) id = (g_session << 16) ^ ++g_burstId; // 0 always counts as "seen" on the other side
    return id;
}

void queuePing(const proto::Ping& p) { // caller holds g_m
    char buf[proto::kMaxPacket];
    const size_t n = proto::writePing(buf, proto::Header{}, p);
    g_outBurst.push_back({proto::kTypePing, std::vector<char>(buf + sizeof(proto::Header), buf + n), kChatSends});
}

void sendPendingBursts() {
    std::vector<std::pair<uint16_t, std::vector<char>>> out;
    {
        std::lock_guard<std::mutex> lock(g_m);
        if (g_sendTo.empty()) { g_outBurst.clear(); return; }
        for (auto& b : g_outBurst) {
            out.emplace_back(b.type, b.payload);
            --b.left;
        }
        g_outBurst.erase(std::remove_if(g_outBurst.begin(), g_outBurst.end(), [](const OutBurst& b) { return b.left <= 0; }),
                         g_outBurst.end());
    }
    char buf[proto::kMaxPacket];
    for (const auto& b : out) {
        if (b.second.size() > sizeof(buf) - sizeof(proto::Header)) continue;
        const proto::Header h = header(b.first);
        memcpy(buf, &h, sizeof(h));
        memcpy(buf + sizeof(h), b.second.data(), b.second.size());
        sendAll(buf, sizeof(h) + b.second.size());
    }
}

void receivePing(Remote& r, bool own, const char* payload, size_t len) { // caller holds g_m
    proto::Ping p;
    if (!proto::readPing(payload, len, p)) { ++g_dropped; return; }
    if (own) return; // solo test: our own marker, already shown when we placed it
    if (!r.pingSeen.add(p.id)) return; // a repeat
    const DWORD now = GetTickCount();
    if (now - r.pingBurstAt > kChatBurstMs) { r.pingBurstAt = now; r.pingBurst = 0; }
    if (++r.pingBurst > kPingBurst) {
        if (r.pingBurst == kPingBurst + 1 && peerLogAllowed()) MLOG("net: player %08x places markers too fast; dropping some", r.id);
        return;
    }
    if (p.flags & proto::kPingClear) {
        for (int i = 0; i < proto::kPingCells; ++i)
            if (p.index == proto::kPingAllCells || p.index == i) r.pings[i].have = false;
        return;
    }
    putPing(r.pings[p.index], p, now, 0);
}

// --- Host side: who may join and what they may send (caller holds g_m) ---------------------------
Member* memberFrom(const sockaddr_in& from) {
    for (auto& m : g_members)
        if (sameAddr(m.addr, from)) return &m;
    return nullptr;
}

// True if another player (or the host itself) already speaks with this session id.
bool sessionInUse(uint32_t session, const sockaddr_in& except) {
    if (session == g_session) return true;
    for (const auto& r : g_reserved)
        if (r.session == session && !sameAddr(r.addr, except)) return true;
    for (const auto& m : g_members)
        if (m.session == session && !sameAddr(m.addr, except)) return true;
    return false;
}

uint32_t sameIpCount(const sockaddr_in& from) {
    uint32_t n = 0;
    for (const auto& m : g_members) n += m.addr.sin_addr.s_addr == from.sin_addr.s_addr;
    return n;
}

// Lets a new joiner in, or refuses it (the reason is logged now and then). Null = refused.
Member* admitMember(const sockaddr_in& from, uint32_t session) {
    static DWORD lastWarn = 0;
    const char* why = nullptr;
    if (g_members.size() >= kMaxPeers) why = "session full";
    else if (sameIpCount(from) >= guard::kMaxPerIp) why = "too many players from this address";
    else if (sessionInUse(session, from)) why = "its session id is in use";
    if (why) {
        if (GetTickCount() - lastWarn > 10000) {
            lastWarn = GetTickCount();
            MLOG("net: not admitting %s: %s (%llu player(s) connected)", addrToString(from).c_str(), why,
                 static_cast<unsigned long long>(g_members.size()));
        }
        return nullptr;
    }
    Member m;
    m.addr = from;
    m.lastSeen = GetTickCount();
    m.session = session;
    g_members.push_back(m);
    g_sendTo.push_back(from);
    if (peerLogAllowed())
        MLOG("net: %s joined (%llu player(s) connected)", addrToString(from).c_str(),
             static_cast<unsigned long long>(g_members.size()));
    return &g_members.back();
}

// --- Quick answers (REACT) -----------------------------------------------------------------------
constexpr int kReactBurst = 6; // at most this many answers from one player per kChatBurstMs
std::deque<ReactionInfo> g_reactions; // answers to our markers, not yet shown; guarded by g_m
DWORD g_lastReactSent = 0;            // guarded by g_m

constexpr size_t kMaxMarks = 16; // answers kept under one marker (the screen shows the first few)

// The marker cell of the player with session id `owner` (ours or a remote's), or null. Caller holds g_m.
PingCell* cellOf(uint32_t owner, uint8_t cell) {
    if (cell >= proto::kPingCells) return nullptr;
    if (owner == g_session) return &g_myPings[cell];
    const auto it = g_remotes.find(owner);
    return it == g_remotes.end() ? nullptr : &it->second.pings[cell];
}

// Remembers an answer under its marker: the same player answering again moves theirs to the end.
void addMark(PingCell& c, ReactionMark mark) {
    for (size_t i = 0; i < c.reactions.size(); ++i)
        if (c.reactions[i].from == mark.from) {
            c.reactions.erase(c.reactions.begin() + static_cast<ptrdiff_t>(i));
            break;
        }
    c.reactions.push_back(std::move(mark));
    if (c.reactions.size() > kMaxMarks) c.reactions.erase(c.reactions.begin());
}

void receiveReact(Remote& r, bool own, const char* payload, size_t len) { // caller holds g_m
    proto::React re;
    if (!proto::readReact(payload, len, re)) { ++g_dropped; return; }
    if (own) return;                           // sendReact already put ours under the marker
    if (!r.reactSeen.add(re.id)) return;       // a repeat
    const DWORD now = GetTickCount();
    // Under the marker, for everyone who has it (one mark per player and marker, so no burst cap needed),
    // unless the answer was for a marker that has been replaced in this cell since.
    if (PingCell* c = cellOf(re.target, re.cell); c && c->live(now) && c->ping.label == re.title) {
        ReactionMark mark;
        mark.from = r.id;
        mark.name = r.name.empty() ? std::string("Runner") : r.name;
        mark.text = re.text;
        mark.glyph = re.glyph;
        memcpy(mark.rgb, re.rgb, sizeof(mark.rgb));
        memcpy(mark.accent, re.accent, sizeof(mark.accent));
        addMark(*c, std::move(mark));
    }
    if (re.target != g_session) return; // the notice is for the marker's owner
    if (now - r.reactBurstAt > kChatBurstMs) { r.reactBurstAt = now; r.reactBurst = 0; }
    if (++r.reactBurst > kReactBurst) return;
    ReactionInfo info;
    info.from = r.name.empty() ? std::string("Runner") : r.name;
    info.text = re.text;
    info.title = re.title;
    g_reactions.push_back(info);
    while (g_reactions.size() > 16) g_reactions.pop_front();
}

// --- Finished runs (RESULT) ------------------------------------------------------------------------
constexpr DWORD kResultGapMs = 3000; // a run takes longer than this: more often is someone flooding
std::deque<std::string> g_results;   // notices about others' finished runs, not yet shown; guarded by g_m

void receiveResult(Remote& r, bool own, const char* payload, size_t len) { // caller holds g_m
    proto::Result re;
    if (!proto::readResult(payload, len, re)) { ++g_dropped; return; }
    if (!r.resultSeen.add(re.id)) return; // a repeat
    const DWORD now = GetTickCount();
    if (r.resultAt && now - r.resultAt < kResultGapMs) return;
    r.resultAt = now;
    const std::string text = proto::resultText(r.name, re);
    MLOG("run: %s%s", text.c_str(), own ? " (solo test: our own)" : "");
    g_results.push_back(text);
    while (g_results.size() > 8) g_results.pop_front();
}

// --- What players are doing (ACTIVITY) --------------------------------------------------------------
constexpr DWORD kActivityForgetMs = 4000; // no ACTIVITY for this long: they are not in a run any more
struct MyActivity {
    uint8_t kind = proto::kActivityNone;
    DWORD started = 0;
    std::string title;
    int endSends = 0; // "none" packets still to send after a run ended
};
MyActivity g_myActivity; // guarded by g_m

void receiveActivity(Remote& r, const char* payload, size_t len) { // caller holds g_m
    proto::Activity a;
    if (!proto::readActivity(payload, len, a)) { ++g_dropped; return; }
    r.activity = a;
    r.activityAt = GetTickCount();
}

// Network thread, every frame: our run, about once a second while in one, then "none" a few times.
void sendActivity() {
    static DWORD last = 0;
    const DWORD now = GetTickCount();
    proto::Activity a;
    {
        std::lock_guard<std::mutex> lock(g_m);
        MyActivity& m = g_myActivity;
        const bool active = m.kind != proto::kActivityNone;
        if (!active && m.endSends <= 0) return;
        if (now - last < (active ? 1000u : 250u)) return;
        if (g_sendTo.empty()) return;
        if (!active) --m.endSends;
        a.kind = m.kind;
        a.elapsedMs = active ? now - m.started : 0;
        a.title = m.title;
    }
    last = now;
    char buf[proto::kMaxPacket];
    sendAll(buf, proto::writeActivity(buf, header(proto::kTypeActivity), a));
}

// --- Summon ----------------------------------------------------------------------------------------
constexpr int kSummonBurst = 6; // at most this many summons / withdrawals from one player per kChatBurstMs
std::deque<SummonInfo> g_summons; // summons to show, not yet taken; guarded by g_m
std::deque<VisitInfo> g_visits;   // VISIT packets for us, not yet taken; guarded by g_m
// Our own open summon: who has answered and how.
struct MySummon {
    bool open = false;
    uint32_t id = 0;
    DWORD at = 0;
    std::vector<uint32_t> targets; // empty = everyone
    std::vector<uint32_t> answered;
    summon::Tally tally;
};
MySummon g_mySummon;                          // guarded by g_m
DWORD g_lastSummonSent = 0;                   // guarded by g_m; 0 = never
std::vector<std::string> g_summonReports;     // guarded by g_m

void finishMySummon() { // caller holds g_m
    if (!g_mySummon.open) return;
    g_summonReports.push_back(summon::reportText(g_mySummon.tally));
    g_mySummon.open = false;
}

// `senderIsHost` / `vouchTrusted`: see summon::Input. Caller holds g_m.
void receiveSummon(Remote& r, bool own, bool senderIsHost, bool vouchTrusted, const char* payload, size_t len) {
    proto::Summon s;
    if (!proto::readSummon(payload, len, s)) { ++g_dropped; return; }
    if (own) return; // solo test: our own summon
    const DWORD now = GetTickCount();
    const bool cancel = (s.flags & proto::kSummonCancel) != 0;
    if (!(cancel ? r.summonCancelSeen : r.summonSeen).add(s.id)) return; // a repeat
    if (now - r.summonBurstAt > kChatBurstMs) { r.summonBurstAt = now; r.summonBurst = 0; }
    if (++r.summonBurst > kSummonBurst) return;
    SummonInfo info;
    info.id = s.id;
    info.from = r.id;
    info.name = r.name.empty() ? std::string("Runner") : r.name;
    if (cancel) {
        info.cancel = true;
    } else {
        summon::Input in;
        in.calledMe = proto::summonCalls(s, g_session);
        in.hardRequested = (s.flags & proto::kSummonHard) != 0;
        in.senderIsHost = senderIsHost;
        in.vouched = (s.flags & proto::kSummonVouched) != 0;
        in.vouchTrusted = vouchTrusted;
        in.optedIn = config::get()->allowForcedSummon;
        in.sinceLastFromSenderMs = r.haveSummon ? now - r.lastSummonAt : 0xffffffffu;
        const summon::Action a = summon::decide(in);
        if (a == summon::Action::Ignore) return;
        r.lastSummonAt = now;
        r.haveSummon = true;
        memcpy(info.pos, s.pos, sizeof(info.pos));
        info.yaw = s.yaw;
        info.hard = a == summon::Action::Hard;
    }
    g_summons.push_back(info);
    while (g_summons.size() > 16) g_summons.pop_front();
}

void receiveSummonReply(Remote& r, bool own, const char* payload, size_t len) { // caller holds g_m
    proto::SummonReply re;
    if (!proto::readSummonReply(payload, len, re)) { ++g_dropped; return; }
    if (own || re.target != g_session) return; // not about us
    MySummon& m = g_mySummon;
    if (!m.open || m.id != re.id) return;
    if (!m.targets.empty() && std::find(m.targets.begin(), m.targets.end(), r.id) == m.targets.end()) return;
    if (std::find(m.answered.begin(), m.answered.end(), r.id) != m.answered.end()) return; // a repeat
    m.answered.push_back(r.id);
    if (re.result == proto::kSummonTeleporting) ++m.tally.teleporting;
    else if (re.result == proto::kSummonBusy) ++m.tally.busy;
    else ++m.tally.declined;
    if (m.tally.expected <= 1) m.tally.singleName = r.name;
    if (summon::complete(m.tally)) finishMySummon();
}

void receiveVisit(Remote& r, bool own, const char* payload, size_t len) { // caller holds g_m
    proto::Visit v;
    if (!proto::readVisit(payload, len, v)) { ++g_dropped; return; }
    if (own || v.target != g_session) return; // not for us
    if (!r.visitSeen.add(v.id * 4u + v.kind)) return; // a repeat
    const DWORD now = GetTickCount();
    if (now - r.visitBurstAt > kChatBurstMs) { r.visitBurstAt = now; r.visitBurst = 0; }
    if (++r.visitBurst > kSummonBurst) return;
    VisitInfo info;
    info.id = v.id;
    info.from = r.id;
    info.name = r.name.empty() ? std::string("Runner") : r.name;
    info.kind = v.kind;
    g_visits.push_back(info);
    while (g_visits.size() > 16) g_visits.pop_front();
}

// --- Admission (protocol 7, see kTypeChallenge in protocol.h) -----------------------------------------
uint32_t cookieMinute() { return GetTickCount() / 60000; }
uint64_t cookieFor(const sockaddr_in& a, uint32_t minute) {
    return proto::admissionCookie(g_cookieKey, ntohl(a.sin_addr.s_addr), ntohs(a.sin_port), minute);
}
// This minute's cookie for the address, or the last minute's (an answer may come just after the minute changed).
bool validCookie(const sockaddr_in& a, uint64_t cookie) {
    const uint32_t m = cookieMinute();
    return cookie == cookieFor(a, m) || (m > 0 && cookie == cookieFor(a, m - 1));
}

// Host, caller holds g_m (sendto does not take the lock): a stranger's HELLO is answered with the cookie that
// proves its address. Nothing is kept for it.
void sendChallenge(const sockaddr_in& to) {
    char out[proto::kMaxPacket];
    VLOG("admission: challenged %s", addrToString(to).c_str());
    const size_t n = proto::writeCookie(out, header(proto::kTypeChallenge), cookieFor(to, cookieMinute()));
    sendto(g_sock, out, static_cast<int>(n), 0, reinterpret_cast<const sockaddr*>(&to), sizeof(to));
}

// A CHALLENGE (from the host, to a joiner) is answered at once, with our HELLO straight after it, so the host lets
// us in. An ANSWER (from a stranger, to the host) with a valid cookie is let in, the same as a joiner always was.
void handleAdmission(const proto::Header& h, const char* buf, int n, const sockaddr_in& from) {
    uint64_t cookie = 0;
    if (!proto::readCookie(buf + sizeof(h), static_cast<size_t>(n) - sizeof(h), cookie)) { ++g_dropped; return; }
    if (h.type == proto::kTypeChallenge) {
        {
            std::lock_guard<std::mutex> lock(g_m);
            if (g_role != Role::Join || !sameAddr(from, g_hostAddr)) { ++g_dropped; return; }
        }
        char out[proto::kMaxPacket];
        sendAll(out, proto::writeCookie(out, header(proto::kTypeAnswer), cookie));
        sendHello();
        return;
    }
    std::lock_guard<std::mutex> lock(g_m);
    if (g_role != Role::Host || h.session == g_session || memberFrom(from) || !validCookie(from, cookie)) {
        VLOG("admission: answer from %s refused (%s)", addrToString(from).c_str(),
             g_role != Role::Host ? "not a host" : !validCookie(from, cookie) ? "wrong or old cookie" : "not a new player");
        ++g_dropped;
        return;
    }
    admitMember(from, h.session);
}

void handlePacket(const char* buf, int n, const sockaddr_in& from) {
    proto::Header h;
    if (n < static_cast<int>(sizeof(h))) { ++g_dropped; return; }
    memcpy(&h, buf, sizeof(h));
    if (h.magic != proto::kMagic) { ++g_dropped; return; }
    if (h.protocol != proto::kProtocol) {
        static DWORD lastWarn = 0;
        if (GetTickCount() - lastWarn > 10000) {
            lastWarn = GetTickCount();
            MLOG("net: %s uses MECMP protocol %u, we use %u; update both to the same version", addrToString(from).c_str(),
                 h.protocol, proto::kProtocol);
        }
        ++g_dropped;
        return;
    }
    // Admission: a challenge from the host, or a stranger's answer. Never stored or forwarded.
    if (h.type == proto::kTypeChallenge || h.type == proto::kTypeAnswer) { handleAdmission(h, buf, n, from); return; }
    // Only the packet types we know may create state or be forwarded to other players.
    if (!proto::knownType(h.type)) { ++g_dropped; return; }
    const bool own = h.session == g_session;
    // What this host changed in the packet (a joiner's HELLO flags, a summon's flags): what is forwarded is
    // also what is processed here, so the host sees exactly what the others see.
    char relay[proto::kMaxPacket + 64];
    const char* data = buf;
    std::unique_lock<std::mutex> lock(g_m);
    if (g_role == Role::Host) {
        // Nothing of ours comes back to the host, so a packet that claims our session id is forged.
        if (own) { ++g_dropped; return; }
        Member* m = memberFrom(from);
        if (!m) { // a stranger: only a HELLO is answered, with a challenge; it is let in by its ANSWER
            if (h.type == proto::kTypeHello) sendChallenge(from);
            ++g_dropped;
            return;
        }
        m->lastSeen = GetTickCount();
        // A player speaks only as itself: its session id is fixed when it joins. Only a HELLO may change it
        // (the game was restarted), and only to an id no other player uses.
        if (h.session != m->session) {
            if (h.type != proto::kTypeHello || sessionInUse(h.session, from)) { ++g_dropped; return; }
            if (peerLogAllowed()) MLOG("net: %s is now session %08x (restarted)", addrToString(from).c_str(), h.session);
            m->session = h.session;
            m->poseDecoder = {};
        }
        if (!m->bucket.take(GetTickCount())) { ++g_dropped; return; } // flooding: not forwarded, not processed
        // A joiner's HELLO goes on without the "I host" flag: only the host's own HELLO may set it.
        const char* out = buf;
        if (h.type == proto::kTypeState) {
            PlayerState state; bool full = false;
            if (!proto::readState(buf + sizeof(h), static_cast<size_t>(n) - sizeof(h), m->poseDecoder, state, full)) {
                ++g_dropped; return;
            }
            if (full) proto::commitBase(m->poseDecoder, state);
            if (!limitPose(state, config::get()->boneReach)) { ++g_dropped; return; }
            // A sanitized full pose does not depend on the sender's original
            // delta base, including for a player who joined midway through it.
            proto::Encoder encoder;
            n = static_cast<int>(proto::writeState(relay, h, state, encoder));
            if (!n) { ++g_dropped; return; }
            out = relay;
        } else if (h.type == proto::kTypeAudio) {
            PlayerAudio sample;
            if (!proto::readAudio(buf + sizeof(h), static_cast<size_t>(n) - sizeof(h), sample)) { ++g_dropped; return; }
        }
        if (h.type == proto::kTypeHello && n > static_cast<int>(sizeof(h) + proto::kHelloFlagsOffset)) {
            memcpy(relay, buf, static_cast<size_t>(n));
            relay[sizeof(h) + proto::kHelloFlagsOffset] =
                static_cast<char>(relay[sizeof(h) + proto::kHelloFlagsOffset] & ~(proto::kHelloHost | proto::kHelloMembersHard));
            out = relay;
        }
        // A joiner's summon is vouched for by this host, or not: whatever the joiner wrote is replaced by the
        // lobby's rule (protocol::applyHostPolicy). Players only ever hear a joiner through the host.
        if (h.type == proto::kTypeSummon) {
            proto::Summon s;
            if (n > static_cast<int>(sizeof(h)) && static_cast<size_t>(n) <= sizeof(relay) &&
                proto::readSummon(buf + sizeof(h), static_cast<size_t>(n) - sizeof(h), s)) {
                memcpy(relay, buf, static_cast<size_t>(n));
                proto::applyHostPolicy(s, false, config::get()->membersMayHardSummon);
                proto::patchSummonFlags(relay, static_cast<size_t>(n), s.flags);
                out = relay;
            } else {
                ++g_dropped;
                return;
            }
        }
        data = out;
        // The sends run without g_m: the socket does not block, and only this thread changes the member list.
        std::vector<sockaddr_in> to;
        for (const auto& other : g_members)
            if (!sameAddr(other.addr, from)) to.push_back(other.addr);
        lock.unlock();
        for (const auto& a : to)
            if (sendto(g_sock, out, n, 0, reinterpret_cast<const sockaddr*>(&a), sizeof(a)) == n) ++g_relayed;
        lock.lock();
    }
    // Joining: everything legitimate comes through the host (a player host forwards its joiners, a server
    // relays everyone), so a packet from any other address is forged or stray. Without this, anyone who knows
    // a joiner's address could send it packets that claim the host's session (a host summon, chat, a pose).
    if (g_role == Role::Join && !sameAddr(from, g_hostAddr)) {
        ++g_dropped;
        static DWORD lastWarn = 0;
        if (GetTickCount() - lastWarn > 10000) {
            lastWarn = GetTickCount();
            MLOG("net: ignored a packet from %s (not the host %s)", addrToString(from).c_str(),
                 addrToString(g_hostAddr).c_str());
        }
        return;
    }
    // Joining: the host answered. A solo test joins itself, so its own packets coming back count too.
    if (g_role == Role::Join && !g_heardFromHost) {
        g_heardFromHost = true;
        MLOG("net: connected to the host %s", addrToString(from).c_str());
    }
#if !MECMP_DEBUG_KEYS
    // The solo test (our own packets coming back as a mirrored ghost) is a developer tool: release
    // builds ignore their own packets, so joining yourself shows nobody.
    if (own) {
        static bool told = false;
        if (!told) {
            told = true;
            MLOG("net: these are our own packets (joined yourself); the solo test is only in developer builds");
        }
        return;
    }
#endif
    auto found = g_remotes.find(h.session);
    if (found == g_remotes.end()) {
        if (g_remotes.size() >= kMaxRemotes) {
            // Flood or spoofed session ids: keep memory bounded by replacing the entry heard from longest ago.
            auto stalest = g_remotes.begin();
            for (auto it = g_remotes.begin(); it != g_remotes.end(); ++it)
                if (it->second.lastHeard < stalest->second.lastHeard) stalest = it;
            g_remotes.erase(stalest);
        }
        found = g_remotes.emplace(h.session, Remote{}).first;
    }
    Remote& r = found->second;
    r.lastHeard = GetTickCount();
    if (!r.id) {
        r.id = h.session;
        r.addr = from;
        r.loopback = own;
        if (peerLogAllowed()) MLOG("net: new player %08x from %s%s", h.session, addrToString(from).c_str(), own ? " (our own packets: solo test)" : "");
    }
    // Sequence numbers restart when a player restarts the game: accept a large backwards jump.
    if (r.haveSeq && h.seq <= r.lastSeq && r.lastSeq - h.seq < 1000) return;
    const uint32_t gap = r.haveSeq && h.seq > r.lastSeq ? h.seq - r.lastSeq - 1 : 0;
    r.loss.add(nowMs(), gap < 1000 ? gap : 0); // a bigger jump is a restart, not loss
    r.lastSeq = h.seq;
    r.haveSeq = true;
    ++g_recv;
    const char* payload = data + sizeof(h);
    const size_t len = static_cast<size_t>(n) - sizeof(h);
    if (h.type == proto::kTypeHello) {
        std::string raw;
        uint32_t look = 0;
        uint8_t flags = 0;
        uint8_t wear = 0;
        proto::OnlineProfile profile;
        proto::readHello(payload, len, raw, look, &flags, &wear, &profile);
        r.profile = profile;
        r.asksVisit = (flags & proto::kHelloAskVisit) != 0;
        // Accepted only straight from the host (forwarded copies come from the host's address too, but have the flag cleared).
        if ((flags & proto::kHelloHost) && !own && g_role == Role::Join && sameAddr(from, g_hostAddr)) {
            g_hostSession = h.session;
            g_hostAllowsMembers = (flags & proto::kHelloMembersHard) != 0 && !(flags & proto::kHelloServer);
            // A dedicated server: not a player (it sends no state, so it never gets a ghost or a list entry).
            if (flags & proto::kHelloServer) {
                const std::string serverName = cleanName(raw.data(), raw.size());
                if (serverName != g_serverName) MLOG("net: the host is the dedicated server '%s'", serverName.c_str());
                g_serverName = serverName;
                r.name = serverName; // its chat lines (welcome, notices) show under its name
                return;
            }
        }
        const uint8_t outfit = look ? 0 : wear;
        if (look != r.look || outfit != r.outfit) {
            r.look = look;
            r.outfit = outfit;
            if (peerLogAllowed()) MLOG("net: player %08x looks like %s (outfit %u)", r.id, look ? "a ghost look" : "Faith", outfit);
        }
        const std::string name = cleanName(raw.data(), raw.size());
        if (r.name != name) {
            r.name = name;
            if (r.nameLogs < kMaxNameLogs && peerLogAllowed()) {
                ++r.nameLogs;
                MLOG("net: player %08x is '%s'%s", r.id, name.c_str(),
                     r.nameLogs == kMaxNameLogs ? " (further name changes of this player are not logged)" : "");
            }
        }
        return;
    }
    if (h.type == proto::kTypeChat) {
        receiveChat(r, own, payload, len);
        return;
    }
    if (h.type == proto::kTypePing) {
        receivePing(r, own, payload, len);
        return;
    }
    if (h.type == proto::kTypeReact) {
        receiveReact(r, own, payload, len);
        return;
    }
    if (h.type == proto::kTypeResult) {
        receiveResult(r, own, payload, len);
        return;
    }
    if (h.type == proto::kTypeActivity) {
        receiveActivity(r, payload, len);
        return;
    }
    if (h.type == proto::kTypeVisit) {
        receiveVisit(r, own, payload, len);
        return;
    }
    if (h.type == proto::kTypeSummon || h.type == proto::kTypeSummonReply) {
        if (h.type == proto::kTypeSummonReply) {
            receiveSummonReply(r, own, payload, len);
            return;
        }
        // The lobby's host (a player, not a dedicated server) may pull players in; so may a member it
        // vouched for. Behind a dedicated server nobody is trusted: it forwards bytes as they are, so a
        // vouch in them proves nothing, and until the host's HELLO is heard we do not know which it is.
        const bool playerHost = g_role == Role::Host || (g_hostSession.load() != 0 && g_serverName.empty());
        const bool senderIsHost = g_role == Role::Join && playerHost && h.session == g_hostSession.load();
        receiveSummon(r, own, senderIsHost, playerHost, payload, len);
        return;
    }
    if (h.type == proto::kTypeAudio) {
        PlayerAudio sample;
        const DWORD now = GetTickCount();
        if (!proto::readAudio(payload, len, sample)) { ++g_dropped; return; }
        if (r.audioSeen.seen(sample.id)) return;
        if (now - r.audioWindow >= 1000) { r.audioWindow = now; r.audioBurst = 0; }
        if (++r.audioBurst > 24 || !r.fill || now - r.lastTick > 1000) return;
        r.audioSeen.add(sample.id);
        if (g_audio.size() >= 256) g_audio.pop_front();
        // Match the pose interpolation delay on the sender's clock. Do not
        // advance the pose clock with an out-of-order audio timestamp.
        const double captured = r.clock.ext + static_cast<int32_t>(sample.timeMs - r.lastTime);
        const double until = captured + r.minDelta + kInterpDelayMs - nowMs();
        if (until < -500. || until > 500.) return;
        const DWORD playAt = now + static_cast<DWORD>(static_cast<int32_t>(std::max(0., until)));
        g_audio.push_back({r.id, now, playAt, sample});
        return;
    }
    if (h.type != proto::kTypeState) return;
    PlayerState s;
    bool full = false;
    if (!proto::readState(payload, len, r.decoder, s, full)) { ++g_dropped; return; }
    if (full) proto::commitBase(r.decoder, s);
    r.stateFlags = s.flags;
    if (own) { // solo test: the mirror stands a fixed distance away along the world X axis, so it does
               // not turn with the camera and you can walk around it (to check it and its name tag)
        s.pos[0] += g_loopbackOffset;
    }
    r.lastTick = GetTickCount();
    if (r.fill && static_cast<int32_t>(s.timeMs - r.lastTime) <= 0) return; // same capture sent twice
    r.lastTime = s.timeMs;
    const double sent = r.clock.extend(s.timeMs);
    r.minDelta = r.delay.add(nowMs(), sent); // follows clock drift and route changes
    r.haveDelta = true;
    // Same rotation sign as the snapshot before, so blending never flips a bone (in-game jitter).
    if (r.fill) proto::alignRotations(s, r.snaps[(r.head + kSnapCount - 1) % kSnapCount].s);
    r.snaps[r.head] = {s, sent};
    r.head = (r.head + 1) % kSnapCount;
    if (r.fill < kSnapCount) ++r.fill;
}

// --- Host setup: router port mapping (UPnP), public address, join code ---------------------------
// STUN (RFC 5389) binding request through our game socket: the reply tells our public ip:port as
// the internet sees it. Fallback for routers without UPnP (the port then has to be forwarded).
constexpr uint32_t kStunCookie = 0x2112A442;
std::atomic<uint32_t> g_stunIp{0};
std::atomic<uint16_t> g_stunPort{0};
// The transaction id of the request we are waiting for. A reply only counts with this id.
std::mutex g_stunM;
unsigned char g_stunTx[12] = {};
bool g_stunPending = false;

void sendStunRequest() {
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* res = nullptr;
    if (getaddrinfo("stun.l.google.com", "19302", &hints, &res) != 0 || !res) return;
    unsigned char req[20] = {0x00, 0x01, 0x00, 0x00, 0x21, 0x12, 0xA4, 0x42};
    {
        // One id for all retries of this setup, so a late reply to an earlier try still counts.
        std::lock_guard<std::mutex> lock(g_stunM);
        if (!g_stunPending) {
            std::random_device rd;
            for (unsigned char& c : g_stunTx) c = static_cast<unsigned char>(rd());
            g_stunPending = true;
        }
        memcpy(req + 8, g_stunTx, sizeof(g_stunTx));
    }
    sendto(g_sock, reinterpret_cast<const char*>(req), sizeof(req), 0, res->ai_addr, static_cast<int>(res->ai_addrlen));
    freeaddrinfo(res);
}

bool handleStun(const unsigned char* b, int n) {
    if (n < 20 || b[0] != 0x01 || b[1] != 0x01) return false; // binding success response
    const uint32_t cookie = (static_cast<uint32_t>(b[4]) << 24) | (static_cast<uint32_t>(b[5]) << 16) |
                            (static_cast<uint32_t>(b[6]) << 8) | b[7];
    if (cookie != kStunCookie) return false;
    {
        std::lock_guard<std::mutex> lock(g_stunM);
        if (!g_stunPending || memcmp(b + 8, g_stunTx, sizeof(g_stunTx)) != 0) return true; // not the answer we asked for
        g_stunPending = false;
    }
    for (int p = 20; p + 4 <= n;) {
        const int type = (b[p] << 8) | b[p + 1], len = (b[p + 2] << 8) | b[p + 3];
        if (type == 0x0020 && len >= 8 && p + 4 + len <= n && b[p + 5] == 0x01) { // XOR-MAPPED-ADDRESS, IPv4
            g_stunPort = static_cast<uint16_t>(((b[p + 6] << 8) | b[p + 7]) ^ (kStunCookie >> 16));
            g_stunIp = ((static_cast<uint32_t>(b[p + 8]) << 24) | (static_cast<uint32_t>(b[p + 9]) << 16) |
                        (static_cast<uint32_t>(b[p + 10]) << 8) | b[p + 11]) ^ kStunCookie;
        }
        p += 4 + ((len + 3) & ~3);
    }
    return true;
}

uint32_t parseIp(const std::string& s) {
    in_addr a{};
    return inet_pton(AF_INET, s.c_str(), &a) == 1 ? ntohl(a.s_addr) : 0;
}

bool isPrivate(uint32_t ip) {
    return (ip >> 24) == 10 || (ip >> 20) == (172 << 4 | 1) || (ip >> 16) == (192 << 8 | 168) ||
           (ip >> 22) == ((100 << 2) | 1) /* 100.64/10, carrier-grade NAT */ || (ip >> 24) == 127;
}

void publishCode(const std::string& code, const std::string& lan) {
    {
        std::lock_guard<std::mutex> lock(g_m);
        g_joinCode = code;
        g_lanCode = lan;
    }
    // join_code.txt next to the mod. The clipboard is only written by the menu's Copy button (S-13).
    const std::wstring path = config::dir() + L"join_code.txt";
    if (FILE* f = _wfopen(path.c_str(), L"wb")) {
        fprintf(f, "MECMP join code: %s\r\nSame network (LAN): %s\r\n", code.c_str(), lan.c_str());
        fclose(f);
    }
}

void hostSetup(uint32_t generation) {
    upnp::Result r;
    if (!routerMap(g_port, &r)) {
        if (generation != g_generation) return;
        if (r.error.empty()) r.error = "not attempted";
    }
    const uint32_t lanIp = parseIp(r.localIp);
    const std::string lan = lanIp ? joincode::encode(lanIp, g_port) : std::string();
    uint32_t pub = parseIp(r.externalIp);
    uint16_t pubPort = g_port;
    if (r.mapped) {
        MLOG("net: router port %u opened with UPnP", g_port);
    } else {
        MLOG("net: UPnP: %s", r.error.c_str());
        // Ask a public STUN server how the internet sees us (waits up to ~2 s for the reply).
        for (int i = 0; i < 4 && !g_stunIp; ++i) {
            sendStunRequest();
            Sleep(500);
        }
        if (g_stunIp) {
            pub = g_stunIp;
            pubPort = g_stunPort;
        }
        MLOG("net: friends outside your network can only join if UDP port %u is forwarded to this PC on your router",
             g_port);
    }
    if (pub && isPrivate(pub)) {
        // The router itself sits behind another router (double NAT) or the ISP's NAT. Ask STUN
        // how the internet sees us; the outer router may still need the port forwarded.
        for (int i = 0; i < 4 && !g_stunIp; ++i) {
            sendStunRequest();
            Sleep(500);
        }
        const uint32_t outer = g_stunIp;
        MLOG("net: your router reports a private address (%u.%u.%u.%u): there is another router or the ISP's NAT in "
             "front of it.", pub >> 24, (pub >> 16) & 255, (pub >> 8) & 255, pub & 255);
        if (outer && !isPrivate(outer)) {
            pub = outer;
            pubPort = g_stunPort;
            MLOG("net: the join code below works if the outer router forwards UDP %u to your router. If friends cannot "
                 "join, let another player host, or use Tailscale/ZeroTier.", g_port);
        } else {
            MLOG("net: not reachable from the internet (carrier-grade NAT). Let another player host, or use Tailscale/ZeroTier.");
            pub = 0;
        }
    }
    const std::string code = pub ? joincode::encode(pub, pubPort) : std::string();
    if (generation != g_generation) return; // the session was changed meanwhile
    publishCode(code, lan);
    if (!code.empty())
        MLOG("net: HOSTING. Join code: %s  (saved in join_code.txt; the menu's Copy button copies it)", code.c_str());
    if (!lan.empty()) MLOG("net: join code for players on your own network: %s", lan.c_str());
}

// Packets read per pass. The rest stay in the socket for the next pass, so a flood cannot keep the
// network thread busy and starve its other work (sends, timeouts, the router lease).
constexpr int kMaxReadsPerPass = 256;

void receiveAll() {
    for (int reads = 0; reads < kMaxReadsPerPass; ++reads) {
        char buf[proto::kMaxPacket + 64];
        sockaddr_in from{};
        int fromLen = sizeof(from);
        const int n = recvfrom(g_sock, buf, sizeof(buf), 0, reinterpret_cast<sockaddr*>(&from), &fromLen);
        if (n == SOCKET_ERROR) {
            const int e = WSAGetLastError();
            if (e == WSAECONNRESET) continue; // ICMP "port unreachable" from an earlier send
            if (e != WSAEWOULDBLOCK) MLOG("net: recvfrom error %d", e);
            return;
        }
        if (handleStun(reinterpret_cast<const unsigned char*>(buf), n)) continue;
        handlePacket(buf, n, from);
    }
}

// The system timer ticks every 15.6 ms unless a program asks for finer, and select() waits in ticks, so
// the send loop below would wake only 21 times a second. A finer tick is asked for while the thread
// runs (winmm is loaded by hand: no import library to link).
struct TimerResolution {
    using Fn = UINT(WINAPI*)(UINT);
    HMODULE lib = nullptr;
    Fn end = nullptr;
    TimerResolution() {
        lib = LoadLibraryW(L"winmm.dll");
        if (!lib) return;
        const auto begin = reinterpret_cast<Fn>(reinterpret_cast<void*>(GetProcAddress(lib, "timeBeginPeriod")));
        end = reinterpret_cast<Fn>(reinterpret_cast<void*>(GetProcAddress(lib, "timeEndPeriod")));
        if (begin && end) begin(1);
        else end = nullptr;
    }
    ~TimerResolution() {
        if (end) end(1);
        if (lib) FreeLibrary(lib);
    }
};

void threadMain() {
    TimerResolution timerResolution;
    const DWORD started = GetTickCount();
    DWORD lastHello = 0, lastStats = started, lastHousekeeping = started, lastJoinWarn = 0, lastLease = started;
    // Our own state goes out at 30 Hz: from a deadline on the high-resolution clock (not the 15.6 ms
    // GetTickCount), and the wait below ends at the deadline.
    constexpr double kSendMs = 1000.0 / 30.0;
    double nextSend = nowMs();
    while (g_run) {
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(g_sock, &rd);
        const double untilSend = nextSend - nowMs();
        timeval tv{0, untilSend < 10.0 ? static_cast<long>(std::max(0.0, untilSend) * 1000.0) : 10000};
        if (select(0, &rd, nullptr, nullptr, &tv) > 0) receiveAll();
        const DWORD now = GetTickCount();
        const double t = nowMs();
        if (t >= nextSend) {
            nextSend += kSendMs;
            if (t - nextSend > kSendMs) nextSend = t + kSendMs; // far behind (the PC stalled): no burst to catch up
            sendState();
            sendPendingChat();
            sendPendingBursts();
            sendActivity();
        }
        if (now - lastHello >= 1000) { lastHello = now; sendHello(); }
        if (now - lastStats >= 30000) { lastStats = now; MLOG("net: %s", describe().c_str()); }
        if (now - lastHousekeeping >= 1000) {
            lastHousekeeping = now;
            std::lock_guard<std::mutex> lock(g_m);
            for (auto it = g_remotes.begin(); it != g_remotes.end();) {
                if (now - it->second.lastHeard > kRemoteForgetMs) it = g_remotes.erase(it);
                else ++it;
            }
            if (g_role == Role::Host) {
                for (size_t i = 0; i < g_members.size();) {
                    if (now - g_members[i].lastSeen < 15000) { ++i; continue; }
                    if (peerLogAllowed()) MLOG("net: %s left", addrToString(g_members[i].addr).c_str());
                    g_reserved.push_back({g_members[i].session, g_members[i].addr, now + kSessionGraceMs});
                    for (size_t k = 0; k < g_sendTo.size(); ++k)
                        if (sameAddr(g_sendTo[k], g_members[i].addr)) { g_sendTo.erase(g_sendTo.begin() + static_cast<long>(k)); break; }
                    g_members.erase(g_members.begin() + static_cast<long>(i));
                }
                g_reserved.erase(std::remove_if(g_reserved.begin(), g_reserved.end(),
                                                [now](const Reserved& r) { return static_cast<int32_t>(now - r.until) >= 0; }),
                                 g_reserved.end());
            } else if (!g_heardFromHost && now - started > 10000 && now - lastJoinWarn > 20000) {
                lastJoinWarn = now;
                MLOG("net: no answer from the host %s yet. Check the join code, and that the host is in game.",
                     addrToString(g_hostAddr).c_str());
            }
        }
        if (g_role == Role::Host && now - lastLease > 30 * 60 * 1000) { // renew the router mapping
            lastLease = now;
            const uint16_t port = g_port;
            std::thread([port] { routerMap(port, nullptr); }).detach();
        }
    }
}
// Starting, stopping and switching sessions are serialised: start() can now run on a helper thread
// (startAsync), while the menu's Leave and the game's exit run on others.
std::mutex g_startM;
std::atomic<uint32_t> g_startRequest{0}; // bumped by each startAsync, leave() and shutdown(): only the latest request starts
std::atomic<bool> g_starting{false};     // a startAsync is still looking up its address
std::atomic<bool> g_closed{false};       // the game is exiting: no session starts any more

void stopLocked() { // caller holds g_startM
    if (!g_run.exchange(false)) return;
    // The router mapping stays until the next start() (re-hosting reuses it) or shutdown().
    if (g_thread.joinable()) g_thread.join();
    if (g_sock != INVALID_SOCKET) closesocket(g_sock);
    g_sock = INVALID_SOCKET;
    WSACleanup();
}

bool startLocked(uint16_t port, const std::string& join, float loopbackOffset, const std::string& name) { // caller holds g_startM
    if (g_run) stopLocked();
    ++g_generation;
    g_routerWanted = join.empty() ? port : 0;
    std::thread(routerUnmap).detach(); // an old mapping this session does not use (blocks a few seconds)
    {
        std::lock_guard<std::mutex> lock(g_m);
        g_remotes.clear();
        g_audio.clear();
        g_sendTo.clear();
        g_members.clear();
        g_reserved.clear();
        g_heardFromHost = false;
        g_hostSession = 0;
        g_serverName.clear();
        g_joinCode.clear();
        g_lanCode.clear();
        g_name = name;
        g_outBurst.clear();
        for (PingCell& c : g_myPings) c = PingCell{};
        g_summons.clear();
        g_mySummon = MySummon{};
        g_lastSummonSent = 0;
        g_summonReports.clear();
        g_hostAllowsMembers = false;
    }
    g_stunIp = 0;
    {
        std::lock_guard<std::mutex> lock(g_stunM);
        g_stunPending = false;
    }
    g_port = port;
    g_loopbackOffset = loopbackOffset;
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { MLOG("net: WSAStartup failed"); return false; }
    g_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_sock == INVALID_SOCKET) {
        MLOG("net: socket() failed: %d", WSAGetLastError());
        WSACleanup();
        return false;
    }
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_port = htons(g_port);
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    bool bound = bind(g_sock, reinterpret_cast<sockaddr*>(&local), sizeof(local)) != SOCKET_ERROR;
    int bindError = bound ? 0 : WSAGetLastError();
    if (!bound && !join.empty() && bindError == WSAEADDRINUSE) {
        // A joiner only talks to its host, which answers whatever port the packets come from, so any free
        // port works. The usual reason the configured one is taken: mecmp_server (or a second game) runs
        // on this PC with the same port.
        local.sin_port = 0;
        bound = bind(g_sock, reinterpret_cast<sockaddr*>(&local), sizeof(local)) != SOCKET_ERROR;
        if (bound) {
            sockaddr_in got{};
            int len = sizeof(got);
            const uint16_t wanted = g_port;
            if (getsockname(g_sock, reinterpret_cast<sockaddr*>(&got), &len) == 0) g_port = ntohs(got.sin_port);
            MLOG("net: UDP port %u is in use by another program (mecmp_server?); joining from port %u instead",
                 wanted, g_port);
        } else {
            bindError = WSAGetLastError();
        }
    }
    if (!bound) {
        if (bindError == WSAEADDRINUSE)
            MLOG("net: cannot use UDP port %u: another program uses it (a mecmp_server or a second game on this "
                 "PC?). To host here, stop it or set port= in mecmp.ini to another number (and forward that one)",
                 g_port);
        else
            MLOG("net: cannot use UDP port %u (error %d)", g_port, bindError);
        closesocket(g_sock);
        g_sock = INVALID_SOCKET;
        WSACleanup();
        return false;
    }
    u_long nb = 1;
    ioctlsocket(g_sock, FIONBIO, &nb);
    g_session = GetTickCount() ^ (GetCurrentProcessId() << 16) ^ 0x5a17c0deu;
    {
        std::random_device rd;
        for (uint8_t& b : g_cookieKey) b = static_cast<uint8_t>(rd());
    }
    if (join.empty()) {
        g_role = Role::Host;
        MLOG("net: hosting on UDP %u; setting up the router and the join code...", g_port);
    } else {
        g_role = Role::Join;
        uint32_t ip = 0;
        uint16_t p = 0;
        sockaddr_in a{};
        if (joincode::decode(join, ip, p)) {
            a.sin_family = AF_INET;
            a.sin_addr.s_addr = htonl(ip);
            a.sin_port = htons(p);
        } else if (!parsePeer(join, a)) {
            MLOG("net: '%s' is not a join code or address; check join= in mecmp.ini", join.c_str());
            closesocket(g_sock);
            g_sock = INVALID_SOCKET;
            WSACleanup();
            return false;
        }
        g_hostAddr = a;
        g_sendTo.push_back(a);
        MLOG("net: joining the host at %s", addrToString(a).c_str());
    }
    g_run = true;
    g_thread = std::thread(threadMain);
    if (g_role == Role::Host) std::thread(hostSetup, g_generation.load()).detach();
    return true;
}

} // namespace

bool start(uint16_t port, const std::string& join, float loopbackOffset, const std::string& name) {
    std::lock_guard<std::mutex> lock(g_startM);
    if (g_closed) return false;
    return startLocked(port, join, loopbackOffset, name);
}

void startAsync(uint16_t port, const std::string& join, float loopbackOffset, const std::string& name) {
    const uint32_t request = ++g_startRequest;
    g_starting = true;
    std::thread([=] {
        // A server's name is looked up here, not on the caller's thread: a slow or unreachable DNS server can
        // take seconds, and the menu runs on the game's render thread. The session then starts with the address.
        std::string target = join;
        uint32_t ip = 0;
        uint16_t p = 0;
        if (!join.empty() && !joincode::decode(join, ip, p)) {
            WSADATA wsa;
            const bool wsaUp = WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
            sockaddr_in a{};
            const bool found = wsaUp && parsePeer(join, a);
            if (wsaUp) WSACleanup();
            if (!found) {
                if (request == g_startRequest) {
                    MLOG("net: '%s' is not a join code or address, or the name could not be looked up", join.c_str());
                    g_starting = false;
                }
                return;
            }
            char text[64] = {};
            inet_ntop(AF_INET, &a.sin_addr, text, sizeof(text));
            target = std::string(text) + ":" + std::to_string(ntohs(a.sin_port));
            if (target != join) VLOG("net: %s is %s", join.c_str(), target.c_str());
        }
        std::lock_guard<std::mutex> lock(g_startM);
        if (request != g_startRequest || g_closed) return; // a newer start, a Leave or the game's exit came meanwhile
        startLocked(port, target, loopbackOffset, name);
        if (request == g_startRequest) g_starting = false; // not when a newer request began meanwhile
    }).detach();
}

bool starting() { return g_starting.load(); }

void stop() {
    std::lock_guard<std::mutex> lock(g_startM);
    stopLocked();
}

void leave() {
    ++g_startRequest; // a session still being looked up does not start after all
    g_starting = false;
    std::lock_guard<std::mutex> lock(g_startM);
    if (!g_run) return;
    stopLocked();
    g_routerWanted = 0;
    std::thread(routerUnmap).detach();
    MLOG("net: left the session; offline");
}

void shutdown(uint32_t routerWaitMs) {
    ++g_startRequest;
    g_closed = true;
    {
        std::lock_guard<std::mutex> lock(g_startM);
        stopLocked();
    }
    g_routerWanted = 0;
    // Closing the router port needs the network; give it a bounded time, the game is exiting.
    HANDLE t = CreateThread(nullptr, 0, [](LPVOID) -> DWORD { routerUnmap(); return 0; }, nullptr, 0, nullptr);
    if (t) {
        if (WaitForSingleObject(t, routerWaitMs) != WAIT_OBJECT_0)
            MLOG("net: the router did not answer in time; its port mapping expires within an hour");
        CloseHandle(t);
    }
}

void publishLocal(const PlayerState& s) {
    std::lock_guard<std::mutex> lock(g_m);
    g_local = s;
    if (!limitPose(g_local)) { g_haveLocal = false; return; }
    g_haveLocal = true;
    g_localAt = GetTickCount();
}

void publishAudio(PlayerAudio sample) {
    std::lock_guard<std::mutex> lock(g_m);
    if (!g_run || g_outBurst.size() >= 128) return;
    sample.id = nextBurstId();
    char buf[proto::kMaxPacket];
    const size_t n = proto::writeAudio(buf, proto::Header{}, sample);
    if (n) g_outBurst.push_back({proto::kTypeAudio, std::vector<char>(buf + sizeof(proto::Header), buf + n), 2});
}
std::vector<RemoteAudio> takeAudio() {
    std::lock_guard<std::mutex> lock(g_m);
    std::vector<RemoteAudio> out(g_audio.begin(), g_audio.end());
    g_audio.clear(); return out;
}

bool localState(PlayerState& out) {
    std::lock_guard<std::mutex> lock(g_m);
    if (!g_haveLocal || GetTickCount() - g_localAt > 1000) return false;
    out = g_local;
    return true;
}

void sendPing(const float pos[3], const PingStyle& style, int limit) {
    if (!proto::sanePosition(pos)) return;
    std::lock_guard<std::mutex> lock(g_m);
    const DWORD now = GetTickCount();
    proto::Ping p;
    p.id = nextBurstId();
    memcpy(p.pos, pos, sizeof(p.pos));
    memcpy(p.rgb, style.rgb, sizeof(p.rgb));
    memcpy(p.accent, style.accent, sizeof(p.accent));
    p.flags = proto::withPingGlyph(0, style.glyph);
    p.label = proto::cleanPingLabel(style.label);
    p.life = proto::pingLifeUnits(style.seconds);
    proto::PingCellView cells[proto::kPingCells];
    for (int i = 0; i < proto::kPingCells; ++i) cells[i] = pingView(g_myPings[i], now);
    p.index = static_cast<uint8_t>(proto::choosePingCell(cells, limit, proto::pingGlyph(p.flags), p.label, p.pos));
    putPing(g_myPings[p.index], p, now, ++g_pingOrder);
    if (g_run) queuePing(p);
}

void sendReact(uint32_t target, uint8_t cell, const std::string& markerTitle, const std::string& text, uint8_t glyph,
               const uint8_t rgb[3], const uint8_t accent[3]) {
    if (cell >= proto::kPingCells) return;
    proto::React re;
    re.target = target;
    re.cell = cell;
    re.title = proto::cleanPingLabel(markerTitle);
    re.text = proto::cleanPingLabel(text);
    re.glyph = glyph;
    memcpy(re.rgb, rgb, sizeof(re.rgb));
    memcpy(re.accent, accent, sizeof(re.accent));
    if (re.text.empty()) return;
    std::lock_guard<std::mutex> lock(g_m);
    const DWORD now = GetTickCount();
    if (now - g_lastReactSent < 300) return; // leaning on the key does not flood the owner
    g_lastReactSent = now;
    // Under the marker here at once; the others get it with the packet.
    if (PingCell* c = cellOf(target, cell); c && c->live(now)) {
        ReactionMark mark;
        mark.from = g_session;
        mark.name = g_name.empty() ? std::string("Runner") : g_name;
        mark.text = re.text;
        mark.glyph = re.glyph;
        memcpy(mark.rgb, re.rgb, sizeof(mark.rgb));
        memcpy(mark.accent, re.accent, sizeof(mark.accent));
        addMark(*c, std::move(mark));
    }
    re.id = nextBurstId();
    if (!g_run) return;
    char buf[proto::kMaxPacket];
    const size_t n = proto::writeReact(buf, proto::Header{}, re);
    g_outBurst.push_back({proto::kTypeReact, std::vector<char>(buf + sizeof(proto::Header), buf + n), kChatSends});
}

void sendResult(const proto::Result& result) {
    std::lock_guard<std::mutex> lock(g_m);
    if (!g_run) return;
    proto::Result r = result;
    r.id = nextBurstId();
    char buf[proto::kMaxPacket];
    const size_t n = proto::writeResult(buf, proto::Header{}, r);
    g_outBurst.push_back({proto::kTypeResult, std::vector<char>(buf + sizeof(proto::Header), buf + n), kChatSends});
}

void setActivity(uint8_t kind, uint32_t startedTick, const std::string& title) {
    std::lock_guard<std::mutex> lock(g_m);
    MyActivity& m = g_myActivity;
    if (kind == proto::kActivityNone) {
        if (m.kind != proto::kActivityNone) m.endSends = 3;
        m.kind = kind;
        return;
    }
    m.kind = kind;
    m.started = startedTick;
    m.title = title;
    m.endSends = 0;
}

bool inRun() {
    std::lock_guard<std::mutex> lock(g_m);
    return g_myActivity.kind != proto::kActivityNone;
}

void setTyping(bool typing) { g_typing = typing; }

std::vector<std::string> takeResults() {
    std::lock_guard<std::mutex> lock(g_m);
    std::vector<std::string> out(g_results.begin(), g_results.end());
    g_results.clear();
    return out;
}

std::vector<ReactionInfo> takeReactions() {
    std::lock_guard<std::mutex> lock(g_m);
    std::vector<ReactionInfo> out(g_reactions.begin(), g_reactions.end());
    g_reactions.clear();
    return out;
}

bool canHardSummon() {
    if (!g_run) return false;
    if (g_role == Role::Host) return true;
    std::lock_guard<std::mutex> lock(g_m);
    return g_hostSession.load() != 0 && g_serverName.empty() && g_hostAllowsMembers.load();
}

bool sendSummon(const float pos[3], float yaw, const std::vector<uint32_t>& targets, bool hard) {
    if (!proto::sanePosition(pos) || !std::isfinite(yaw) || !g_run) return false;
    const bool canHard = canHardSummon(); // before g_m: it takes the lock itself
    std::lock_guard<std::mutex> lock(g_m);
    const DWORD now = GetTickCount();
    if (g_lastSummonSent && now - g_lastSummonSent < summon::kSendCooldownMs) return false;
    proto::Summon s;
    s.id = nextBurstId();
    memcpy(s.pos, pos, sizeof(s.pos));
    s.yaw = yaw;
    s.flags = hard && canHard ? proto::kSummonHard : 0;
    MySummon m;
    m.open = true;
    m.id = s.id;
    m.at = now;
    if (targets.empty()) {
        for (const auto& kv : g_remotes) {
            const Remote& r = kv.second;
            if (r.fill && !r.loopback && now - r.lastTick <= 5000) {
                ++m.tally.expected;
                m.tally.singleName = r.name;
            }
        }
    } else {
        for (uint32_t t : targets) {
            if (static_cast<int>(m.targets.size()) >= proto::kMaxSummonTargets || t == g_session) continue;
            if (std::find(m.targets.begin(), m.targets.end(), t) != m.targets.end()) continue;
            m.targets.push_back(t);
        }
        m.tally.expected = static_cast<int>(m.targets.size());
        if (m.targets.size() == 1) {
            auto it = g_remotes.find(m.targets[0]);
            if (it != g_remotes.end()) m.tally.singleName = it->second.name;
        }
    }
    if (m.tally.expected == 0) return false; // nobody to call
    s.targetCount = static_cast<uint8_t>(m.targets.size());
    for (size_t i = 0; i < m.targets.size(); ++i) s.targets[i] = m.targets[i];
    char buf[proto::kMaxPacket];
    const size_t n = proto::writeSummon(buf, proto::Header{}, s);
    g_outBurst.push_back({proto::kTypeSummon, std::vector<char>(buf + sizeof(proto::Header), buf + n), kChatSends});
    g_mySummon = m;
    g_lastSummonSent = now;
    return true;
}

bool summonOpen() {
    std::lock_guard<std::mutex> lock(g_m);
    return g_mySummon.open;
}

uint32_t summonCooldownLeftMs() {
    std::lock_guard<std::mutex> lock(g_m);
    if (!g_lastSummonSent) return 0;
    const DWORD since = GetTickCount() - g_lastSummonSent;
    return since >= summon::kSendCooldownMs ? 0 : summon::kSendCooldownMs - since;
}

void cancelSummon() {
    std::lock_guard<std::mutex> lock(g_m);
    if (!g_mySummon.open) return;
    proto::Summon s;
    s.id = g_mySummon.id; // the summon withdrawn
    s.flags = proto::kSummonCancel;
    // The cooldown stays: receivers would drop a new call this soon anyway (summon::kMinGapMs).
    g_mySummon.open = false;
    char buf[proto::kMaxPacket];
    const size_t n = proto::writeSummon(buf, proto::Header{}, s);
    g_outBurst.push_back({proto::kTypeSummon, std::vector<char>(buf + sizeof(proto::Header), buf + n), kChatSends});
}

std::vector<SummonInfo> takeSummons() {
    std::lock_guard<std::mutex> lock(g_m);
    std::vector<SummonInfo> out(g_summons.begin(), g_summons.end());
    g_summons.clear();
    return out;
}

void sendSummonReply(uint32_t id, uint32_t to, uint8_t result) {
    proto::SummonReply re;
    re.id = id;
    re.target = to;
    re.result = result;
    std::lock_guard<std::mutex> lock(g_m);
    if (!g_run) return;
    char buf[proto::kMaxPacket];
    const size_t n = proto::writeSummonReply(buf, proto::Header{}, re);
    g_outBurst.push_back({proto::kTypeSummonReply, std::vector<char>(buf + sizeof(proto::Header), buf + n), kChatSends});
}

uint32_t sendVisit(uint32_t target, uint8_t kind, uint32_t id) {
    std::lock_guard<std::mutex> lock(g_m);
    if (!g_run) return 0;
    proto::Visit v;
    v.id = id ? id : nextBurstId();
    v.target = target;
    v.kind = kind;
    char buf[proto::kMaxPacket];
    const size_t n = proto::writeVisit(buf, proto::Header{}, v);
    g_outBurst.push_back({proto::kTypeVisit, std::vector<char>(buf + sizeof(proto::Header), buf + n), kChatSends});
    return v.id;
}

std::vector<VisitInfo> takeVisits() {
    std::lock_guard<std::mutex> lock(g_m);
    std::vector<VisitInfo> out(g_visits.begin(), g_visits.end());
    g_visits.clear();
    return out;
}

std::vector<std::string> takeSummonReports() {
    std::lock_guard<std::mutex> lock(g_m);
    if (g_mySummon.open && GetTickCount() - g_mySummon.at >= summon::kReportWindowMs) finishMySummon();
    std::vector<std::string> out;
    out.swap(g_summonReports);
    return out;
}

bool clearPing() {
    std::lock_guard<std::mutex> lock(g_m);
    const DWORD now = GetTickCount();
    bool any = false, live = false;
    for (PingCell& c : g_myPings) {
        any = any || c.have;
        live = live || c.live(now);
        c.have = false;
    }
    if (!any) return false;
    proto::Ping p;
    p.id = nextBurstId();
    p.flags = proto::kPingClear;
    p.index = proto::kPingAllCells;
    if (g_run) queuePing(p);
    return live;
}

namespace {
// Empties one of our cells and tells everyone. Caller holds g_m.
void removeMyPing(int index) {
    g_myPings[index].have = false;
    proto::Ping p;
    p.id = nextBurstId();
    p.flags = proto::kPingClear;
    p.index = static_cast<uint8_t>(index);
    if (g_run) queuePing(p);
}

// Sends one of our cells again after `edit` changed it; its time starts again. `fresh` = it counts
// as a new marker (it flashes). Caller holds g_m.
template <class Edit> bool resendMyPing(uint8_t index, Edit edit, bool fresh = true) {
    const DWORD now = GetTickCount();
    if (index >= proto::kPingCells || !g_myPings[index].live(now)) return false;
    PingCell& c = g_myPings[index];
    edit(c.ping);
    c.ping.id = nextBurstId();
    c.setAt = now;
    if (fresh) c.arrivedAt = now;
    c.order = ++g_pingOrder;
    if (g_run) queuePing(c.ping);
    return true;
}
} // namespace

void removePing(uint8_t index) {
    std::lock_guard<std::mutex> lock(g_m);
    if (index < proto::kPingCells && g_myPings[index].live(GetTickCount())) removeMyPing(index);
}

bool movePing(uint8_t index, const float pos[3]) {
    if (!proto::sanePosition(pos)) return false;
    std::lock_guard<std::mutex> lock(g_m);
    return resendMyPing(index, [pos](proto::Ping& p) { memcpy(p.pos, pos, sizeof(p.pos)); });
}

bool renamePing(uint8_t index, const std::string& label) {
    const std::string clean = proto::cleanPingLabel(label);
    std::lock_guard<std::mutex> lock(g_m);
    return resendMyPing(index, [&clean](proto::Ping& p) { p.label = clean; });
}

bool renewPing(uint8_t index) {
    std::lock_guard<std::mutex> lock(g_m);
    return resendMyPing(index, [](proto::Ping&) {}, false);
}

void trimPings(int limit) {
    std::lock_guard<std::mutex> lock(g_m);
    const DWORD now = GetTickCount();
    for (;;) {
        int live = 0, oldest = -1;
        for (int i = 0; i < proto::kPingCells; ++i) {
            if (!g_myPings[i].live(now)) continue;
            ++live;
            if (oldest < 0 || g_myPings[i].order < g_myPings[oldest].order) oldest = i;
        }
        if (live <= (limit < 1 ? 1 : limit) || oldest < 0) return;
        removeMyPing(oldest);
    }
}

std::vector<PingInfo> pings() {
    std::vector<PingInfo> v;
    const DWORD now = GetTickCount();
    std::lock_guard<std::mutex> lock(g_m);
    auto add = [&](uint32_t owner, const std::string& name, const PingCell* cells, bool own) {
        for (int k = 0; k < proto::kPingCells; ++k) {
            const PingCell& c = cells[k];
            if (!c.live(now)) continue;
            const proto::Ping& p = c.ping;
            PingInfo i;
            i.owner = owner;
            i.index = static_cast<uint8_t>(k);
            i.name = name.empty() ? std::string("Runner") : name;
            memcpy(i.pos, p.pos, sizeof(i.pos));
            memcpy(i.rgb, p.rgb, sizeof(i.rgb));
            memcpy(i.accent, p.accent, sizeof(i.accent));
            i.glyph = proto::pingGlyph(p.flags);
            i.label = p.label;
            i.ageMs = now - c.arrivedAt;
            i.lifeMs = proto::pingLifeMs(p.life);
            i.leftMs = i.lifeMs - (now - c.setAt);
            i.own = own;
            i.reactions = c.reactions;
            v.push_back(i);
        }
    };
    add(g_session, g_name, g_myPings, true);
    for (const auto& kv : g_remotes) add(kv.second.id, kv.second.name, kv.second.pings, false);
    return v;
}

void sendChat(const std::string& raw) {
    const std::string text = proto::cleanChatText(raw);
    if (text.empty()) return;
    std::lock_guard<std::mutex> lock(g_m);
    addChatLine({g_name.empty() ? std::string("Runner") : g_name, text, static_cast<uint32_t>(GetTickCount()), true});
    if (g_run) g_outChat.push_back({(g_session << 16) ^ ++g_chatId, text, kChatSends});
}

std::vector<ChatLine> chatLines(size_t max) {
    std::lock_guard<std::mutex> lock(g_m);
    const size_t from = g_chat.size() > max ? g_chat.size() - max : 0;
    return std::vector<ChatLine>(g_chat.begin() + static_cast<long>(from), g_chat.end());
}

std::vector<RemoteInfo> remotes(uint32_t maxAgeMs) {
    std::vector<RemoteInfo> v;
    const DWORD now = GetTickCount();
    std::lock_guard<std::mutex> lock(g_m);
    for (const auto& kv : g_remotes) {
        const Remote& r = kv.second;
        if (!r.fill || now - r.lastTick > maxAgeMs) continue;
        RemoteInfo info{r.id, r.name, r.loopback, now - r.lastTick, r.loss.percent(nowMs()), r.look, r.outfit, r.profile.id,
                         r.profile.name};
        info.typing = (r.stateFlags & kStateTyping) != 0;
        info.asksVisit = r.asksVisit;
        if (r.activity.kind != proto::kActivityNone && r.activityAt && now - r.activityAt < kActivityForgetMs) {
            info.runKind = r.activity.kind;
            info.runMs = r.activity.elapsedMs + (now - r.activityAt);
            info.runTitle = r.activity.title;
        }
        v.push_back(std::move(info));
    }
    return v;
}

bool remoteState(uint32_t id, PlayerState& out, uint32_t& ageMs) {
    std::lock_guard<std::mutex> lock(g_m);
    auto it = g_remotes.find(id);
    if (it == g_remotes.end() || !it->second.fill) return false;
    const Remote& r = it->second;
    ageMs = GetTickCount() - r.lastTick;
    // Find the two snapshots around (now - delay), newest first, and blend them.
    const double t = nowMs() - r.minDelta - kInterpDelayMs; // on the sender's clock
    const Snap* newer = nullptr;
    for (int k = 0; k < r.fill; ++k) {
        const Snap& sn = r.snaps[(r.head - 1 - k + 2 * kSnapCount) % kSnapCount];
        if (sn.t <= t) {
            if (!newer) { out = sn.s; return true; } // nothing newer arrived: hold the newest
            const double span = newer->t - sn.t;
            const float f = span > 0.001 ? static_cast<float>((t - sn.t) / span) : 1.f;
            blend(sn.s, newer->s, f < 0 ? 0 : (f > 1 ? 1 : f), out);
            return true;
        }
        newer = &sn;
    }
    out = newer->s; // everything is newer than the render time: show the oldest
    return true;
}

std::string joinCode() {
    std::lock_guard<std::mutex> lock(g_m);
    return g_joinCode;
}

bool hosting() { return g_role == Role::Host; }
std::string serverName() {
    std::lock_guard<std::mutex> lock(g_m);
    return g_role == Role::Join ? g_serverName : std::string();
}
uint32_t sessionKey() {
    if (!g_run) return 0;
    return g_role == Role::Host ? g_session : g_hostSession.load();
}
bool running() { return g_run.load(); }
bool connectedToHost() { return g_heardFromHost; }
std::string lanCode() {
    std::lock_guard<std::mutex> lock(g_m);
    return g_lanCode;
}
void setOnlineProfile(uint64_t id, const char* name) {
    std::lock_guard<std::mutex> lock(g_m);
    g_profile = proto::OnlineProfile{};
    if (!id || !name) return;
    g_profile.id = id;
    for (size_t i = 0; i + 1 < proto::kProfileLen && name[i]; ++i) g_profile.name[i] = name[i];
}

void setLook(uint32_t look, uint8_t outfit) {
    g_look = look;
    g_outfit = look ? 0 : outfit;
}

void setName(const std::string& name) {
    std::lock_guard<std::mutex> lock(g_m);
    g_name = name;
}

std::string describe() {
    size_t peers = 0;
    std::string code;
    {
        std::lock_guard<std::mutex> lock(g_m);
        peers = g_role == Role::Host ? g_members.size() : g_sendTo.size();
        code = g_joinCode;
    }
    const uint64_t sent = g_sent.load();
    char buf[320];
    if (g_role == Role::Host)
        snprintf(buf, sizeof(buf), "hosting on port %u (code %s), %llu player(s) joined, sent %llu (avg %llu bytes), received %llu, forwarded %llu, dropped %llu",
                 g_port, code.empty() ? "-" : code.c_str(), static_cast<unsigned long long>(peers), static_cast<unsigned long long>(sent),
                 static_cast<unsigned long long>(sent ? g_bytesSent.load() / sent : 0),
                 static_cast<unsigned long long>(g_recv.load()), static_cast<unsigned long long>(g_relayed.load()),
                 static_cast<unsigned long long>(g_dropped.load()));
    else
        snprintf(buf, sizeof(buf), "joined %s (%s), sent %llu (avg %llu bytes), received %llu, dropped %llu",
                 addrToString(g_hostAddr).c_str(), g_heardFromHost ? "connected" : "no answer yet",
                 static_cast<unsigned long long>(sent), static_cast<unsigned long long>(sent ? g_bytesSent.load() / sent : 0),
                 static_cast<unsigned long long>(g_recv.load()), static_cast<unsigned long long>(g_dropped.load()));
    return buf;
}

} // namespace mecmp::net
