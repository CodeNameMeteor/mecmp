#pragma once
// The rules a hosting player applies to the players who join, kept apart from the sockets so they can
// be tested on any system (tests/host_guard_test.cpp). net/net.cpp does the bookkeeping.
//
//   - at most kMaxPerIp players from one IP address: one machine cannot take every slot of a session
//     (the dedicated server uses the same limit)
//   - each player may send at most kRatePerSec packets a second (burst kBurst): the host forwards every
//     packet to the other players, so an unlimited sender would multiply into everyone's download

#include <cstdint>

namespace mecmp::net::guard {

constexpr uint32_t kMaxPerIp = 4;
constexpr double kRatePerSec = 80; // a player sends about 30 states, 1 hello and a few chat/marker packets a second
constexpr double kBurst = 160;

// Token bucket for one player. `nowMs` is any millisecond clock; wrap-around is handled.
struct Bucket {
    double tokens = kBurst;
    uint32_t at = 0;
    // True if one packet may pass now (and takes a token).
    bool take(uint32_t nowMs) {
        const double refill = static_cast<double>(static_cast<uint32_t>(nowMs - at)) * kRatePerSec / 1000.0;
        tokens = tokens + refill > kBurst ? kBurst : tokens + refill;
        at = nowMs;
        if (tokens < 1) return false;
        tokens -= 1;
        return true;
    }
};

} // namespace mecmp::net::guard
