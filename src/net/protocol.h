#pragma once
// Wire format. All little-endian.
//
// Header (16 bytes): u32 magic "MECM" | u16 protocol | u16 type | u32 session | u32 seq
//
// type 1 STATE: f32 pos[3] | f32 yaw | u32 flags | u32 timeMs | pose
//   pose: u8 boneCount | u8 flags (bit0 = full) | u16 0 | u32 translation mask
//         | boneCount x u32 rotation (smallest-three: 2-bit index + 3 x 10 bits)
//         | per mask bit: 3 x i16 translation (1/1024 m)
//   Bones whose translation is not in the packet keep the last full packet's value (a full packet,
//   with every translation, goes out every kFullEvery packets). The trajectory bone's translation
//   is world space and is clamped; receivers place it at pos instead.
// type 2 HELLO (about once a second): char name[24] (UTF-8, at most 23 bytes, NUL padded)
//   | u32 look (0 = Faith, else the game's echo CustomizationHash; absent from older versions = 0)
// type 3 CHAT and type 4 PING: see below.

#include <cstddef>
#include <cstdint>
#include <string>

#include "net/net.h"

namespace mecmp::net::protocol {

constexpr uint32_t kMagic = 0x4d43454d; // "MECM"
constexpr uint16_t kProtocol = 6;
constexpr uint16_t kTypeState = 1;
constexpr uint16_t kTypeHello = 2;
constexpr uint16_t kTypeChat = 3;
constexpr uint16_t kTypePing = 4; // a world marker
// The packet types this version understands. Anything else is dropped before it can create state or
// be forwarded, so a new type needs no protocol bump: older versions simply ignore it.
constexpr bool knownType(uint16_t type) {
    return type == kTypeState || type == kTypeHello || type == kTypeChat || type == kTypePing;
}
constexpr size_t kMaxChatBytes = 120; // UTF-8, after cleaning
constexpr size_t kNameLen = 24;
constexpr uint32_t kFullEvery = 30;
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

struct Encoder { // sender side: translations of the last full packet
    int16_t lastFullT[kMaxPoseBones][3] = {};
    uint32_t lastFullCount = 0;
    uint32_t sinceFull = kFullEvery;
};
struct Decoder { // per remote player
    float baseT[kMaxPoseBones][3] = {};
    uint32_t baseCount = 0;
};

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
    uint8_t rgb[3] = {255, 210, 60};
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
// "ffd23c" or "#ffd23c" -> r, g, b. False (rgb untouched) for anything else.
bool parseRgb(const std::string& hex, uint8_t rgb[3]);
std::string formatRgb(const uint8_t rgb[3]);

size_t writeState(char* buf, const Header& h, const PlayerState& s, Encoder& enc);
// HELLO: char name[24] | u32 look | u8 flags (bit 0: the sender hosts this session; bit 1: the sender is
// a dedicated server, not a player: it sends no STATE and has no ghost). Older senders stop after the
// name or the look; missing fields read as 0.
constexpr uint8_t kHelloHost = 1;
constexpr uint8_t kHelloServer = 2;
constexpr size_t kHelloFlagsOffset = kNameLen + sizeof(uint32_t); // payload offset of the flags byte (after name and look)
size_t writeHello(char* buf, const Header& h, const char* name, uint32_t look = 0, uint8_t flags = 0);
// HELLO payload (after the header). `name` = the raw field (cleaned by the caller); `look` = 0 if absent.
bool readHello(const char* payload, size_t n, std::string& name, uint32_t& look, uint8_t* flags = nullptr);
// Decodes a STATE payload (after the header). `full` tells whether to commit it as the new base.
bool readState(const char* payload, size_t n, const Decoder& dec, PlayerState& out, bool& full);
void commitBase(Decoder& dec, const PlayerState& s);

} // namespace mecmp::net::protocol
