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
// type 2 HELLO (about once a second): char name[24] (UTF-8, NUL padded)

#include <cstddef>
#include <cstdint>

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

size_t writeState(char* buf, const Header& h, const PlayerState& s, Encoder& enc);
size_t writeHello(char* buf, const Header& h, const char* name);
// Decodes a STATE payload (after the header). `full` tells whether to commit it as the new base.
bool readState(const char* payload, size_t n, const Decoder& dec, PlayerState& out, bool& full);
void commitBase(Decoder& dec, const PlayerState& s);

} // namespace mecmp::net::protocol
