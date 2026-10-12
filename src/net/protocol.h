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

#include <cstddef>
#include <cstdint>
#include <string>

#include "net/net.h"

namespace mecmp::net::protocol {

constexpr uint32_t kMagic = 0x4d43454d; // "MECM"
constexpr uint16_t kProtocol = 5;
constexpr uint16_t kTypeState = 1;
constexpr uint16_t kTypeHello = 2;
constexpr uint16_t kTypeChat = 3;
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

size_t writeState(char* buf, const Header& h, const PlayerState& s, Encoder& enc);
// HELLO: char name[24] | u32 look | u8 flags (bit 0: the sender hosts this session). Older senders stop
// after the name or the look; missing fields read as 0.
constexpr uint8_t kHelloHost = 1;
constexpr size_t kHelloFlagsOffset = kNameLen + sizeof(uint32_t); // payload offset of the flags byte (after name and look)
size_t writeHello(char* buf, const Header& h, const char* name, uint32_t look = 0, uint8_t flags = 0);
// HELLO payload (after the header). `name` = the raw field (cleaned by the caller); `look` = 0 if absent.
bool readHello(const char* payload, size_t n, std::string& name, uint32_t& look, uint8_t* flags = nullptr);
// Decodes a STATE payload (after the header). `full` tells whether to commit it as the new base.
bool readState(const char* payload, size_t n, const Decoder& dec, PlayerState& out, bool& full);
void commitBase(Decoder& dec, const PlayerState& s);

} // namespace mecmp::net::protocol
