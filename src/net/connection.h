#pragma once

#include <cstdint>

namespace mecmp::net {

constexpr uint32_t kConnectionTimeoutMs = 10000;
constexpr uint32_t kProbeEveryMs = 2000;
constexpr uint32_t kProbeSpacingMs = 250;
constexpr uint32_t kProbeFreshMs = 6000;

// Header::seq of STATE packets: each sender numbers its STATE packets on their own (other packet types
// have their own counter), so a gap is a lost STATE and an older number is a stale or repeated one.
// Hosts and relays forward a STATE with the sender's number. Wraps after 2^32 packets: a step forward
// of less than half the range is newer.
struct StateSequence {
    bool have = false;
    uint32_t last = 0;

    bool accepts(uint32_t seq) const {
        const uint32_t step = seq - last;
        return !have || (step != 0 && step < 0x80000000u);
    }
    // Takes `seq` as the newest; returns how many STATE packets before it never arrived.
    uint32_t commit(uint32_t seq) {
        const uint32_t missed = have ? seq - last - 1 : 0;
        have = true;
        last = seq;
        return missed;
    }
};

struct Heartbeat {
    bool heard = false;
    uint32_t at = 0;

    void receive(uint32_t now) { heard = true; at = now; }
    bool connected(uint32_t now) const { return heard && now - at < kConnectionTimeoutMs; }
};

struct RoundTrip {
    bool pending = false, sampled = false;
    uint32_t id = 0, sentAt = 0, sampledAt = 0, ms = 0;

    bool due(uint32_t now) const {
        if (pending) return now - sentAt >= kProbeFreshMs;
        return !sampled || now - sentAt >= kProbeEveryMs;
    }
    void request(uint32_t token, uint32_t now) { pending = true; id = token; sentAt = now; }
    bool reply(uint32_t token, uint32_t now) {
        if (!pending || token != id || now - sentAt > kProbeFreshMs) return false;
        ms = now - sentAt;
        sampledAt = now;
        sampled = true;
        pending = false;
        return true;
    }
    int value(uint32_t now, uint32_t maxAge = kProbeFreshMs) const {
        return sampled && now - sampledAt < maxAge ? static_cast<int>(ms) : -1;
    }
};

} // namespace mecmp::net
