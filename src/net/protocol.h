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

#include <cstddef>
#include <cstdint>
#include <string>

#include "net/net.h"

namespace mecmp::net::protocol {

constexpr uint32_t kMagic = 0x4d43454d; // "MECM"
constexpr uint16_t kProtocol = 5;
constexpr uint16_t kTypeState = 1;
constexpr uint16_t kTypeHello = 2;
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

size_t writeState(char* buf, const Header& h, const PlayerState& s, Encoder& enc);
size_t writeHello(char* buf, const Header& h, const char* name);
// Decodes a STATE payload (after the header). `full` tells whether to commit it as the new base.
bool readState(const char* payload, size_t n, const Decoder& dec, PlayerState& out, bool& full);
void commitBase(Decoder& dec, const PlayerState& s);

} // namespace mecmp::net::protocol
