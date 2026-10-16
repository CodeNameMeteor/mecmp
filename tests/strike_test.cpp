// Tests for game/strike_math.h:
//   g++ -std=c++17 -Isrc tests/strike_test.cpp -o strike_test && ./strike_test
#include <cmath>
#include <cstdio>

#include "game/strike_math.h"

using namespace mecmp::strike;

static int g_failures = 0;
#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            std::printf("FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                 \
        }                                                                 \
    } while (0)

// A striker at the origin facing +z (yaw 0).
static Pose atOrigin(float yaw = 0.f) { return Pose{{0.f, 0.f, 0.f}, yaw}; }

int main() {
    // Straight ahead: within reach hits, beyond reach + both radii misses.
    {
        const float near1[3] = {0, 0, 1.0f}, edge[3] = {0, 0, 1.9f}, far1[3] = {0, 0, 2.3f};
        CHECK(hits(atOrigin(), kPunch, near1, true, 0.f));
        CHECK(hits(atOrigin(), kPunch, edge, true, 0.f));  // 1.2 + 0.4 + 0.3 = 1.9
        CHECK(!hits(atOrigin(), kPunch, far1, true, 0.f));
        CHECK(hits(atOrigin(), kKick, far1, true, 0.f));   // a kick reaches 1.8 + 0.7 = 2.5
    }
    // Sideways and behind.
    {
        const float side[3] = {1.5f, 0, 0}, behind[3] = {0, 0, -0.5f};
        CHECK(!hits(atOrigin(), kPunch, side, true, 0.f));
        CHECK(!hits(atOrigin(), kPunch, behind, true, 0.f));
        CHECK(hits(atOrigin(), kPunch, behind, false, 0.f)); // the receiver's check ignores the facing
    }
    // Yaw follows the mod's convention: forward = {sin yaw, 0, cos yaw}.
    {
        const float east[3] = {1.0f, 0, 0};
        CHECK(hits(atOrigin(1.5707963f), kPunch, east, true, 0.f));
        CHECK(!hits(atOrigin(-1.5707963f), kPunch, east, true, 0.f));
    }
    // Height: the target's capsule is feet..feet + 1.8; a punch is at 1.1 above the striker's feet.
    {
        const float above[3] = {0, 3.0f, 1.0f}, below[3] = {0, -3.0f, 1.0f}, rooftop[3] = {0, 0.5f, 1.0f},
                    justAbove[3] = {0, -0.5f, 1.0f};
        CHECK(!hits(atOrigin(), kPunch, above, true, 0.f));
        CHECK(!hits(atOrigin(), kPunch, below, true, 0.f));
        CHECK(hits(atOrigin(), kPunch, rooftop, true, 0.f));
        CHECK(hits(atOrigin(), kPunch, justAbove, true, 0.f)); // capsule reaches up to 1.3 over the striker's feet
    }
    // Non-finite input never hits.
    {
        const float bad[3] = {NAN, 0, 1.0f}, ok[3] = {0, 0, 1.0f};
        CHECK(!hits(atOrigin(), kPunch, bad, true, 0.f));
        Pose p = atOrigin();
        p.yaw = INFINITY;
        CHECK(!hits(p, kPunch, ok, true, 0.f));
    }
    // Overlapping players (inside the capsule) count whichever way the striker faces.
    {
        const float on[3] = {0.1f, 0, 0.1f};
        CHECK(hits(atOrigin(3.14159f), kPunch, on, true, 0.f));
    }
    // The receiver's check is generous, but not unlimited.
    {
        const float me[3] = {0, 0, 3.0f}, farAway[3] = {0, 0, 9.0f};
        CHECK(plausible(atOrigin(), kPunch, me));
        CHECK(!plausible(atOrigin(), kPunch, farAway));
        CHECK(!plausible(atOrigin(), kKick, farAway));
    }
    // The nearest touched candidate wins; ids of 0 and players out of reach are skipped.
    {
        const Candidate c[] = {{5, {0, 0, 1.8f}}, {7, {0, 0, 0.9f}}, {0, {0, 0, 0.5f}}, {9, {5, 0, 0}}};
        CHECK(pickTarget(atOrigin(), kPunch, c, 4) == 7);
        CHECK(pickTarget(atOrigin(), kPunch, c, 1) == 5);
        CHECK(pickTarget(atOrigin(), kPunch, c + 3, 1) == 0);
        CHECK(pickTarget(atOrigin(), kPunch, c, 0) == 0);
    }
    // Health after a hit.
    {
        bool killed = true;
        CHECK(afterHit(6.f, 0, &killed) == 6.f && !killed);
        CHECK(afterHit(6.f, 1, &killed) == 5.f && !killed);
        CHECK(afterHit(1.5f, 1, &killed) == 1.f && !killed);
        CHECK(afterHit(1.f, 1, &killed) == 1.f && !killed);
        CHECK(afterHit(0.5f, 1, &killed) == 0.5f && !killed);
        CHECK(afterHit(6.f, 2, &killed) == 5.f && !killed);
        CHECK(afterHit(1.f, 2, &killed) == 0.f && killed);
        CHECK(afterHit(0.f, 2, &killed) == 0.f && !killed);
        CHECK(afterHit(-3.f, 2, &killed) == -3.f && !killed);
    }
    // Cooldowns, including a wrapping clock.
    {
        CHECK(elapsed(1000, 400, 600));
        CHECK(!elapsed(999, 400, 600));
        CHECK(elapsed(100, 0xFFFFFF00u, 300));
        CHECK(!elapsed(50, 0xFFFFFF00u, 400));
        HitLimiter l;
        CHECK(l.allow(1, 1000));
        CHECK(!l.allow(1, 1100));  // the global gap
        CHECK(!l.allow(2, 1100));
        CHECK(l.allow(2, 1250));   // another player after the global gap (200 ms)
        CHECK(!l.allow(1, 1300));  // inside the global gap again
        CHECK(l.allow(7, 5000));
        CHECK(!l.allow(7, 5300));  // the same player inside its own 350 ms (the global gap has passed)
        CHECK(l.allow(7, 5350));
        // More senders than slots: the oldest is forgotten, nothing breaks.
        uint32_t t = 10000;
        for (uint32_t id = 100; id < 140; ++id, t += 250) CHECK(l.allow(id, t));
        CHECK(l.allow(100, t + 10000));
    }
    if (g_failures) {
        std::printf("%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("strike: all checks passed\n");
    return 0;
}
