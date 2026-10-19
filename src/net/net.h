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
#include "net/player_audio.h"

namespace mecmp::net {
namespace protocol { struct Result; } // net/protocol.h

constexpr uint32_t kMaxPoseBones = 96;
// PlayerState::flags (STATE packet).
constexpr uint32_t kStateInLevel = 1;
constexpr uint32_t kStateTyping = 2; // the chat box is open (older versions ignore it)

struct PlayerState {
    float pos[3] = {0, 0, 0};
    float yaw = 0;      // radians, atan2(forward.x, forward.z)
    uint32_t flags = 0; // kState... bits
    uint32_t timeMs = 0; // sender's clock when this state was captured (receivers replay on it)
    // Body pose in the replay player's format: per bone {translation x,y,z,w; rotation x,y,z,w}.
    // boneIdx[i] = the skeleton bone slot i belongs to (the recorder's setup). 0 bones = no pose.
    uint32_t boneCount = 0;
    uint8_t boneIdx[kMaxPoseBones] = {};
    float bones[kMaxPoseBones][8] = {};
};

struct RemoteInfo {
    uint32_t id = 0;        // the remote player's session id
    std::string name;
    bool loopback = false;  // our own packets (solo test)
    uint32_t ageMs = 0;     // since the last state arrived
    float lossPct = 0;      // their packets lost over the last 5 s
    uint32_t look = 0;      // the look they chose: 0 = Faith, else the game's echo CustomizationHash
    uint8_t outfit = 0;     // with look 0: what they changed about Faith's outfit (protocol::kWear... bits)
    uint64_t onlineId = 0;  // their EA online id, if they share it (for the game's map markers); 0 = not shared
    std::string profileName; // their EA profile name, with onlineId
    bool typing = false;    // their chat box is open
    uint8_t runKind = 0xff; // the run they are in (protocol::ResultKind); 0xff = none (protocol::kActivityNone)
    uint32_t runMs = 0;     // how long that run has been going, counted on to now
    std::string runTitle;   // its name
    bool asksVisit = false; // they want to be asked before anyone goes to them (VISIT, protocol.h)
};

// join: empty = host; otherwise a join code or ip:port of the host. Calling it again switches
// session (the old one is stopped first).
bool start(uint16_t port, const std::string& join, float loopbackOffset, const std::string& name);
// The same on a helper thread, for the menu (the render thread): a server's name is looked up there, so a slow
// DNS server never stalls the game. The latest request wins; leave() cancels one still looking up.
void startAsync(uint16_t port, const std::string& join, float loopbackOffset, const std::string& name);
bool starting(); // a startAsync has not started its session yet (looking up the address)
void stop();
// Menu "Leave": stop and close the router port this session opened (in the background).
void leave();
// Game exit: stop, and close the router port this session opened (waits at most routerWaitMs).
void shutdown(uint32_t routerWaitMs = 2000);

// Game thread: newest local state (sent by the network thread).
void publishLocal(const PlayerState& s);
void publishAudio(PlayerAudio sample);
struct RemoteAudio { uint32_t peer = 0, receivedAt = 0, playAt = 0; PlayerAudio sample; };
std::vector<RemoteAudio> takeAudio();
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
// A quick answer to a marker, as shown under it: the answering player's icon and colors.
struct ReactionMark {
    uint32_t from = 0; // session id of the answering player
    std::string name;
    std::string text;
    uint8_t glyph = 31; // the speech bubble for an answer from a version that sent no icon
    uint8_t rgb[3] = {255, 255, 255};
    uint8_t accent[3] = {255, 255, 255};
};
struct PingInfo {
    uint32_t owner = 0; // session id of the player who placed it
    uint8_t index = 0;  // which of the owner's cells it is in
    std::string name;
    float pos[3] = {0, 0, 0};
    uint8_t rgb[3] = {232, 50, 46};
    uint8_t accent[3] = {255, 255, 255}; // the detail inside the glyph
    uint8_t glyph = 0;  // the shape to draw (protocol::pingGlyph)
    std::string label;  // its title, as its owner wrote it; may be empty
    uint32_t ageMs = 0;  // since it appeared (placing the same marker again right away does not restart it)
    uint32_t leftMs = 0; // until it disappears
    uint32_t lifeMs = 0; // how long it lasts in all
    bool own = false;
    std::vector<ReactionMark> reactions; // answers to it, oldest first; gone with the marker
};
// What a new marker looks like and how long it stays.
struct PingStyle {
    uint8_t rgb[3] = {232, 50, 46};
    uint8_t accent[3] = {255, 255, 255};
    uint8_t glyph = 0;
    std::string label;
    int seconds = 30; // 5..300
};
// Places a marker of ours; `limit` = how many we may have at once (1..protocol::kPingCells). The same
// marker close to one of ours moves that one; over the limit, the oldest goes.
void sendPing(const float pos[3], const PingStyle& style, int limit);
bool clearPing();          // removes all of ours, here and for everyone; false if there were none
// A quick answer ("On my way") to the marker in cell `cell` of the player whose session id is `target`
// (PingInfo::owner / ::index); `markerTitle` is the marker's title as seen here. Any thread.
// `glyph` and the colors are the answer's icon, shown under the marker for everyone.
void sendReact(uint32_t target, uint8_t cell, const std::string& markerTitle, const std::string& text, uint8_t glyph,
               const uint8_t rgb[3], const uint8_t accent[3]);
// Answers other players sent to our markers since the last call, oldest first.
struct ReactionInfo {
    std::string from;  // the answering player's name
    std::string text;  // the answer
    std::string title; // our marker's title, as they saw it; may be empty
};
std::vector<ReactionInfo> takeReactions();

// Finished runs (RESULT, protocol.h). Ours goes to everyone (id assigned here); others' arrive as notice
// texts ("Meteor finished the dash The View in 1:23.45"). Any thread.
void sendResult(const protocol::Result& result);
std::vector<std::string> takeResults();
// The run we are in now (ACTIVITY, protocol.h): kind = protocol::ResultKind, startedTick = GetTickCount when it
// started; kind protocol::kActivityNone = none (sent a few times so the others' name tags clear). Any thread.
void setActivity(uint8_t kind, uint32_t startedTick, const std::string& title);
bool inRun(); // we are in a run now (setActivity's kind is not kActivityNone). Any thread.
// Time of day, seconds since midnight (game/time_of_day.h). setTimeOfDay: our clock, sent with our HELLO while we
// host (below 0 = none). hostTimeOfDay: the host's, while we join a player host and its HELLO carried one in the
// last 5 s. Any thread.
void setTimeOfDay(float seconds);
bool hostTimeOfDay(float& seconds);
// Our chat box is open: a flag in our STATE packets, shown by the others' name tags. Any thread.
void setTyping(bool typing);

// Summon ("come to me"; packets in protocol.h, rules in net/summon_policy.h). Any thread.
// Calls the players whose session ids are in `targets` (empty = everyone heard from) to `pos`/`yaw`.
// `hard` pulls them in after a countdown, and only works for those who allow it: the lobby host may
// always ask, a member only while canHardSummon(). False when nobody can be called, the position is
// not sane, or our last summon was too recent (summon::kSendCooldownMs).
bool sendSummon(const float pos[3], float yaw, const std::vector<uint32_t>& targets, bool hard);
bool summonOpen();    // our summon still waits for answers
uint32_t summonCooldownLeftMs(); // until sendSummon may be used again (0 = now)
void cancelSummon();  // withdraws our open summon (the markers at the others disappear)
// True when asking for a hard summon is worth it: we host, or the host allows members to (its
// config setting members_may_hard_summon, announced in its HELLO).
bool canHardSummon();
// Summons other players sent to us that we should show, oldest first. `hard` = the sender is trusted
// and we opted in (the UI still counts down and lets the player cancel). `cancel` = the sender
// withdrew summon `id` (only `id`, `from` and `name` are set).
struct SummonInfo {
    uint32_t id = 0;
    uint32_t from = 0; // the summoner's session id
    std::string name;
    float pos[3] = {0, 0, 0};
    float yaw = 0;
    bool hard = false;
    bool cancel = false;
};
std::vector<SummonInfo> takeSummons();
// Answers summon `id` of player `to` (protocol::SummonResult).
void sendSummonReply(uint32_t id, uint32_t to, uint8_t result);
// VISIT (protocol.h): asking someone who wants to be asked before others go to them, and the answers.
// sendVisit sends `kind` (protocol::VisitKind) to `target`; id 0 = a new request (its id is returned; 0 =
// not sent). takeVisits = the ones addressed to us, oldest first. Any thread.
struct VisitInfo {
    uint32_t id = 0;
    uint32_t from = 0; // the sender's session id
    std::string name;
    uint8_t kind = 0;
};
uint32_t sendVisit(uint32_t target, uint8_t kind, uint32_t id = 0);
std::vector<VisitInfo> takeVisits();
// What happened to our summon, as sentences for the notices: one line once everyone called answered
// or the time ran out.
std::vector<std::string> takeSummonReports();
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
void setLook(uint32_t look, uint8_t outfit = 0); // our look, sent to everyone (see RemoteInfo::look, outfit)
// Our EA online id and profile name, sent in HELLO so the others' maps can show our Runner Tag; id 0 = not shared.
void setOnlineProfile(uint64_t id, const char* name);
std::string joinCode(); // the host's code (empty until set up, or when not reachable)
std::string describe();

} // namespace mecmp::net
