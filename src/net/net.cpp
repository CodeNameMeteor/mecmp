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
#include <thread>

#include "core/log.h"
#include "net/protocol.h"

namespace mecmp::net {
namespace {
namespace proto = protocol;

constexpr int kSnapCount = 16;
constexpr double kInterpDelayMs = 100.0; // remote players are shown this far in the past
constexpr size_t kMaxPeers = 16;

double nowMs() {
    static const LARGE_INTEGER f = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return x; }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) * 1000.0 / static_cast<double>(f.QuadPart);
}

struct Snap { PlayerState s; double t = 0; };
struct Remote {
    uint32_t id = 0;
    sockaddr_in addr{};
    std::string name;
    bool loopback = false;
    uint32_t lastSeq = 0;
    bool haveSeq = false;
    DWORD lastTick = 0;
    proto::Decoder decoder;
    Snap snaps[kSnapCount];
    int head = 0, fill = 0;
};

std::mutex g_m;
std::map<uint32_t, Remote> g_remotes;     // by session id
std::vector<sockaddr_in> g_sendTo;        // configured peer + whoever contacted us
PlayerState g_local;
bool g_haveLocal = false;
std::string g_name;

std::atomic<bool> g_run{false};
std::thread g_thread;
SOCKET g_sock = INVALID_SOCKET;
uint32_t g_session = 0;
float g_loopbackOffset = 3.0f;
uint16_t g_port = 47800;
std::atomic<uint64_t> g_sent{0}, g_recv{0}, g_dropped{0}, g_bytesSent{0};

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
    sendAll(buf, proto::writeHello(buf, header(proto::kTypeHello), g_name.c_str()));
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
    const bool own = h.session == g_session;
    std::lock_guard<std::mutex> lock(g_m);
    if (!own) { // reply to whoever talks to us
        bool known = false;
        for (const auto& a : g_sendTo) known = known || sameAddr(a, from);
        if (!known && g_sendTo.size() < kMaxPeers) {
            g_sendTo.push_back(from);
            MLOG("net: %s contacted us; sending our state there too", addrToString(from).c_str());
        }
    }
    Remote& r = g_remotes[h.session];
    if (!r.id) {
        r.id = h.session;
        r.addr = from;
        r.loopback = own;
        MLOG("net: new player %08x from %s%s", h.session, addrToString(from).c_str(), own ? " (our own packets: solo test)" : "");
    }
    // Sequence numbers restart when a player restarts the game: accept a large backwards jump.
    if (r.haveSeq && h.seq <= r.lastSeq && r.lastSeq - h.seq < 1000) return;
    r.lastSeq = h.seq;
    r.haveSeq = true;
    ++g_recv;
    const char* payload = buf + sizeof(h);
    const size_t len = static_cast<size_t>(n) - sizeof(h);
    if (h.type == proto::kTypeHello) {
        char name[proto::kNameLen + 1] = {};
        memcpy(name, payload, len < proto::kNameLen ? len : proto::kNameLen);
        if (r.name != name) {
            r.name = name;
            MLOG("net: player %08x is '%s'", r.id, name);
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
    r.snaps[r.head] = {s, nowMs()};
    r.head = (r.head + 1) % kSnapCount;
    if (r.fill < kSnapCount) ++r.fill;
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
        handlePacket(buf, n, from);
    }
}

void threadMain() {
    DWORD lastSend = 0, lastHello = 0, lastStats = GetTickCount();
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
    }
}
} // namespace

bool start(uint16_t port, const std::string& peer, float loopbackOffset, const std::string& name) {
    g_port = port;
    g_loopbackOffset = loopbackOffset;
    g_name = name;
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { MLOG("net: WSAStartup failed"); return false; }
    g_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_sock == INVALID_SOCKET) { MLOG("net: socket() failed: %d", WSAGetLastError()); return false; }
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_port = htons(g_port);
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(g_sock, reinterpret_cast<sockaddr*>(&local), sizeof(local)) == SOCKET_ERROR) {
        MLOG("net: cannot bind UDP port %u (error %d); is another copy running?", g_port, WSAGetLastError());
        closesocket(g_sock);
        g_sock = INVALID_SOCKET;
        return false;
    }
    u_long nb = 1;
    ioctlsocket(g_sock, FIONBIO, &nb);
    g_session = GetTickCount() ^ (GetCurrentProcessId() << 16) ^ 0x5a17c0deu;
    sockaddr_in p{};
    if (!peer.empty() && parsePeer(peer, p)) {
        g_sendTo.push_back(p);
        MLOG("net: listening on UDP %u, sending to %s", g_port, addrToString(p).c_str());
    } else {
        if (!peer.empty()) MLOG("net: could not resolve peer '%s'", peer.c_str());
        MLOG("net: listening on UDP %u, waiting for other players to contact us", g_port);
    }
    g_run = true;
    g_thread = std::thread(threadMain);
    return true;
}

void stop() {
    if (!g_run.exchange(false)) return;
    if (g_thread.joinable()) g_thread.join();
    if (g_sock != INVALID_SOCKET) closesocket(g_sock);
    g_sock = INVALID_SOCKET;
    WSACleanup();
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
    const double t = nowMs() - kInterpDelayMs;
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

std::string describe() {
    size_t peers = 0;
    {
        std::lock_guard<std::mutex> lock(g_m);
        peers = g_sendTo.size();
    }
    const uint64_t sent = g_sent.load();
    char buf[256];
    snprintf(buf, sizeof(buf), "port %u, sending to %zu address(es), sent %llu (avg %llu bytes), received %llu, dropped %llu",
             g_port, peers, static_cast<unsigned long long>(sent),
             static_cast<unsigned long long>(sent ? g_bytesSent.load() / sent : 0),
             static_cast<unsigned long long>(g_recv.load()), static_cast<unsigned long long>(g_dropped.load()));
    return buf;
}

} // namespace mecmp::net
