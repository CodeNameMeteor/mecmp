#include "net/serverlist.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <atomic>
#include <cstring>
#include <mutex>
#include <thread>

#include "core/log.h"
#include "net/protocol.h"

namespace mecmp::net::servers {
namespace {
namespace proto = protocol;

constexpr DWORD kAskEveryMs = 5000;
constexpr DWORD kAnswerKeepMs = 15000;
constexpr DWORD kResolveEveryMs = 60000;
constexpr DWORD kRetryMs = 30000; // a name that did not resolve is looked up again after this long

std::mutex g_m;
std::vector<std::string> g_wanted; // guarded by g_m
std::vector<Entry> g_entries;      // parallel to g_wanted; guarded by g_m
bool g_active = false;             // guarded by g_m
std::atomic<bool> g_run{false}, g_refresh{false};
std::thread g_thread;

struct Probe {
    std::string address;
    sockaddr_in addr{};
    bool resolved = false;
    DWORD resolvedAt = 0;
    bool tried = false; // a lookup has been made (resolved or not)
    DWORD triedAt = 0;
    uint32_t seq = 0;
    DWORD sentAt = 0, answeredAt = 0;
};

bool resolve(const std::string& text, sockaddr_in& out) {
    std::string host = text, port = "47800";
    const size_t colon = text.rfind(':');
    if (colon != std::string::npos) { host = text.substr(0, colon); port = text.substr(colon + 1); }
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* res = nullptr;
    if (getaddrinfo(host.c_str(), port.c_str(), &hints, &res) != 0 || !res) return false;
    memcpy(&out, res->ai_addr, sizeof(sockaddr_in));
    freeaddrinfo(res);
    return true;
}

void threadMain() {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return;
    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) { WSACleanup(); return; }
    sockaddr_in local{};
    local.sin_family = AF_INET; // any free port: the game's own port stays the session's
    bind(sock, reinterpret_cast<sockaddr*>(&local), sizeof(local));
    u_long nb = 1;
    ioctlsocket(sock, FIONBIO, &nb);
    std::vector<Probe> probes;
    uint32_t seq = GetTickCount();
    DWORD askedAt = 0;
    while (g_run) {
        std::vector<std::string> wanted;
        bool active = false;
        {
            std::lock_guard<std::mutex> lock(g_m);
            wanted = g_wanted;
            active = g_active;
        }
        if (!active) { Sleep(200); continue; }
        // Keep the probes in step with the list (a server's lookup and timing survive a reorder).
        std::vector<Probe> next;
        for (const auto& a : wanted) {
            Probe p;
            p.address = a;
            for (const auto& old : probes)
                if (old.address == a) p = old;
            next.push_back(p);
        }
        probes.swap(next);
        const DWORD now = GetTickCount();
        if (g_refresh.exchange(false) || !askedAt || now - askedAt >= kAskEveryMs) {
            askedAt = now;
            for (auto& p : probes) {
                // A lookup can block this thread for seconds, so a name that fails is not looked up at every ask:
                // the other servers' answers would wait behind it.
                const bool due = !p.tried || (p.resolved ? now - p.resolvedAt > kResolveEveryMs : now - p.triedAt >= kRetryMs);
                if (due) {
                    p.tried = true;
                    p.triedAt = GetTickCount();
                    p.resolved = resolve(p.address, p.addr);
                    p.resolvedAt = p.triedAt;
                }
                if (!p.resolved) continue;
                char buf[proto::kInfoRequestSize];
                p.seq = ++seq;
                const proto::Header h{proto::kMagic, proto::kProtocol, proto::kTypeInfo, 0, p.seq};
                const size_t n = proto::writeInfoRequest(buf, h);
                p.sentAt = GetTickCount();
                sendto(sock, buf, static_cast<int>(n), 0, reinterpret_cast<const sockaddr*>(&p.addr), sizeof(p.addr));
            }
        }
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(sock, &rd);
        timeval tv{0, 100000};
        if (select(0, &rd, nullptr, nullptr, &tv) > 0) {
            char buf[proto::kMaxPacket];
            sockaddr_in from{};
            int fromLen = sizeof(from);
            int n = 0;
            // At most 256 answers per pass: a server that floods answers must not hold up the menu's asking.
            for (int reads = 0; reads < 256 && (n = recvfrom(sock, buf, sizeof(buf), 0, reinterpret_cast<sockaddr*>(&from), &fromLen)) > 0; ++reads) {
                proto::Header h;
                proto::ServerInfo info;
                if (n < static_cast<int>(sizeof(h))) continue;
                memcpy(&h, buf, sizeof(h));
                if (h.magic != proto::kMagic || h.type != proto::kTypeInfo ||
                    !proto::readInfoAnswer(buf + sizeof(h), static_cast<size_t>(n) - sizeof(h), info))
                    continue;
                const DWORD at = GetTickCount();
                std::lock_guard<std::mutex> lock(g_m);
                for (auto& p : probes) {
                    if (!p.resolved || p.seq != h.seq || p.addr.sin_addr.s_addr != from.sin_addr.s_addr ||
                        p.addr.sin_port != from.sin_port)
                        continue;
                    p.answeredAt = at;
                    for (size_t i = 0; i < g_wanted.size() && i < g_entries.size(); ++i) {
                        if (g_wanted[i] != p.address) continue;
                        Entry& e = g_entries[i];
                        e.answered = true;
                        e.name = info.name;
                        e.players = info.players;
                        e.maxPlayers = info.maxPlayers;
                        e.protocol = h.protocol;
                        e.pingMs = static_cast<int>(at - p.sentAt);
                    }
                }
                fromLen = sizeof(from);
            }
        }
        // Publish lookups and forget answers that stopped coming.
        std::lock_guard<std::mutex> lock(g_m);
        const DWORD t = GetTickCount();
        for (size_t i = 0; i < g_wanted.size() && i < g_entries.size(); ++i)
            for (const auto& p : probes)
                if (p.address == g_wanted[i]) {
                    g_entries[i].resolved = p.resolved;
                    if (!p.answeredAt || t - p.answeredAt > kAnswerKeepMs) g_entries[i].answered = false;
                }
    }
    closesocket(sock);
    WSACleanup();
}
} // namespace

void update(const std::vector<std::string>& addresses, bool active) {
    std::lock_guard<std::mutex> lock(g_m);
    if (addresses != g_wanted) {
        std::vector<Entry> next;
        for (const auto& a : addresses) {
            Entry e;
            e.address = a;
            for (const auto& old : g_entries)
                if (old.address == a) e = old;
            next.push_back(e);
        }
        g_wanted = addresses;
        g_entries.swap(next);
    }
    if (active && !g_active) g_refresh = true; // menu just opened: ask at once
    g_active = active;
    if (active && !g_run.exchange(true)) g_thread = std::thread(threadMain);
}

std::vector<Entry> snapshot() {
    std::lock_guard<std::mutex> lock(g_m);
    return g_entries;
}

void refreshNow() { g_refresh = true; }

void shutdown() {
    if (!g_run.exchange(false)) return;
    if (g_thread.joinable()) g_thread.join();
}

std::vector<std::string> split(const std::string& list) {
    std::vector<std::string> out;
    std::string cur;
    auto flush = [&] {
        size_t a = 0, b = cur.size();
        while (a < b && (cur[a] == ' ' || cur[a] == '\t')) ++a;
        while (b > a && (cur[b - 1] == ' ' || cur[b - 1] == '\t')) --b;
        if (b > a) out.push_back(cur.substr(a, b - a));
        cur.clear();
    };
    for (char c : list) {
        if (c == ',' || c == ';') flush();
        else cur.push_back(c);
    }
    flush();
    return out;
}

std::string join(const std::vector<std::string>& addresses) {
    std::string out;
    for (const auto& a : addresses) out += (out.empty() ? "" : ",") + a;
    return out;
}

} // namespace mecmp::net::servers
