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
#include <random>
#include <thread>
#include <cstdio>
#include <cstdlib>

#include "core/config.h"
#include "core/log.h"
#include "net/joincode.h"
#include "net/protocol.h"
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
struct Remote {
    uint32_t id = 0;
    sockaddr_in addr{};
    std::string name;
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
    uint32_t lastTime = 0;
    int nameLogs = 0;         // name changes logged for this player (at most kMaxNameLogs)
    proto::ClockUnwrap clock; // sender's timestamps without the 49-day wrap
    Snap snaps[kSnapCount];
    int head = 0, fill = 0;
};

// Session roles. HOST: accepts joiners and forwards every player's packets to all the others,
// so joiners only ever talk to the host (only the host needs a reachable port). JOIN: sends to
// the host only.
enum class Role { Host, Join };
Role g_role = Role::Host;

struct Member { sockaddr_in addr{}; DWORD lastSeen = 0; };

std::mutex g_m;
std::map<uint32_t, Remote> g_remotes;     // by session id
std::vector<sockaddr_in> g_sendTo;        // JOIN: the host. HOST: kept in sync with g_members
std::vector<Member> g_members;            // HOST: connected joiners
sockaddr_in g_hostAddr{};                 // JOIN
std::atomic<bool> g_heardFromHost{false};
std::string g_joinCode, g_lanCode;
std::atomic<uint32_t> g_generation{0}; // bumped on every start(); stale host setups don't publish
PlayerState g_local;
bool g_haveLocal = false;
std::string g_name;

std::atomic<bool> g_run{false};
std::thread g_thread;
SOCKET g_sock = INVALID_SOCKET;
uint32_t g_session = 0;
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

std::string addrToString(const sockaddr_in& a) {
    char ip[64] = {};
    inet_ntop(AF_INET, &a.sin_addr, ip, sizeof(ip));
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
    for (const auto& a : to)
        if (sendto(g_sock, buf, static_cast<int>(len), 0, reinterpret_cast<const sockaddr*>(&a), sizeof(a)) ==
            static_cast<int>(len)) {
            ++g_sent;
            g_bytesSent += len;
        }
}

proto::Header header(uint16_t type) {
    static uint32_t seq = 0;
    return proto::Header{proto::kMagic, proto::kProtocol, type, g_session, ++seq};
}

void sendState() {
    PlayerState s;
    {
        std::lock_guard<std::mutex> lock(g_m);
        if (!g_haveLocal || g_sendTo.empty()) return;
        s = g_local;
    }
    static proto::Encoder enc;
    char buf[proto::kMaxPacket];
    sendAll(buf, proto::writeState(buf, header(proto::kTypeState), s, enc));
}

void sendHello() {
    char buf[proto::kMaxPacket];
    std::string name;
    {
        std::lock_guard<std::mutex> lock(g_m);
        name = g_name;
    }
    sendAll(buf, proto::writeHello(buf, header(proto::kTypeHello), name.c_str()));
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
    // Only the packet types we know may create state or be forwarded to other players.
    if (h.type != proto::kTypeState && h.type != proto::kTypeHello) { ++g_dropped; return; }
    const bool own = h.session == g_session;
    std::lock_guard<std::mutex> lock(g_m);
    if (!own && g_role == Role::Host) {
        // A joiner: remember it, and forward its packet unchanged to everyone else.
        bool known = false;
        for (auto& m : g_members)
            if (sameAddr(m.addr, from)) { m.lastSeen = GetTickCount(); known = true; }
        if (!known) {
            if (g_members.size() >= kMaxPeers) {
                static DWORD lastFullWarn = 0;
                if (GetTickCount() - lastFullWarn > 10000) {
                    lastFullWarn = GetTickCount();
                    MLOG("net: session full (%llu players); not admitting %s", static_cast<unsigned long long>(g_members.size()),
                         addrToString(from).c_str());
                }
                return;
            }
            g_members.push_back({from, GetTickCount()});
            g_sendTo.push_back(from);
            if (peerLogAllowed())
                MLOG("net: %s joined (%llu player(s) connected)", addrToString(from).c_str(),
                     static_cast<unsigned long long>(g_members.size()));
        }
        for (const auto& m : g_members)
            if (!sameAddr(m.addr, from))
                if (sendto(g_sock, buf, n, 0, reinterpret_cast<const sockaddr*>(&m.addr), sizeof(m.addr)) == n) ++g_relayed;
    }
    // Joining: the host answered. A solo test joins itself, so its own packets coming back count too.
    if (g_role == Role::Join && sameAddr(from, g_hostAddr) && !g_heardFromHost) {
        g_heardFromHost = true;
        MLOG("net: connected to the host %s", addrToString(from).c_str());
    }
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
    r.lastSeq = h.seq;
    r.haveSeq = true;
    ++g_recv;
    const char* payload = buf + sizeof(h);
    const size_t len = static_cast<size_t>(n) - sizeof(h);
    if (h.type == proto::kTypeHello) {
        const std::string name = cleanName(payload, len < proto::kNameLen ? len : proto::kNameLen);
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
    if (h.type != proto::kTypeState) return;
    PlayerState s;
    bool full = false;
    if (!proto::readState(payload, len, r.decoder, s, full)) { ++g_dropped; return; }
    if (full) proto::commitBase(r.decoder, s);
    if (own) { // solo test: show the mirror to our right so it does not stand inside us
        s.pos[0] += std::cos(s.yaw) * g_loopbackOffset;
        s.pos[2] -= std::sin(s.yaw) * g_loopbackOffset;
    }
    r.lastTick = GetTickCount();
    if (r.fill && static_cast<int32_t>(s.timeMs - r.lastTime) <= 0) return; // same capture sent twice
    r.lastTime = s.timeMs;
    const double sent = r.clock.extend(s.timeMs);
    const double delta = nowMs() - sent;
    if (!r.haveDelta || delta < r.minDelta) { r.minDelta = delta; r.haveDelta = true; }
    else r.minDelta += 0.02; // let the estimate creep up slowly (clock drift, route changes)
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

void receiveAll() {
    for (;;) {
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

void threadMain() {
    const DWORD started = GetTickCount();
    DWORD lastSend = 0, lastHello = 0, lastStats = started, lastHousekeeping = started, lastJoinWarn = 0, lastLease = started;
    while (g_run) {
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(g_sock, &rd);
        timeval tv{0, 10000};
        if (select(0, &rd, nullptr, nullptr, &tv) > 0) receiveAll();
        const DWORD now = GetTickCount();
        if (now - lastSend >= 33) { lastSend = now; sendState(); }
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
                    for (size_t k = 0; k < g_sendTo.size(); ++k)
                        if (sameAddr(g_sendTo[k], g_members[i].addr)) { g_sendTo.erase(g_sendTo.begin() + static_cast<long>(k)); break; }
                    g_members.erase(g_members.begin() + static_cast<long>(i));
                }
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
} // namespace

bool start(uint16_t port, const std::string& join, float loopbackOffset, const std::string& name) {
    if (g_run) stop();
    ++g_generation;
    g_routerWanted = join.empty() ? port : 0;
    std::thread(routerUnmap).detach(); // an old mapping this session does not use (blocks a few seconds)
    {
        std::lock_guard<std::mutex> lock(g_m);
        g_remotes.clear();
        g_sendTo.clear();
        g_members.clear();
        g_heardFromHost = false;
        g_joinCode.clear();
        g_lanCode.clear();
        g_name = name;
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
    if (bind(g_sock, reinterpret_cast<sockaddr*>(&local), sizeof(local)) == SOCKET_ERROR) {
        MLOG("net: cannot use UDP port %u (error %d); is another copy running?", g_port, WSAGetLastError());
        closesocket(g_sock);
        g_sock = INVALID_SOCKET;
        WSACleanup();
        return false;
    }
    u_long nb = 1;
    ioctlsocket(g_sock, FIONBIO, &nb);
    g_session = GetTickCount() ^ (GetCurrentProcessId() << 16) ^ 0x5a17c0deu;
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

void stop() {
    if (!g_run.exchange(false)) return;
    // The router mapping stays until the next start() (re-hosting reuses it) or shutdown().
    if (g_thread.joinable()) g_thread.join();
    if (g_sock != INVALID_SOCKET) closesocket(g_sock);
    g_sock = INVALID_SOCKET;
    WSACleanup();
}

void shutdown(uint32_t routerWaitMs) {
    stop();
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
    g_haveLocal = true;
}

std::vector<RemoteInfo> remotes(uint32_t maxAgeMs) {
    std::vector<RemoteInfo> v;
    const DWORD now = GetTickCount();
    std::lock_guard<std::mutex> lock(g_m);
    for (const auto& kv : g_remotes) {
        const Remote& r = kv.second;
        if (!r.fill || now - r.lastTick > maxAgeMs) continue;
        v.push_back({r.id, r.name, addrToString(r.addr), r.loopback, now - r.lastTick});
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
bool running() { return g_run.load(); }
bool connectedToHost() { return g_heardFromHost; }
std::string lanCode() {
    std::lock_guard<std::mutex> lock(g_m);
    return g_lanCode;
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
