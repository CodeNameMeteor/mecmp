#pragma once

#include <cstdint>

namespace mecmp::net {

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

} // namespace mecmp::net
