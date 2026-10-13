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
//   - kick: dropped and refused for `kickBlockMs`

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <vector>

namespace mecmp::server {

struct Addr {
    uint32_t ip = 0;   // host byte order
    uint16_t port = 0; // host byte order
    bool operator==(const Addr& o) const { return ip == o.ip && port == o.port; }
    bool operator!=(const Addr& o) const { return !(*this == o); }
};
std::string toString(const Addr& a);

struct Config {
    std::string name = "MECMP server";
    size_t maxPlayers = 16;       // the game shows at most 15 other players
    uint32_t timeoutMs = 10000;
    uint32_t helloEveryMs = 1000;
    double ratePerSec = 80;       // a player sends 30 states + 1 HELLO a second; chat and markers add a few
    double burst = 160;
    uint32_t kickBlockMs = 60000;
    bool logChat = true;
    // Test mode: each player also gets their own packets back as a second player ("mirror", 3 m away
    // along world X), so one person can test a server alone. Chat is not mirrored.
    bool mirror = false;
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
    bool announced = false; // its join was logged with a name
};

struct Stats {
    uint64_t received = 0, relayed = 0, dropped = 0;
};

class Relay {
public:
    using Send = std::function<void(const Addr& to, const char* data, size_t len)>;
    using Log = std::function<void(const std::string& line)>;

    Relay(Config cfg, uint32_t serverSession, Send send, Log log);

    void onPacket(const Addr& from, const char* buf, size_t n, uint32_t nowMs);
    // Every ~50 ms: drop silent players, send our HELLO when due, send queued server chat.
    void tick(uint32_t nowMs);

    // Console. `who` is a session id (hex, as `list` shows it) or a player name.
    bool kick(const std::string& who, uint32_t nowMs);
    void say(const std::string& text); // chat from the server (sent 3 times, like a player's)

    const std::vector<Member>& members() const { return members_; }
    const Stats& stats() const { return stats_; }
    const Config& config() const { return cfg_; }
    uint32_t session() const { return session_; }

private:
    Member* find(const Addr& a);
    bool sessionTaken(uint32_t session, const Addr& except) const;
    bool blocked(const Addr& a, uint32_t nowMs);
    void drop(size_t index, const char* why);
    void sendHello();

    Config cfg_;
    uint32_t session_;
    uint32_t seq_ = 0;
    Send send_;
    Log log_;
    std::vector<Member> members_;
    Stats stats_;
    uint32_t helloAt_ = 0;
    bool haveHello_ = false;
    struct Block { uint32_t ip; uint16_t port; uint32_t until; };
    std::vector<Block> blocks_;
    struct OutChat { uint32_t id; std::string text; int left; };
    std::deque<OutChat> chat_;
    uint32_t chatId_;
    uint32_t chatSeen_[32] = {};
    size_t chatSeenNext_ = 0;
    uint32_t fullWarnAt_ = 0;
    bool fullWarned_ = false;
};

// Player names and chat as the clients clean them: no control characters.
std::string cleanText(const std::string& raw);

} // namespace mecmp::server
