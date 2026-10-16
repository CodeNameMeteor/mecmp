#pragma once
// The dedicated server's relay: one shared room. Every player's packets go to every other player,
// exactly as a hosting player's game forwards them, but the server is not a player itself: it sends
// only its own HELLO (name, flags host|server) so clients know the session and the server's name.
//
// No sockets and no clock here (the caller passes them in), so the rules can be tested anywhere:
//   - packets: right magic and protocol, a known type, at most kMaxPacket bytes
//   - a player is an address; the first packet binds its session id to that address. Packets from
//     that address with another id are dropped, except a HELLO (the game was restarted: new id).
//     An id already used by another address is refused (no taking over someone else's player).
//   - at most `maxPlayers`; a player silent for `timeoutMs` is dropped
//   - per player at most `ratePerSec` packets (burst `burst`); more are dropped
//   - a forwarded HELLO never carries the host or server flag
//   - kick: dropped, and their address refused for `kickBlockMs` (whichever port they come back from)

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <set>
#include <string>
#include <vector>

#include "net/protocol.h"

namespace mecmp::server {

struct Addr {
    uint32_t ip = 0;   // host byte order
    uint16_t port = 0; // host byte order
    bool operator==(const Addr& o) const { return ip == o.ip && port == o.port; }
    bool operator!=(const Addr& o) const { return !(*this == o); }
};
std::string toString(const Addr& a);
std::string ipToString(uint32_t ip);
bool parseIp(const std::string& text, uint32_t& ip); // "1.2.3.4" -> host byte order

struct Config {
    std::string name = "MECMP server";
    size_t maxPlayers = 16;       // the game shows at most 15 other players
    uint32_t timeoutMs = 10000;
    uint32_t helloEveryMs = 1000;
    double ratePerSec = 80;       // a player sends 30 states + 1 HELLO a second; chat and markers add a few
    double burst = 160;
    uint32_t kickBlockMs = 60000;
    uint32_t sessionGraceMs = 60000; // a player who timed out keeps its session id this long (see Relay::tick)
    bool logChat = true;
    float boneReach = 1.f; // limb translation clamp; receivers also enforce 2 m
    // Test mode: each player also gets their own packets back as a second player ("mirror", 3 m away
    // along world X), so one person can test a server alone. Chat is not mirrored.
    bool mirror = false;
    std::string motd;             // chat line sent to each player when they join (empty: none)
    size_t maxPerIp = 4;          // players from one IP address (a household, or one person flooding)
};

// The id a player's mirror uses in test mode.
constexpr uint32_t mirrorSession(uint32_t session) { return session ^ 0x4d495252u; }

struct Member {
    Addr addr;
    uint32_t session = 0;
    std::string name;
    uint32_t joinedAt = 0, lastSeen = 0;
    double tokens = 0;
    uint32_t refillAt = 0;
    uint64_t packets = 0, dropped = 0;
    net::protocol::Decoder poseDecoder;
    net::protocol::Encoder poseEncoder; // re-encodes its checked poses: deltas stay deltas
    bool announced = false; // its join was logged with a name
    uint32_t logAt = 0;     // the start of its current log window (see Relay::logAllowed)
    int logBurst = 0;       // log lines it has caused in that window
    uint32_t resultAt = 0;  // when its last finished run was reported (at most one per kResultGapMs)
};

// Something the server's outside world may want to know (the Discord webhook).
struct Event {
    enum Kind { Joined, Left, Result } kind = Joined;
    std::string name;   // the player's name
    size_t players = 0; // players connected
    net::protocol::Result result; // Result: the finished run, as the player's game reported it
};

struct Stats {
    uint64_t received = 0, relayed = 0, dropped = 0, infoAnswered = 0;
    uint64_t introductions = 0; // hole punching: joiners introduced to a registered host
};

class Relay {
public:
    using Send = std::function<void(const Addr& to, const char* data, size_t len)>;
    using Log = std::function<void(const std::string& line)>;
    using OnEvent = std::function<void(const Event&)>;

    Relay(Config cfg, uint32_t serverSession, Send send, Log log);

    void onPacket(const Addr& from, const char* buf, size_t n, uint32_t nowMs);
    // Every ~50 ms: drop silent players, send our HELLO when due, send queued server chat.
    void tick(uint32_t nowMs);

    // Console. `who` is a session id (hex, as `list` shows it) or a player name.
    // How many players `who` matches, and the first of them in `*first`. Names can repeat: a name that
    // matches several players is refused by kick and ban, and the id has to be used.
    size_t matchPlayers(const std::string& who, size_t* first = nullptr) const;
    bool kick(const std::string& who, uint32_t nowMs);
    void say(const std::string& text); // chat from the server (sent 3 times, like a player's)
    // Bans are by IP address. `who`: a player's id or name (their address is banned) or an IP. Players
    // on a banned address are dropped at once. Returns the banned IP (0 = no such player / address).
    uint32_t ban(const std::string& who);
    bool unban(const std::string& ip);
    const std::set<uint32_t>& bans() const { return bans_; }
    void setBans(std::set<uint32_t> ips); // e.g. bans.txt (re)loaded; drops players now banned
    // `reset`: every player is dropped (each leave is reported), and the session-id reservations, pending notices,
    // server chat and counters start again. Bans stay, and so do kick blocks (they run out by themselves).
    void reset();
    // The settings read again (`reset`): players stay connected; a lower limit only refuses new ones.
    void reconfigure(Config cfg);

    void setOnEvent(OnEvent f) { onEvent_ = std::move(f); }
    const std::vector<Member>& members() const { return members_; }
    const Stats& stats() const { return stats_; }
    const Config& config() const { return cfg_; }
    uint32_t session() const { return session_; }

private:
    Member* find(const Addr& a);
    bool sessionTaken(uint32_t session, const Addr& except) const;
    // Admission (protocol 7): a stranger's HELLO is answered with a CHALLENGE; an ANSWER with its cookie admits.
    void challenge(const Addr& to, uint32_t nowMs);
    bool cookieValid(const Addr& from, uint64_t cookie, uint32_t nowMs) const;
    void onAnswer(const Addr& from, uint32_t session, const char* payload, size_t len, uint32_t nowMs);
    void admit(const Addr& from, uint32_t session, uint32_t nowMs);
    // Hole punching (protocol.h, RENDEZVOUS): hosts register, joiners are introduced; nothing is relayed.
    void onRendezvous(const Addr& from, uint32_t session, const char* payload, size_t len, uint32_t nowMs);
    void sendRendezvous(const Addr& to, uint8_t op, const Addr& about, uint64_t cookie);
    bool blocked(const Addr& a, uint32_t nowMs);
    // A player's lines in the log: a few per window, so one player cannot fill the log.
    bool logAllowed(Member& m, uint32_t nowMs);
    void drop(size_t index, const char* why);
    void sendHello();
    void sendHelloTo(const Addr& to);
    // A short chat line to an address that is not let in (full, banned): at most one per 15 s each.
    void notify(const Addr& to, const std::string& text, uint32_t nowMs);
    void queueChat(const std::string& text, const Addr* only);

    Config cfg_;
    uint32_t session_;
    uint32_t seq_ = 0;
    uint8_t cookieKey_[16] = {}; // random per process: the cookies it hands out are only valid for it
    Send send_;
    Log log_;
    OnEvent onEvent_;
    std::vector<Member> members_;
    Stats stats_;
    uint32_t helloAt_ = 0;
    bool haveHello_ = false;
    struct Block { uint32_t ip; uint16_t port; uint32_t until; }; // port 0: every port of the address
    std::vector<Block> blocks_;
    // Session ids of players who timed out: no other address may take them for sessionGraceMs.
    struct Reserved { uint32_t session; Addr addr; uint32_t until; };
    std::vector<Reserved> reserved_;
public:
    // A host registered for hole punching: its address as this server sees it.
    struct Registered { Addr addr; uint32_t session; uint32_t lastSeen; uint32_t burstAt; int burst; };
    const std::vector<Registered>& registered() const { return registered_; }
    static constexpr uint32_t kRegisterTimeoutMs = 60000; // a host that stopped registering is forgotten
    static constexpr size_t kMaxRegistered = 512;
    static constexpr int kIntroBurst = 10;                // introductions to one host per kIntroWindowMs
    static constexpr uint32_t kIntroWindowMs = 10000;
private:
    std::vector<Registered> registered_;
    uint32_t rvLogAt_ = 0;
    int rvLogBurst_ = 0;
    struct OutChat { uint32_t id; std::string text; int left; bool targeted; Addr to; };
    std::deque<OutChat> chat_;
    uint32_t chatId_;
    uint32_t chatSeen_[32] = {};
    size_t chatSeenNext_ = 0;
    uint32_t resultSeen_[32] = {}; // RESULT ids (xor the sender's session) already reported
    size_t resultSeenNext_ = 0;
    uint32_t fullWarnAt_ = 0;
    bool fullWarned_ = false;
    std::set<uint32_t> bans_;
    struct Notice { Addr to; uint32_t at; };
    std::vector<Notice> notices_;
};

// Player names and chat as the clients clean them: no control characters.
std::string cleanText(const std::string& raw);

} // namespace mecmp::server
