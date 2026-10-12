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
// Game exit: stop, and close the router port this session opened (waits at most routerWaitMs).
void shutdown(uint32_t routerWaitMs = 2000);

// Game thread: newest local state (sent by the network thread).
void publishLocal(const PlayerState& s);
// Remote players heard from in the last `maxAgeMs`.
std::vector<RemoteInfo> remotes(uint32_t maxAgeMs = 5000);
// Smoothed state of a remote player (shown ~100 ms in the past, blended between packets).
bool remoteState(uint32_t id, PlayerState& out, uint32_t& ageMs);

bool running();
bool hosting();
bool connectedToHost();   // joiner: the host has answered
// The same number for everyone in a session (the host's session id); 0 while not known (e.g. solo test).
uint32_t sessionKey();
std::string lanCode();    // host: code for players on the same network
void setName(const std::string& name);
void setLook(uint32_t look); // our look, sent to everyone (see RemoteInfo::look)
std::string joinCode(); // the host's code (empty until set up, or when not reachable)
std::string describe();

} // namespace mecmp::net
