#pragma once
// Wire format. All little-endian.
//
// Header (16 bytes): u32 magic "MECM" | u16 protocol | u16 type | u32 session | u32 seq
//
// type 1 STATE: f32 pos[3] | f32 yaw | u32 flags | u32 timeMs | pose
//   pose: u8 boneCount | u8 flags (bit0 = full) | u16 0 | u32 translation mask
//         | boneCount x u32 rotation (smallest-three: 2-bit index + 3 x 10 bits)
//         | per mask bit: 3 x i16 translation (1/1024 m)
//   Every pose carries all translations. STATE seq counts only movement snapshots; other packet types
//   use a separate sequence space. The trajectory bone is placed at pos by receivers.
// type 2 HELLO (about once a second): char name[24] (UTF-8, at most 23 bytes, NUL padded)
//   | u32 look (0 = Faith, else the game's echo CustomizationHash; absent from older versions = 0)
//   | u8 flags (kHelloHost, kHelloServer) | u8 wear (Faith's outfit changes, kWear...; absent = 0)
//   [| u64 online id | char[32] profile name] [| f32 time of day: a player host's clock, seconds since
//   midnight 0..86400; only a host sends it, joiners use it (game/time_of_day.h)]. Each part is optional; the
//   length says which are there (40 or 4 or 44 bytes after wear).
//   | u64 online id | char profile[32] (the player's EA online id and profile name, NUL padded; only when
//     the player shares them, for the game's map markers; absent = not shared)
// type 3 CHAT, type 4 PING, type 6 REACT, type 7 SUMMON and type 8 SUMMON_REPLY: see below.

#include <cstddef>
#include <cstdint>
#include <string>

#include "net/net.h"

namespace mecmp::net::protocol {

constexpr uint32_t kMagic = 0x4d43454d; // "MECM"
// 8: independent poses, movement-only sequence numbers, RTT probes and host-authored departures.
constexpr uint16_t kProtocol = 8;
constexpr uint16_t kTypeState = 1;
constexpr uint16_t kTypeHello = 2;
constexpr uint16_t kTypeChat = 3;
constexpr uint16_t kTypePing = 4; // a world marker
constexpr uint16_t kTypeReact = 6; // a quick answer to someone's marker (see REACT below)
constexpr uint16_t kTypeSummon = 7; // "come to me" (see SUMMON below)
constexpr uint16_t kTypeSummonReply = 8; // a summoned player's answer (see SUMMON_REPLY below)
constexpr uint16_t kTypeResult = 9; // a finished dash or other run, with its time (see RESULT below)
constexpr uint16_t kTypeActivity = 10; // the run the sender is in now, with its clock (see ACTIVITY below)
constexpr uint16_t kTypeVisit = 13; // "may I come to you?" and its answer (see VISIT below)
constexpr uint16_t kTypeAudio = 15; // native movement event + scalar inputs (player_audio.h)
constexpr uint16_t kTypeProbe = 16;
constexpr uint16_t kTypeDeparture = 17;
// 14 is RENDEZVOUS (below): not a session packet, so it is not in knownType.
// type 5 INFO: a server list's question "who are you?" and a dedicated server's answer. Not a session
// packet: it is not in knownType (never stored or forwarded), and a server answers it whatever the
// protocol in the header, so a list can say "other version" instead of nothing.
//   request (kInfoRequestSize bytes in all): u8 kind = 0 | zero padding. Padded so that the answer is
//     never bigger than the question (nobody can use a server to send more traffic at someone else).
//   answer: u8 kind = 1 | u8 players | u8 maxPlayers | u8 flags (0) | char name[24]. The header carries
//     the server's protocol and session, and the request's seq (to match it and time the round trip).
constexpr uint16_t kTypeInfo = 5;
constexpr size_t kInfoRequestSize = 80;
// Admission (protocol 7): a host or server does not let in a player it has not heard back from. A stranger's
// HELLO gets a CHALLENGE (u64 cookie); the stranger answers with an ANSWER carrying the same cookie, and only
// then is it a player. The cookie is a keyed hash of the stranger's address and the minute (the previous
// minute is accepted too), so nothing is kept for strangers. Neither type is stored or forwarded.
constexpr uint16_t kTypeChallenge = 11;
constexpr uint16_t kTypeAnswer = 12;
constexpr size_t kCookieLen = 8;
// type 14 RENDEZVOUS (hole punching; see Rendezvous below): between a player and a dedicated server acting as
// the go-between. Not a session packet: never stored or forwarded, and older versions drop it.
constexpr uint16_t kTypeRendezvous = 14;
// The packet types this version understands. Anything else is dropped before it can create state or
// be forwarded, so a new type needs no protocol bump: older versions simply ignore it.
constexpr bool knownType(uint16_t type) {
    return type == kTypeState || type == kTypeHello || type == kTypeChat || type == kTypePing || type == kTypeReact ||
           type == kTypeSummon || type == kTypeSummonReply || type == kTypeResult || type == kTypeActivity ||
           type == kTypeVisit || type == kTypeAudio || type == kTypeProbe || type == kTypeDeparture;
}
constexpr size_t kMaxChatBytes = 120; // UTF-8, after cleaning
constexpr size_t kNameLen = 24;
constexpr size_t kMaxPacket = 1024;

#pragma pack(push, 1)
struct Header {
    uint32_t magic;
    uint16_t protocol;
    uint16_t type;
    uint32_t session;
    uint32_t seq;
};
#pragma pack(pop)
static_assert(sizeof(Header) == 16, "header");

struct Probe {
    uint32_t target = 0, id = 0;
    bool reply = false;
};
enum class DepartureReason : uint8_t { Left, TimedOut, Kicked, Banned, SessionClosed };
struct Departure {
    uint32_t player = 0;
    DepartureReason reason = DepartureReason::Left;
};
size_t writeProbe(char* buf, const Header& h, const Probe& probe);
bool readProbe(const char* payload, size_t n, Probe& out);
size_t writeDeparture(char* buf, const Header& h, const Departure& departure);
bool readDeparture(const char* payload, size_t n, Departure& out);
const char* departureText(DepartureReason reason);
size_t writeAudio(char* buf, const Header& h, const PlayerAudio& sample);
bool readAudio(const char* payload, size_t size, PlayerAudio& out);


// Longest player name that fits the HELLO field (one byte is kept for the NUL terminator).
constexpr size_t kMaxNameBytes = kNameLen - 1;
// Length of the longest prefix of `s` that is at most `maxBytes` long and does not split a UTF-8 character.
size_t utf8PrefixLength(const std::string& s, size_t maxBytes);

// q and -q are the same rotation, and decoding may give either. The game blends replayed poses
// component by component, so a bone whose sign flips between two poses shakes on screen. Flips the
// rotations of `s` that point away from the same bone in `prev`, so consecutive poses stay continuous.
void alignRotations(PlayerState& s, const PlayerState& prev);

// Receivers replay a sender's poses on the sender's clock: our time = sender time + offset, where the
// offset is the smallest (arrival - sent) seen recently (one-way delay with the least queueing). Only
// the last `windowMs` count, so the estimate follows clocks that run slightly fast or slow.
struct DelayEstimator {
    static constexpr int kSamples = 64;
    static constexpr double kWindowMs = 2000.0;
    double at[kSamples] = {}, delta[kSamples] = {};
    int head = 0, fill = 0;
    // Adds (arrival, sent) in our and the sender's ms; returns the current offset.
    double add(double arrivalMs, double sentMs) {
        at[head] = arrivalMs;
        delta[head] = arrivalMs - sentMs;
        head = (head + 1) % kSamples;
        if (fill < kSamples) ++fill;
        double best = 1e300;
        for (int i = 0; i < fill; ++i)
            if (arrivalMs - at[i] <= kWindowMs && delta[i] < best) best = delta[i];
        return best;
    }
};

// A sender's 32-bit millisecond clock, made continuous: it wraps after 49.7 days of uptime, and a
// wrap must not look like a 49-day jump (B-4). Each new time is taken relative to the previous one,
// so steps of up to +-24 days in either direction come out right.
struct ClockUnwrap {
    bool have = false;
    uint32_t last = 0;
    double ext = 0;
    double extend(uint32_t t) {
        if (!have) { have = true; ext = t; }
        else ext += static_cast<double>(static_cast<int32_t>(t - last));
        last = t;
        return ext;
    }
};

// Share of a player's packets lost over the last few seconds, from gaps in their sequence numbers
// (a packet arriving out of order is dropped by the receiver, so it counts as lost too).
struct LossWindow {
    static constexpr int kSeconds = 5;
    int64_t second[kSeconds] = {};
    uint32_t got[kSeconds] = {}, lost[kSeconds] = {};
    // A packet arrived at `nowMs`; `missed` packets before it never did.
    void add(double nowMs, uint32_t missed) {
        const int64_t s = static_cast<int64_t>(nowMs / 1000.0);
        const int i = static_cast<int>(((s % kSeconds) + kSeconds) % kSeconds);
        if (second[i] != s) { second[i] = s; got[i] = 0; lost[i] = 0; }
        ++got[i];
        lost[i] += missed;
    }
    // Percent lost in the seconds before `nowMs` (0 when nothing was heard).
    float percent(double nowMs) const {
        const int64_t s = static_cast<int64_t>(nowMs / 1000.0);
        uint64_t g = 0, l = 0;
        for (int i = 0; i < kSeconds; ++i)
            if (second[i] > s - kSeconds && second[i] <= s) { g += got[i]; l += lost[i]; }
        return g + l ? 100.f * static_cast<float>(l) / static_cast<float>(g + l) : 0.f;
    }
};

// type 3 CHAT: u32 message id | u8 length | text (UTF-8, at most kMaxChatBytes). Each message is sent
// a few times (UDP may drop one); receivers keep the ids they have seen and show it once.
// Keeps what can be shown: no control characters (a message must not be able to forge log lines or
// move the cursor), at most kMaxChatBytes without splitting a UTF-8 character, no leading/trailing spaces.
std::string cleanChatText(const std::string& raw);
size_t writeChat(char* buf, const Header& h, uint32_t id, const std::string& text);
bool readChat(const char* payload, size_t n, uint32_t& id, std::string& text);

// Ids of the messages already handled, for packets that are sent several times (the newest N).
// Id 0 always counts as seen (the slots start as 0), so senders never use it.
template <size_t N> struct RecentIds {
    uint32_t ids[N] = {};
    size_t head = 0;
    bool seen(uint32_t id) const {
        for (uint32_t v : ids)
            if (v == id) return true;
        return false;
    }
    // True the first time `id` is offered (and remembers it).
    bool add(uint32_t id) {
        if (seen(id)) return false;
        ids[head] = id;
        head = (head + 1) % N;
        return true;
    }
};

// A world position a packet may carry: finite and within the level's range (as for STATE).
bool sanePosition(const float pos[3]);

// type 4 PING: u32 id | f32 pos[3] | u8 flags | u8 r, g, b | u8 labelLength | label | u8 life
//   | u8 accent r, g, b | u8 index.
// flags: bit 0 = remove, bits 1-6 = glyph (the shape drawn; values nobody draws yet show as glyph 0).
// The label is the marker's title (UTF-8, at most kMaxPingLabelBytes, cleaned like chat text); it is the
// sender's own wording, so receivers show it as it is. life = how long the marker stays, in units of
// 5 s (1..60; 0 = the default 30 s). accent = the colour of the detail inside the glyph.
// A player has kPingCells cells, each holding one marker: a PING puts its marker into cell `index`
// (replacing what was there), or with the remove bit empties that cell (index kPingAllCells: all of
// them). The sender decides which cell a new marker takes (choosePingCell); receivers only store.
// Sent a few times like CHAT. Every field after pos is optional, so older senders may stop early:
// after pos (flags = 0, the default colour), after the colour (no label), or after the label (30 s,
// white accent, cell 0; a remove without an index empties all cells).
constexpr uint8_t kPingClear = 1;
constexpr uint8_t kPingGlyphShift = 1;
constexpr uint8_t kPingGlyphs = 64; // bits 1-6
constexpr size_t kMaxPingLabelBytes = 32;
constexpr int kPingCells = 5;
constexpr uint8_t kPingAllCells = 0xFF;
constexpr uint32_t kPingLifeUnitMs = 5000;
constexpr uint32_t kPingDefaultLifeMs = 30000;
constexpr uint8_t kPingMaxLife = 60; // 5 minutes
// A marker is drawn this far above pos (pos began as the placing player's feet). A sender that
// wants its marker on an exact spot sends that spot lowered by this much.
constexpr float kPingLift = 1.2f;
inline uint8_t pingGlyph(uint8_t flags) { return static_cast<uint8_t>((flags >> kPingGlyphShift) & (kPingGlyphs - 1)); }
inline uint8_t withPingGlyph(uint8_t flags, uint8_t glyph) {
    return static_cast<uint8_t>((flags & ~((kPingGlyphs - 1) << kPingGlyphShift)) |
                                ((glyph & (kPingGlyphs - 1)) << kPingGlyphShift));
}
// The `life` byte -> milliseconds, clamped to what a marker may last (5 s .. 5 min).
inline uint32_t pingLifeMs(uint8_t life) {
    if (life == 0) return kPingDefaultLifeMs;
    return (life > kPingMaxLife ? kPingMaxLife : life) * kPingLifeUnitMs;
}
// Seconds -> the `life` byte (rounded to 5 s, 1..kPingMaxLife).
inline uint8_t pingLifeUnits(int seconds) {
    const int u = (seconds + 2) / 5;
    return static_cast<uint8_t>(u < 1 ? 1 : (u > kPingMaxLife ? kPingMaxLife : u));
}
// A marker title as it may go on the wire: like cleanChatText, but at most kMaxPingLabelBytes.
std::string cleanPingLabel(const std::string& raw);
struct Ping {
    uint32_t id = 0;
    float pos[3] = {0, 0, 0};
    uint8_t flags = 0;
    uint8_t rgb[3] = {232, 50, 46};
    std::string label; // cleaned; empty = none
    uint8_t life = 0;  // see pingLifeMs
    uint8_t accent[3] = {255, 255, 255};
    uint8_t index = 0; // the cell: 0..kPingCells-1, or kPingAllCells (remove only)
};
size_t writePing(char* buf, const Header& h, const Ping& p);
// False for a short packet or a position outside the sane range. `out.index` is always a valid cell
// (or kPingAllCells for a remove).
bool readPing(const char* payload, size_t n, Ping& out);

// Which of a player's cells a new marker goes into. "The same marker" = same glyph and title (what the
// others see). In order: the nearest live cell holding the same marker within kPingMergeMetres (it
// moves there, so pressing twice on one spot does not stack markers, while different markers may sit
// side by side); else a free cell while fewer than `limit` are live; else the live cell set longest ago.
constexpr float kPingMergeMetres = 1.5f;
struct PingCellView {
    bool live = false;
    uint8_t glyph = 0;
    std::string label;
    float pos[3] = {0, 0, 0};
    uint64_t order = 0; // grows with every placement: the smallest is the oldest
};
int choosePingCell(const PingCellView cells[kPingCells], int limit, uint8_t glyph, const std::string& label,
                   const float pos[3]);
// True when a marker at `pos` is the one in `cell` again (same glyph and title, within kPingMergeMetres).
bool samePingMarker(const PingCellView& cell, uint8_t glyph, const std::string& label, const float pos[3]);
// type 6 REACT: u32 id | u32 target | u8 cell | u8 titleLength | title | u8 textLength | text
//   [| u8 glyph | u8 rgb[3] | u8 accent[3]].
// The last seven bytes are the answer's icon and colors, shown under the marker for everyone; an answer from a
// version that does not send them is shown as the speech bubble, white.
// A quick answer to the marker in `cell` of the player whose session id is `target` ("On my way").
// `title` is that marker's title as the sender saw it (empty = none), so the owner can name it even if
// the marker was replaced meanwhile. Everyone receives it (the host and servers forward it like any
// packet); only `target` shows it. Sent a few times like PING, `id` makes the repeats the same event.
// Older versions do not know the type and drop it.
struct React {
    uint32_t id = 0;
    uint32_t target = 0;
    uint8_t cell = 0;   // 0..kPingCells-1
    std::string title;  // cleaned, at most kMaxPingLabelBytes; may be empty
    std::string text;   // cleaned, at most kMaxPingLabelBytes; never empty
    uint8_t glyph = 31; // the speech bubble
    uint8_t rgb[3] = {255, 255, 255};
    uint8_t accent[3] = {255, 255, 255};
};
size_t writeReact(char* buf, const Header& h, const React& r);
// False for a short or damaged packet, a cell out of range, or an empty answer.
bool readReact(const char* payload, size_t n, React& out);

// type 7 SUMMON: u32 id | f32 pos[3] | f32 yaw | u8 flags | u8 targetCount | u32 targets[targetCount].
// "Come to me": pos and yaw are where the sender stood when sending (a receiver goes there, not to
// wherever the sender is by then). targetCount 0 = everyone, else the session ids of the players called
// (at most kMaxSummonTargets); everyone receives the packet, only those called act on it. Sent a few times
// like PING, `id` makes the repeats the same event.
// flags: kSummonHard = "pull them in" (a receiver may still turn it into a plain summon: it is opt-in on
// their side and only honoured from the host, see below); kSummonVouched = the lobby host allows a hard
// summon from this sender; kSummonCancel = the sender withdraws summon `id`.
// kSummonVouched is only ever set by a player-host's relay. Whatever a sender wrote there is replaced
// (applyHostPolicy), and a receiver connected to a dedicated server (which forwards bytes as they are)
// must not trust it at all. Bits above kSummonKnownFlags are dropped on reading.
// Older versions do not know the type and drop it.
constexpr uint8_t kSummonHard = 1;
constexpr uint8_t kSummonVouched = 2;
constexpr uint8_t kSummonCancel = 4;
constexpr uint8_t kSummonKnownFlags = kSummonHard | kSummonVouched | kSummonCancel;
constexpr int kMaxSummonTargets = 16;
struct Summon {
    uint32_t id = 0;
    float pos[3] = {0, 0, 0};
    float yaw = 0;
    uint8_t flags = 0;
    uint8_t targetCount = 0; // 0 = everyone
    uint32_t targets[kMaxSummonTargets] = {};
};
size_t writeSummon(char* buf, const Header& h, const Summon& s);
// False for a short or damaged packet, a position outside the sane range, a non-finite yaw, or more
// targets than kMaxSummonTargets.
bool readSummon(const char* payload, size_t n, Summon& out);
// True when `session` is among those called (or the summon is for everyone). The sender never is.
bool summonCalls(const Summon& s, uint32_t session);
// What a player-host's relay does to a summon before it forwards it and acts on it itself: the sender is
// the host (always allowed) or a member (allowed only while `membersMayHard`). A hard summon from a
// member the host does not allow becomes a plain one; kSummonVouched is set exactly for an allowed
// hard summon from a member, and cleared otherwise.
void applyHostPolicy(Summon& s, bool senderIsHost, bool membersMayHard);
// Rewrites the flags byte inside an already written SUMMON packet of `n` bytes (what the relay forwards).
// False when `n` is too short to hold one.
bool patchSummonFlags(char* packet, size_t n, uint8_t flags);

// type 8 SUMMON_REPLY: u32 id | u32 target | u8 result.
// A called player's answer to summon `id`, for the player whose session id is `target` (the one who
// called). Everyone receives it; only `target` shows it. Sent a few times, `id` + sender make the
// repeats the same event.
enum SummonResult : uint8_t {
    kSummonDeclined = 0,    // said no, or the answer window ran out
    kSummonTeleporting = 1, // is on the way (accepted, or a hard summon's countdown ended)
    kSummonBusy = 2,        // could not go right now (loading, a menu, a teleport already running)
};
struct SummonReply {
    uint32_t id = 0;
    uint32_t target = 0;
    uint8_t result = kSummonDeclined;
};
size_t writeSummonReply(char* buf, const Header& h, const SummonReply& r);
// False for a short packet or a result code nobody knows.
bool readSummonReply(const char* payload, size_t n, SummonReply& out);

// type 13 VISIT: u32 id | u32 target | u8 kind.
// Go to someone who asks first (their HELLO has kHelloAskVisit): the visitor sends kVisitAsk to `target`, who
// answers with kVisitYes or kVisitNo for the same `id` (target = the visitor then); kVisitWithdraw = the visitor
// gave up asking. Everyone receives it, only `target` acts. Sent a few times, `id` + sender make the repeats
// one event. Older versions do not know the type and drop it; they never set kHelloAskVisit either, so
// nobody waits for them.
enum VisitKind : uint8_t { kVisitAsk = 0, kVisitYes = 1, kVisitNo = 2, kVisitWithdraw = 3 };
struct Visit {
    uint32_t id = 0;
    uint32_t target = 0;
    uint8_t kind = kVisitAsk;
};
size_t writeVisit(char* buf, const Header& h, const Visit& v);
// False for a short packet or a kind nobody knows.
bool readVisit(const char* payload, size_t n, Visit& out);

// type 14 RENDEZVOUS: u8 op | u8 0 | u16 port | u32 ip | u64 cookie (16 bytes; ip and port in host byte order).
// Hole punching without a relay: a dedicated server only introduces a host and a joiner to each other; their game
// traffic goes directly between them.
//   kRvRegister   host -> server, every ~20 s while hosting (keeps the host's router mapping open as well)
//   kRvIntroduce  joiner -> server: ip:port = the host it is joining (its join code)
//   kRvChallenge  server -> sender: cookie to repeat (proves the sender's address, as admission does)
//   kRvPeer       server -> host: ip:port = a joiner; server -> joiner: ip:port = the host, as the server sees it
//   kRvUnknown    server -> joiner: no host with that address is registered here
// Both sides then send to each other at once: the host a CHALLENGE (admission), the joiner its HELLO, and most
// home routers let both through. A host registers and a joiner asks only with the right cookie, so nobody can
// make the server (or a host) send to an address that did not ask.
enum RendezvousOp : uint8_t { kRvRegister = 0, kRvIntroduce = 1, kRvChallenge = 2, kRvPeer = 3, kRvUnknown = 4 };
constexpr size_t kRendezvousLen = 16;
struct Rendezvous {
    uint8_t op = kRvRegister;
    uint32_t ip = 0;
    uint16_t port = 0;
    uint64_t cookie = 0;
};
size_t writeRendezvous(char* buf, const Header& h, const Rendezvous& r);
bool readRendezvous(const char* payload, size_t n, Rendezvous& out); // false: short, or an op nobody knows

// "ffd23c" or "#ffd23c" -> r, g, b. False (rgb untouched) for anything else.
bool parseRgb(const std::string& hex, uint8_t rgb[3]);
std::string formatRgb(const uint8_t rgb[3]);

size_t writeState(char* buf, const Header& h, const PlayerState& s);

struct ServerInfo {
    uint8_t players = 0, maxPlayers = 0;
    std::string name;
};
size_t writeInfoRequest(char* buf, const Header& h);
size_t writeInfoAnswer(char* buf, const Header& h, const ServerInfo& info);
// Payload after the header. False if it is not a request / an answer.
bool isInfoRequest(const char* payload, size_t n);
bool readInfoAnswer(const char* payload, size_t n, ServerInfo& out);

// SipHash-2-4 (reference test vectors apply) of `len` bytes with a 128-bit key.
uint64_t siphash24(const uint8_t key[16], const uint8_t* in, size_t len);
// The cookie for a stranger at ip:port (host byte order) in `minute` (GetTickCount() / 60000).
uint64_t admissionCookie(const uint8_t key[16], uint32_t ip, uint16_t port, uint32_t minute);
// CHALLENGE and ANSWER both carry just the cookie after the header.
size_t writeCookie(char* buf, const Header& h, uint64_t cookie);
bool readCookie(const char* payload, size_t n, uint64_t& cookie);
// HELLO: char name[24] | u32 look | u8 flags (bit 0: the sender hosts this session; bit 1: the sender is
// a dedicated server, not a player: it sends no STATE and has no ghost). Older senders stop after the
// name or the look; missing fields read as 0.
constexpr uint8_t kHelloHost = 1;
constexpr uint8_t kHelloServer = 2;
// bit 2: only in a player-host's own HELLO: the host lets members start hard summons (see SUMMON), so a
// member's menu can say whether holding the summon key will pull players in. Like kHelloHost, it is
// cleared from what a host or a server forwards for a joiner, and a joiner reads it from the host only.
constexpr uint8_t kHelloMembersHard = 4;
// bit 3: the sender wants to be asked before anyone goes to them (VISIT). Passed on as it is.
constexpr uint8_t kHelloAskVisit = 8;
// `wear`: what a player whose look is 0 (Faith) changed about her outfit; 0 = nothing, each viewer
// shows her as in their own game. The bits are game::models::Wear. Hosts and servers pass the byte on
// with the rest of the packet; older versions do not send it and ignore it.
constexpr uint8_t kWearPrologue = 1;   // the prologue outfit instead of the runner's (the gear stays on)
constexpr uint8_t kWearTattoo = 2;     // the arm tattoo, whatever the story says
constexpr uint8_t kWearNoTattoo = 4;   // no arm tattoo (kWearTattoo wins if both are set)
constexpr uint8_t kWearNoMagRope = 8;  // without the MagRope and its upgrades
constexpr uint8_t kWearNoToolbag = 16; // without the tool bag
constexpr uint8_t kWearGridLink = 32;  // with the GridLink
constexpr size_t kHelloFlagsOffset = kNameLen + sizeof(uint32_t); // payload offset of the flags byte (after name and look)
constexpr size_t kProfileLen = 32;
struct OnlineProfile {
    uint64_t id = 0;            // 0 = not shared
    char name[kProfileLen] = {}; // NUL terminated
};
constexpr float kDaySeconds = 86400.f;
// `timeOfDay`: seconds since midnight (0..86400), or below 0 for none.
size_t writeHello(char* buf, const Header& h, const char* name, uint32_t look = 0, uint8_t flags = 0, uint8_t wear = 0,
                  const OnlineProfile* profile = nullptr, float timeOfDay = -1.f);
// HELLO payload (after the header). `name` = the raw field (cleaned by the caller); `look`, `flags` and
// `wear` = 0 if absent.
// `timeOfDay` = -1 if absent (or not a time).
bool readHello(const char* payload, size_t n, std::string& name, uint32_t& look, uint8_t* flags = nullptr,
               uint8_t* wear = nullptr, OnlineProfile* profile = nullptr, float* timeOfDay = nullptr);
// Decodes a complete STATE payload (after the header). No previous packet is needed.
bool readState(const char* payload, size_t n, PlayerState& out, bool& full);


// type 9 RESULT: u32 id | u32 contentId | u8 kind | u8 flags | i64 value | i64 delta | u8 titleLength | title.
// The sender finished a run: one of the game's named challenges (a dash, a delivery, ...). `value` is the
// main result in milliseconds, `delta` the improvement on the previous record (when kResultHadRecord).
// `title` is the run's name as the sender's game shows it (UTF-8, cleaned, at most kMaxResultTitleBytes).
// Shown to everyone (a notice) and posted by a server's Discord webhook. Sent a few times like PING; `id`
// makes the repeats one event. The sender's own game reports it: nothing checks that it is true.
enum ResultKind : uint8_t { kRunOther = 0, kRunDash = 1, kRunTimeTrial = 2, kRunDelivery = 3, kRunGridNode = 4,
                            kRunBillboard = 5, kRunMetaGrid = 6, kRunAssault = 7 };
constexpr uint8_t kResultNewRecord = 1; // better than the player's previous best
constexpr uint8_t kResultHadRecord = 2; // there was a previous best (delta is meaningful)
constexpr size_t kMaxResultTitleBytes = 64;
struct Result {
    uint32_t id = 0;
    uint32_t contentId = 0;
    uint8_t kind = kRunOther;
    uint8_t flags = 0;
    int64_t value = 0; // ms
    int64_t delta = 0; // ms
    std::string title;
};
size_t writeResult(char* buf, const Header& h, const Result& r);
bool readResult(const char* payload, size_t n, Result& out); // false: short, damaged or a negative time
const char* runKindName(uint8_t kind);    // "Dash", "Time trial", ... ("Run" for other)
std::string formatRunTime(int64_t ms);    // "1:23.45", "12.07", "1:02:03.40"
// "Meteor finished the dash The View in 1:23.45 (new record, 2.10 faster)" (plain text).
std::string resultText(const std::string& player, const Result& r);

// type 10 ACTIVITY: u8 kind | u32 elapsedMs | u8 titleLength | title.
// What the sender is doing now: kind kRunDash, kRunTimeTrial, ... while in a run (elapsedMs = how long it has
// been going when the packet left, title = its name), or kActivityNone once it ended. Sent about once a
// second while in a run and a few times after it ends; receivers count the clock on from the last packet
// and forget it when none arrived for a few seconds. Name tags show it ("Dash: The View 0:12.4").
constexpr uint8_t kActivityNone = 0xff;
struct Activity {
    uint8_t kind = kActivityNone;
    uint32_t elapsedMs = 0;
    std::string title;
};
size_t writeActivity(char* buf, const Header& h, const Activity& a);
bool readActivity(const char* payload, size_t n, Activity& out); // false: short or damaged

} // namespace mecmp::net::protocol
