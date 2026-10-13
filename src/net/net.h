#pragma once
// Peer-to-peer link: our own small UDP protocol on its own port. It never touches the game's
// networking or online services.
//
// Sessions: one player hosts (join= empty), the others join with the host's join code. The host
// opens its port on the router with UPnP, and forwards every player's packets to all the others,
// so only the host has to be reachable. Each remote player is identified by the random session id
// in its packets (forwarded packets keep the original id).

#include <cstdint>
#include <string>
#include <vector>

namespace mecmp::net {

constexpr uint32_t kMaxPoseBones = 32;

struct PlayerState {
    float pos[3] = {0, 0, 0};
    float yaw = 0;      // radians, atan2(forward.x, forward.z)
    uint32_t flags = 0; // bit0: in a level
    uint32_t timeMs = 0; // sender's clock when this state was captured (receivers replay on it)
    // Body pose in the replay player's format: per bone {translation x,y,z,w; rotation x,y,z,w}.
    // Bone order = the recorder's, which maps 1:1 onto a ghost's replay pose. 0 = no pose.
    uint32_t boneCount = 0;
    float bones[kMaxPoseBones][8] = {};
};

struct RemoteInfo {
    uint32_t id = 0;        // the remote player's session id
    std::string name;
    bool loopback = false;  // our own packets (solo test)
    uint32_t ageMs = 0;     // since the last state arrived
    float lossPct = 0;      // their packets lost over the last 5 s
    uint32_t look = 0;      // the look they chose: 0 = Faith, else the game's echo CustomizationHash
};

// join: empty = host; otherwise a join code or ip:port of the host. Calling it again switches
// session (the old one is stopped first).
bool start(uint16_t port, const std::string& join, float loopbackOffset, const std::string& name);
void stop();
// Menu "Leave": stop and close the router port this session opened (in the background).
void leave();
// Game exit: stop, and close the router port this session opened (waits at most routerWaitMs).
void shutdown(uint32_t routerWaitMs = 2000);

// Game thread: newest local state (sent by the network thread).
void publishLocal(const PlayerState& s);
// Our own newest state. False when none was published in the last second (not in a level). Any thread.
bool localState(PlayerState& out);
// Remote players heard from in the last `maxAgeMs`.
std::vector<RemoteInfo> remotes(uint32_t maxAgeMs = 5000);

// Text chat. sendChat shows the message here at once and sends it to everyone (any thread).
struct ChatLine {
    std::string name;
    std::string text;
    uint32_t atMs = 0; // GetTickCount when it arrived (or was sent)
    bool self = false;
};
void sendChat(const std::string& text);
std::vector<ChatLine> chatLines(size_t max = 50); // oldest first
// World markers ("pings"). Each player has a few at once (see PING in protocol.h); each disappears
// after the time its owner gave it. sendPing shows ours here at once and sends it to everyone (any thread).
struct PingInfo {
    uint32_t owner = 0; // session id of the player who placed it
    uint8_t index = 0;  // which of the owner's cells it is in
    std::string name;
    float pos[3] = {0, 0, 0};
    uint8_t rgb[3] = {255, 210, 60};
    uint8_t accent[3] = {255, 255, 255}; // the detail inside the glyph
    uint8_t glyph = 0;  // the shape to draw (protocol::pingGlyph)
    std::string label;  // its title, as its owner wrote it; may be empty
    uint32_t ageMs = 0;  // since it appeared (placing the same marker again right away does not restart it)
    uint32_t leftMs = 0; // until it disappears
    uint32_t lifeMs = 0; // how long it lasts in all
    bool own = false;
};
// What a new marker looks like and how long it stays.
struct PingStyle {
    uint8_t rgb[3] = {255, 210, 60};
    uint8_t accent[3] = {255, 255, 255};
    uint8_t glyph = 0;
    std::string label;
    int seconds = 30; // 5..300
};
// Places a marker of ours; `limit` = how many we may have at once (1..protocol::kPingCells). The same
// marker close to one of ours moves that one; over the limit, the oldest goes.
void sendPing(const float pos[3], const PingStyle& style, int limit);
bool clearPing();          // removes all of ours, here and for everyone; false if there were none
// One of ours, by its cell (PingInfo::index). move, rename and renew start its time again, as placing
// it anew would (renew does nothing else, and the marker does not flash again); they return false if
// that cell holds no marker now.
void removePing(uint8_t index);
bool movePing(uint8_t index, const float pos[3]);
bool renamePing(uint8_t index, const std::string& label);
bool renewPing(uint8_t index);
void trimPings(int limit); // removes our oldest markers until at most `limit` are left
std::vector<PingInfo> pings(); // the markers to show now (ours first)
// Smoothed state of a remote player (shown ~100 ms in the past, blended between packets).
bool remoteState(uint32_t id, PlayerState& out, uint32_t& ageMs);

bool running();
bool hosting();
bool connectedToHost();   // joiner: the host has answered
std::string serverName(); // joiner: the dedicated server's name; empty when a player hosts (or not heard yet)
// The same number for everyone in a session (the host's session id); 0 while not known (e.g. solo test).
uint32_t sessionKey();
std::string lanCode();    // host: code for players on the same network
void setName(const std::string& name);
void setLook(uint32_t look); // our look, sent to everyone (see RemoteInfo::look)
std::string joinCode(); // the host's code (empty until set up, or when not reachable)
std::string describe();

} // namespace mecmp::net
