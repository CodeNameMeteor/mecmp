// mecmp_server: a dedicated MECMP server (one shared room). Runs without the game, on Windows or
// Linux (a VPS). Players join it like a host: join=<address>:<port> in mecmp.ini, or the address in
// the menu's join field. See docs/SERVER.md.
//
//   mecmp_server [--port 47800] [--name "My server"] [--max-players 16] [--config mecmp_server.ini]
//                [--log mecmp_server.log] [--bans bans.txt] [--mirror]
//
// --mirror (test mode): each player also sees their own moves as a second player 3 m away, so one
// person can test a server alone.
//
// Console commands (stdin): list, kick <id|name>, ban <id|name|ip>, unban <ip>, bans, say <text>,
// status, help, quit. Bans are kept in bans.txt (reloaded when the file changes).

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "net/protocol.h"
#include "net/joincode.h"
#include "server/relay.h"

namespace {
using mecmp::server::Addr;

#ifdef _WIN32
using Socket = SOCKET;
constexpr Socket kNoSocket = INVALID_SOCKET;
void closeSocket(Socket s) { closesocket(s); }
#else
using Socket = int;
constexpr Socket kNoSocket = -1;
void closeSocket(Socket s) { close(s); }
#endif

std::atomic<bool> g_quit{false};
std::mutex g_logM;
std::ofstream g_logFile;

void logLine(const std::string& line) {
    std::time_t t = std::time(nullptr);
    char ts[32];
    std::strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", std::localtime(&t));
    std::lock_guard<std::mutex> lock(g_logM);
    std::printf("[%s] %s\n", ts, line.c_str());
    std::fflush(stdout);
    if (g_logFile.is_open()) {
        g_logFile << '[' << ts << "] " << line << '\n';
        g_logFile.flush();
    }
}

uint32_t nowMs() {
    using namespace std::chrono;
    return static_cast<uint32_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

std::string trim(std::string s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\r' || s.back() == '\n' || s.back() == '\t')) s.pop_back();
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    return s.substr(i);
}

struct Options {
    uint16_t port = 47800;
    std::string logPath = "mecmp_server.log";
    std::string bansPath = "bans.txt";
    mecmp::server::Config relay;
};
Options g_opts;

// mecmp_server.ini: key=value lines (port, name, max_players, log, log_chat); '#' or ';' comments.
bool readConfig(const std::string& path, Options& o, bool required) {
    std::ifstream in(path);
    if (!in) {
        if (required) logLine("cannot read " + path);
        return !required;
    }
    std::string line;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';' || line[0] == '[') continue;
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = trim(line.substr(0, eq)), v = trim(line.substr(eq + 1));
        if (k == "port") o.port = static_cast<uint16_t>(std::atoi(v.c_str()));
        else if (k == "name") o.relay.name = v;
        else if (k == "max_players") o.relay.maxPlayers = static_cast<size_t>(std::atoi(v.c_str()));
        else if (k == "log") o.logPath = v;
        else if (k == "log_chat") o.relay.logChat = v != "0";
        else if (k == "mirror") o.relay.mirror = v == "1";
        else if (k == "motd") o.relay.motd = v;
        else if (k == "max_per_ip") o.relay.maxPerIp = static_cast<size_t>(std::atoi(v.c_str()));
        else if (k == "bans") o.bansPath = v;
    }
    return true;
}

void usage() {
    std::printf("mecmp_server [--port 47800] [--name \"My server\"] [--max-players 16]\n"
                "             [--config mecmp_server.ini] [--log mecmp_server.log] [--bans bans.txt] [--mirror]\n");
}

void onSignal(int) { g_quit = true; }

// --- This server's address, for the log ---------------------------------------------------------
std::string ipString(uint32_t ip) {
    const std::string s = mecmp::server::toString({ip, 0});
    return s.substr(0, s.rfind(':'));
}

// The address this machine uses on its network (no packet is sent: a UDP connect only picks a route).
uint32_t localIp() {
    Socket s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == kNoSocket) return 0;
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(53);
    to.sin_addr.s_addr = htonl(0x08080808);
    uint32_t ip = 0;
    if (connect(s, reinterpret_cast<sockaddr*>(&to), sizeof(to)) == 0) {
        sockaddr_in me{};
#ifdef _WIN32
        int len = sizeof(me);
#else
        socklen_t len = sizeof(me);
#endif
        if (getsockname(s, reinterpret_cast<sockaddr*>(&me), &len) == 0) ip = ntohl(me.sin_addr.s_addr);
    }
    closeSocket(s);
    return ip;
}

// STUN (RFC 5389) binding request through the server socket: the reply says the public ip:port the
// internet sees, which is what players outside this network join.
constexpr uint32_t kStunCookie = 0x2112A442;
unsigned char g_stunTx[12] = {};
bool sendStun(Socket sock) {
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* res = nullptr;
    if (getaddrinfo("stun.l.google.com", "19302", &hints, &res) != 0 || !res) return false;
    unsigned char req[20] = {0x00, 0x01, 0x00, 0x00, 0x21, 0x12, 0xA4, 0x42};
    memcpy(req + 8, g_stunTx, sizeof(g_stunTx));
    sendto(sock, reinterpret_cast<const char*>(req), sizeof(req), 0, res->ai_addr, static_cast<int>(res->ai_addrlen));
    freeaddrinfo(res);
    return true;
}
// True if `b` is our STUN answer (then ip/port are set, host byte order).
bool readStun(const unsigned char* b, int n, uint32_t& ip, uint16_t& port) {
    if (n < 20 || b[0] != 0x01 || b[1] != 0x01 || memcmp(b + 8, g_stunTx, sizeof(g_stunTx)) != 0) return false;
    for (int p = 20; p + 4 <= n;) {
        const int type = (b[p] << 8) | b[p + 1], len = (b[p + 2] << 8) | b[p + 3];
        if (type == 0x0020 && len >= 8 && p + 4 + len <= n && b[p + 5] == 0x01) { // XOR-MAPPED-ADDRESS, IPv4
            port = static_cast<uint16_t>(((b[p + 6] << 8) | b[p + 7]) ^ (kStunCookie >> 16));
            ip = ((static_cast<uint32_t>(b[p + 8]) << 24) | (static_cast<uint32_t>(b[p + 9]) << 16) |
                  (static_cast<uint32_t>(b[p + 10]) << 8) | b[p + 11]) ^ kStunCookie;
            return true;
        }
        p += 4 + ((len + 3) & ~3);
    }
    return false;
}

void logAddresses(uint16_t port, uint32_t publicIp, uint16_t publicPort) {
    const uint32_t lan = localIp();
    const std::string p = std::to_string(port);
    if (publicIp && publicIp == lan) { // a machine with a public address (a VPS)
        logLine("players join with " + ipString(publicIp) + ":" + p + " (or the join code " +
                mecmp::net::joincode::encode(publicIp, port) + ", or a domain name pointing here)");
        return;
    }
    if (lan) logLine("same network: players join with " + ipString(lan) + ":" + p + " (on this PC: 127.0.0.1:" + p + ")");
    if (publicIp) {
        logLine("internet: players join with " + ipString(publicIp) + ":" + p + " (join code " +
                mecmp::net::joincode::encode(publicIp, port) + "), once UDP port " + p + " is forwarded on your router to " +
                (lan ? ipString(lan) : std::string("this PC")));
        if (publicPort && publicPort != port)
            logLine("note: your router changed the port to " + std::to_string(publicPort) + " on the way out; forward " + p +
                    " by hand");
    } else {
        logLine("internet: the public address could not be found (no answer from the STUN server)");
    }
}

// Console input on its own thread (blocking reads); commands are run by the main loop.
std::mutex g_cmdM;
std::vector<std::string> g_cmds;
void readConsole() {
    std::string line;
    while (!g_quit && std::getline(std::cin, line)) {
        std::lock_guard<std::mutex> lock(g_cmdM);
        g_cmds.push_back(trim(line));
    }
    // stdin closed (e.g. run as a service): keep serving.
}

// --- bans.txt: one IPv4 address per line; anything after it ('#' comment, name, date) is ignored ----
std::filesystem::file_time_type g_bansTime{};
bool g_bansSeen = false;

std::set<uint32_t> readBans(const std::string& path, size_t* bad) {
    std::set<uint32_t> out;
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        const std::string first = line.substr(0, line.find_first_of(" \t#"));
        uint32_t ip = 0;
        if (mecmp::server::parseIp(first, ip)) out.insert(ip);
        else if (bad) ++*bad;
    }
    return out;
}

void noteBansTime() {
    std::error_code ec;
    const auto t = std::filesystem::last_write_time(g_opts.bansPath, ec);
    g_bansSeen = !ec;
    if (!ec) g_bansTime = t;
}

// Keeps the comments of existing lines; adds `note` to a new one.
void writeBans(const std::set<uint32_t>& ips, uint32_t added, const std::string& note) {
    if (g_opts.bansPath.empty()) return;
    std::vector<std::string> kept;
    {
        std::ifstream in(g_opts.bansPath);
        std::string line;
        while (std::getline(in, line)) {
            const std::string t = trim(line);
            uint32_t ip = 0;
            if (t.empty() || t[0] == '#' || (mecmp::server::parseIp(t.substr(0, t.find_first_of(" \t#")), ip) && ips.count(ip)))
                kept.push_back(t);
        }
    }
    if (kept.empty()) kept.push_back("# MECMP server bans: one IPv4 address per line; text after it is a comment.");
    if (added) {
        std::time_t now = std::time(nullptr);
        char date[16];
        std::strftime(date, sizeof(date), "%Y-%m-%d", std::localtime(&now));
        kept.push_back(mecmp::server::ipToString(added) + "  # " + note + " " + date);
    }
    std::ofstream out(g_opts.bansPath, std::ios::trunc);
    for (const auto& l : kept) out << l << '\n';
    out.close();
    noteBansTime();
}

void loadBans(mecmp::server::Relay& relay, bool startup) {
    if (g_opts.bansPath.empty()) return;
    size_t bad = 0;
    std::set<uint32_t> ips = readBans(g_opts.bansPath, &bad);
    noteBansTime();
    if (!startup || !ips.empty())
        logLine(std::string(startup ? "" : "bans file changed: ") + std::to_string(ips.size()) + " banned address(es) from " +
                g_opts.bansPath + (bad ? " (" + std::to_string(bad) + " line(s) not understood)" : ""));
    relay.setBans(std::move(ips));
}

void reloadBansIfChanged(mecmp::server::Relay& relay) {
    if (g_opts.bansPath.empty()) return;
    std::error_code ec;
    const auto t = std::filesystem::last_write_time(g_opts.bansPath, ec);
    if (ec ? g_bansSeen : (!g_bansSeen || t != g_bansTime)) loadBans(relay, false);
}

void runCommand(const std::string& cmd, mecmp::server::Relay& relay) {
    const size_t sp = cmd.find(' ');
    const std::string verb = cmd.substr(0, sp), arg = sp == std::string::npos ? "" : trim(cmd.substr(sp + 1));
    if (verb.empty()) return;
    if (verb == "quit" || verb == "stop" || verb == "exit") {
        g_quit = true;
    } else if (verb == "list") {
        const auto& ms = relay.members();
        logLine(std::to_string(ms.size()) + " of " + std::to_string(relay.config().maxPlayers) + " players");
        for (const auto& m : ms) {
            char line[256];
            std::snprintf(line, sizeof(line), "  %08x  %-24s %-21s %4us  %llu packets, %llu dropped", m.session, m.name.c_str(),
                          mecmp::server::toString(m.addr).c_str(), (nowMs() - m.joinedAt) / 1000,
                          static_cast<unsigned long long>(m.packets), static_cast<unsigned long long>(m.dropped));
            logLine(line);
        }
    } else if (verb == "kick") {
        if (!relay.kick(arg, nowMs())) logLine("no player '" + arg + "' (use the id or name from 'list')");
    } else if (verb == "ban") {
        std::string name;
        for (const auto& m : relay.members())
            if (m.name == arg) name = m.name;
        if (const uint32_t ip = relay.ban(arg)) writeBans(relay.bans(), ip, name.empty() ? std::string("banned") : "'" + name + "'");
        else logLine("no player or IP address '" + arg + "' (use the id or name from 'list', or an IP)");
    } else if (verb == "unban") {
        if (relay.unban(arg)) writeBans(relay.bans(), 0, "");
        else logLine("'" + arg + "' is not banned (bans lists them)");
    } else if (verb == "bans") {
        logLine(std::to_string(relay.bans().size()) + " banned address(es)" + (g_opts.bansPath.empty() ? "" : " (" + g_opts.bansPath + ")"));
        for (uint32_t ip : relay.bans()) logLine("  " + mecmp::server::ipToString(ip));
    } else if (verb == "say") {
        relay.say(arg);
    } else if (verb == "status") {
        const auto& s = relay.stats();
        logLine("'" + relay.config().name + "': " +
                std::to_string(relay.members().size()) + " players, " + std::to_string(s.received) + " received, " +
                std::to_string(s.relayed) + " relayed, " + std::to_string(s.dropped) + " dropped, " +
                std::to_string(s.infoAnswered) + " server-list queries answered");
    } else if (verb == "help") {
        logLine("commands: list | kick <id|name> | ban <id|name|ip> | unban <ip> | bans | say <text> | status | quit");
    } else {
        logLine("unknown command '" + verb + "' (help lists them)");
    }
}
} // namespace

int main(int argc, char** argv) {
    Options& o = g_opts;
    std::string configPath = "mecmp_server.ini";
    bool configGiven = false;
    for (int i = 1; i < argc; ++i)
        if (std::string(argv[i]) == "--config" && i + 1 < argc) { configPath = argv[i + 1]; configGiven = true; }
    if (!readConfig(configPath, o, configGiven)) return 1;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const bool hasValue = i + 1 < argc;
        if (a == "--port" && hasValue) o.port = static_cast<uint16_t>(std::atoi(argv[++i]));
        else if (a == "--name" && hasValue) o.relay.name = argv[++i];
        else if (a == "--max-players" && hasValue) o.relay.maxPlayers = static_cast<size_t>(std::atoi(argv[++i]));
        else if (a == "--log" && hasValue) o.logPath = argv[++i];
        else if (a == "--mirror") o.relay.mirror = true;
        else if (a == "--bans" && hasValue) o.bansPath = argv[++i];
        else if (a == "--config" && hasValue) ++i;
        else if (a == "--help" || a == "-h") { usage(); return 0; }
        else { usage(); return 1; }
    }
    o.relay.name = mecmp::server::cleanText(o.relay.name);
    if (o.relay.name.size() > mecmp::net::protocol::kMaxNameBytes)
        o.relay.name.resize(mecmp::net::protocol::utf8PrefixLength(o.relay.name, mecmp::net::protocol::kMaxNameBytes));
    if (o.relay.maxPlayers < 1) o.relay.maxPlayers = 1;
    if (o.relay.maxPlayers > 64) o.relay.maxPlayers = 64;
    if (!o.port) { logLine("invalid port"); return 1; }
    if (!o.logPath.empty()) g_logFile.open(o.logPath, std::ios::app);

#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { logLine("WSAStartup failed"); return 1; }
#endif
    Socket sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == kNoSocket) { logLine("socket() failed"); return 1; }
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_port = htons(o.port);
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(sock, reinterpret_cast<sockaddr*>(&local), sizeof(local)) != 0) {
        logLine("cannot use UDP port " + std::to_string(o.port) + " (in use?)");
        closeSocket(sock);
        return 1;
    }

    std::random_device rd;
    uint32_t session = (static_cast<uint32_t>(rd()) << 1) ^ static_cast<uint32_t>(rd());
    if (!session) session = 1;
    mecmp::server::Relay relay(
        o.relay, session,
        [sock](const Addr& to, const char* data, size_t len) {
            sockaddr_in a{};
            a.sin_family = AF_INET;
            a.sin_addr.s_addr = htonl(to.ip);
            a.sin_port = htons(to.port);
            sendto(sock, data, static_cast<int>(len), 0, reinterpret_cast<const sockaddr*>(&a), sizeof(a));
        },
        logLine);

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
    loadBans(relay, true);
    std::thread(readConsole).detach();
    char hexSession[16];
    std::snprintf(hexSession, sizeof(hexSession), "%08x", session);
    logLine("MECMP server '" + o.relay.name + "' on UDP " + std::to_string(o.port) + " (protocol " +
            std::to_string(mecmp::net::protocol::kProtocol) + ", up to " + std::to_string(o.relay.maxPlayers) +
            " players, session " + hexSession + "). Type 'help' for commands.");
    if (o.relay.mirror) logLine("test mode (--mirror): every player also sees a copy of themselves 3 m away");

    for (unsigned char& c : g_stunTx) c = static_cast<unsigned char>(rd());
    int stunTries = 0;
    uint32_t stunAt = nowMs();
    bool addressShown = false;
    if (sendStun(sock)) ++stunTries;

    char buf[mecmp::net::protocol::kMaxPacket + 64];
    uint32_t lastTick = 0;
    while (!g_quit) {
        fd_set rs;
        FD_ZERO(&rs);
        FD_SET(sock, &rs);
        timeval tv{0, 20000};
        const int ready = select(static_cast<int>(sock + 1), &rs, nullptr, nullptr, &tv);
        if (ready > 0) {
            for (int burst = 0; burst < 256; ++burst) {
                sockaddr_in from{};
#ifdef _WIN32
                int fromLen = sizeof(from);
#else
                socklen_t fromLen = sizeof(from);
#endif
                const int n = static_cast<int>(recvfrom(sock, buf, sizeof(buf), 0, reinterpret_cast<sockaddr*>(&from), &fromLen));
                if (n <= 0) break;
                uint32_t pubIp = 0;
                uint16_t pubPort = 0;
                if (!addressShown && readStun(reinterpret_cast<const unsigned char*>(buf), n, pubIp, pubPort)) {
                    addressShown = true;
                    logAddresses(o.port, pubIp, pubPort);
                } else {
                    relay.onPacket({ntohl(from.sin_addr.s_addr), ntohs(from.sin_port)}, buf, static_cast<size_t>(n), nowMs());
                }
                fd_set more;
                FD_ZERO(&more);
                FD_SET(sock, &more);
                timeval zero{0, 0};
                if (select(static_cast<int>(sock + 1), &more, nullptr, nullptr, &zero) <= 0) break;
            }
        }
        const uint32_t now = nowMs();
        if (!addressShown && now - stunAt >= 2000) { // no answer yet: try twice more, then say so
            stunAt = now;
            if (stunTries < 3 && sendStun(sock)) ++stunTries;
            else { addressShown = true; logAddresses(o.port, 0, 0); }
        }
        if (now - lastTick >= 50) {
            lastTick = now;
            relay.tick(now);
        }
        std::vector<std::string> cmds;
        {
            std::lock_guard<std::mutex> lock(g_cmdM);
            cmds.swap(g_cmds);
        }
        for (const auto& c : cmds) runCommand(c, relay);
        static uint32_t bansCheckedAt = 0;
        if (now - bansCheckedAt >= 5000) {
            bansCheckedAt = now;
            reloadBansIfChanged(relay);
        }
    }
    logLine("server stopping");
    closeSocket(sock);
#ifdef _WIN32
    WSACleanup();
#endif
    return 0;
}
