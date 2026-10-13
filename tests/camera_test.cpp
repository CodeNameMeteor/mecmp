// Linux test of the name-tag projection and the screen-edge pointers (game/camera_math.h).
#include <cmath>
#include <cstdio>

#include "game/camera_math.h"

static int g_failures = 0;
#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) {                                                 \
            std::printf("FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                              \
        }                                                              \
    } while (0)

static bool near(float a, float b, float eps = 0.5f) { return std::fabs(a - b) < eps; }

int main() {
    using mecmp::game::CameraView;
    using mecmp::game::projectToScreen;
    // Camera at the origin looking down -Z: Frostbite's forward row (+Z here) points backwards.
    CameraView c{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {0, 0, 0}, 90.f};
    float sx = 0, sy = 0, d = 0;
    const float ahead[3] = {0, 0, -10};
    CHECK(projectToScreen(c, ahead, 1920, 1080, sx, sy, d));
    CHECK(near(sx, 960) && near(sy, 540) && near(d, 10));
    // 90 degree vertical FOV: a point as high as it is far sits at the top edge.
    const float top[3] = {0, 10, -10};
    CHECK(projectToScreen(c, top, 1920, 1080, sx, sy, d) && near(sy, 0) && near(sx, 960));
    // Right of the camera -> right half of the screen; the edge is at tan(fovX/2) = 16/9.
    const float right[3] = {10 * 1920.f / 1080.f, 0, -10};
    CHECK(projectToScreen(c, right, 1920, 1080, sx, sy, d) && near(sx, 1920));
    // Behind the camera: not drawn.
    const float behind[3] = {0, 0, 5};
    CHECK(!projectToScreen(c, behind, 1920, 1080, sx, sy, d));
    // The camera from the game log (forward row -0.108 0.013 -0.994): it looks along +0.108 -0.013 +0.994.
    CameraView g{{-0.994f, -0.001f, 0.108f}, {0.001f, 1.f, 0.013f}, {-0.108f, 0.013f, -0.994f}, {56.42f, 68.52f, 168.73f}, 55.f};
    const float infront[3] = {56.42f + 0.108f * 20, 68.52f - 0.013f * 20, 168.73f + 0.994f * 20};
    CHECK(projectToScreen(g, infront, 1920, 1080, sx, sy, d) && near(sx, 960, 2) && near(sy, 540, 2) && near(d, 20, 0.1f));
    // A point to the camera's right (+right vector) lands right of centre.
    const float toRight[3] = {infront[0] - 0.994f * 3, infront[1], infront[2] + 0.108f * 3};
    CHECK(projectToScreen(g, toRight, 1920, 1080, sx, sy, d) && sx > 1000);

    // --- Distance fade ---
    using mecmp::game::distanceFade;
    CHECK(distanceFade(0) == 1.f && distanceFade(40) == 1.f && distanceFade(150) == 0.f && distanceFade(1000) == 0.f);
    CHECK(near(distanceFade(95), 0.5f, 0.01f));
    using mecmp::game::kMarkerFadeFullUntil;
    using mecmp::game::kMarkerFadeGoneAt;
    CHECK(distanceFade(50, kMarkerFadeFullUntil, kMarkerFadeGoneAt) == 1.f && distanceFade(300, kMarkerFadeFullUntil, kMarkerFadeGoneAt) == 0.f);
    CHECK(near(distanceFade(175, kMarkerFadeFullUntil, kMarkerFadeGoneAt), 0.5f, 0.01f));

    // --- Pointers on the screen's edge (markers, off-screen players) ---
    using mecmp::game::EdgeIndicator;
    using mecmp::game::edgeIndicator;
    const float W = 1920, H = 1080, M = 40;
    EdgeIndicator e;
    // On screen: the point itself.
    CHECK(edgeIndicator(c, ahead, W, H, M, e) && e.onScreen && near(e.x, 960) && near(e.y, 540) && near(e.distance, 10));
    // Ahead but far to the right: the right edge, at the same height as the centre, pointing right.
    const float farRight[3] = {100, 0, -10};
    CHECK(edgeIndicator(c, farRight, W, H, M, e) && !e.onScreen && near(e.x, W - M) && near(e.y, 540));
    CHECK(near(e.dirX, 1, 0.01f) && near(e.dirY, 0, 0.01f));
    // Far above: the top edge (screen y grows downwards, so the direction's y is negative).
    const float farUp[3] = {0, 100, -10};
    CHECK(edgeIndicator(c, farUp, W, H, M, e) && !e.onScreen && near(e.y, M) && near(e.x, 960) && near(e.dirY, -1, 0.01f));
    // Left and below.
    const float farLeft[3] = {-100, 0, -10};
    CHECK(edgeIndicator(c, farLeft, W, H, M, e) && !e.onScreen && near(e.x, M) && near(e.y, 540));
    const float farDown[3] = {0, -100, -10};
    CHECK(edgeIndicator(c, farDown, W, H, M, e) && !e.onScreen && near(e.y, H - M) && near(e.x, 960));
    // Behind the camera and to the left: still the left edge (not mirrored).
    const float behindLeft[3] = {-5, 0, 20};
    CHECK(edgeIndicator(c, behindLeft, W, H, M, e) && !e.onScreen && near(e.x, M) && near(e.y, 540) && e.dirX < -0.99f);
    // Behind and up-right: a corner direction, clamped onto the border of the inset rectangle.
    const float behindUpRight[3] = {5, 5, 20};
    CHECK(edgeIndicator(c, behindUpRight, W, H, M, e) && !e.onScreen && e.dirX > 0 && e.dirY < 0);
    CHECK(e.x >= M - 0.5f && e.x <= W - M + 0.5f && e.y >= M - 0.5f && e.y <= H - M + 0.5f);
    CHECK(near(e.y, M)); // 45 degrees on a wide screen meets the top edge first
    // Exactly behind: no direction of its own, so it points down.
    CHECK(edgeIndicator(c, behind, W, H, M, e) && !e.onScreen && near(e.x, 960) && near(e.y, H - M) && e.dirY == 1.f);
    // Just inside the margin band counts as off screen (the spot stays inside the margin).
    const float nearEdge[3] = {10 * 1920.f / 1080.f * 0.99f, 0, -10};
    CHECK(edgeIndicator(c, nearEdge, W, H, M, e) && !e.onScreen && near(e.x, W - M));
    // The real camera: a point to its right and behind it points right.
    const float gBehindRight[3] = {56.42f - 0.108f * 20 - 0.994f * 5, 68.52f, 168.73f - 0.994f * 20 + 0.108f * 5};
    CHECK(edgeIndicator(g, gBehindRight, W, H, M, e) && !e.onScreen && e.dirX > 0.9f && near(e.x, W - M));
    // Nothing to draw on: no screen, a margin that leaves no room, or a position that is not a number.
    CHECK(!edgeIndicator(c, ahead, 0, 0, M, e));
    CHECK(!edgeIndicator(c, ahead, 60, 60, 40, e));
    const float nan3[3] = {std::nanf(""), 0, 0};
    CHECK(!edgeIndicator(c, nan3, W, H, M, e));
    // The arrival pulse: the rings start at once and only spread, the marker blinks white twice (two
    // separate peaks), and everything ends at rest.
    {
        using mecmp::game::kPulseMs;
        using mecmp::game::pingPulse;
        const auto at = [](float frac) { return pingPulse(static_cast<unsigned>(frac * kPulseMs)); };
        CHECK(near(pingPulse(0).ring, 0.f, 0.001f) && near(pingPulse(0).flash, 0.f, 0.001f));
        CHECK(pingPulse(kPulseMs).ring == 1.f && pingPulse(kPulseMs).scale == 1.f && pingPulse(kPulseMs).flash == 0.f);
        CHECK(pingPulse(kPulseMs + 20000).ring == 1.f && pingPulse(4000000000u).scale == 1.f);
        CHECK(at(0.05f).flash > 0.97f && at(0.16f).flash > 0.97f); // two blinks, ~200 ms apart...
        CHECK(at(0.105f).flash < 0.05f && at(0.7f).flash == 0.f);  // ...with a gap between and nothing after
        CHECK(at(0.05f).scale > 1.5f);                              // swollen at a blink
        float lastRing = -1.f;
        for (unsigned t = 0; t <= kPulseMs; t += 25) {
            const auto p = pingPulse(t);
            CHECK(p.ring >= 0.f && p.ring <= 1.f && p.flash >= 0.f && p.flash <= 1.f);
            CHECK(p.scale >= 1.f && p.scale <= 1.76f);
            CHECK(p.ring >= lastRing);
            lastRing = p.ring;
        }
    }
    // The marker wheel: nine wedges, the first straight up and the rest clockwise; the middle picks nothing.
    {
        using mecmp::game::radialSlot;
        const float R = 100.f, dz = 20.f;
        const auto at = [&](float deg) { // a point on the circle, `deg` clockwise from straight up
            const float a = deg * 3.14159265f / 180.f;
            return radialSlot(R * std::sin(a), -R * std::cos(a), dz, 9);
        };
        for (int i = 0; i < 9; ++i) {
            CHECK(at(i * 40.f) == i);          // the wedge's own direction
            CHECK(at(i * 40.f + 17.f) == i);   // inside it, either side
            CHECK(at(i * 40.f - 17.f) == i);
        }
        CHECK(at(359.f) == 0 && at(1.f) == 0);                 // wraps at the top
        CHECK(radialSlot(0, 5, dz, 9) == -1 && radialSlot(0, 0, dz, 9) == -1);
        CHECK(radialSlot(0, -21, dz, 9) == 0 && radialSlot(21, 0, dz, 9) == 2);
        CHECK(radialSlot(NAN, 50, dz, 9) == -1 && radialSlot(50, NAN, dz, 9) == -1);
        CHECK(radialSlot(50, 50, dz, 0) == -1);
        CHECK(radialSlot(INFINITY, 0, dz, 9) >= 0 && radialSlot(INFINITY, 0, dz, 9) < 9);
    }
    if (g_failures == 0) std::printf("camera tests: all passed\n");
    return g_failures == 0 ? 0 : 1;
}
