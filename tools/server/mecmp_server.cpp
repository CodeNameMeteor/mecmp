// mecmp_server: a dedicated MECMP server (one shared room). Runs without the game, on Windows or
// Linux (a VPS). Players join it like a host: join=<address>:<port> in mecmp.ini, or the address in
// the menu's join field. See docs/SERVER.md.
//
//   mecmp_server [--port 47800] [--name "My server"] [--max-players 16] [--config mecmp_server.ini]
//                [--log mecmp_server.log] [--mirror]
//
// --mirror (test mode): each player also sees their own moves as a second player 3 m away, so one
// person can test a server alone.
//
// Console commands (stdin): list, kick <id|name>, say <text>, status, help, quit.

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
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
#include <fstream>
#include <iostream>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "net/protocol.h"
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
    mecmp::server::Config relay;
};

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
    }
    return true;
}

void usage() {
    std::printf("mecmp_server [--port 47800] [--name \"My server\"] [--max-players 16]\n"
                "             [--config mecmp_server.ini] [--log mecmp_server.log] [--mirror]\n");
}

void onSignal(int) { g_quit = true; }

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
    } else if (verb == "say") {
        relay.say(arg);
    } else if (verb == "status") {
        const auto& s = relay.stats();
        logLine("'" + relay.config().name + "': " +
                std::to_string(relay.members().size()) + " players, " + std::to_string(s.received) + " received, " +
                std::to_string(s.relayed) + " relayed, " + std::to_string(s.dropped) + " dropped");
    } else if (verb == "help") {
        logLine("commands: list | kick <id|name> | say <text> | status | quit");
    } else {
        logLine("unknown command '" + verb + "' (help lists them)");
    }
}
} // namespace

int main(int argc, char** argv) {
    Options o;
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
    std::thread(readConsole).detach();
    char hexSession[16];
    std::snprintf(hexSession, sizeof(hexSession), "%08x", session);
    logLine("MECMP server '" + o.relay.name + "' on UDP " + std::to_string(o.port) + " (protocol " +
            std::to_string(mecmp::net::protocol::kProtocol) + ", up to " + std::to_string(o.relay.maxPlayers) +
            " players, session " + hexSession + "). Players join with <this address>:" + std::to_string(o.port) +
            ". Type 'help' for commands.");
    if (o.relay.mirror) logLine("test mode (--mirror): every player also sees a copy of themselves 3 m away");

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
                relay.onPacket({ntohl(from.sin_addr.s_addr), ntohs(from.sin_port)}, buf, static_cast<size_t>(n), nowMs());
                fd_set more;
                FD_ZERO(&more);
                FD_SET(sock, &more);
                timeval zero{0, 0};
                if (select(static_cast<int>(sock + 1), &more, nullptr, nullptr, &zero) <= 0) break;
            }
        }
        const uint32_t now = nowMs();
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
    }
    logLine("server stopping");
    closeSocket(sock);
#ifdef _WIN32
    WSACleanup();
#endif
    return 0;
}
