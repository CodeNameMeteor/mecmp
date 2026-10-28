#pragma once
// The start of a replay recording as the game writes it (ReplaySetup_WriteHeader 0x1439516b0) and reads it
// (ReplayPlayback_ReadRecordingHeader 0x14394e560): a bit stream read 32 bits at a time, big-endian, that starts
// with the version (5), a hash of the bone setup and the bone count. No game access, so the host-side test
// (tests/replay_header_test.cpp) covers it.

#include <cstddef>
#include <cstdint>

namespace mecmp::game::replay_header {

constexpr uint32_t kVersion = 5;
// The game's own recordings: the recorder's 6 RecordingBones with their ancestors. Every run the game records has
// these; earlier MECMP betas recorded 85.
constexpr uint32_t kGameBones = 23;

struct Header {
    uint32_t version = 0;
    uint32_t bones = 0;
};

inline uint32_t readBigEndian(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) | (static_cast<uint32_t>(p[2]) << 8) |
           static_cast<uint32_t>(p[3]);
}

// False when the data is too short to hold a header.
inline bool parse(const uint8_t* data, size_t size, Header& out) {
    if (!data || size < 12) return false;
    out.version = readBigEndian(data);
    out.bones = readBigEndian(data + 8);
    return true;
}

// A recording the game's ghosts cannot play: more bones than the game records. The game's ghost players are built
// for its own 23 and its loader keeps a ghost's keyframe buffer between recordings, so such a run T-poses the ghost
// and overruns the heap. Recordings of another version are left to the game (it rejects them itself).
inline bool tooManyBones(const Header& h) { return h.version == kVersion && h.bones > kGameBones; }

} // namespace mecmp::game::replay_header
