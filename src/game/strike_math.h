#pragma once
// Player-versus-player strikes, as plain maths (no engine calls, unit-tested on Linux: tests/strike_test.cpp).
//
// Ghosts have no collision, so a "hit" is decided from positions: the striker's reach is a short horizontal
// segment in front of the striker (at chest height for a punch, lower for a kick), thickened by a small
// radius; the target is the player's capsule (a vertical axis from the feet up, with a radius). They touch
// when the distance between the two axes is at most the sum of the radii. Positions are the players' feet
// (the trajectory bone) and yaw is the facing used everywhere in the mod (forward = {sin yaw, 0, cos yaw}).

#include <cmath>
#include <cstddef>
#include <cstdint>

namespace mecmp::strike {

constexpr float kTargetRadius = 0.4f;    // the capsule of a standing runner
constexpr float kTargetHeight = 1.8f;    // feet to the top of the head
constexpr float kPunchReach = 1.2f;      // from the striker's centre
constexpr float kKickReach = 1.8f;
constexpr float kPunchHeight = 1.1f;     // chest height above the feet
constexpr float kKickHeight = 0.7f;
constexpr float kStrikeRadius = 0.3f;    // the reach is a segment this thick
constexpr float kMaxFacingCos = 0.2588f; // cos(75 degrees): the target must be within +-75 degrees of the facing
constexpr float kReceiveSlack = 1.0f;    // the receiver's allowance for latency and for the sender's older position
constexpr uint32_t kSendCooldownMs = 400;    // one strike per this long from the striker (2 to 2.5 swings a second)
constexpr uint32_t kReceiveCooldownMs = 350; // one hit per this long from one player
constexpr uint32_t kGlobalCooldownMs = 200;  // ...and from everyone together (a flood limit)

enum Kind : uint8_t { kPunch = 0, kKick = 1 }; // = net::protocol::DamageKind

struct Pose {
    float pos[3];
    float yaw;
};

inline float reach(uint8_t kind) { return kind == kKick ? kKickReach : kPunchReach; }
inline float height(uint8_t kind) { return kind == kKick ? kKickHeight : kPunchHeight; }

// Distance from the point (px,pz) to the segment from (ax,az) to (bx,bz), on the ground plane.
inline float pointSegment2d(float px, float pz, float ax, float az, float bx, float bz) {
    const float dx = bx - ax, dz = bz - az;
    const float len2 = dx * dx + dz * dz;
    float t = len2 > 0.f ? ((px - ax) * dx + (pz - az) * dz) / len2 : 0.f;
    t = t < 0.f ? 0.f : (t > 1.f ? 1.f : t);
    const float cx = ax + t * dx - px, cz = az + t * dz - pz;
    return std::sqrt(cx * cx + cz * cz);
}

// True when a strike of `kind` by `striker` touches a player standing at `target`. With `needFacing` the
// target must also be in front of the striker; the receiver's check leaves that out (the sender's facing it
// knows is older) and adds `slack` metres instead.
inline bool hits(const Pose& striker, uint8_t kind, const float target[3], bool needFacing, float slack) {
    for (int i = 0; i < 3; ++i)
        if (!std::isfinite(striker.pos[i]) || !std::isfinite(target[i])) return false;
    if (!std::isfinite(striker.yaw)) return false;
    const float fx = std::sin(striker.yaw), fz = std::cos(striker.yaw);
    const float r = reach(kind);
    const float ox = target[0] - striker.pos[0], oz = target[2] - striker.pos[2];
    const float h = std::sqrt(ox * ox + oz * oz);
    if (needFacing && h > kTargetRadius && (ox * fx + oz * fz) / h < kMaxFacingCos) return false;
    const float horizontal = pointSegment2d(target[0], target[2], striker.pos[0], striker.pos[2],
                                            striker.pos[0] + fx * r, striker.pos[2] + fz * r);
    const float y = striker.pos[1] + height(kind);
    const float vertical = y < target[1] ? target[1] - y : (y > target[1] + kTargetHeight ? y - target[1] - kTargetHeight : 0.f);
    const float limit = kTargetRadius + kStrikeRadius + slack;
    return horizontal * horizontal + vertical * vertical <= limit * limit;
}

// The receiver's check from the sender's last known position (no facing, with the latency allowance).
inline bool plausible(const Pose& striker, uint8_t kind, const float me[3]) {
    return hits(striker, kind, me, false, kReceiveSlack + reach(kind));
}

struct Candidate {
    uint32_t id;
    float pos[3];
};

// The nearest candidate the strike touches (0 = none). Candidate ids must not be 0.
inline uint32_t pickTarget(const Pose& striker, uint8_t kind, const Candidate* c, size_t n) {
    uint32_t best = 0;
    float bestD = 1e30f;
    for (size_t i = 0; i < n; ++i) {
        if (!c[i].id || !hits(striker, kind, c[i].pos, true, 0.f)) continue;
        const float dx = c[i].pos[0] - striker.pos[0], dz = c[i].pos[2] - striker.pos[2];
        if (dx * dx + dz * dz < bestD) { bestD = dx * dx + dz * dz; best = c[i].id; }
    }
    return best;
}

// The health after one hit. mode: 0 off, 1 never below 1, 2 lethal (config::kHits...). A player already at or
// below 0 is not touched; `killed` says the hit took the last point.
inline float afterHit(float hp, int mode, bool* killed = nullptr) {
    if (killed) *killed = false;
    if (mode <= 0 || !(hp > 0.f)) return hp;
    if (mode == 1) return hp <= 1.f ? hp : (hp - 1.f < 1.f ? 1.f : hp - 1.f);
    const float next = hp - 1.f;
    if (!(next > 0.f)) {
        if (killed) *killed = true;
        return 0.f;
    }
    return next;
}

// Millisecond cooldowns on a wrapping 32-bit clock.
inline bool elapsed(uint32_t now, uint32_t last, uint32_t gapMs) {
    return static_cast<int32_t>(now - last) >= static_cast<int32_t>(gapMs);
}

// Receiver side: at most one hit per sender per kReceiveCooldownMs and one per kGlobalCooldownMs overall.
struct HitLimiter {
    static constexpr size_t kSlots = 16;
    uint32_t sender[kSlots] = {};
    uint32_t at[kSlots] = {};
    bool used[kSlots] = {};
    uint32_t globalAt = 0;
    bool haveGlobal = false;
    // True when a hit from `from` at `now` may land (and counts it).
    bool allow(uint32_t from, uint32_t now) {
        if (haveGlobal && !elapsed(now, globalAt, kGlobalCooldownMs)) return false;
        size_t slot = kSlots;
        for (size_t i = 0; i < kSlots; ++i)
            if (used[i] && sender[i] == from) { slot = i; break; }
        if (slot < kSlots) {
            if (!elapsed(now, at[slot], kReceiveCooldownMs)) return false;
        } else {
            size_t oldest = 0;
            for (size_t i = 0; i < kSlots; ++i) {
                if (!used[i]) { oldest = i; break; }
                if (static_cast<int32_t>(at[i] - at[oldest]) < 0) oldest = i;
            }
            slot = oldest;
        }
        used[slot] = true;
        sender[slot] = from;
        at[slot] = now;
        globalAt = now;
        haveGlobal = true;
        return true;
    }
};

} // namespace mecmp::strike
